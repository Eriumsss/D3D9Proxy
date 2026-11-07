// D3D9Proxy: NATIVE D3D DEVICE-RESET RESOURCE LIST 64 -> 1024 / 4096.
//
// This is NOT the ModelInfo registry, Havok pool, or a second level loader.
// Ground truth: Magellan MgEntityMgr.cpp + byte-exact recovered ConquestLLC.exe
// SHA256 3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858.
//
// 00967C27 (EXE CRT boot): engine's 0067E662 realloc(0,0x100,0) creates
// the [00A41538] pointer array, atexit 0096F72D frees that SAME pointer.
// 0068D892: device resource success/flag +0x51 => [base+count*4]=object;
//            increment [00A4153C] WITHOUT BOUNDS CHECK. 65th slot is OOB.
// 0068D299: unregister swaps with last; 0068D861: device-reset walker reads
//            the same global base and count. Neither hardcodes capacity.
//
// Opt-in patch:
//   * Boot allocation operand (00967C2A): 0x100 -> cap * sizeof(void*).
//   * If CRT boot ran before Direct3DCreate9, safely GROW THE EXISTING array
//     through the ORIGINAL retail 0067E662 allocator while count == 0;
//     update the global base pointer to the native realloc result.
//   * Replace exactly the two MOV instructions at 0068D8A3 with a guarded
//     executable trampoline. At cap/null base, clear registration flag
//     [+0x51] and take original return path. Otherwise replay both original
//     MOVs and let original append/increment/unregister/reset proceed.
//   * Never touch the sibling 00967C06 allocation of [00A4152C].
//   * Both opcode sites are version-gated and patched pre-level; default OFF.
//
// If the device-reset list already contains live entries, bail out rather
// than migrating pointers underneath D3D9's reset/recreate paths.
// 
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "device_reset_pool_4096.h"

#ifndef _M_IX86
#error The retail D3D9 device-reset list uses 32-bit pointers; x86 required.
#endif

