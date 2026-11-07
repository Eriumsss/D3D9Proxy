// OpenConquest: experimental 4096-entry texture registry for ONE merged level.
// Static Isengard texture records imported into Helm's Deep's own PAK.
// This is the CPU-side keyed TextureInfo registry, NOT GPU texture memory.
//
// IMPORTANT: opt-in and fail-closed. The exact instruction bytes below are
// taken from the user's reconstructed ConquestEA.exe.gzf File Bytes, PE SHA256
// 3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858
// containing the custom .1911 reserve8 implementation. Other builds reject.
//
//   00876DF0  8D 73 24           LEA ESI,[EBX+24h]
//   00876DF3  BF 00 08 00 00     MOV EDI,800h
//   00876DF8  E8 FB F9 B8 FF     CALL 004067F8h (reserve8)
//   00876DFD  8D 73 2C           LEA ESI,[EBX+2Ch]
//   00876E00  E8 F3 F9 B8 FF     CALL 004067F8h
//
//   00876B73  0F BF 47 2C        MOVSX EAX,WORD PTR [EDI+2Ch]
//   00876B7A  03 C1              ADD EAX,ECX
//   00876B7C  3D 00 08 00 00     CMP EAX,800h
//   00876B83  0F 8F 0A 01 00 00 JG 876C93h
//
// Both immediates must change to 0x1000, BEFORE level construction.
// Separate 0x800 hard guard at 00877A1B belongs to the level+0xF4 registry,
// NOT textures. Never touch it as part of this patch.
//
// [StaticMapPools] ExpandTextures4096=0 (default)
// Debug: model_registry_2048.log + texture_registry_4096.log (separate toggles).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "texture_registry_4096.h"

#ifndef _M_IX86
#error Retail TextureInfo registry patch is x86-only
#endif

