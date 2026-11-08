// D3D9Proxy: native ResourceOther + PField registry capacity extensions.
//
// Verified ORIGINAL ConquestLLC.exe image SHA256:
// 3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858
// Confirmed original x86 code bytes, instruction boundaries, helper CALLs,
// and early factory guards, not guesses from the Magellan C++ stand-in.
//
// ResourceOther: level+0xF4, 8-byte {key,pointer} records; 2048->4096/8192.
//   ctor 00876e38 CALL 0040b321 JMP 004347ab (MOV EDI,0x800 at 004347ae).
//   guard CMP EAX,0x800 at 00877a1b (imm32 at 00877a1c).
// PField: level+0x114, 12-byte {key0,key1,pointer} records; 4096->8192.
//   ctor 00876e64 CALL 0040b430 JMP 00464ac7 (MOV EDI,0x1000 at 00464aca).
//   guard CMP EDX,0x1000 at 00875c05 (imm32 at 00875c07).
//
// IMPORTANT: The old MgLevelObject.cpp port's temporary 0x200 PField
// allocation estimate was WRONG. The original x86 actually allocates 0x1000.
// Both reserve routines have only the one verified native CALL site each;
// their original allocation/copy/free semantics remain unchanged.
// No pool aliases, new world, PAK writes or runtime container replacement.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "auxiliary_registry_8192.h"
#ifndef _M_IX86
#error These native reserve and factory patches require x86
#endif

