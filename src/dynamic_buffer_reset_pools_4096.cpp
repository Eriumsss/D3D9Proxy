// OpenConquest D3D9Proxy: the *two* native dynamic D3D buffer recreate registries.
//
// The retail game uses 64 pointers for VB and 64 for IB, each allocated by
// its own CRT initializer with 0x100 bytes. The original register path has
// no bounds test. These pools are NOT the generic device-reset resource list
// (Eriumsss/D3D9Proxy PR #4), the static ModelInfo registry or Havok.
//
// Original ConquestLLC.exe SHA256:
// 3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858
// Verified against actual .text bytes and MgGameBuffers.cpp. No EXE redistributed.
//
// Native pointers:
//   VB base [00A41520] count [00A41524], boot 00967BE5,
//      register 0068DFD3, destroy 0068E056, device walks 0068DEBC/0068DF77.
//   IB base [00A4152C] count [00A41530], boot 00967C06,
//      register 0068DCC1, destroy 0068DD06, device walks 0068DB93/0068DC5A.
//
// Patch each actual CRT PUSH size operand (0x100 -> cap*4), grow an already
// booted empty array through ORIGINAL engine locked realloc (0067E662), and
// route only the original eleven-byte register prologue into a tiny, guarded
// x86 trampoline. The original append, destroy, reset and atexit code remain.
//
// On a full/null list the untracked dynamic wrapper is still a valid buffer:
// DO NOT clear flag bit 2. That bit also governs DISCARD lock semantics!
// Original unregister searches the list and returns if entry is absent.
// Exceeding the new cap therefore prevents reset reconstruction for overflow
// entries rather than scribbling heap memory. Default OFF; no second world.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dynamic_buffer_reset_pools_4096.h"

#ifndef _M_IX86
#error Native D3D dynamic buffer registry extension is Windows x86 only.
#endif

