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
    {"effect_c",0x10c,0x200}, {"pfield",0x114,0x1000 /* verified native x86 */},
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
static char g_pressureOutput[MAX_PATH];
static char g_vaOutput[MAX_PATH];
static unsigned g_resetPeaks[3]={0,0,0};
static unsigned g_clipPeak=0;
static unsigned __int64 g_innerPairsPeak=0;
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
// Extra READ-ONLY telemetry. Do not confuse these runtime counts with the
// on-disk PAK/BIN tables or promise that a config toggle actually activated.
// Three independent D3D reset lists hold u32 pointer bases and u32 counts.
// Their physical allocated byte sizes are not exposed by the original engine;
// therefore the CSV deliberately reports NO invented numeric capacity here.
struct ResetList {
    const char* name;
    DWORD baseCell;
    DWORD countCell;
};
static const ResetList kResetLists[3]={
    {"device_reset",0x00A41538,0x00A4153C},
    {"dynamic_vb",0x00A41520,0x00A41524},
    {"dynamic_ib",0x00A4152C,0x00A41530}
};
static const DWORD kAnimBindRegCell=0x00CD7E5C; // MgAgBindReg*
static const DWORD kAnimBindRegVtable=0x009C7A1C;
static const unsigned kMaxObservePairs=8192;

struct ResetSample {
    unsigned count;
    unsigned valid;
};
static ResetSample ObserveReset(const ResetList& list) {
    ResetSample result={0,0};
    DWORD base=0,count=0;
    if (!ReadDword(list.baseCell,base) || !ReadDword(list.countCell,count))
        return result;
    // 4096 is a supported cap, not a proof of the active allocation.
    // Reject pathological/corrupted counts without following any pointers.
    if (count>0x10000 || (count!=0 && base==0) ||
        (count!=0 && !CanRead((uintptr_t)base,(size_t)count*4u)))
        return result;
    result.count=count;
    result.valid=1;
    return result;
}

struct BindSample {
    unsigned present;
    unsigned valid;
    unsigned clips;
    unsigned clipCap;
    unsigned innerMaps;
    unsigned __int64 innerPairs;
    unsigned maxInnerCount;
    unsigned invalidInner;
};
static BindSample ObserveBindings() {
    BindSample out={0,0,0,0,0,0,0,0};
    DWORD reg=0;
    if (!ReadDword(kAnimBindRegCell,reg) || reg==0)
        return out;
    out.present=1;
    DWORD vt=0,counts=0,base=0;
    if (!ReadDword((uintptr_t)reg,vt) ||
        !ReadDword((uintptr_t)reg+0x0C,counts) ||
        !ReadDword((uintptr_t)reg+0x10,base) ||
        vt!=kAnimBindRegVtable)
        return out;
    const short count=(short)(counts&0xffff);
    const short cap=(short)(counts>>16);
    if (count<0 || cap<0 || count>cap ||
        (unsigned)cap>kMaxObservePairs ||
        (count>0 && !base))
        return out;
    out.valid=1;
    out.clips=(unsigned)count;
    out.clipCap=(unsigned)cap;
    // Outer list: 8-byte records {CRC key, MgAgBindMap*}. Inspect only the
    // binding map pointer in the second dword. No writes, allocations or
    // runtime hooks. Validate EVERY pointer before reading the next one.
    for (unsigned i=0;i<out.clips;++i) {
        DWORD map=0,packed=0,pairs=0;
        const uintptr_t slot=(uintptr_t)base+(uintptr_t)i*8u;
        if (!ReadDword(slot+4,map) || !map ||
            !ReadDword((uintptr_t)map,packed) ||
            !ReadDword((uintptr_t)map+4,pairs)) {
            ++out.invalidInner;
            continue;
        }
        const short n=(short)(packed&0xffff);
        const short capacity=(short)(packed>>16);
        if (n<0 || capacity<0 || n>capacity ||
            (unsigned)capacity>kMaxObservePairs ||
            (n>0 && !pairs)) {
            ++out.invalidInner;
            continue;
        }
        // If this map holds bindings, ensure the LAST 8-byte pair is
        // presently readable too. A concurrent level teardown might
        // invalidate the pointer at any instant; CanRead+SEH are guards,
        // not a claim of an atomic registry snapshot.
        if (n>0 && !CanRead((uintptr_t)pairs,(size_t)n*8u)) {
            ++out.invalidInner;
            continue;
        }
        ++out.innerMaps;
        out.innerPairs+=(unsigned)n;
        if ((unsigned)n>out.maxInnerCount)
            out.maxInnerCount=(unsigned)n;
    }
    return out;
}
static BindSample g_latestBinding={0,0,0,0,0,0,0,0};
static ResetSample g_latestReset[3]={{0,0},{0,0},{0,0}};

