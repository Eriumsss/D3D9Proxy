// D3D9Proxy: NATIVE ANIMATION CLIP/SKELETON BINDING CACHE 64 -> 256/512.
//
// This is an actual pair of level-lifetime engine caches, NOT the 1024-slot
// inlined animation blend pool, not GFX, and not a PAK/level importer.
//
// Verified original 32-bit ConquestLLC.exe SHA-256:
// 3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858
// Cross-referenced with Magellan MgAnTaskExec.inc, original .text and .rdata.
//
// Outer cache: clip CRC -> per-skeleton map. VTable 009C7A24 points to
//   008F90BC, which reserves 64 x 8-byte pairs in reg+0x0C. The animation
//   setup calls that slot at 00917E2B. We detour ONLY this method to a
//   trampoline that uses the ORIGINAL SmallVec8 reserve (00EA3A30),
//   then performs the original method's 0089F25C init flag gate.
// Inner cache: skeleton CRC -> resolved animation binding, created by
//   the 008F9301 CALL to shared 0040B4CB (reserve 64 entries).
//   That function is ALSO used by the separate level GFX block registry.
//   We replace ONLY animation's CALL with another small trampoline that
//   invokes the ORIGINAL SmallVec8 reserve using the map pointer in EBX.
//
// The existing map append, reg append, bsearch, qsort, release, free and
// registration lifetimes use the SmallVec count/capacity and pointer, not a
// hard-coded 64. Do NOT change global FUN_0040B4CB to break the GFX path.
// Patching MUST occur after the existing GFX plugin's exact opcode checks,
// before the first game level or binding registry is created. OFF by default.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "animation_binding_cache_512.h"

#ifndef _M_IX86
#error Animation binding cache trampoline targets original retail x86 ABI only.
#endif

