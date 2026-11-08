// D3D9Proxy - NATIVE GFX BLOCK REGISTRY, A REAL ALLOCATION + FACTORY GUARD PATCH.
//
// Verified ORIGINAL ConquestLLC.exe image: SHA-256
// 3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858.
// The same bytes are the previously recovered ConquestEA.exe GZF source.
//
// Source: MgLevelObject.cpp (00876e69 GFX reserve) and MgLevelConstruct.cpp
// (FUN_00875ab8, the sorted GFX registry factory). This is NOT a map loader.
// Model/texture/effect capacity patches are independent.
//
// The 64-pair allocator at 0040b4cb is ALSO CALLED by 008f9301 for animation
// binding maps. Patching its global 0x40 would mangle those unrelated maps.
// Instead ONLY the level ctor's CALL at 00876e6f is rerouted to a tiny stub
// invoking the *existing* retail reserve8 at 00ea3a30 with ESI=level+0x11c
// and EDI=256/512. Original allocator, original free and ownership, NO new
// replacement container and no second-world magic.
//
// GFX factory 00875ab8 has:
//   00875ac8 83 F9 40     CMP ECX,64
//   00875acb 89 5D F8     MOV [EBP-8],EBX
//   00875ace 0F 8F ...    JG original early-out
// A site-local JMP at 00875ac8 uses an allocated RX trampoline to emit
// CMP ECX,256/512, replay MOV [EBP-8],EBX and JMP 00875ace. The ORIGINAL
// JG and registration/unload/bsearch/qsort code continue untouched.
// Both hooks are installed only for this exact 32-bit image, before the level
// is created, with best-effort rollback. All options default OFF.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "gfx_registry_512.h"

#ifndef _M_IX86
#error Retail GFX registry patch requires x86
#endif

