// OpenConquest - RETAIL MODEL REGISTRY CAPACITY, NOT THE HEAP.
// The target is ONE Helm's Deep level containing imported static Isengard
// ModelInfo rows. Never create/load a second mission or NPC population.
//
// Source oracles:
//   Vespucci/Engine/Magellan/MgLevelObject.cpp, FUN_00876dd5
//   Vespucci/Engine/Magellan/MgModelRegistry.cpp, FUN_00875d1e
//   User's ConquestEA.exe.gzf, original PE image SHA256
//   3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858
//
// Verified EXE instructions (GZF original File Bytes, NOT guessed source):
//   00876e0b  BF 00 04 00 00      MOV EDI,0x400
//   00876e17  E8 DC F9 B8 FF      CALL 0x004067F8 (JMP to reserve8 at 0x00EA3A30)
//   00876e22  E8 D1 F9 B8 FF      CALL same function, model vec B
//   00876e2d  E8 C6 F9 B8 FF      CALL same function, model vec C
//   00875d34  81 F9 00 04 00 00   CMP ECX,0x400
//   00875d3a  0F 8F DC 00 00 00   JG 00875e1c
// The three reserve calls consume the SAME EDI value. Both immediates must
// change together to 0x800. Modifies no executable on disk.
//
// Risk: This is still an EXPERIMENTAL runtime patch, OFF by default. It
// does not enlarge texture/effect/GFX capacities or validate native PAK refs.
// NEVER apply after a level object has been installed.
//
// Configuration, game directory/proxy.ini:
//   [StaticMapPools]
//   ExpandModels2048=0
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "model_registry_2048.h"

#ifndef _M_IX86
#error Retail ModelInfo registry patch is x86-only
#endif