namespace {
static const DWORD kImageBase=0x00400000;
static const DWORD kRegReserveMethod=0x008F90BC;
static const DWORD kRegReserveContinuation=0x0089F25C;
static const DWORD kAnimMapContext=0x008F92FF;
static const DWORD kAnimMapCall=0x008F9301;
static const DWORD kSharedReserve40=0x0040B4CB;
static const DWORD kNativeReserve8=0x00EA3A30;
static const DWORD kBindingVtableSlot=0x009C7A24;
static const DWORD kBindingSetupCaller=0x00917E23;
static const DWORD kLevelGlobal=0x00CD7EA0;
static const DWORD kResourceManagerGlobal=0x00CD80D8;
static const DWORD kBindingRegistryGlobal=0x00CD7E5C;
static const DWORD kCap256=0x100;
static const DWORD kCap512=0x200;

static const BYTE kRegReserveBefore[]={
    0x53,0x56,0x6A,0x40,0x8B,0xF1,0x5B,
    0x66,0x39,0x5E,0x0E,0x74,0x5B
};
static const BYTE kRegReserveTail[]={
    0x8B,0xCE,0x5E,0x5B,0xE9,0x2F,0x61,0xFA,0xFF
};
static const BYTE kBindingVtableBefore[]={
    0xBC,0x90,0x8F,0x00
};
static const BYTE kBindingSetupBefore[]={
    0x8B,0x0D,0x5C,0x7E,0xCD,0x00,0x8B,0x01,0xFF,0x50,0x08
};
static const BYTE kMapCallContextBefore[]={
    0x8B,0xF3,0xE8,0xC5,0x21,0xB1,0xFF,0x8B,0x45,0x0C
};
static const BYTE kSharedReserve40Before[]={
    0x51,0x53,0x57,0x6A,0x40,0x5B,0x66,0x39,0x5E,0x02
};
static const BYTE kNativeReserve8Before[]={
    0x66,0x39,0x7E,0x02,0x53,0x0F,0x84,0x67,0x00,0x00,0x00
};
static const BYTE kRegGateBefore[]={
    0x8B,0x41,0x08,0xA8,0x03,0x75,0x09,
    0x83,0xC8,0x01,0x89,0x41,0x08
};

static volatile LONG g_attempted=0;
static BYTE* g_stubPage=NULL;
static char g_log[MAX_PATH]={0};

static void Report(const char* status,const char* detail) {
    if (!g_log[0]) return;
    FILE* file=NULL;
    if (fopen_s(&file,g_log,"ab")!=0 || !file) return;
    fprintf(file,"[%lu] %s: %s\n",
            (unsigned long)GetTickCount(),status,detail);
    fclose(file);
}
static bool Readable(DWORD address,size_t length) {
    if (!length || address>0xFFFFFFFFu-(DWORD)length) return false;
    MEMORY_BASIC_INFORMATION m={0};
    if (!VirtualQuery((LPCVOID)(uintptr_t)address,&m,sizeof(m)) ||
        m.State!=MEM_COMMIT ||
        (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
        !(m.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|
                     PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|
                     PAGE_EXECUTE_WRITECOPY))) return false;
    return (uintptr_t)address+length <=
           (uintptr_t)m.BaseAddress+m.RegionSize;
}
static bool BytesEqual(DWORD address,const BYTE* expected,size_t size) {
    if (!Readable(address,size)) return false;
    __try {
        return memcmp((const void*)(uintptr_t)address,expected,size)==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static bool ExactImage() {
    return (DWORD)(uintptr_t)GetModuleHandleA(NULL)==kImageBase &&
        BytesEqual(kRegReserveMethod,kRegReserveBefore,sizeof(kRegReserveBefore)) &&
        BytesEqual(0x008F9124,kRegReserveTail,sizeof(kRegReserveTail)) &&
        BytesEqual(kBindingVtableSlot,kBindingVtableBefore,sizeof(kBindingVtableBefore)) &&
        BytesEqual(kBindingSetupCaller,kBindingSetupBefore,sizeof(kBindingSetupBefore)) &&
        BytesEqual(kAnimMapContext,kMapCallContextBefore,sizeof(kMapCallContextBefore)) &&
        BytesEqual(kSharedReserve40,kSharedReserve40Before,sizeof(kSharedReserve40Before)) &&
        BytesEqual(kNativeReserve8,kNativeReserve8Before,sizeof(kNativeReserve8Before)) &&
        BytesEqual(kRegReserveContinuation,kRegGateBefore,sizeof(kRegGateBefore));
}
static bool BeforeBindings() {
    if (!Readable(kLevelGlobal,4) ||
        !Readable(kResourceManagerGlobal,4) ||
        !Readable(kBindingRegistryGlobal,4)) return false;
    __try {
        return *(volatile const DWORD*)(uintptr_t)kLevelGlobal==0 &&
               *(volatile const DWORD*)(uintptr_t)kResourceManagerGlobal==0 &&
               *(volatile const DWORD*)(uintptr_t)kBindingRegistryGlobal==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static void Put32(BYTE* dst,DWORD value) {
    memcpy(dst,&value,sizeof(value));
}
static void PutRel32(BYTE* at,DWORD next,DWORD target) {
    // x86 signed rel32 displacement is interpreted modulo the 32-bit VA.
    Put32(at,target-next);
}
static bool WriteBytes(DWORD address,const BYTE* bytes,size_t length) {
    __try {
        memcpy((void*)(uintptr_t)address,bytes,length);
        return memcmp((const void*)(uintptr_t)address,bytes,length)==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static bool BuildStubs(BYTE* page,DWORD cap) {
    // Cache registry method (THISCALL ECX=registry), replacing vtbl+8 target:
    //  pushfd; pushad; lea esi,[ecx+0x0c]; mov edi,capacity;
    //  mov eax,00EA3A30; call eax; popad; popfd;
    //  mov eax,0089F25C; jmp eax
    // Original 008F90BC exits through 0089F25C, which sets the registry's
    // flags bit 0. Preserving that tail is REQUIRED for level teardown.
    BYTE reg[]={
        0x9C,0x60,0x8D,0x71,0x0C,
        0xBF,0,0,0,0,
        0xB8,0x30,0x3A,0xEA,0x00,
        0xFF,0xD0,
        0x61,0x9D,
        0xB8,0x5C,0xF2,0x89,0x00,
        0xFF,0xE0
    };
    // Map creation CALL at 008F9301 (ESI=EBX set at 008F92FF):
    //  pushfd; pushad; mov esi,ebx; mov edi,capacity;
    //  mov eax,00EA3A30; call eax; popad; popfd; ret
    BYTE map[]={
        0x9C,0x60,0x8B,0xF3,
        0xBF,0,0,0,0,
        0xB8,0x30,0x3A,0xEA,0x00,
        0xFF,0xD0,
        0x61,0x9D,0xC3
    };
    static_assert(sizeof(reg)==26,"unexpected outer cache trampoline length");
    static_assert(sizeof(map)==19,"unexpected inner cache trampoline length");
    Put32(reg+6,cap);
    Put32(map+5,cap);
    memcpy(page,reg,sizeof(reg));
    memcpy(page+128,map,sizeof(map));
    DWORD old=0;
    if (!VirtualProtect(page,4096,PAGE_EXECUTE_READ,&old))
        return false;
    return FlushInstructionCache(GetCurrentProcess(),page,4096)!=FALSE;
}
static bool Install(DWORD cap) {
    if (cap!=kCap256 && cap!=kCap512) {
        Report("REJECT","unsupported native animation binding cache tier");
        return false;
    }
    if (!ExactImage() || !BeforeBindings()) {
        Report("REJECT","EXE signature unknown or level/binding manager already running");
        return false;
    }
    BYTE* code=(BYTE*)VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!code) {
        Report("REJECT","could not allocate x86 trampolines");
        return false;
    }
    if (!BuildStubs(code,cap)) {
        VirtualFree(code,0,MEM_RELEASE);
        Report("REJECT","could not publish executable trampolines");
        return false;
    }
    BYTE regJmp[7]={0xE9,0,0,0,0,0x90,0x90};
    PutRel32(regJmp+1,kRegReserveMethod+5,(DWORD)(uintptr_t)code);
    BYTE mapCall[5]={0xE8,0,0,0,0};
    PutRel32(mapCall+1,kAnimMapCall+5,(DWORD)(uintptr_t)(code+128));

    DWORD regOld=0,mapOld=0,unused=0;
    if (!VirtualProtect((LPVOID)(uintptr_t)kRegReserveMethod,
                        sizeof(regJmp),PAGE_EXECUTE_READWRITE,&regOld)) {
        VirtualFree(code,0,MEM_RELEASE);
        Report("REJECT","cannot write-protect native outer cache method");
        return false;
    }
    if (!VirtualProtect((LPVOID)(uintptr_t)kAnimMapCall,
                        sizeof(mapCall),PAGE_EXECUTE_READWRITE,&mapOld)) {
        VirtualProtect((LPVOID)(uintptr_t)kRegReserveMethod,
                       sizeof(regJmp),regOld,&unused);
        VirtualFree(code,0,MEM_RELEASE);
        Report("REJECT","cannot write-protect inner cache CALL");
        return false;
    }

    bool okay=ExactImage() && BeforeBindings();
    bool triedReg=false,triedMap=false;
    if (okay) {
        triedReg=true;
        okay=WriteBytes(kRegReserveMethod,regJmp,sizeof(regJmp));
        if (okay) {
            triedMap=true;
            okay=WriteBytes(kAnimMapCall,mapCall,sizeof(mapCall));
        }
    }
    bool restored=true;
    if (!okay) {
        // Reverse publication order. If any restoration fails, never free
        // executable stubs that a surviving patch might still reference.
        if (triedMap)
            restored=WriteBytes(kAnimMapCall,kMapCallContextBefore+2,
                                sizeof(mapCall)) && restored;
        if (triedReg)
            restored=WriteBytes(kRegReserveMethod,kRegReserveBefore,
                                sizeof(regJmp)) && restored;
        Report(restored?"REJECT":"FATAL",
               restored?"patch aborted and original x86 bytes restored":
                        "cache patch rollback failed; terminate process");
    }
    FlushInstructionCache(GetCurrentProcess(),
                          (LPCVOID)(uintptr_t)kRegReserveMethod,sizeof(regJmp));
    FlushInstructionCache(GetCurrentProcess(),
                          (LPCVOID)(uintptr_t)kAnimMapCall,sizeof(mapCall));
    DWORD unused1=0,unused2=0;
    bool regProtected=VirtualProtect((LPVOID)(uintptr_t)kRegReserveMethod,
                                   sizeof(regJmp),regOld,&unused1)!=FALSE;
    bool mapProtected=VirtualProtect((LPVOID)(uintptr_t)kAnimMapCall,
                                   sizeof(mapCall),mapOld,&unused2)!=FALSE;
    if (!regProtected || !mapProtected) {
        g_stubPage=code;
        Report("FATAL","could not restore original x86 page protections");
        return false;
    }
    if (!okay) {
        if (BytesEqual(kRegReserveMethod,kRegReserveBefore,sizeof(regJmp)) &&
            BytesEqual(kAnimMapCall,kMapCallContextBefore+2,sizeof(mapCall)))
            VirtualFree(code,0,MEM_RELEASE);
        else {
            g_stubPage=code;
            Report("FATAL","original JMP/CALL was not restored; keep stubs mapped");
        }
        return false;
    }
    g_stubPage=code;
    char detail[192]={0};
    _snprintf_s(detail,sizeof(detail),_TRUNCATE,
                "native clip registry AND skeleton binding map 64 -> %lu each; GFX allocator untouched",
                (unsigned long)cap);
    Report("ACTIVE",detail);
    return true;
}
} // namespace

void OC_AnimationBindingCache512_Init() {
    if (InterlockedCompareExchange(&g_attempted,1,0)!=0) return;
    char path[MAX_PATH]={0};
    DWORD n=GetModuleFileNameA(NULL,path,MAX_PATH);
    if (!n || n>=MAX_PATH) return;
    char* slash=strrchr(path,'\\');
    if (!slash) return;
    slash[1]='\0';
    char ini[MAX_PATH]={0};
    if (_snprintf_s(ini,sizeof(ini),_TRUNCATE,"%sproxy.ini",path)<0) return;
    const bool high=GetPrivateProfileIntA("StaticMapPools",
                      "ExpandAnimationBindingCache512",0,ini)!=0;
    const bool low=GetPrivateProfileIntA("StaticMapPools",
                     "ExpandAnimationBindingCache256",0,ini)!=0;
    if (!high && !low) return; // both OFF by default
    if (_snprintf_s(g_log,sizeof(g_log),_TRUNCATE,
                    "%sanimation_binding_cache_512.log",path)<0) return;
    Report("BEGIN",high?"opt-in native binding cache 512":"opt-in native binding cache 256");
    Install(high?kCap512:kCap256);
}