namespace {
static const DWORD kImageBase=0x00400000;
static const DWORD kCtorContext=0x00876E69;
static const DWORD kCtorCall=0x00876E6F;
static const DWORD kFactoryEntry=0x00875AB8;
static const DWORD kGuardSite=0x00875AC8;
static const DWORD kGuardResume=0x00875ACE;
static const DWORD kReserve8=0x00EA3A30;
static const DWORD kSharedReserve40=0x0040B4CB;
static const DWORD kAnimationSharedCaller=0x008F9301;
static const DWORD kLevelCell=0x00CD7EA0;
static const DWORD kResourceManagerCell=0x00CD80D8;
static const DWORD kCap256=0x100;
static const DWORD kCap512=0x200;
static const DWORD kCap1024=0x400; // isolated GFX vector and guard

static const BYTE kCtorExpected[]={
    0x8D,0xB3,0x1C,0x01,0x00,0x00, // LEA ESI,[EBX+0x11C]
    0xE8,0x57,0x46,0xB9,0xFF       // CALL 0040B4CB
};
static const BYTE kCtorNextExpected[]={0x83,0xA3,0x44,0x0A,0x00,0x00,0x00};
static const BYTE kFactoryExpected[]={
    0x55,0x8B,0xEC,0x51,0x51,0x53,
    0x0F,0xBF,0x9E,0x1C,0x01,0x00,0x00, // MOVSX EBX,[ESI+0x11C]
    0x8D,0x0C,0x03,                     // LEA ECX,[EBX+EAX]
    0x83,0xF9,0x40,                     // CMP ECX,64
    0x89,0x5D,0xF8,                     // MOV [EBP-8],EBX
    0x0F,0x8F,0xAB,0x00,0x00,0x00        // JG +0xAB
};
static const BYTE kGuardExpected[]={0x83,0xF9,0x40,0x89,0x5D,0xF8};
static const BYTE kSharedExpected[]={
    0x51,0x53,0x57,0x6A,0x40,0x5B,0x66,0x39,0x5E,0x02
};
static const BYTE kAnimationCallerExpected[]={0xE8,0xC5,0x21,0xB1,0xFF};
static const BYTE kReserve8Expected[]={
    0x66,0x39,0x7E,0x02,0x53,0x0F,0x84,0x67,0x00,0x00,0x00
};
static volatile LONG g_attempted=0;
static char g_log[MAX_PATH]={0};
// Remains allocated for the lifetime of the process after successful patch.
static BYTE* g_executableStubs=NULL;

static void Log(const char* phase,const char* detail) {
    if (!g_log[0]) return;
    FILE* f=NULL;
    if (fopen_s(&f,g_log,"ab")!=0 || !f) return;
    fprintf(f,"[%lu] %s: %s\n",(unsigned long)GetTickCount(),phase,detail);
    fclose(f);
}
static bool CanRead(DWORD ptr,size_t len) {
    if (!ptr || !len || ptr>0xFFFFFFFFu-(DWORD)len) return false;
    MEMORY_BASIC_INFORMATION m={0};
    if (!VirtualQuery((LPCVOID)(uintptr_t)ptr,&m,sizeof(m))) return false;
    if (m.State!=MEM_COMMIT || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))
        return false;
    if (!(m.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|
                    PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|
                    PAGE_EXECUTE_WRITECOPY))) return false;
    return (uintptr_t)ptr+len <= (uintptr_t)m.BaseAddress+m.RegionSize;
}
static bool EqualCode(DWORD at,const BYTE* expected,size_t len) {
    if (!CanRead(at,len)) return false;
    __try {
        return memcmp((const void*)(uintptr_t)at,expected,len)==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static bool BeforeLevelInit() {
    if (!CanRead(kLevelCell,4) || !CanRead(kResourceManagerCell,4))
        return false;
    __try {
        return *(volatile const DWORD*)(uintptr_t)kLevelCell==0 &&
               *(volatile const DWORD*)(uintptr_t)kResourceManagerCell==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static bool ExactRetail() {
    return (DWORD)(uintptr_t)GetModuleHandleA(NULL)==kImageBase &&
           EqualCode(kCtorContext,kCtorExpected,sizeof(kCtorExpected)) &&
           EqualCode(kCtorCall+5,kCtorNextExpected,sizeof(kCtorNextExpected)) &&
           EqualCode(kFactoryEntry,kFactoryExpected,sizeof(kFactoryExpected)) &&
           EqualCode(kGuardSite,kGuardExpected,sizeof(kGuardExpected)) &&
           EqualCode(kSharedReserve40,kSharedExpected,sizeof(kSharedExpected)) &&
           EqualCode(kAnimationSharedCaller,kAnimationCallerExpected,
                     sizeof(kAnimationCallerExpected)) &&
           EqualCode(kReserve8,kReserve8Expected,sizeof(kReserve8Expected));
}
static void PutU32(BYTE* dst,DWORD val) { memcpy(dst,&val,sizeof(val)); }
static void PutRel32(BYTE* where,DWORD nextInstruction,DWORD target) {
    // x86 32-bit EIP arithmetic wraps modulo 2^32, so any x86 VA can be
    // reached by E8/E9 rel32. Never interpret this as an x64 near jump.
    const DWORD displacement=target-nextInstruction;
    PutU32(where,displacement);
}
static bool Store(DWORD at,const BYTE* bytes,size_t size) {
    __try {
        memcpy((void*)(uintptr_t)at,bytes,size);
        return memcmp((const void*)(uintptr_t)at,bytes,size)==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool BuildStubs(BYTE* page,DWORD cap) {
    // pushfd; pushad; lea esi,[ebx+11Ch]; mov edi,cap;
    // mov eax,00EA3A30h; call eax; popad; popfd; ret
    // The level constructor has EBX=level (confirmed by its real LEA).
    // Preserve all caller registers and flags while using the engine's
    // existing allocation/free/copy routine on the correct level vector.
    BYTE ctor[]={
        0x9C,0x60,
        0x8D,0xB3,0x1C,0x01,0x00,0x00,
        0xBF,0,0,0,0,
        0xB8,0x30,0x3A,0xEA,0x00,
        0xFF,0xD0,0x61,0x9D,0xC3
    };
    PutU32(ctor+9,cap);
    // Original factory instructions at 00875ac8, widened from CMP imm8
    // to CMP imm32 in an executable thunk. The original JG is untouched.
    BYTE guard[]={
        0x81,0xF9,0,0,0,0,       // CMP ECX,cap
        0x89,0x5D,0xF8,         // MOV [EBP-8],EBX
        0xE9,0,0,0,0            // JMP 00875ACE
    };
    PutU32(guard+2,cap);
    PutRel32(guard+10,(DWORD)(uintptr_t)(page+64+sizeof(guard)),kGuardResume);
    memcpy(page,ctor,sizeof(ctor));
    memcpy(page+64,guard,sizeof(guard));
    DWORD old=0;
    if (!VirtualProtect(page,4096,PAGE_EXECUTE_READ,&old)) return false;
    return !!FlushInstructionCache(GetCurrentProcess(),page,4096);
}
static bool Install(DWORD cap) {
    if (cap!=kCap256 && cap!=kCap512 && cap!=kCap1024) {
        Log("REJECT","unsupported GFX capacity");
        return false;
    }
    if (!ExactRetail() || !BeforeLevelInit()) {
        Log("REJECT","unknown x86 instruction signatures or level already constructed");
        return false;
    }
    BYTE* stub=(BYTE*)VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if (!stub) {
        Log("REJECT","cannot allocate x86 trampolines");
        return false;
    }
    if (!BuildStubs(stub,cap)) {
        VirtualFree(stub,0,MEM_RELEASE);
        Log("REJECT","cannot make trampolines executable");
        return false;
    }
    BYTE ctorCall[5]={0xE8,0,0,0,0};
    PutRel32(ctorCall+1,kCtorCall+5,(DWORD)(uintptr_t)stub);
    BYTE guardJump[6]={0xE9,0,0,0,0,0x90};
    PutRel32(guardJump+1,kGuardSite+5,(DWORD)(uintptr_t)(stub+64));

    DWORD protectCtor=0,protectGuard=0,dummy=0;
    if (!VirtualProtect((LPVOID)(uintptr_t)kCtorCall,sizeof(ctorCall),
                        PAGE_EXECUTE_READWRITE,&protectCtor)) {
        VirtualFree(stub,0,MEM_RELEASE);
        Log("REJECT","cannot protect GFX constructor CALL");
        return false;
    }
    if (!VirtualProtect((LPVOID)(uintptr_t)kGuardSite,sizeof(guardJump),
                        PAGE_EXECUTE_READWRITE,&protectGuard)) {
        VirtualProtect((LPVOID)(uintptr_t)kCtorCall,sizeof(ctorCall),protectCtor,&dummy);
        VirtualFree(stub,0,MEM_RELEASE);
        Log("REJECT","cannot protect GFX factory guard");
        return false;
    }
    // Take both page protections before touching either site. This is a
    // pre-level patch, not cross-thread atomic executable editing.
    bool good=BeforeLevelInit() && ExactRetail();
    bool wroteGuard=false,wroteCtor=false;
    if (good) {
        wroteGuard=true;
        good=Store(kGuardSite,guardJump,sizeof(guardJump));
        if (good) {
            wroteCtor=true;
            good=Store(kCtorCall,ctorCall,sizeof(ctorCall));
        }
    }
    if (!good) {
        bool restored=true;
        if (wroteCtor)
            restored=Store(kCtorCall,kCtorExpected+6,sizeof(ctorCall))&&restored;
        if (wroteGuard)
            restored=Store(kGuardSite,kGuardExpected,sizeof(guardJump))&&restored;
        if (!restored)
            Log("FATAL","GFX byte rollback failed. Terminate game process.");
        else
            Log("REJECT","GFX patch validation/write failed; original bytes restored");
    }
    FlushInstructionCache(GetCurrentProcess(),(LPCVOID)(uintptr_t)kCtorCall,sizeof(ctorCall));
    FlushInstructionCache(GetCurrentProcess(),(LPCVOID)(uintptr_t)kGuardSite,sizeof(guardJump));
    DWORD unused1=0,unused2=0;
    const bool p1=!!VirtualProtect((LPVOID)(uintptr_t)kCtorCall,
                                   sizeof(ctorCall),protectCtor,&unused1);
    const bool p2=!!VirtualProtect((LPVOID)(uintptr_t)kGuardSite,
                                   sizeof(guardJump),protectGuard,&unused2);
    if (!p1 || !p2) {
        Log("FATAL","failed to restore original code page permissions");
        // On uncertain protection state it is unsafe to free code memory
        // that could already be the target of a live instruction.
        g_executableStubs=stub;
        return false;
    }
    if (!good) {
        if (EqualCode(kGuardSite,kGuardExpected,sizeof(kGuardExpected)) &&
            EqualCode(kCtorCall,kCtorExpected+6,sizeof(ctorCall)))
            VirtualFree(stub,0,MEM_RELEASE);
        else {
            g_executableStubs=stub; // avoid freeing possibly referenced code
            Log("FATAL","patch sites not restored; game must be restarted");
        }
        return false;
    }
    g_executableStubs=stub;
    char msg[160]={0};
    _snprintf_s(msg,sizeof(msg),_TRUNCATE,
                "GFX level registry reserve + sorted factory guard: 64 -> %lu; animation 64 untouched",
                (unsigned long)cap);
    Log("ACTIVE",msg);
    return true;
}
} // anonymous namespace

void OC_GfxRegistry512_Init() {
    if (InterlockedCompareExchange(&g_attempted,1,0)!=0) return;
    char exe[MAX_PATH]={0};
    DWORD n=GetModuleFileNameA(NULL,exe,MAX_PATH);
    if (!n || n>=MAX_PATH) return;
    char* slash=strrchr(exe,'\\');
    if (!slash) return;
    slash[1]=0;
    char ini[MAX_PATH]={0};
    if (_snprintf_s(ini,sizeof(ini),_TRUNCATE,"%sproxy.ini",exe)<0) return;
    const bool ultra=GetPrivateProfileIntA("StaticMapPools","ExpandGfx1024",0,ini)!=0;
    const bool high=GetPrivateProfileIntA("StaticMapPools","ExpandGfx512",0,ini)!=0;
    const bool low=GetPrivateProfileIntA("StaticMapPools","ExpandGfx256",0,ini)!=0;
    if (!ultra && !high && !low) return; // all OFF by default
    if (_snprintf_s(g_log,sizeof(g_log),_TRUNCATE,"%sgfx_registry_512.log",exe)<0)
        return;
    Log("BEGIN",ultra?"GFX native registry 1024":high?"GFX native registry 512":"GFX native registry 256");
    Install(ultra?kCap1024:(high?kCap512:kCap256));
}