namespace {
static const DWORD kExeBase=0x00400000;
static const DWORD kLevelCell=0x00CD7EA0;
static const DWORD kResourceMgrCell=0x00CD80D8;
static volatile LONG g_once=0;
static char g_log[MAX_PATH]={0};

static const BYTE kOtherConstructor[]={
    0x8D,0xB3,0xF4,0x00,0x00,0x00,
    0xE8,0xE4,0x44,0xB9,0xFF
};
static const BYTE kOtherThunk[]={0xE9,0x85,0x94,0x02,0x00};
static const BYTE kOtherReserve[]={
    0x51,0x53,0x57,0xBF,0x00,0x08,0x00,0x00,
    0x66,0x39,0x7E,0x02,0x0F,0x84,0x59,0x00,0x00,0x00
};
static const BYTE kOtherGuard[]={
    0x3D,0x00,0x08,0x00,0x00,
    0x0F,0x8F,0x18,0x05,0x00,0x00
};
static const BYTE kPfConstructor[]={
    0x8D,0xB3,0x14,0x01,0x00,0x00,
    0xE8,0xC7,0x45,0xB9,0xFF
};
static const BYTE kPfThunk[]={0xE9,0x92,0x96,0x05,0x00};
static const BYTE kPfReserve[]={
    0x51,0x53,0x57,0xBF,0x00,0x10,0x00,0x00,
    0x66,0x39,0x7E,0x02,0x0F,0x84,0x59,0x00,0x00,0x00
};
static const BYTE kPfGuard[]={
    0x0F,0xBF,0x8E,0x14,0x01,0x00,0x00,
    0x8D,0x14,0x01,
    0x81,0xFA,0x00,0x10,0x00,0x00,
    0x89,0x4D,0xFC,
    0x0F,0x8F,0xC6,0x00,0x00,0x00
};

struct PatchSpec {
    const char* name;
    DWORD constructorAt;
    const BYTE* constructor;
    unsigned constructorLen;
    DWORD thunkAt;
    const BYTE* thunk;
    unsigned thunkLen;
    DWORD reserveAt;
    const BYTE* reserve;
    unsigned reserveLen;
    DWORD guardAt;
    const BYTE* guard;
    unsigned guardLen;
    DWORD reserveImm;
    DWORD guardImm;
    DWORD oldCapacity;
    DWORD lowCapacity;
    DWORD highCapacity;
    DWORD ultraCapacity;
};

static const PatchSpec kResourceOther={
    "resource_other",
    0x00876E32,kOtherConstructor,sizeof(kOtherConstructor),
    0x0040B321,kOtherThunk,sizeof(kOtherThunk),
    0x004347AB,kOtherReserve,sizeof(kOtherReserve),
    0x00877A1B,kOtherGuard,sizeof(kOtherGuard),
    0x004347AF,0x00877A1C,0x800,0x1000,0x2000,0x4000
};
static const PatchSpec kPField={
    "pfield",
    0x00876E5E,kPfConstructor,sizeof(kPfConstructor),
    0x0040B430,kPfThunk,sizeof(kPfThunk),
    0x00464AC7,kPfReserve,sizeof(kPfReserve),
    0x00875BFB,kPfGuard,sizeof(kPfGuard),
    0x00464ACB,0x00875C07,0x1000,0x2000,0x2000,0x4000
};

static void Report(const char* state,const char* name,const char* detail) {
    if (!g_log[0]) return;
    FILE* f=NULL;
    if (fopen_s(&f,g_log,"ab")!=0 || !f) return;
    fprintf(f,"[%lu] %s %s: %s\n",(unsigned long)GetTickCount(),name,state,detail);
    fclose(f);
}
static bool CanRead(DWORD at,size_t size) {
    if (!size || at>0xFFFFFFFFu-(DWORD)size) return false;
    MEMORY_BASIC_INFORMATION m={0};
    if (!VirtualQuery((LPCVOID)(uintptr_t)at,&m,sizeof(m))) return false;
    if (m.State!=MEM_COMMIT || (m.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return false;
    if (!(m.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|
                    PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|
                    PAGE_EXECUTE_WRITECOPY))) return false;
    return (uintptr_t)at+size <= (uintptr_t)m.BaseAddress+m.RegionSize;
}
static bool Match(DWORD at,const BYTE* expected,size_t size) {
    if (!CanRead(at,size)) return false;
    __try {
        return memcmp((const void*)(uintptr_t)at,expected,size)==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static bool NoLevelYet() {
    if (!CanRead(kLevelCell,4) || !CanRead(kResourceMgrCell,4)) return false;
    __try {
        return *(volatile const DWORD*)(uintptr_t)kLevelCell==0 &&
               *(volatile const DWORD*)(uintptr_t)kResourceMgrCell==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool Exact(const PatchSpec& spec) {
    return (DWORD)(uintptr_t)GetModuleHandleA(NULL)==kExeBase &&
           Match(spec.constructorAt,spec.constructor,spec.constructorLen) &&
           Match(spec.thunkAt,spec.thunk,spec.thunkLen) &&
           Match(spec.reserveAt,spec.reserve,spec.reserveLen) &&
           Match(spec.guardAt,spec.guard,spec.guardLen);
}
static bool WriteCap(DWORD ptr,DWORD value) {
    __try {
        memcpy((void*)(uintptr_t)ptr,&value,sizeof(value));
        return *(volatile const DWORD*)(uintptr_t)ptr==value;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool Install(const PatchSpec& spec,DWORD cap) {
    if (cap!=spec.lowCapacity && cap!=spec.highCapacity && cap!=spec.ultraCapacity) {
        Report("REJECT",spec.name,"unsupported registry capacity");
        return false;
    }
    if (cap<=spec.oldCapacity || cap>32767) {
        Report("REJECT",spec.name,"invalid signed 16-bit native SmallVec capacity");
        return false;
    }
    if (!NoLevelYet() || !Exact(spec)) {
        Report("REJECT",spec.name,"EXE signature mismatch or level already constructed");
        return false;
    }
    DWORD originalReserveProtection=0,originalGuardProtection=0,unused=0;
    if (!VirtualProtect((LPVOID)(uintptr_t)spec.reserveImm,4,
                        PAGE_EXECUTE_READWRITE,&originalReserveProtection)) {
        Report("REJECT",spec.name,"reserve operand page not writable");
        return false;
    }
    if (!VirtualProtect((LPVOID)(uintptr_t)spec.guardImm,4,
                        PAGE_EXECUTE_READWRITE,&originalGuardProtection)) {
        VirtualProtect((LPVOID)(uintptr_t)spec.reserveImm,4,originalReserveProtection,&unused);
        Report("REJECT",spec.name,"factory guard page not writable; nothing written");
        return false;
    }
    // No write unless both pages were protected AND the whole version/level
    // check still passes. This is a pre-level change, not cross-thread atomic.
    bool good=NoLevelYet() && Exact(spec);
    bool wroteReserve=false,wroteGuard=false;
    if (good) {
        wroteReserve=true;
        good=WriteCap(spec.reserveImm,cap);
        if (good) {
            wroteGuard=true;
            good=WriteCap(spec.guardImm,cap);
        }
    }
    if (!good) {
        bool restored=true;
        if (wroteGuard)
            restored=WriteCap(spec.guardImm,spec.oldCapacity)&&restored;
        if (wroteReserve)
            restored=WriteCap(spec.reserveImm,spec.oldCapacity)&&restored;
        Report(restored?"REJECT":"FATAL",spec.name,
               restored?"patch rejected; attempted writes rolled back":
                        "registry rollback failed; game process must terminate");
    }
    FlushInstructionCache(GetCurrentProcess(),(LPCVOID)(uintptr_t)spec.reserveImm,4);
    FlushInstructionCache(GetCurrentProcess(),(LPCVOID)(uintptr_t)spec.guardImm,4);
    DWORD u1=0,u2=0;
    const bool protectedReserve=!!VirtualProtect(
        (LPVOID)(uintptr_t)spec.reserveImm,4,originalReserveProtection,&u1);
    const bool protectedGuard=!!VirtualProtect(
        (LPVOID)(uintptr_t)spec.guardImm,4,originalGuardProtection,&u2);
    if (!protectedReserve || !protectedGuard) {
        Report("FATAL",spec.name,"failed to restore executable page protection");
        return false;
    }
    if (good) {
        char line[128]={0};
        _snprintf_s(line,sizeof(line),_TRUNCATE,
                    "native allocator reserve + factory guard %lu -> %lu",
                    (unsigned long)spec.oldCapacity,(unsigned long)cap);
        Report("ACTIVE",spec.name,line);
    }
    return good;
}
} // namespace

void OC_AuxiliaryRegistry8192_Init() {
    if (InterlockedCompareExchange(&g_once,1,0)!=0) return;
    char exe[MAX_PATH]={0};
    DWORD n=GetModuleFileNameA(NULL,exe,MAX_PATH);
    if (!n || n>=MAX_PATH) return;
    char* slash=strrchr(exe,'\\');
    if (!slash) return;
    slash[1]=0;
    char ini[MAX_PATH]={0};
    if (_snprintf_s(ini,sizeof(ini),_TRUNCATE,"%sproxy.ini",exe)<0) return;
    const bool otherUltra=GetPrivateProfileIntA("StaticMapPools","ExpandResourceOther16384",0,ini)!=0;
    const bool otherHigh=GetPrivateProfileIntA("StaticMapPools","ExpandResourceOther8192",0,ini)!=0;
    const bool otherLow=GetPrivateProfileIntA("StaticMapPools","ExpandResourceOther4096",0,ini)!=0;
    const bool pfieldsUltra=GetPrivateProfileIntA("StaticMapPools","ExpandPFields16384",0,ini)!=0;
    const bool pfieldsHigh=GetPrivateProfileIntA("StaticMapPools","ExpandPFields8192",0,ini)!=0;
    if (!otherUltra && !otherHigh && !otherLow && !pfieldsUltra && !pfieldsHigh) return; // OFF by default
    if (_snprintf_s(g_log,sizeof(g_log),_TRUNCATE,
                    "%sauxiliary_registry_8192.log",exe)<0) return;
    if (otherUltra || otherHigh || otherLow)
        Install(kResourceOther,otherUltra?kResourceOther.ultraCapacity:
                 (otherHigh?kResourceOther.highCapacity:kResourceOther.lowCapacity));
    if (pfieldsUltra || pfieldsHigh)
        Install(kPField,pfieldsUltra?kPField.ultraCapacity:kPField.highCapacity);
}
