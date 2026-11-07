// OpenConquest: static-map registry census, NOT a heap/pool extender.
// Ground truth: MgLevelObject.h/.cpp + MgModelRegistry.cpp +
// MgTextureRegistry.cpp + MgEffectRegistry.cpp + MgLevelConstruct.cpp.
// Intended use: measure Helm's Deep versus a test merge of Isengard static
// geometry. No mission scripts, no NPC logic, no executable patching.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "static_pool_census.h"

#ifndef _M_IX86
#error Retail static pool census requires the 32-bit game ABI
#endif

namespace {
static const uintptr_t kExpectedImageBase = 0x00400000;
static const uintptr_t kLevelObjectCell = 0x00CD7EA0;
static const uintptr_t kDynamicGeometryPoolCell = 0x00CD80D4;
static const DWORD kPollMs = 250;
static const DWORD kLogMs = 2000;
static const unsigned kMaxReasonableCount = 0x4000;

struct Registry {
    const char* name;
    unsigned off;
    unsigned expectedCap;
};
static const Registry kRegistries[] = {
    {"texture_a",0x24,0x800}, {"texture_b",0x2c,0x800},
    {"model_a",0x8c,0x400}, {"model_b",0x94,0x400},
    {"model_c",0x9c,0x400}, {"resource_other",0xf4,0x800},
    {"effect_a",0xfc,0x200}, {"effect_b",0x104,0x200},
    {"effect_c",0x10c,0x200}, {"pfield",0x114,0 /* unverified */},
    {"gfx_block",0x11c,0x40}
};
enum { kRegistryCount = sizeof(kRegistries)/sizeof(kRegistries[0]) };
struct Cell {
    unsigned count;
    unsigned cap;
    uintptr_t entries;
    bool valid;
};
static volatile LONG g_started;
static volatile LONG g_enabled;
static char g_output[MAX_PATH];
static unsigned g_high[kRegistryCount];
static DWORD g_lastLog;
static uintptr_t g_lastLevel;
static unsigned g_rows;

static bool CanRead(uintptr_t address, size_t len) {
    if (!address || !len || address > UINTPTR_MAX - len) return false;
    uintptr_t cur=address;
    const uintptr_t limit=address+len;
    while (cur < limit) {
        MEMORY_BASIC_INFORMATION m={0};
        if (!VirtualQuery((LPCVOID)cur, &m, sizeof(m)) ||
            m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_NOACCESS|PAGE_GUARD)) ||
            !(m.Protect & (PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|
                           PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))
            return false;
        uintptr_t end=(uintptr_t)m.BaseAddress + m.RegionSize;
        if (end <= cur) return false;
        cur = end < limit ? end : limit;
    }
    return true;
}
static bool ReadDword(uintptr_t address, DWORD& result) {
    if (!CanRead(address,sizeof(DWORD))) return false;
    __try { result=*(volatile const DWORD*)address; return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool ReadRegistry(uintptr_t level,const Registry& reg,Cell& out) {
    memset(&out,0,sizeof(out));
    if (!CanRead(level+reg.off,8)) return false;
    __try {
        const unsigned char* p=(const unsigned char*)(level+reg.off);
        const short count=*(const volatile short*)(p+0);
        const short capacity=*(const volatile short*)(p+2);
        const DWORD entries=*(const volatile DWORD*)(p+4);
        if (count<0 || capacity<0 || (unsigned)capacity>kMaxReasonableCount ||
            count>capacity || (count>0 && !entries)) return false;
        out.count=(unsigned)count;
        out.cap=(unsigned)capacity;
        out.entries=entries;
        out.valid=true;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool ExeIsRetailVariant() {
    if ((uintptr_t)GetModuleHandleA(NULL)!=kExpectedImageBase) return false;
    if (!CanRead(0x00401AAD,1)) return false;
    __try {
        BYTE b=*(volatile const BYTE*)0x00401AAD;
        // Both known EA and LLC variants; not sufficient to authorize mutation.
        return b==0x6A || b==0x90;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Read-only instruction-window evidence for the *next* patch generation.
// The Magellan addresses tell us which locations to investigate, NOT the
// exact expected opcode bytes. No code patch is authorized by this dump.
struct CodeSite { const char* purpose; DWORD address; };
static const CodeSite kCodeSites[] = {
    {"model_ctor_A",0x00876E17}, {"model_ctor_B",0x00876E22},
    {"model_ctor_C",0x00876E2D}, {"model_factory_guard",0x00875D2F},
    {"texture_ctor_A",0x00876DF8}, {"texture_ctor_B",0x00876E00},
    {"texture_factory_guard",0x00876B73},
    {"effect_ctor_A",0x00876E43}, {"effect_ctor_B",0x00876E4E},
    {"effect_ctor_C",0x00876E59}, {"effect_factory_guard",0x008773A0},
    {"gfx_ctor",0x00876E6F}, {"gfx_factory_guard",0x00875ABE}
};
static void DumpCodeSiteFingerprints(const char* gameDir) {
    char path[MAX_PATH]={0};
    if (_snprintf_s(path,sizeof(path),_TRUNCATE,
                    "%sstatic_map_opcode_windows.txt",gameDir)<0) return;
    FILE* f=NULL;
    if (fopen_s(&f,path,"wb")!=0 || !f) return;
    fputs("# Read-only retail instruction bytes; NOT a patch manifest.\n",f);
    for (unsigned i=0;i<sizeof(kCodeSites)/sizeof(kCodeSites[0]);++i) {
        const CodeSite& site=kCodeSites[i];
        const size_t n=24;
        BYTE data[n]={0};
        bool ok=false;
        if (CanRead(site.address,n)) {
            __try {
                memcpy(data,(const void*)(uintptr_t)site.address,n);
                ok=true;
            } __except(EXCEPTION_EXECUTE_HANDLER) { ok=false; }
        }
        fprintf(f,"%s,0x%08lX,%s",site.purpose,
                (unsigned long)site.address,ok?"OK":"UNREADABLE");
        if (ok) for (size_t j=0;j<n;++j) fprintf(f," %02X",(unsigned)data[j]);
        fputc('\n',f);
    }
    fclose(f);
}

static void Header(FILE* f) {
    fputs("time_ms,level_object,registry,entry_count,capacity,peak_count,expected_retail_capacity,entries_ptr,valid,dyn_geom_pool,dyn_a_count,dyn_c_count,dyn_phase\n",f);
}
static void Poll() {
    DWORD levelRaw=0, poolRaw=0;
    if (!ReadDword(kLevelObjectCell,levelRaw)) return;
    ReadDword(kDynamicGeometryPoolCell,poolRaw);
    const uintptr_t level=levelRaw;
    if (level != g_lastLevel) {
        g_lastLevel=level;
        memset(g_high,0,sizeof(g_high));
        g_lastLog=0;
    }
    if (!level) return; // no active level object
    Cell cells[kRegistryCount]={};
    for (unsigned i=0;i<kRegistryCount;++i) {
        ReadRegistry(level,kRegistries[i],cells[i]);
        if (cells[i].valid && cells[i].count>g_high[i]) g_high[i]=cells[i].count;
    }
    DWORD poolA=0,poolC=0,phase=0;
    // Dynamic geometry pool is separate from static map VBuffInfo/IBuffInfo.
    // Read only three known counters; do NOT infer model capacity from them.
    if (poolRaw && CanRead(poolRaw,0x17FD10)) {
        ReadDword((uintptr_t)poolRaw+0x17FD00,poolA);
        ReadDword((uintptr_t)poolRaw+0x17FD08,poolC);
        ReadDword((uintptr_t)poolRaw+0x17FD0C,phase);
    }
    DWORD now=GetTickCount();
    if (g_lastLog && (DWORD)(now-g_lastLog)<kLogMs) return;
    g_lastLog=now;
    FILE* f=NULL;
    if (fopen_s(&f,g_output,"ab+")!=0 || !f) return;
    if (fseek(f,0,SEEK_END)==0 && ftell(f)==0) Header(f);
    for(unsigned i=0;i<kRegistryCount;++i) {
        const Cell& c=cells[i];
        fprintf(f,"%lu,%08lX,%s,%u,%u,%u,%u,%08lX,%u,%08lX,%lu,%lu,%lu\n",
                (unsigned long)now,(unsigned long)levelRaw,kRegistries[i].name,
                c.count,c.cap,g_high[i],kRegistries[i].expectedCap,
                (unsigned long)c.entries,(unsigned)c.valid,
                (unsigned long)poolRaw,(unsigned long)poolA,
                (unsigned long)poolC,(unsigned long)phase);
    }
    fclose(f);
    ++g_rows;
}
static DWORD WINAPI Worker(LPVOID) {
    while (InterlockedCompareExchange(&g_enabled,1,1)==1) {
        Poll();
        Sleep(kPollMs);
    }
    return 0;
}
} // namespace

void OC_StaticPoolCensus_Init() {
    if (InterlockedCompareExchange(&g_started,1,0)!=0) return;
    char exe[MAX_PATH]={0};
    DWORD n=GetModuleFileNameA(NULL,exe,MAX_PATH);
    if (!n || n>=MAX_PATH || !ExeIsRetailVariant()) return;
    char* slash=strrchr(exe,'\\');
    if (!slash) return;
    slash[1]=0;
    char ini[MAX_PATH]={0};
    if (_snprintf_s(ini,sizeof(ini),_TRUNCATE,"%sproxy.ini",exe)<0) return;
    if (!GetPrivateProfileIntA("StaticMapPools","Monitor",0,ini)) return;
    if (_snprintf_s(g_output,sizeof(g_output),_TRUNCATE,
                    "%sstatic_map_pools.csv",exe)<0) return;
    // Evidence only: dump constructor and factory guard opcode windows, which
    // the next patch must verify against the Ghidra database before writes.
    DumpCodeSiteFingerprints(exe);
    // A pinned module means this worker never executes unloaded DLL code.
    // Call only from Direct3DCreate9/Ex initialization, never DllMain.
    HMODULE module=NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR)(uintptr_t)&OC_StaticPoolCensus_Init,&module))
        return;
    InterlockedExchange(&g_enabled,1);
    HANDLE thread=CreateThread(NULL,0,Worker,NULL,0,NULL);
    if (thread) CloseHandle(thread);
    else InterlockedExchange(&g_enabled,0);
}