namespace {
static const DWORD kExeBase=0x00400000;
static const DWORD kModelCtor=0x00876E05;
static const DWORD kModelCtorImm=0x00876E0C;
static const DWORD kModelFactory=0x00875D2F;
static const DWORD kFactoryCmpImm=0x00875D36;
static const DWORD kLevelCell=0x00CD7EA0;
static const DWORD kResourceMgrCell=0x00CD80D8;
static const DWORD kNewCap=0x800;
static const DWORD kHigherCap=0x1000; // 4096 model records
static const DWORD kUltraCap=0x2000; // 8192 native model records
static const DWORD kOldCap=0x400;

static const BYTE kConstructorBefore[] = {
    0x8D,0xB3,0x8C,0x00,0x00,0x00,  // LEA ESI,[EBX+0x8C]
    0xBF,0x00,0x04,0x00,0x00,        // MOV EDI,1024
    0xC6,0x83,0x88,0x00,0x00,0x00,0x00,
    0xE8,0xDC,0xF9,0xB8,0xFF,
    0x8D,0xB3,0x94,0x00,0x00,0x00
};
static const BYTE kReserveThunkBefore[] = {
    0xE9,0x33,0xD2,0xA9,0x00      // JMP 0x00EA3A30; injected .1911 section
};
static const BYTE kReserve8Before[] = {
    0x66,0x39,0x7E,0x02,0x53,0x0F,0x84,0x67,0x00,0x00,0x00
};
static const BYTE kPairInsertBefore[] = {
    0x0F,0xB7,0x01,0x66,0x3B,0x41,0x02,0x7C,0x06
};
static const BYTE kFactoryBefore[] = {
    0x0F,0xBF,0x0B, 0x03,0xC8,       // MOVSX ECX,[EBX]; ADD ECX,EAX
    0x81,0xF9,0x00,0x04,0x00,0x00,  // CMP ECX,1024
    0x0F,0x8F,0xDC,0x00,0x00,0x00   // JG -> early-out
};
static const struct {
    DWORD at;
    BYTE expected[5];
} kThreeReserveCalls[] = {
    {0x00876E17,{0xE8,0xDC,0xF9,0xB8,0xFF}},
    {0x00876E22,{0xE8,0xD1,0xF9,0xB8,0xFF}},
    {0x00876E2D,{0xE8,0xC6,0xF9,0xB8,0xFF}}
};
static volatile LONG g_attempted;
static char g_logFile[MAX_PATH];

static void Report(const char* stage,const char* desc) {
    if (!g_logFile[0]) return;
    FILE* f=NULL;
    if (fopen_s(&f,g_logFile,"ab")!=0 || !f) return;
    fprintf(f,"[%lu] %s : %s\n",(unsigned long)GetTickCount(),stage,desc);
    fclose(f);
}
static bool Readable(DWORD ptr,size_t length) {
    if (!length || ptr > 0xFFFFFFFFu-(DWORD)length) return false;
    MEMORY_BASIC_INFORMATION m={0};
    if (!VirtualQuery((LPCVOID)(uintptr_t)ptr,&m,sizeof(m))) return false;
    if (m.State!=MEM_COMMIT || (m.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return false;
    if ((m.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|
                    PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|
                    PAGE_EXECUTE_WRITECOPY))==0) return false;
    return (uintptr_t)ptr + length <= (uintptr_t)m.BaseAddress+m.RegionSize;
}
static bool Matches(DWORD ptr,const BYTE* expected,size_t length) {
    if (!Readable(ptr,length)) return false;
    __try { return memcmp((const void*)(uintptr_t)ptr,expected,length)==0; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool LevelNotConstructed() {
    if (!Readable(kLevelCell,4) || !Readable(kResourceMgrCell,4)) return false;
    __try {
        return *(volatile const DWORD*)(uintptr_t)kLevelCell==0 &&
               *(volatile const DWORD*)(uintptr_t)kResourceMgrCell==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool ValidateRetail() {
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL)!=kExeBase) return false;
    if (!Matches(kModelCtor,kConstructorBefore,sizeof(kConstructorBefore)) ||
        !Matches(kModelFactory,kFactoryBefore,sizeof(kFactoryBefore)) ||
        !Matches(0x004067F8,kReserveThunkBefore,sizeof(kReserveThunkBefore)) ||
        !Matches(0x00EA3A30,kReserve8Before,sizeof(kReserve8Before)) ||
        !Matches(0x00404BF2,kPairInsertBefore,sizeof(kPairInsertBefore)))
        return false;
    for (unsigned i=0;i<sizeof(kThreeReserveCalls)/sizeof(kThreeReserveCalls[0]);++i)
        if (!Matches(kThreeReserveCalls[i].at,kThreeReserveCalls[i].expected,5))
            return false;
    return true;
}
static bool WriteNewCap(DWORD address,DWORD val) {
    __try {
        memcpy((void*)(uintptr_t)address,&val,sizeof(val));
        return *(volatile const DWORD*)(uintptr_t)address==val;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool Install(DWORD requestedCap) {
    // Hard whitelist: don't let INI values become arbitrary x86 opcode writes.
    if (requestedCap!=kNewCap && requestedCap!=kHigherCap && requestedCap!=kUltraCap) {
        Report("REJECT","unsupported ModelInfo cap");
        return false;
    }
    // Validation BEFORE making executable pages writable; this includes
    // the linked MOV EDI operand, all three CALL sites and factory's JG.
    if (!ValidateRetail()) {
        Report("REJECT","retail instruction signature mismatch; nothing changed");
        return false;
    }
    if (!LevelNotConstructed()) {
        Report("REJECT","level/resource manager already present (too late to expand)");
        return false;
    }
    DWORD oldCtor=0,oldFactory=0;
    // Acquire BOTH page protections before writing EITHER immediate. Partial
    // preparation is undone without creating a half-expanded registry.
    if (!VirtualProtect((LPVOID)(uintptr_t)kModelCtorImm,4,
                        PAGE_EXECUTE_READWRITE,&oldCtor)) {
        Report("REJECT","cannot protect constructor immediate");
        return false;
    }
    if (!VirtualProtect((LPVOID)(uintptr_t)kFactoryCmpImm,4,
                        PAGE_EXECUTE_READWRITE,&oldFactory)) {
        DWORD unused=0;
        VirtualProtect((LPVOID)(uintptr_t)kModelCtorImm,4,oldCtor,&unused);
        Report("REJECT","cannot protect factory immediate; constructor untouched");
        return false;
    }
    // Recheck the level after protection work, before any instruction writes.
    bool good=LevelNotConstructed() && ValidateRetail();
    bool attemptedCtor=false,attemptedFactory=false;
    if (good) {
        attemptedCtor=true;
        const bool ctorWritten=WriteNewCap(kModelCtorImm,requestedCap);
        if (ctorWritten) {
            attemptedFactory=true;
            good=WriteNewCap(kFactoryCmpImm,requestedCap);
        } else {
            good=false;
        }
    }
    if (!good) {
        // A concurrent foreign modification before our first write must NOT
        // be overwritten. Only roll back immediates that we tried to write.
        bool restoredCtor=true,restoredFactory=true;
        if (attemptedFactory)
            restoredFactory=WriteNewCap(kFactoryCmpImm,kOldCap);
        if (attemptedCtor)
            restoredCtor=WriteNewCap(kModelCtorImm,kOldCap);
        if (!restoredCtor || !restoredFactory)
            Report("FATAL","rollback could not restore every attempted write");
        else
            Report("REJECT","precommit check or write failed; attempted writes restored");
    }
    FlushInstructionCache(GetCurrentProcess(),(LPCVOID)(uintptr_t)kModelCtorImm,4);
    FlushInstructionCache(GetCurrentProcess(),(LPCVOID)(uintptr_t)kFactoryCmpImm,4);
    DWORD unused1=0,unused2=0;
    const bool restored1=!!VirtualProtect((LPVOID)(uintptr_t)kModelCtorImm,4,oldCtor,&unused1);
    const bool restored2=!!VirtualProtect((LPVOID)(uintptr_t)kFactoryCmpImm,4,oldFactory,&unused2);
    if (!restored1 || !restored2) {
        Report("FATAL","failed to restore original EXE page protection");
        return false;
    }
    if (good) {
        char detail[128]={0};
        _snprintf_s(detail,sizeof(detail),_TRUNCATE,
                    "model registry A/B/C reserve + factory guard: 1024 -> %lu",
                    (unsigned long)requestedCap);
        Report("ACTIVE",detail);
    }
    return good;
}
} // namespace

void OC_ModelRegistry2048_Init() {
    if (InterlockedCompareExchange(&g_attempted,1,0)!=0) return;
    char exe[MAX_PATH]={0};
    DWORD n=GetModuleFileNameA(NULL,exe,MAX_PATH);
    if (!n || n>=MAX_PATH) return;
    char* last=strrchr(exe,'\\');
    if (!last) return;
    last[1]=0;
    char ini[MAX_PATH]={0};
    if (_snprintf_s(ini,sizeof(ini),_TRUNCATE,"%sproxy.ini",exe)<0) return;
    const bool ultra=GetPrivateProfileIntA("StaticMapPools","ExpandModels8192",0,ini)!=0;
    const bool high=GetPrivateProfileIntA("StaticMapPools","ExpandModels4096",0,ini)!=0;
    const bool low=GetPrivateProfileIntA("StaticMapPools","ExpandModels2048",0,ini)!=0;
    if (!ultra && !high && !low) return; // OFF by default, no edits
    if (_snprintf_s(g_logFile,sizeof(g_logFile),_TRUNCATE,
                    "%smodel_registry_2048.log",exe)<0)
        return;
    Report("BEGIN",ultra?"optional 8192-entry ModelInfo patch":
                   high?"optional 4096-entry ModelInfo patch":"optional 2048-entry ModelInfo patch");
    Install(ultra?kUltraCap:(high?kHigherCap:kNewCap));
}