// Sample every 250ms on the EXISTING worker, log latest sample every 2s.
static void SamplePressure() {
    g_latestBinding=ObserveBindings();
    if (g_latestBinding.valid && g_latestBinding.clips>g_clipPeak)
        g_clipPeak=g_latestBinding.clips;
    if (g_latestBinding.valid && g_latestBinding.innerPairs>g_innerPairsPeak)
        g_innerPairsPeak=g_latestBinding.innerPairs;
    for (unsigned i=0;i<3;++i) {
        g_latestReset[i]=ObserveReset(kResetLists[i]);
        if (g_latestReset[i].valid && g_latestReset[i].count>g_resetPeaks[i])
            g_resetPeaks[i]=g_latestReset[i].count;
    }
}
static void PressureHeader(FILE* f) {
    fputs("time_ms,level_object,bind_present,bind_valid,clip_count,clip_cap,"
          "clip_peak,inner_maps,inner_pairs,inner_pairs_peak,max_inner_count,"
          "invalid_inner_maps,device_reset_count,device_reset_peak,device_reset_valid,"
          "dynamic_vb_count,dynamic_vb_peak,dynamic_vb_valid,"
          "dynamic_ib_count,dynamic_ib_peak,dynamic_ib_valid\n",f);
}
static void LogPressure(DWORD now,DWORD levelRaw) {
    if (!g_pressureOutput[0]) return;
    const BindSample& binding=g_latestBinding;
    const ResetSample& dev=g_latestReset[0];
    const ResetSample& vb=g_latestReset[1];
    const ResetSample& ib=g_latestReset[2];
    FILE* f=NULL;
    if (fopen_s(&f,g_pressureOutput,"ab+")!=0 || !f) return;
    if (fseek(f,0,SEEK_END)==0 && ftell(f)==0)
        PressureHeader(f);
    fprintf(f,"%lu,%08lX,%u,%u,%u,%u,%u,%u,%llu,%llu,%u,%u,"
              "%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
            (unsigned long)now,(unsigned long)levelRaw,
            binding.present,binding.valid,binding.clips,binding.clipCap,
            g_clipPeak,binding.innerMaps,binding.innerPairs,g_innerPairsPeak,
            binding.maxInnerCount,binding.invalidInner,
            dev.count,g_resetPeaks[0],dev.valid,
            vb.count,g_resetPeaks[1],vb.valid,
            ib.count,g_resetPeaks[2],ib.valid);
    fclose(f);
}