namespace {
static const DWORD kImageBase=0x00400000;
static const DWORD kBoot=0x00967C27;
static const DWORD kBootBytesImm=0x00967C2A;
static const DWORD kSiblingBoot=0x00967C06;
static const DWORD kAppend=0x0068D8A3;
static const DWORD kAppendOriginal=0x0068D8AE;
static const DWORD kAppendReturn=0x0068D8B7;
static const DWORD kUnregister=0x0068D299;
static const DWORD kResetWalk=0x0068D861;
static const DWORD kAtexit=0x0096F72D;
static const DWORD kNativeRealloc=0x0067E662;
static const DWORD kBaseCell=0x00A41538;
static const DWORD kCountCell=0x00A4153C;
static const DWORD kLevelCell=0x00CD7EA0;
static const DWORD kResourceMgrCell=0x00CD80D8;

static const BYTE kBootBytesBefore[]={
    0x6A,0x00,0x68,0x00,0x01,0x00,0x00,0x6A,0x00,
    0xE8,0x2D,0x6A,0xD1,0xFF,
    0x68,0x2D,0xF7,0x96,0x00,
    0xA3,0x38,0x15,0xA4,0x00
};
static const BYTE kSiblingBootBefore[]={
    0x6A,0x00,0x68,0x00,0x01,0x00,0x00,0x6A,0x00,
    0xE8,0x4E,0x6A,0xD1,0xFF
};
static const BYTE kAppendBefore[]={
    0xA1,0x3C,0x15,0xA4,0x00,
    0x8B,0x0D,0x38,0x15,0xA4,0x00
};
static const BYTE kAppendAfter[]={
    0x89,0x3C,0x81,
    0xFF,0x05,0x3C,0x15,0xA4,0x00,
    0x8A,0x47,0x50,0x5F,0xC3
};
static const BYTE kUnregisterBefore[]={
    0x56,0x8B,0xF1,0xE8,0xB7,0xFE,0xFF,0xFF,
    0x85,0xC0,0x75,0x36
};
static const BYTE kResetWalkBefore[]={
    0xA1,0x3C,0x15,0xA4,0x00,0x56,
    0x8B,0x35,0x38,0x15,0xA4,0x00
};
static const BYTE kAtexitBefore[]={
    0xFF,0x35,0x38,0x15,0xA4,0x00,
    0x83,0x25,0x3C,0x15,0xA4,0x00,0x00,
    0xE8,0x64,0xEE,0xD0,0xFF
};
static const BYTE kReallocBefore[]={
    0x51,0x56,0x6A,0x02,0x58,
    0xE8,0x87,0xD1,0xFF,0xFF,
    0xFF,0x74,0x24,0x14
};
static const DWORD kCap1024=0x400;
static const DWORD kCap4096=0x1000;
static volatile LONG g_once=0;
static char g_log[MAX_PATH]={0};
static BYTE* g_executableStub=NULL; // process lifetime if installed

static void Report(const char* stage,const char* detail) {
    if (!g_log[0]) return;
    FILE* f=NULL;
    if (fopen_s(&f,g_log,"ab")!=0 || !f) return;
    fprintf(f,"[%lu] %s: %s\n",(unsigned long)GetTickCount(),stage,detail);
    fclose(f);
}
static bool Readable(DWORD address,size_t length) {
    if (!length || address>0xFFFFFFFFu-(DWORD)length) return false;
    MEMORY_BASIC_INFORMATION info={0};
    if (!VirtualQuery((LPCVOID)(uintptr_t)address,&info,sizeof(info)) ||
        info.State!=MEM_COMMIT ||
        (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
        !(info.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|
                        PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|
                        PAGE_EXECUTE_WRITECOPY)))
        return false;
    return (uintptr_t)address+length<=(uintptr_t)info.BaseAddress+info.RegionSize;
}
static bool Match(DWORD address,const BYTE* data,size_t length) {
    if (!Readable(address,length)) return false;
    __try { return memcmp((const void*)(uintptr_t)address,data,length)==0; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool VersionMatches() {
    return (DWORD)(uintptr_t)GetModuleHandleA(NULL)==kImageBase &&
           Match(kBoot,kBootBytesBefore,sizeof(kBootBytesBefore)) &&
           Match(kSiblingBoot,kSiblingBootBefore,sizeof(kSiblingBootBefore)) &&
           Match(kAppend,kAppendBefore,sizeof(kAppendBefore)) &&
           Match(kAppendOriginal,kAppendAfter,sizeof(kAppendAfter)) &&
           Match(kUnregister,kUnregisterBefore,sizeof(kUnregisterBefore)) &&
           Match(kResetWalk,kResetWalkBefore,sizeof(kResetWalkBefore)) &&
           Match(kAtexit,kAtexitBefore,sizeof(kAtexitBefore)) &&
           Match(kNativeRealloc,kReallocBefore,sizeof(kReallocBefore));
}
static bool ReadState(DWORD& ptr,DWORD& count) {
    if (!Readable(kBaseCell,4) || !Readable(kCountCell,4) ||
        !Readable(kLevelCell,4) || !Readable(kResourceMgrCell,4))
        return false;
    __try {
        if (*(volatile DWORD*)(uintptr_t)kLevelCell ||
            *(volatile DWORD*)(uintptr_t)kResourceMgrCell)
            return false;
        ptr=*(volatile DWORD*)(uintptr_t)kBaseCell;
        count=*(volatile DWORD*)(uintptr_t)kCountCell;
        return count==0; // never relocate already registered resources
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void Put32(BYTE* addr,DWORD x) { memcpy(addr,&x,4); }
static void PutRel32(BYTE* out,DWORD next,DWORD target) {
    // x86 rel32 wraps modulo 2^32; this is not an x64 hook.
    Put32(out,target-next);
}
static bool Store(DWORD address,const BYTE* bytes,size_t size) {
    __try {
        memcpy((void*)(uintptr_t)address,bytes,size);
        return memcmp((void*)(uintptr_t)address,bytes,size)==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool BuildStub(BYTE* page,DWORD cap) {
    // 00  mov ecx,[00A41538]      ; live engine pointer
    // 06  test ecx,ecx
    // 08  jz +0x11                ; empty/null => skip registration
    // 10  mov eax,[00A4153C]     ; live count
    // 15  cmp eax,cap
    // 20  jae +5                 ; full => skip registration
    // 22  jmp 0068D8AE          ; replay original append path
    // 27  mov byte ptr [edi+51h],0 ; unregister state must stay false
    // 31  jmp 0068D8B7          ; original result/return
    BYTE stub[]={
        0x8B,0x0D,0x38,0x15,0xA4,0x00,
        0x85,0xC9,
        0x74,0x11,
        0xA1,0x3C,0x15,0xA4,0x00,
        0x3D,0,0,0,0,
        0x73,0x05,
        0xE9,0,0,0,0,
        0xC6,0x47,0x51,0x00,
        0xE9,0,0,0,0
    };
    static_assert(sizeof(stub)==36,"guard trampoline byte count changed");
    Put32(stub+16,cap);
    PutRel32(stub+23,(DWORD)(uintptr_t)(page+27),kAppendOriginal);
    PutRel32(stub+32,(DWORD)(uintptr_t)(page+36),kAppendReturn);
    memcpy(page,stub,sizeof(stub));
    DWORD old=0;
    if (!VirtualProtect(page,4096,PAGE_EXECUTE_READ,&old)) return false;
    return !!FlushInstructionCache(GetCurrentProcess(),page,4096);
}
typedef void* (__cdecl *NativeRealloc)(void*,DWORD,DWORD);
static bool ResizeExistingList(DWORD original,DWORD bytes) {
    if (!original) return true; // CRT boot will use patched byte count
    if (!Readable(original,0x100)) {
        Report("REJECT","existing 64-slot engine pointer is invalid");
        return false;
    }
    // This is the original EXE's mutexed *realloc*, not VirtualAlloc,
    // malloc, or a new allocator. CRT atexit still owns the returned block.
    NativeRealloc fn=(NativeRealloc)(uintptr_t)kNativeRealloc;
    void* expanded=NULL;
    __try { expanded=fn((void*)(uintptr_t)original,bytes,0); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        Report("FATAL","native realloc exception; abort capacity patch");
        return false;
    }
    if (!expanded) {
        Report("REJECT","native realloc returned NULL; original array not changed");
        return false;
    }
    // Preserve the engine's single pointer cell, which its append, unregister,
    // lost/reset walkers and atexit free already read dynamically.
    InterlockedExchange((volatile LONG*)(uintptr_t)kBaseCell,
                        (LONG)(uintptr_t)expanded);
    return true;
}
static bool Install(DWORD cap) {
    if (cap!=kCap1024 && cap!=kCap4096) {
        Report("REJECT","unrecognized device-reset list capacity");
        return false;
    }
    DWORD originalBase=0,originalCount=0;
    if (!VersionMatches() || !ReadState(originalBase,originalCount)) {
        Report("REJECT","unknown executable, existing level or nonempty reset list");
        return false;
    }
    BYTE* stub=(BYTE*)VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!stub || !BuildStub(stub,cap)) {
        if (stub) VirtualFree(stub,0,MEM_RELEASE);
        Report("REJECT","guard stub allocation or RX transition failed");
        return false;
    }
    BYTE jmp[sizeof(kAppendBefore)]={0xE9};
    PutRel32(jmp+1,kAppend+5,(DWORD)(uintptr_t)stub);
    for (size_t i=5;i<sizeof(jmp);++i) jmp[i]=0x90;

    const DWORD desiredBytes=cap*4u;
    DWORD oldBootProtect=0,oldAppendProtect=0,dummy=0;
    if (!VirtualProtect((LPVOID)(uintptr_t)kBootBytesImm,4,
                        PAGE_EXECUTE_READWRITE,&oldBootProtect)) {
        VirtualFree(stub,0,MEM_RELEASE);
        Report("REJECT","could not protect CRT allocation immediate");
        return false;
    }
    if (!VirtualProtect((LPVOID)(uintptr_t)kAppend,sizeof(jmp),
                        PAGE_EXECUTE_READWRITE,&oldAppendProtect)) {
        VirtualProtect((LPVOID)(uintptr_t)kBootBytesImm,4,oldBootProtect,&dummy);
        VirtualFree(stub,0,MEM_RELEASE);
        Report("REJECT","could not protect reset-list append");
        return false;
    }

    DWORD originalBase2=0,count2=0;
    bool good=VersionMatches() && ReadState(originalBase2,count2) &&
              originalBase2==originalBase;
    // If the game's CRT constructor already allocated 64 slots (normal when
    // D3DCreate9 loads after CRT init), expand it FIRST. No stage ever exposes
    // the high-cap guard while the old 64-slot allocation is still live.
    if (good) good=ResizeExistingList(originalBase,desiredBytes);
    bool wroteBoot=false,wroteAppend=false;
    if (good) {
        BYTE newBytes[4]={0};
        Put32(newBytes,desiredBytes);
        wroteBoot=true;
        good=Store(kBootBytesImm,newBytes,sizeof(newBytes));
        if (good) {
            wroteAppend=true;
            good=Store(kAppend,jmp,sizeof(jmp));
        }
    }
    if (!good) {
        bool restored=true;
        if (wroteAppend)
            restored=Store(kAppend,kAppendBefore,sizeof(kAppendBefore))&&restored;
        if (wroteBoot)
            restored=Store(kBootBytesImm,kBootBytesBefore+3,4)&&restored;
        Report(restored?"REJECT":"FATAL",
               restored?"patch failed; all attempted instruction edits restored":
                        "rollback failed; stop game process now");
    }
    FlushInstructionCache(GetCurrentProcess(),(LPCVOID)(uintptr_t)kBootBytesImm,4);
    FlushInstructionCache(GetCurrentProcess(),(LPCVOID)(uintptr_t)kAppend,sizeof(jmp));
    DWORD unused1=0,unused2=0;
    bool p1=!!VirtualProtect((LPVOID)(uintptr_t)kBootBytesImm,4,oldBootProtect,&unused1);
    bool p2=!!VirtualProtect((LPVOID)(uintptr_t)kAppend,sizeof(jmp),oldAppendProtect,&unused2);
    if (!p1 || !p2) {
        g_executableStub=stub; // may have been published
        Report("FATAL","could not restore EXE page protections");
        return false;
    }
    if (!good) {
        if (Match(kAppend,kAppendBefore,sizeof(kAppendBefore))) {
            VirtualFree(stub,0,MEM_RELEASE);
        } else {
            g_executableStub=stub;
            Report("FATAL","append still targets stub, process restart required");
        }
        return false;
    }
    g_executableStub=stub;
    char line[160]={0};
    _snprintf_s(line,sizeof(line),_TRUNCATE,
                "native reset-list allocation and append guard: 64 -> %lu; booted=%s",
                (unsigned long)cap,originalBase?"yes/reallocated":"no/future-boot");
    Report("ACTIVE",line);
    return true;
}
} // namespace

void OC_DeviceResetPool4096_Init() {
    if (InterlockedCompareExchange(&g_once,1,0)!=0) return;
    char path[MAX_PATH]={0};
    DWORD n=GetModuleFileNameA(NULL,path,MAX_PATH);
    if (!n || n>=MAX_PATH) return;
    char* slash=strrchr(path,'\\');
    if (!slash) return;
    slash[1]=0;
    char ini[MAX_PATH]={0};
    if (_snprintf_s(ini,sizeof(ini),_TRUNCATE,"%sproxy.ini",path)<0) return;
    bool high=GetPrivateProfileIntA("StaticMapPools","ExpandDeviceResetResources4096",0,ini)!=0;
    bool low=GetPrivateProfileIntA("StaticMapPools","ExpandDeviceResetResources1024",0,ini)!=0;
    if (!high && !low) return;
    if (_snprintf_s(g_log,sizeof(g_log),_TRUNCATE,
                    "%sdevice_reset_pool_4096.log",path)<0) return;
    Report("BEGIN",high?"4096 reset resource slots":"1024 reset resource slots");
    Install(high?kCap4096:kCap1024);
}
