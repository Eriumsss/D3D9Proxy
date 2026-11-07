// OpenConquest - native static FX registry, not mission FX or NPC expansion.
// Supports MORE native EffectInfo definitions in ONE merged Helm's Deep PAK.
// The engine's FX templates still require valid GameObjs, shaders, textures,
// emitters, and independently sized pools. This patch changes ONLY the level
// effect registry vectors and their early factory capacity guard.
//
// Original ConquestEA.exe extracted from user's Ghidra GZF:
// SHA256 3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858.
// Not a fingerprint for any other build! Also includes injected .1911 code.
//
// x86 evidence:
//   00876E43 / 4E / 59 CALL 0040B38A -> JMP 00461D2C (reserve 8-byte pairs)
//   00461D2F BF 00 02 00 00   MOV EDI,512 (all three vectors)
//   008773AA 81 FA 00 02 00 00 CMP EDX,512 (EffectInfo factory)
// Change only immediate operands at 00461D30 and 008773AC to 1024.
//
// Configuration (proxy.ini by EXE): [StaticMapPools] ExpandEffects1024=0
// There is NO game runtime test and no persistent PAK/BIN mutation.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "effect_registry_1024.h"
#ifndef _M_IX86
#error Retail effect registry patch is 32-bit only
#endif

namespace {
const DWORD kImageBase=0x00400000;
const DWORD kReserveEntry=0x00461D2C;
const DWORD kReserveImmediate=0x00461D30;
const DWORD kFactoryEntry=0x008773A0;
const DWORD kFactoryImmediate=0x008773AC;
const DWORD kLevelGlobal=0x00CD7EA0;
const DWORD kResourceManagerGlobal=0x00CD80D8;
const DWORD kOldCap=0x200;
const DWORD kNewCap=0x400;
const DWORD kHigherCap=0x800; // 2048 effect records

const BYTE kReserveBefore[] = {
    0x51,0x53,0x57, 0xBF,0x00,0x02,0x00,0x00,
    0x66,0x39,0x7E,0x02,0x0F,0x84,0x59,0x00,0x00,0x00
};
const BYTE kFactoryBefore[] = {
    0x0F,0xBF,0x8B,0xFC,0x00,0x00,0x00,
    0x8D,0x14,0x01,
    0x81,0xFA,0x00,0x02,0x00,0x00,
    0x56,0x57,0x89,0x4D,0xF8,
    0x0F,0x8F,0xE4,0x01,0x00,0x00
};
const BYTE kReserveThunkBefore[] = {0xE9,0x9D,0x69,0x05,0x00};
const BYTE kPairAppendBefore[] = {
    0x0F,0xB7,0x01,0x66,0x3B,0x41,0x02,0x7C,0x06
};
struct Callsite { DWORD at; BYTE bytes[5]; };
const Callsite kCalls[] = {
    {0x00876E43,{0xE8,0x42,0x45,0xB9,0xFF}},
    {0x00876E4E,{0xE8,0x37,0x45,0xB9,0xFF}},
    {0x00876E59,{0xE8,0x2C,0x45,0xB9,0xFF}}
};

static volatile LONG g_once=0;
static char g_log[MAX_PATH];
static void Report(const char* state,const char* why) {
    if (!g_log[0]) return;
    FILE* f=NULL;
    if (fopen_s(&f,g_log,"ab")!=0 || !f) return;
    fprintf(f,"[%lu] %s: %s\n",(unsigned long)GetTickCount(),state,why);
    fclose(f);
}
static bool Readable(DWORD at,size_t n) {
    if (!n || n>0xFFFFFFFFu || at>0xFFFFFFFFu-(DWORD)n) return false;
    MEMORY_BASIC_INFORMATION info={0};
    if (!VirtualQuery((LPCVOID)(uintptr_t)at,&info,sizeof(info))) return false;
    if (info.State!=MEM_COMMIT || (info.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return false;
    if (!(info.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|
                       PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|
                       PAGE_EXECUTE_WRITECOPY))) return false;
    return (uintptr_t)at+n<=(uintptr_t)info.BaseAddress+info.RegionSize;
}
static bool Same(DWORD at,const BYTE* expected,size_t n) {
    if (!Readable(at,n)) return false;
    __try { return memcmp((const void*)(uintptr_t)at,expected,n)==0; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool BeforeLevelInit() {
    if (!Readable(kLevelGlobal,4)||!Readable(kResourceManagerGlobal,4))
        return false;
    __try {
        return !*(volatile DWORD*)(uintptr_t)kLevelGlobal &&
               !*(volatile DWORD*)(uintptr_t)kResourceManagerGlobal;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool ExactRetailInstructions() {
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL)!=kImageBase ||
        !Same(kReserveEntry,kReserveBefore,sizeof(kReserveBefore)) ||
        !Same(kFactoryEntry,kFactoryBefore,sizeof(kFactoryBefore)) ||
        !Same(0x0040B38A,kReserveThunkBefore,sizeof(kReserveThunkBefore)) ||
        !Same(0x0040B3F3,kPairAppendBefore,sizeof(kPairAppendBefore)))
        return false;
    for(unsigned i=0;i<sizeof(kCalls)/sizeof(kCalls[0]);++i)
        if (!Same(kCalls[i].at,kCalls[i].bytes,sizeof(kCalls[i].bytes)))
            return false;
    return true;
}
static bool StoreImmediate(DWORD at,DWORD value) {
    __try {
        memcpy((void*)(uintptr_t)at,&value,sizeof(value));
        return *(volatile DWORD*)(uintptr_t)at==value;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool Install(DWORD requestedCap) {
    if (requestedCap!=kNewCap && requestedCap!=kHigherCap) {
        Report("REJECT","unsupported EffectInfo cap");
        return false;
    }
    if (!ExactRetailInstructions()) {
        Report("REJECT","unknown executable or effect constructor/factory/helper instructions");
        return false;
    }
    if (!BeforeLevelInit()) {
        Report("REJECT","level manager already exists; cannot retroactively resize its arrays");
        return false;
    }
    DWORD oldReserve=0,oldFactory=0,tmp=0;
    if (!VirtualProtect((LPVOID)(uintptr_t)kReserveImmediate,4,
                        PAGE_EXECUTE_READWRITE,&oldReserve)) {
        Report("REJECT","cannot protect FX reserve immediate");
        return false;
    }
    if (!VirtualProtect((LPVOID)(uintptr_t)kFactoryImmediate,4,
                        PAGE_EXECUTE_READWRITE,&oldFactory)) {
        VirtualProtect((LPVOID)(uintptr_t)kReserveImmediate,4,oldReserve,&tmp);
        Report("REJECT","cannot protect FX factory immediate, no writes attempted");
        return false;
    }
    // NOT a globally atomic engine patch: apply before any other engine
    // thread may run level construction. Never enable in a live level.
    bool good=BeforeLevelInit()&&ExactRetailInstructions();
    bool reserveAttempted=false,factoryAttempted=false;
    if (good) {
        reserveAttempted=true;
        good=StoreImmediate(kReserveImmediate,requestedCap);
        if (good) {
            factoryAttempted=true;
            good=StoreImmediate(kFactoryImmediate,requestedCap);
        }
    }
    if (!good) {
        bool restored=true;
        if (factoryAttempted)
            restored=StoreImmediate(kFactoryImmediate,kOldCap)&&restored;
        if (reserveAttempted)
            restored=StoreImmediate(kReserveImmediate,kOldCap)&&restored;
        Report(restored?"REJECT":"FATAL",
            restored?"FX patch failed; attempted immediate writes rolled back":
                     "FX patch rollback failed; terminate this game process");
    }
    FlushInstructionCache(GetCurrentProcess(),(LPCVOID)(uintptr_t)kReserveImmediate,4);
    FlushInstructionCache(GetCurrentProcess(),(LPCVOID)(uintptr_t)kFactoryImmediate,4);
    DWORD dummy1=0,dummy2=0;
    bool restoredProt1=!!VirtualProtect((LPVOID)(uintptr_t)kReserveImmediate,4,
                                       oldReserve,&dummy1);
    bool restoredProt2=!!VirtualProtect((LPVOID)(uintptr_t)kFactoryImmediate,4,
                                       oldFactory,&dummy2);
    if (!restoredProt1 || !restoredProt2) {
        Report("FATAL","failed to restore EXE code page permissions");
        return false;
    }
    if (good) {
        char detail[128]={0};
        _snprintf_s(detail,sizeof(detail),_TRUNCATE,
                    "FX registry A/B/C reserve + factory guard: 512 -> %lu",
                    (unsigned long)requestedCap);
        Report("ACTIVE",detail);
    }
    return good;
}
} // namespace

void OC_EffectRegistry1024_Init() {
    if (InterlockedCompareExchange(&g_once,1,0)!=0) return;
    char dir[MAX_PATH]={0};
    DWORD n=GetModuleFileNameA(NULL,dir,MAX_PATH);
    if (!n || n>=MAX_PATH) return;
    char* last=strrchr(dir,'\\');
    if (!last) return;
    last[1]=0;
    char ini[MAX_PATH]={0};
    if (_snprintf_s(ini,sizeof(ini),_TRUNCATE,"%sproxy.ini",dir)<0) return;
    const bool high=GetPrivateProfileIntA("StaticMapPools","ExpandEffects2048",0,ini)!=0;
    const bool low=GetPrivateProfileIntA("StaticMapPools","ExpandEffects1024",0,ini)!=0;
    if (!high && !low) return;
    if (_snprintf_s(g_log,sizeof(g_log),_TRUNCATE,
                    "%seffect_registry_1024.log",dir)<0) return;
    Report("BEGIN",high?"opt-in 2048-entry EffectInfo patch":"opt-in 1024-entry EffectInfo patch");
    Install(high?kHigherCap:kNewCap);
}