// The registry can still load no additional geometry because this is an x86
// PROCESS with a finite VA space. GlobalMemoryStatusEx is system-wide and
// tells us nothing about the largest contiguous allocation THIS game can make.
//
// Walk committed/reserved/free VM regions via VirtualQuery. This is read-only.
// It works before the first level object exists, including a stuck loader.
// Use unsigned 64-bit counters to prevent 32-bit address-span arithmetic from
// wrapping near the process's maximum application address.
struct VaPressure {
    unsigned valid;
    DWORD minAddress;
    DWORD maxAddress;
    unsigned __int64 freeBytes;
    unsigned __int64 reservedBytes;
    unsigned __int64 committedBytes;
    unsigned __int64 largestFreeBytes;
    unsigned freeRegions;
    unsigned reservedRegions;
    unsigned committedRegions;
};
static VaPressure MeasureVirtualAddressPressure() {
    VaPressure stats={};
    SYSTEM_INFO info={};
    GetSystemInfo(&info);
    const uintptr_t minimum=(uintptr_t)info.lpMinimumApplicationAddress;
    const uintptr_t maximum=(uintptr_t)info.lpMaximumApplicationAddress;
    stats.minAddress=(DWORD)minimum;
    stats.maxAddress=(DWORD)maximum;
    if (!minimum || maximum<minimum) return stats;

    const unsigned __int64 endExclusive=(unsigned __int64)maximum+1ULL;
    uintptr_t current=minimum;
    while ((unsigned __int64)current<endExclusive) {
        MEMORY_BASIC_INFORMATION region={};
        if (!VirtualQuery((LPCVOID)current,&region,sizeof(region)))
            return stats; // valid=0: partial results cannot be trusted
        const unsigned __int64 begin=(unsigned __int64)(uintptr_t)region.BaseAddress;
        const unsigned __int64 rawEnd=begin+(unsigned __int64)region.RegionSize;
        if (begin>(unsigned __int64)current ||
            rawEnd<=(unsigned __int64)current ||
            region.RegionSize==0)
            return stats; // non-forward progress or invalid VM metadata
        const unsigned __int64 next=rawEnd<endExclusive?rawEnd:endExclusive;
        const unsigned __int64 bytes=next-(unsigned __int64)current;
        switch (region.State) {
        case MEM_FREE:
            stats.freeBytes+=bytes;
            ++stats.freeRegions;
            if (bytes>stats.largestFreeBytes)
                stats.largestFreeBytes=bytes;
            break;
        case MEM_RESERVE:
            stats.reservedBytes+=bytes;
            ++stats.reservedRegions;
            break;
        case MEM_COMMIT:
            stats.committedBytes+=bytes;
            ++stats.committedRegions;
            break;
        default:
            return stats;
        }
        if (next==endExclusive) break;
        current=(uintptr_t)next;
    }
    stats.valid=1;
    return stats;
}
static void LogVirtualAddressPressure(DWORD tick,DWORD levelRaw) {
    if (!g_vaOutput[0]) return;
    const VaPressure stats=MeasureVirtualAddressPressure();
    FILE* f=NULL;
    if (fopen_s(&f,g_vaOutput,"ab+")!=0 || !f) return;
    if (fseek(f,0,SEEK_END)==0 && ftell(f)==0)
        fputs("time_ms,level_object,valid,minimum_address,maximum_address,"
              "free_bytes,reserved_bytes,committed_bytes,largest_free_bytes,"
              "free_regions,reserved_regions,committed_regions\n",f);
    fprintf(f,"%lu,%08lX,%u,%08lX,%08lX,%llu,%llu,%llu,%llu,%u,%u,%u\n",
            (unsigned long)tick,(unsigned long)levelRaw,stats.valid,
            (unsigned long)stats.minAddress,(unsigned long)stats.maxAddress,
            stats.freeBytes,stats.reservedBytes,stats.committedBytes,
            stats.largestFreeBytes,
            stats.freeRegions,stats.reservedRegions,stats.committedRegions);
    fclose(f);
}

static void Poll() {
    DWORD levelRaw=0, poolRaw=0;
    if (!ReadDword(kLevelObjectCell,levelRaw)) return;
    ReadDword(kDynamicGeometryPoolCell,poolRaw);
    const uintptr_t level=levelRaw;
    if (level != g_lastLevel) {
        g_lastLevel=level;
        memset(g_high,0,sizeof(g_high));
        memset(g_resetPeaks,0,sizeof(g_resetPeaks));
        g_clipPeak=0;
        g_innerPairsPeak=0;
        g_lastLog=0;
    }
    if (!level) {
        // Crucial: record x86 address-space pressure EVEN WHEN NO LEVEL WAS
        // CONSTRUCTED. An infinite loading screen may fail before all the
        // other registry logs have a level pointer worth recording.
        const DWORD now=GetTickCount();
        if (!g_lastLog || (DWORD)(now-g_lastLog)>=kLogMs) {
            g_lastLog=now;
            LogVirtualAddressPressure(now,0);
        }
        return;
    }
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
    SamplePressure(); // 250ms readings, independent from CSV write cadence
    DWORD now=GetTickCount();
    if (g_lastLog && (DWORD)(now-g_lastLog)<kLogMs) return;
    g_lastLog=now;
    LogPressure(now,levelRaw); // write sampled peak + latest values every 2s
    LogVirtualAddressPressure(now,levelRaw); // free/commit/fragmentation, including level
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
    if (_snprintf_s(g_pressureOutput,sizeof(g_pressureOutput),_TRUNCATE,
                    "%sruntime_pool_pressure.csv",exe)<0) return;
    if (_snprintf_s(g_vaOutput,sizeof(g_vaOutput),_TRUNCATE,
                    "%svirtual_address_pressure.csv",exe)<0) return;
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