namespace {
static const DWORD kBase=0x00400000;
static const DWORD kCtorWindow=0x00876DF0;
static const DWORD kCtorImm=0x00876DF4;
static const DWORD kFactoryWindow=0x00876B73;
static const DWORD kFactoryImm=0x00876B7D;
static const DWORD kLevelCell=0x00CD7EA0;
static const DWORD kResourceManager=0x00CD80D8;
static const DWORD kOldCap=0x800;
static const DWORD kNewCap=0x1000;

// Stop exactly BEFORE 0x00876E05, where the separate model extension
// changes an immediate. Either patch may be enabled independently.
static const BYTE kCtorBefore[] = {
    0x8D,0x73,0x24,
    0xBF,0x00,0x08,0x00,0x00,
    0xE8,0xFB,0xF9,0xB8,0xFF,
    0x8D,0x73,0x2C,
    0xE8,0xF3,0xF9,0xB8,0xFF
};
static const BYTE kFactoryBefore[] = {
    0x0F,0xBF,0x47,0x2C,
    0x83,0xEC,0x14,
    0x03,0xC1,
    0x3D,0x00,0x08,0x00,0x00,
    0x53,0x56,
    0x0F,0x8F,0x0A,0x01,0x00,0x00
};
static const BYTE kReserveThunkBefore[] = {0xE9,0x33,0xD2,0xA9,0x00};
static const BYTE kReserve8Before[] = {
    0x66,0x39,0x7E,0x02,0x53,0x0F,0x84,0x67,0x00,0x00,0x00
};
static const BYTE kTexPairInsertBefore[] = {
    0x0F,0xB7,0x01,0x66,0x3B,0x41,0x02,0x7C,0x06
};
static const BYTE kResolverBefore[] = {
    0x56,0xFF,0x74,0x24,0x08,0x8D,0x77,0x2C,
    0xE8,0xEB,0x54,0xB9,0xFF
};
static volatile LONG g_started=0;
static char g_logPath[MAX_PATH];

static void Log(const char* state,const char* why) {
    if (!g_logPath[0]) return;
    FILE* f=NULL;
    if (fopen_s(&f,g_logPath,"ab")!=0 || !f) return;
    fprintf(f,"[%lu] %s: %s\n",(unsigned long)GetTickCount(),state,why);
    fclose(f);
}
static bool CanRead(DWORD address,size_t n) {
    if (!n || n>0xFFFFFFFFu || address>0xFFFFFFFFu-(DWORD)n) return false;
    MEMORY_BASIC_INFORMATION info={0};
    if (!VirtualQuery((LPCVOID)(uintptr_t)address,&info,sizeof(info))) return false;
    if (info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
    if (!(info.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|
                        PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|
                        PAGE_EXECUTE_WRITECOPY))) return false;
    return (uintptr_t)address+n <= (uintptr_t)info.BaseAddress+info.RegionSize;
}
static bool Match(DWORD address,const BYTE* data,size_t size) {
    if (!CanRead(address,size)) return false;
    __try { return memcmp((const void*)(uintptr_t)address,data,size)==0; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool NoLevelYet() {
    if (!CanRead(kLevelCell,sizeof(DWORD)) ||
        !CanRead(kResourceManager,sizeof(DWORD))) return false;
    __try {
        return *(volatile DWORD*)(uintptr_t)kLevelCell==0 &&
               *(volatile DWORD*)(uintptr_t)kResourceManager==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool ExactVersion() {
    return (DWORD)(uintptr_t)GetModuleHandleA(NULL)==kBase &&
           Match(kCtorWindow,kCtorBefore,sizeof(kCtorBefore)) &&
           Match(kFactoryWindow,kFactoryBefore,sizeof(kFactoryBefore)) &&
           Match(0x004067F8,kReserveThunkBefore,sizeof(kReserveThunkBefore)) &&
           Match(0x00EA3A30,kReserve8Before,sizeof(kReserve8Before)) &&
           Match(0x0040B3F3,kTexPairInsertBefore,sizeof(kTexPairInsertBefore)) &&
           Match(0x0087603A,kResolverBefore,sizeof(kResolverBefore));
}
static bool WriteImmediate(DWORD at,DWORD cap) {
    __try {
        memcpy((void*)(uintptr_t)at,&cap,sizeof(cap));
        return *(volatile DWORD*)(uintptr_t)at==cap;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool Apply() {
    if (!ExactVersion()) {
        Log("REJECT","unknown/replaced EXE instructions or reserve/insert paths");
        return false;
    }
    if (!NoLevelYet()) {
        Log("REJECT","level or resource manager already exists; no retrofit of live vectors");
        return false;
    }
    DWORD oldCtorProtect=0,oldFactoryProtect=0,unused=0;
    if (!VirtualProtect((LPVOID)(uintptr_t)kCtorImm,4,
                        PAGE_EXECUTE_READWRITE,&oldCtorProtect)) {
        Log("REJECT","cannot make constructor immediate writable");
        return false;
    }
    if (!VirtualProtect((LPVOID)(uintptr_t)kFactoryImm,4,
                        PAGE_EXECUTE_READWRITE,&oldFactoryProtect)) {
        VirtualProtect((LPVOID)(uintptr_t)kCtorImm,4,oldCtorProtect,&unused);
        Log("REJECT","cannot protect factory immediate; no writes attempted");
        return false;
    }

    // Best-effort two-site patch with rollback. This is NOT a globally
    // atomic write transaction. Must run early, before any level-loading
    // threads execute these code paths.
    bool okay=ExactVersion() && NoLevelYet();
    bool wroteCtor=false,wroteFactory=false;
    if (okay) {
        wroteCtor=true;
        okay=WriteImmediate(kCtorImm,kNewCap);
        if (okay) {
            wroteFactory=true;
            okay=WriteImmediate(kFactoryImm,kNewCap);
        }
    }
    if (!okay) {
        // Do not change bytes on signature mismatch before our own write.
        bool restored=true;
        if (wroteFactory) restored=WriteImmediate(kFactoryImm,kOldCap)&&restored;
        if (wroteCtor) restored=WriteImmediate(kCtorImm,kOldCap)&&restored;
        Log(restored?"REJECT":"FATAL",
            restored?"preflight/write failed; attempted writes rolled back":
                     "rollback failed; do not run this game instance");
    }
    FlushInstructionCache(GetCurrentProcess(),(LPCVOID)(uintptr_t)kCtorImm,4);
    FlushInstructionCache(GetCurrentProcess(),(LPCVOID)(uintptr_t)kFactoryImm,4);
    DWORD trash1=0,trash2=0;
    const bool protectedCtor=!!VirtualProtect((LPVOID)(uintptr_t)kCtorImm,4,
                                              oldCtorProtect,&trash1);
    const bool protectedFactory=!!VirtualProtect((LPVOID)(uintptr_t)kFactoryImm,4,
                                                 oldFactoryProtect,&trash2);
    if (!protectedCtor || !protectedFactory) {
        Log("FATAL","could not restore code page protection");
        return false;
    }
    if (okay)
        Log("ACTIVE","texture vectors +0x24/+0x2C and texture factory guard: 2048 -> 4096");
    return okay;
}
} // namespace

void OC_TextureRegistry4096_Init() {
    if (InterlockedCompareExchange(&g_started,1,0)!=0) return;
    char gamePath[MAX_PATH]={0};
    DWORD n=GetModuleFileNameA(NULL,gamePath,MAX_PATH);
    if (!n || n>=MAX_PATH) return;
    char* last=strrchr(gamePath,'\\');
    if (!last) return;
    last[1]=0;
    char ini[MAX_PATH]={0};
    if (_snprintf_s(ini,sizeof(ini),_TRUNCATE,"%sproxy.ini",gamePath)<0) return;
    if (!GetPrivateProfileIntA("StaticMapPools","ExpandTextures4096",0,ini))
        return; // default OFF
    if (_snprintf_s(g_logPath,sizeof(g_logPath),_TRUNCATE,
                    "%stexture_registry_4096.log",gamePath)<0) return;
    Log("BEGIN","GZF-gated static TextureInfo registry 4096 experiment");
    Apply();
}
