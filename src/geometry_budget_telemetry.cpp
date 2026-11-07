// OpenConquest: static mesh GPU buffer ALLOCATION PRESSURE, READ-ONLY.
// Helm's Deep (base) + imported Isengard static assets in ONE level.
// Not a pool extender or texture vault: a failed CreateVB/CreateIB is
// observed, not intercepted; all original HRESULT/COM objects pass through.
//
// Magellan source + GZF-retail callsite evidence:
// 0068e0e9: FF 51 68   CALL [ECX+68h] -> returns at 0068e0ec
// 0068dd74: FF 51 6C   CALL [ECX+6Ch] -> returns at 0068dd77
// These callsites build native VBuffInfo/IBuffInfo for static models.
// Dynamic ring sources use OTHER callsites and remain in "other" counts.
// There is NO guaranteed max static VB/IB count; measure byte pressure and
// allocation failures rather than inventing a fake 512/1024 pool cap.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "geometry_budget_telemetry.h"
#ifndef _M_IX86
#error The retail geometry budget monitor is x86-only
#endif

namespace {
static INIT_ONCE g_once=INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;
static volatile LONG g_ready=0;
static volatile LONG g_enabled=0;
static bool g_verifiedStaticCallsites=false;
static const DWORD kVBStaticCall=0x0068E0E9;
static const DWORD kIBStaticCall=0x0068DD74;
static const DWORD kVBReturn=0x0068E0EC;
static const DWORD kIBReturn=0x0068DD77;
static const DWORD kLevelCell=0x00CD7EA0;
static char g_csv[MAX_PATH];

struct Counts {
    unsigned __int64 calls;
    unsigned __int64 succeeded;
    unsigned __int64 failed;
    unsigned __int64 bytesOk;
    unsigned __int64 bytesFailed;
    unsigned __int64 managedBytes;
    unsigned __int64 defaultBytes;
    unsigned __int64 systemBytes;
    unsigned __int64 staticCallsOk;
    unsigned __int64 staticBytesOk;
    unsigned __int64 staticCallsFailed;
};
static Counts g_vb={0},g_ib={0};

static BOOL CALLBACK InitLock(PINIT_ONCE,PVOID,PVOID*) {
    InitializeCriticalSection(&g_lock);
    InterlockedExchange(&g_ready,1);
    return TRUE;
}
static bool SafeMatch(DWORD address,const BYTE* seq,unsigned n) {
    MEMORY_BASIC_INFORMATION m={0};
    if (!VirtualQuery((LPCVOID)(uintptr_t)address,&m,sizeof(m)) ||
        m.State!=MEM_COMMIT || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
        (uintptr_t)m.BaseAddress+m.RegionSize < (uintptr_t)address+n)
        return false;
    __try { return memcmp((const void*)(uintptr_t)address,seq,n)==0; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static DWORD LevelObject() {
    MEMORY_BASIC_INFORMATION m={0};
    if (!VirtualQuery((LPCVOID)(uintptr_t)kLevelCell,&m,sizeof(m)) ||
        m.State!=MEM_COMMIT || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
        (uintptr_t)m.BaseAddress+m.RegionSize < kLevelCell+4)
        return 0;
    __try { return *(volatile DWORD*)(uintptr_t)kLevelCell; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void Record(Counts& counts,UINT bytes,D3DPOOL pool,
                   HRESULT hr,bool outOk,void* returnAddress,DWORD expected) {
    if (InterlockedCompareExchange(&g_enabled,1,1)!=1 || !g_ready) return;
    const bool succeeded=SUCCEEDED(hr)&&outOk;
    EnterCriticalSection(&g_lock);
    ++counts.calls;
    if (succeeded) {
        ++counts.succeeded;
        counts.bytesOk+=bytes;
        if (pool==D3DPOOL_MANAGED) counts.managedBytes+=bytes;
        else if (pool==D3DPOOL_DEFAULT) counts.defaultBytes+=bytes;
        else if (pool==D3DPOOL_SYSTEMMEM) counts.systemBytes+=bytes;
    } else {
        ++counts.failed;
        counts.bytesFailed+=bytes;
    }
    if (g_verifiedStaticCallsites && (DWORD)(uintptr_t)returnAddress==expected) {
        if (succeeded) {
            ++counts.staticCallsOk;
            counts.staticBytesOk+=bytes;
        } else ++counts.staticCallsFailed;
    }
    LeaveCriticalSection(&g_lock);
}
static void WriteSample() {
    Counts vb={0},ib={0};
    EnterCriticalSection(&g_lock);
    vb=g_vb; ib=g_ib;
    LeaveCriticalSection(&g_lock);
    MEMORYSTATUSEX st={0};
    st.dwLength=sizeof(st);
    const bool hasVA=!!GlobalMemoryStatusEx(&st);
    FILE* f=NULL;
    if (fopen_s(&f,g_csv,"ab+")!=0||!f) return;
    if (!fseek(f,0,SEEK_END)&&ftell(f)==0) {
        fputs("ms,level,va_total_mib,va_free_mib,"
              "vb_calls,vb_ok,vb_fail,vb_bytes_ok,vb_bytes_failed,vb_managed_bytes,vb_default_bytes,vb_system_bytes,static_vb_ok,static_vb_bytes,static_vb_fail,"
              "ib_calls,ib_ok,ib_fail,ib_bytes_ok,ib_bytes_failed,ib_managed_bytes,ib_default_bytes,ib_system_bytes,static_ib_ok,static_ib_bytes,static_ib_fail,static_callsite_signature_ok\n",f);
    }
    // Cumulative allocation attempts since game start: not live GPU memory.
    fprintf(f,"%lu,%08lX,%llu,%llu,"
            "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
            "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u\n",
            (unsigned long)GetTickCount(),(unsigned long)LevelObject(),
            hasVA?(unsigned __int64)(st.ullTotalVirtual>>20):0ull,
            hasVA?(unsigned __int64)(st.ullAvailVirtual>>20):0ull,
            vb.calls,vb.succeeded,vb.failed,vb.bytesOk,vb.bytesFailed,
            vb.managedBytes,vb.defaultBytes,vb.systemBytes,
            vb.staticCallsOk,vb.staticBytesOk,vb.staticCallsFailed,
            ib.calls,ib.succeeded,ib.failed,ib.bytesOk,ib.bytesFailed,
            ib.managedBytes,ib.defaultBytes,ib.systemBytes,
            ib.staticCallsOk,ib.staticBytesOk,ib.staticCallsFailed,
            g_verifiedStaticCallsites?1u:0u);
    fclose(f);
}
static DWORD WINAPI Worker(LPVOID) {
    while (InterlockedCompareExchange(&g_enabled,1,1)==1) {
        WriteSample();
        Sleep(1000); // still samples during renderless level loading
    }
    return 0;
}
} // namespace

void OC_GeometryBudget_Init() {
    // Only called by the proxy's patch InitOnce, never under DllMain lock.
    char dir[MAX_PATH]={0};
    DWORD len=GetModuleFileNameA(NULL,dir,MAX_PATH);
    if (!len||len>=MAX_PATH) return;
    char* slash=strrchr(dir,'\\');
    if (!slash) return;
    slash[1]=0;
    char ini[MAX_PATH]={0};
    if (_snprintf_s(ini,sizeof(ini),_TRUNCATE,"%sproxy.ini",dir)<0) return;
    if (!GetPrivateProfileIntA("StaticMapPools","GeometryBudget",0,ini))
        return;  // default OFF
    if (_snprintf_s(g_csv,sizeof(g_csv),_TRUNCATE,
                    "%sstatic_geometry_budget.csv",dir)<0) return;
    if (!InitOnceExecuteOnce(&g_once,InitLock,NULL,NULL) || !g_ready)
        return;
    const BYTE vbCall[]={0xFF,0x51,0x68}, ibCall[]={0xFF,0x51,0x6C};
    g_verifiedStaticCallsites=((DWORD)(uintptr_t)GetModuleHandleA(NULL)==0x00400000 &&
       SafeMatch(kVBStaticCall,vbCall,sizeof(vbCall)) &&
       SafeMatch(kIBStaticCall,ibCall,sizeof(ibCall)));
    // Keep code mapped for process lifetime. Deliberately leak nothing other
    // than the module pin; OS reclaims it at process exit.
    HMODULE self=NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR)(uintptr_t)&OC_GeometryBudget_Init,&self))
        return;
    InterlockedExchange(&g_enabled,1);
    HANDLE thread=CreateThread(NULL,0,Worker,NULL,0,NULL);
    if (thread) CloseHandle(thread);
    else InterlockedExchange(&g_enabled,0);
}

void OC_GeometryBudget_VB(UINT size,DWORD,D3DPOOL pool,
                          HRESULT hr,bool valid,void* caller) {
    Record(g_vb,size,pool,hr,valid,caller,kVBReturn);
}
void OC_GeometryBudget_IB(UINT size,DWORD,D3DPOOL pool,
                          HRESULT hr,bool valid,void* caller) {
    Record(g_ib,size,pool,hr,valid,caller,kIBReturn);
}