namespace {

static const DWORD kExeBase = 0x00400000;
static const DWORD kNativeRealloc = 0x0067E662;
static const DWORD kLevelCell = 0x00CD7EA0;
static const DWORD kResourceManagerCell = 0x00CD80D8;
static const DWORD kCap1024 = 0x400;
static const DWORD kCap4096 = 0x1000;
static const DWORD kInitialCap = 64;

// Distinct original allocation and ownership paths for each list.
static const BYTE kVBBoot[] = {
    0x6A,0x00,0x68,0x00,0x01,0x00,0x00,0x6A,0x00,
    0xE8,0x6F,0x6A,0xD1,0xFF,0x68,0x05,0xF7,0x96,0x00,
    0xA3,0x20,0x15,0xA4,0x00,0xE8,0x07,0x5B,0xCA,0xFF,
    0x83,0xC4,0x10,0xC3
};
static const BYTE kIBBoot[] = {
    0x6A,0x00,0x68,0x00,0x01,0x00,0x00,0x6A,0x00,
    0xE8,0x4E,0x6A,0xD1,0xFF,0x68,0x19,0xF7,0x96,0x00,
    0xA3,0x2C,0x15,0xA4,0x00,0xE8,0xE6,0x5A,0xCA,0xFF,
    0x83,0xC4,0x10,0xC3
};
static const BYTE kVBRegisterPrefix[] = {
    0xA1,0x24,0x15,0xA4,0x00, 0x8B,0x0D,0x20,0x15,0xA4,0x00
};
static const BYTE kIBRegisterPrefix[] = {
    0xA1,0x30,0x15,0xA4,0x00, 0x8B,0x0D,0x2C,0x15,0xA4,0x00
};
static const BYTE kVBAppendAndExit[] = {
    0x89,0x34,0x81,0xFF,0x05,0x24,0x15,0xA4,0x00,
    0x5E,0xC2,0x04,0x00
};
static const BYTE kIBAppendAndExit[] = {
    0x89,0x34,0x81,0xFF,0x05,0x30,0x15,0xA4,0x00,
    0x5E,0xC2,0x04,0x00
};
static const BYTE kVBUnregister[] = {
    0xA1,0x20,0x15,0xA4,0x00,0x8B,0x0D,0x24,0x15,0xA4,0x00
};
static const BYTE kIBUnregister[] = {
    0xA1,0x2C,0x15,0xA4,0x00,0x8B,0x0D,0x30,0x15,0xA4,0x00
};
static const BYTE kVBRelease[] = {
    0xA1,0x24,0x15,0xA4,0x00,0x57,0x8B,0x3D,0x20,0x15,0xA4,0x00
};
static const BYTE kIBRelease[] = {
    0xA1,0x30,0x15,0xA4,0x00,0x57,0x8B,0x3D,0x2C,0x15,0xA4,0x00
};
static const BYTE kVBRecreate[] = {
    0xA1,0x24,0x15,0xA4,0x00,0x57,0x8B,0x3D,0x20,0x15,0xA4,0x00
};
static const BYTE kIBRecreate[] = {
    0xA1,0x30,0x15,0xA4,0x00,0x57,0x8B,0x3D,0x2C,0x15,0xA4,0x00
};
static const BYTE kVBAtExit[] = {
    0xFF,0x35,0x20,0x15,0xA4,0x00,
    0x83,0x25,0x24,0x15,0xA4,0x00,0x00,
    0xE8,0x8C,0xEE,0xD0,0xFF,0x59,0xC3
};
static const BYTE kIBAtExit[] = {
    0xFF,0x35,0x2C,0x15,0xA4,0x00,
    0x83,0x25,0x30,0x15,0xA4,0x00,0x00,
    0xE8,0x78,0xEE,0xD0,0xFF,0x59,0xC3
};
static const BYTE kNativeReallocStart[] = {
    0x51,0x56,0x6A,0x02,0x58,0xE8,0x87,0xD1,0xFF,0xFF,
    0xFF,0x74,0x24,0x14,0xFF,0x74,0x24,0x14,
    0xFF,0x74,0x24,0x14
};

struct Pool {
    const char* name;
    DWORD boot;
    const BYTE* bootSig;
    unsigned bootLen;
    DWORD registerAt;
    const BYTE* registerSig;
    unsigned registerLen;
    DWORD originalAppend;
    DWORD originalReturn;
    const BYTE* appendSig;
    unsigned appendLen;
    DWORD unregisterAt;
    const BYTE* unregisterSig;
    unsigned unregisterLen;
    DWORD releaseAt;
    const BYTE* releaseSig;
    unsigned releaseLen;
    DWORD recreateAt;
    const BYTE* recreateSig;
    unsigned recreateLen;
    DWORD atExit;
    const BYTE* atExitSig;
    unsigned atExitLen;
    DWORD baseGlobal;
    DWORD countGlobal;
    BYTE* stub;
};

static Pool g_pools[2] = {
    {
        "dynamic VB",0x00967BE5,kVBBoot,sizeof(kVBBoot),
        0x0068DFD3,kVBRegisterPrefix,sizeof(kVBRegisterPrefix),
        0x0068DFDE,0x0068DFE7,kVBAppendAndExit,sizeof(kVBAppendAndExit),
        0x0068E056,kVBUnregister,sizeof(kVBUnregister),
        0x0068DEBC,kVBRelease,sizeof(kVBRelease),
        0x0068DF77,kVBRecreate,sizeof(kVBRecreate),
        0x0096F705,kVBAtExit,sizeof(kVBAtExit),
        0x00A41520,0x00A41524,NULL
    },
    {
        "dynamic IB",0x00967C06,kIBBoot,sizeof(kIBBoot),
        0x0068DCC1,kIBRegisterPrefix,sizeof(kIBRegisterPrefix),
        0x0068DCCC,0x0068DCD5,kIBAppendAndExit,sizeof(kIBAppendAndExit),
        0x0068DD06,kIBUnregister,sizeof(kIBUnregister),
        0x0068DB93,kIBRelease,sizeof(kIBRelease),
        0x0068DC5A,kIBRecreate,sizeof(kIBRecreate),
        0x0096F719,kIBAtExit,sizeof(kIBAtExit),
        0x00A4152C,0x00A41530,NULL
    }
};

static volatile LONG g_attempted=0;
static char g_log[MAX_PATH]={0};
static BYTE* g_stubPage=NULL;

static void Log(const char* state,const char* detail) {
    if (!g_log[0]) return;
    FILE* f=NULL;
    if (fopen_s(&f,g_log,"ab")!=0 || !f) return;
    fprintf(f,"[%lu] %s: %s\n",(unsigned long)GetTickCount(),state,detail);
    fclose(f);
}

static bool CanRead(DWORD ptr,size_t n) {
    if (!n || ptr>0xFFFFFFFFu-(DWORD)n) return false;
    MEMORY_BASIC_INFORMATION v={0};
    if (!VirtualQuery((LPCVOID)(uintptr_t)ptr,&v,sizeof(v))) return false;
    if (v.State!=MEM_COMMIT || (v.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
    if (!(v.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|
                    PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|
                    PAGE_EXECUTE_WRITECOPY))) return false;
    return (uintptr_t)ptr+n <= (uintptr_t)v.BaseAddress+v.RegionSize;
}
static bool EqualBytes(DWORD at,const BYTE* source,size_t len) {
    if (!CanRead(at,len)) return false;
    __try {
        return memcmp((const void*)(uintptr_t)at,source,len)==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool ExactImage() {
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL)!=kExeBase ||
        !EqualBytes(kNativeRealloc,kNativeReallocStart,sizeof(kNativeReallocStart)))
        return false;
    for (unsigned i=0;i<2;++i) {
        const Pool& p=g_pools[i];
        if (!EqualBytes(p.boot,p.bootSig,p.bootLen) ||
            !EqualBytes(p.registerAt,p.registerSig,p.registerLen) ||
            !EqualBytes(p.originalAppend,p.appendSig,p.appendLen) ||
            !EqualBytes(p.unregisterAt,p.unregisterSig,p.unregisterLen) ||
            !EqualBytes(p.releaseAt,p.releaseSig,p.releaseLen) ||
            !EqualBytes(p.recreateAt,p.recreateSig,p.recreateLen) ||
            !EqualBytes(p.atExit,p.atExitSig,p.atExitLen))
            return false;
    }
    return true;
}

static bool CleanStart(DWORD bases[2]) {
    if (!CanRead(kLevelCell,4) || !CanRead(kResourceManagerCell,4))
        return false;
    for (unsigned i=0;i<2;++i)
        if (!CanRead(g_pools[i].baseGlobal,4) ||
            !CanRead(g_pools[i].countGlobal,4))
            return false;
    __try {
        if (*(volatile DWORD*)(uintptr_t)kLevelCell ||
            *(volatile DWORD*)(uintptr_t)kResourceManagerCell)
            return false;
        for (unsigned i=0;i<2;++i) {
            bases[i]=*(volatile DWORD*)(uintptr_t)g_pools[i].baseGlobal;
            if (*(volatile DWORD*)(uintptr_t)g_pools[i].countGlobal!=0)
                return false;
            if (bases[i] && !CanRead(bases[i],kInitialCap*4u))
                return false;
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void PutU32(BYTE* out,DWORD value) { memcpy(out,&value,4); }
static void PutRel32(BYTE* where,DWORD nextAddress,DWORD targetAddress) {
    // x86 EIP rel32 arithmetic is modulo 2^32.
    PutU32(where,targetAddress-nextAddress);
}
static bool Store(DWORD at,const BYTE* bytes,size_t n) {
    __try {
        memcpy((void*)(uintptr_t)at,bytes,n);
        return memcmp((const void*)(uintptr_t)at,bytes,n)==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// This trampoline is deliberately 32 bytes, not a hand-wavy pseudo-hook:
//   mov ecx,[native base]    6
//   test ecx,ecx             2
//   jz skip                 2 (+17)
//   mov eax,[native count]   5
//   cmp eax,capacity         5
//   jae skip                2 (+5)
//   jmp original append      5
// skip: jmp original exit    5
// On the success path EAX and ECX contain EXACTLY the original register values
// needed by MOV [ECX+EAX*4],ESI. On skip the retail return is a void RET 4.
// We never change the dynamic flag bit, as it controls DISCARD lock policy.
static bool BuildTrampoline(BYTE* at,const Pool& p,DWORD cap) {
    BYTE code[]={
        0x8B,0x0D,0,0,0,0, 0x85,0xC9, 0x74,0x11,
        0xA1,0,0,0,0, 0x3D,0,0,0,0, 0x73,0x05,
        0xE9,0,0,0,0,
        0xE9,0,0,0,0
    };
    static_assert(sizeof(code)==32,"dynamic register trampoline length changed");
    PutU32(code+2,p.baseGlobal);
    PutU32(code+11,p.countGlobal);
    PutU32(code+16,cap);
    PutRel32(code+23,(DWORD)(uintptr_t)(at+27),p.originalAppend);
    PutRel32(code+28,(DWORD)(uintptr_t)(at+32),p.originalReturn);
    memcpy(at,code,sizeof(code));
    return true;
}

typedef void* (__cdecl *NativeRealloc)(void*,DWORD,DWORD);
static bool GrowEmptyList(const Pool& p,DWORD original,DWORD bytes) {
    if (!original) return true; // CRT boot not yet executed; patched size wins.
    // A real in-process native realloc, NOT malloc followed by an alien free.
    NativeRealloc native=(NativeRealloc)(uintptr_t)kNativeRealloc;
    void* newBase=NULL;
    __try {
        newBase=native((void*)(uintptr_t)original,bytes,0);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("FATAL","native heap realloc threw; cannot install dynamic pool patch");
        return false;
    }
    if (!newBase) {
        Log("REJECT","native heap realloc failed; old array untouched");
        return false;
    }
    InterlockedExchange((volatile LONG*)(uintptr_t)p.baseGlobal,
                        (LONG)(uintptr_t)newBase);
    return true;
}

struct CodePage {
    DWORD at;
    size_t length;
    DWORD oldProtection;
    bool protectedNow;
};
static bool ProtectAll(CodePage pages[4]) {
    for (unsigned i=0;i<4;++i) {
        if (!VirtualProtect((LPVOID)(uintptr_t)pages[i].at,pages[i].length,
                            PAGE_EXECUTE_READWRITE,&pages[i].oldProtection)) {
            DWORD unused=0;
            while (i>0) {
                --i;
                VirtualProtect((LPVOID)(uintptr_t)pages[i].at,pages[i].length,
                               pages[i].oldProtection,&unused);
                pages[i].protectedNow=false;
            }
            return false;
        }
        pages[i].protectedNow=true;
    }
    return true;
}
static bool RestoreAll(CodePage pages[4]) {
    bool ok=true;
    for (int i=3;i>=0;--i) {
        if (!pages[i].protectedNow) continue;
        DWORD unused=0;
        if (!VirtualProtect((LPVOID)(uintptr_t)pages[i].at,pages[i].length,
                            pages[i].oldProtection,&unused))
            ok=false;
        pages[i].protectedNow=false;
    }
    return ok;
}

static bool Install(DWORD cap) {
    if (cap!=kCap1024 && cap!=kCap4096) {
        Log("REJECT","invalid dynamic buffer pool capacity");
        return false;
    }
    DWORD original[2]={0,0};
    if (!ExactImage() || !CleanStart(original)) {
        Log("REJECT","unknown executable or dynamic buffer list already active");
        return false;
    }

    BYTE* thunks=(BYTE*)VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!thunks) {
        Log("REJECT","cannot allocate trampoline page");
        return false;
    }
    g_pools[0].stub=thunks;
    g_pools[1].stub=thunks+128;
    BuildTrampoline(g_pools[0].stub,g_pools[0],cap);
    BuildTrampoline(g_pools[1].stub,g_pools[1],cap);
    DWORD oldStubProtection=0;
    if (!VirtualProtect(thunks,4096,PAGE_EXECUTE_READ,&oldStubProtection) ||
        !FlushInstructionCache(GetCurrentProcess(),thunks,4096)) {
        VirtualFree(thunks,0,MEM_RELEASE);
        Log("REJECT","could not publish executable x86 trampolines");
        return false;
    }

    // The CRT boot-size immediates share a page; so do the VB/IB registration
    // sites. Keep four distinct protection records and restore in REVERSE
    // order so the second VirtualProtect never leaves an original RX page RWX.
    CodePage page[4]={
        {g_pools[0].boot+3,4,0,false},
        {g_pools[1].boot+3,4,0,false},
        {g_pools[0].registerAt,g_pools[0].registerLen,0,false},
        {g_pools[1].registerAt,g_pools[1].registerLen,0,false}
    };
    if (!ProtectAll(page)) {
        VirtualFree(thunks,0,MEM_RELEASE);
        Log("REJECT","cannot protect all four code sites; no bytes changed");
        return false;
    }

    DWORD recheck[2]={0,0};
    bool good=ExactImage() && CleanStart(recheck) &&
              recheck[0]==original[0] && recheck[1]==original[1];
    // The CRT normally already ran by the first D3D9 entry point. Resize both
    // native zero-count lists BEFORE any append trampoline becomes reachable.
    // If one realloc succeeds but the other fails, the enlarged allocation
    // still has its original native destructor and is safe to leave in place.
    if (good) good=GrowEmptyList(g_pools[0],original[0],cap*4u);
    if (good) good=GrowEmptyList(g_pools[1],original[1],cap*4u);

    bool wroteBoot[2]={false,false},wroteRegister[2]={false,false};
    if (good) {
        BYTE bytes[4]={0};
        PutU32(bytes,cap*4u);
        for (unsigned i=0;i<2 && good;++i) {
            wroteBoot[i]=true;
            good=Store(g_pools[i].boot+3,bytes,sizeof(bytes));
        }
        for (unsigned i=0;i<2 && good;++i) {
            BYTE jump[sizeof(kVBRegisterPrefix)]={0xE9};
            PutRel32(jump+1,g_pools[i].registerAt+5,
                     (DWORD)(uintptr_t)g_pools[i].stub);
            for (unsigned j=5;j<sizeof(jump);++j) jump[j]=0x90;
            wroteRegister[i]=true;
            good=Store(g_pools[i].registerAt,jump,sizeof(jump));
        }
    }
    bool rollback=true;
    if (!good) {
        for (int i=1;i>=0;--i)
            if (wroteRegister[i])
                rollback=Store(g_pools[i].registerAt,g_pools[i].registerSig,
                               g_pools[i].registerLen) && rollback;
        for (int i=1;i>=0;--i)
            if (wroteBoot[i])
                rollback=Store(g_pools[i].boot+3,g_pools[i].bootSig+3,4) && rollback;
        Log(rollback?"REJECT":"FATAL",
            rollback?"instruction patch failed; code bytes restored":
                     "original code restoration failed; terminate game");
    }

    for (unsigned i=0;i<2;++i) {
        FlushInstructionCache(GetCurrentProcess(),
            (LPCVOID)(uintptr_t)(g_pools[i].boot+3),4);
        FlushInstructionCache(GetCurrentProcess(),
            (LPCVOID)(uintptr_t)g_pools[i].registerAt,g_pools[i].registerLen);
    }
    const bool permissionsOK=RestoreAll(page);
    if (!permissionsOK) Log("FATAL","could not restore original RX code protection");
    if (!good || !permissionsOK) {
        // Free only if we have proved all original JMP sites are restored;
        // otherwise a callable hook may still target this page.
        if (EqualBytes(g_pools[0].registerAt,g_pools[0].registerSig,
                       g_pools[0].registerLen) &&
            EqualBytes(g_pools[1].registerAt,g_pools[1].registerSig,
                       g_pools[1].registerLen))
            VirtualFree(thunks,0,MEM_RELEASE);
        else {
            g_stubPage=thunks;
            Log("FATAL","hook still published; keep executable page mapped");
        }
        return false;
    }
    g_stubPage=thunks;
    char detail[192]={0};
    _snprintf_s(detail,sizeof(detail),_TRUNCATE,
        "native VB and IB recreate lists: 64 -> %lu each (%lu bytes per list); native owner preserved",
        (unsigned long)cap,(unsigned long)(cap*4u));
    Log("ACTIVE",detail);
    return true;
}
} // namespace

void OC_DynamicBufferResetPools4096_Init() {
    if (InterlockedCompareExchange(&g_attempted,1,0)!=0) return;
    char path[MAX_PATH]={0};
    const DWORD n=GetModuleFileNameA(NULL,path,MAX_PATH);
    if (!n || n>=MAX_PATH) return;
    char* slash=strrchr(path,'\\');
    if (!slash) return;
    slash[1]=0;
    char ini[MAX_PATH]={0};
    if (_snprintf_s(ini,sizeof(ini),_TRUNCATE,"%sproxy.ini",path)<0) return;
    const bool high=GetPrivateProfileIntA("StaticMapPools",
                       "ExpandDynamicBufferReset4096",0,ini)!=0;
    const bool low=GetPrivateProfileIntA("StaticMapPools",
                      "ExpandDynamicBufferReset1024",0,ini)!=0;
    if (!high && !low) return;
    if (_snprintf_s(g_log,sizeof(g_log),_TRUNCATE,
                    "%sdynamic_buffer_reset_pools_4096.log",path)<0)
        return;
    Log("BEGIN",high?"native dynamic VB+IB 4096 per-list":"native dynamic VB+IB 1024 per-list");
    Install(high?kCap4096:kCap1024);
}
