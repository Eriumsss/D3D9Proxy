// OpenConquest: native 1024 -> 4096 animation EVENT record pool.
// This is not the 1024 x 0xF8 BLEND arena at FUN_0045549c.
// Verified Magellan oracle:
// MgPhysCharProxy.cpp Mg_009555df_AnEvtPoolInit, Mg_00404c2f_PoolInit
// (ret 0x0C), Mg_0096e44c_AnEvtPoolFree, Mg_0088aa78_PostAnimEvent,
// Mg_00404cbb_ReleaseAnimQueue, Mg_0042e090_ClearAnimQueues.
// Original native helper uses 0x0067e610 allocator and 0x404c6c free-list
// threader, which is why we call the ORIGINAL PoolInit instead of VirtualAlloc.
//
// WARNING: the old 1024-slot arena deliberately remains allocated (48 KiB)
// after success, because pointers from previous users cannot be disproved
// without a real retail runtime probe. The game's ORIGINAL atexit owner will
// delete[] the NEW base, allocated by the game's own original PoolInit.
// Not a Havok allocator or a second-world loader.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "animation_event_pool_4096.h"
#ifndef _M_IX86
#error Retail animation-event pool extension requires x86 native ABI
#endif
namespace {
static const DWORD kExeBase=0x00400000;
static const DWORD kLevelCell=0x00CD7EA0;
static const DWORD kResourceMgrCell=0x00CD80D8;
static const DWORD kFreeHeadCell=0x00CD996C;
static const DWORD kPoolBaseCell=0x00CD9970;
static const DWORD kLiveCountCell=0x00CD9974;
static const DWORD kPostGuard=0x0088AA85;
static const DWORD kPostGuardImm=0x0088AA8B;
static const DWORD kNativePoolInit=0x00404C2F;
static const DWORD kKnownPoolNeighbour=0x00404BF2;
static const DWORD kStockCap=0x400;
static const DWORD kNewCap=0x1000;
static const DWORD kStride=0x30;
static const BYTE kGuardStock[]={0x81,0x3D,0x74,0x99,0xCD,0x00,0x00,0x04,0x00,0x00};
static const BYTE kNeighbourBefore[]={0x0F,0xB7,0x01,0x66,0x3B,0x41,0x02,0x7C,0x06};
static volatile LONG g_started=0;
static volatile LONG g_active=0;
static char g_logPath[MAX_PATH]={0};

static void Log(const char* status,const char* detail) {
    if (!g_logPath[0]) return;
    FILE* f=NULL;
    if (fopen_s(&f,g_logPath,"ab") || !f) return;
    fprintf(f,"[%lu] %s: %s\n",(unsigned long)GetTickCount(),status,detail);
    fclose(f);
}
static bool Readable(uintptr_t addr,size_t bytes) {
    if (!addr || !bytes || addr>UINTPTR_MAX-bytes) return false;
    uintptr_t end=addr+bytes;
    while (addr<end) {
        MEMORY_BASIC_INFORMATION v={0};
        if (!VirtualQuery((LPCVOID)addr,&v,sizeof(v)) ||
            v.State!=MEM_COMMIT || (v.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
            !(v.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|
                         PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|
                         PAGE_EXECUTE_WRITECOPY))) return false;
        uintptr_t last=(uintptr_t)v.BaseAddress+v.RegionSize;
        if (last<=addr) return false;
        addr=last<end?last:end;
    }
    return true;
}
static bool Word(DWORD addr,DWORD& value) {
    if (!Readable(addr,4)) return false;
    __try { value=*(volatile const DWORD*)(uintptr_t)addr; return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool Same(DWORD addr,const BYTE* bytes,size_t n) {
    if (!Readable(addr,n)) return false;
    __try { return memcmp((const void*)(uintptr_t)addr,bytes,n)==0; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool Idle() {
    DWORD lvl=1,mgr=1;
    return Word(kLevelCell,lvl) && Word(kResourceMgrCell,mgr) &&
           lvl==0 && mgr==0;
}
static bool StockImage() {
    return (DWORD)(uintptr_t)GetModuleHandleA(NULL)==kExeBase &&
           Same(kPostGuard,kGuardStock,sizeof(kGuardStock)) &&
           Same(kKnownPoolNeighbour,kNeighbourBefore,sizeof(kNeighbourBefore)) &&
           Readable(kNativePoolInit,0x3D);
}
struct Header {
    DWORD freeHead;
    DWORD base;
};
static bool ValidFreeList(const Header& h,DWORD capacity) {
    if (capacity!=kStockCap && capacity!=kNewCap) return false;
    if (!h.base || !h.freeHead || (h.base&3) || (h.freeHead&3)) return false;
    const DWORD bytes=capacity*kStride;
    if (h.base>0xFFFFFFFFu-bytes || !Readable(h.base,bytes)) return false;
    // Every slot must appear EXACTLY once, no false 4096-by-48-byte pointer
    // bounds and no cycles. A live count of 0 alone cannot prove this.
    BYTE visited[kNewCap]={0};
    DWORD at=h.freeHead;
    for (DWORD i=0;i<capacity;++i) {
        if (!at || at<h.base || at>=h.base+bytes) return false;
        DWORD off=at-h.base;
        if (off%kStride) return false;
        DWORD idx=off/kStride;
        if (visited[idx]) return false;
        visited[idx]=1;
        DWORD next=0;
        if (!Word(at,next)) return false;
        at=next;
    }
    return at==0;
}
static bool ReadHeader(Header& h,DWORD& count) {
    return Word(kFreeHeadCell,h.freeHead) &&
           Word(kPoolBaseCell,h.base) && Word(kLiveCountCell,count);
}
// Use retail's original __stdcall FUN_00404c2f, already used by the
// original static constructor at FUN_009555df with (0x30,0x400).
typedef void* (__stdcall *NativePoolInit)(void*,DWORD,DWORD);

static bool Install() {
    if (!Idle() || !StockImage()) {
        Log("REJECT","EXE instruction signature unknown, old event guard patched, or world already running");
        return false;
    }
    Header old={0,0};
    DWORD count=~0u;
    if (!ReadHeader(old,count) || count!=0 || !ValidFreeList(old,kStockCap)) {
        Log("REJECT","old event pool is not a fully free native 1024-slot linked arena");
        return false;
    }
    Header replacement={0,0};
    // No process-foreign allocator and no VirtualAlloc: the native helper
    // allocates with the same original heap that atexit's delete[] owns.
    bool invoked=false;
    __try {
        NativePoolInit init=(NativePoolInit)(uintptr_t)kNativePoolInit;
        invoked=init(&replacement,kStride,kNewCap)==(void*)&replacement;
    } __except(EXCEPTION_EXECUTE_HANDLER) { invoked=false; }
    if (!invoked || !ValidFreeList(replacement,kNewCap) ||
        replacement.base==old.base) {
        Log("REJECT","native 4096-slot PoolInit failed verification; game pool unchanged");
        // The game does not own the failed candidate; deliberately retain it
        // for process exit rather than call an unverified CRT deallocator.
        return false;
    }
    // The game has no level/resource manager and all 1024 original entries
    // are free. Prepare the single 4-byte CMP operand for a surgical write.
    DWORD prot=0,unused=0;
    if (!VirtualProtect((LPVOID)(uintptr_t)kPostGuardImm,4,
                        PAGE_EXECUTE_READWRITE,&prot)) {
        Log("REJECT","could not protect PostAnimEvent CMP operand; old pool intact");
        return false;
    }
    bool published=false;
    bool partial=false;
    __try {
        Header again={0,0};
        DWORD live=~0u;
        if (Idle() && StockImage() && ReadHeader(again,live) && live==0 &&
            again.base==old.base && again.freeHead==old.freeHead) {
            // No active callers are allowed at this pre-world point. Three
            // globals are NOT a generally atomic operation, so reject the
            // install once any level or event consumer exists.
            *(volatile DWORD*)(uintptr_t)kPoolBaseCell=replacement.base;
            partial=true;
            *(volatile DWORD*)(uintptr_t)kFreeHeadCell=replacement.freeHead;
            *(volatile DWORD*)(uintptr_t)kLiveCountCell=0;
            *(volatile DWORD*)(uintptr_t)kPostGuardImm=kNewCap;
            DWORD newGuard=0;
            published=Word(kPostGuardImm,newGuard) && newGuard==kNewCap &&
                      ReadHeader(again,live) && live==0 &&
                      again.base==replacement.base &&
                      again.freeHead==replacement.freeHead && Idle();
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { published=false; }
    if (!published && partial) {
        // Pre-level only, no event dispatch was allowed to begin. If the
        // state changed underneath us, don't risk writing an active pool:
        // mark FATAL and leave the candidate untouched for forensic logs.
        DWORD live=~0u;
        Header current={0,0};
        if (Idle() && ReadHeader(current,live) && live==0 &&
            current.base==replacement.base) {
            __try {
                *(volatile DWORD*)(uintptr_t)kPostGuardImm=kStockCap;
                *(volatile DWORD*)(uintptr_t)kFreeHeadCell=old.freeHead;
                *(volatile DWORD*)(uintptr_t)kPoolBaseCell=old.base;
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        } else {
            Log("FATAL","concurrent game state changed during event arena publication; do not continue test");
        }
    }
    FlushInstructionCache(GetCurrentProcess(),(LPCVOID)(uintptr_t)kPostGuard,sizeof(kGuardStock));
    const bool restored=!!VirtualProtect((LPVOID)(uintptr_t)kPostGuardImm,4,prot,&unused);
    if (!restored) {
        Log("FATAL","failed to restore event CMP page protection");
        return false;
    }
    if (!published) {
        Log("REJECT","native pool publication failed; 1024-slot stock pool should remain active");
        return false;
    }
    InterlockedExchange(&g_active,1);
    Log("ACTIVE","animation EVENT pool 1024 -> 4096 via original FUN_00404c2f; original atexit owner frees NEW arena; old 48 KiB quarantined");
    return true;
}
} // namespace

void OC_AnimationEventPool4096_Init() {
    if (InterlockedCompareExchange(&g_started,1,0)!=0) return;
    char exe[MAX_PATH]={0};
    DWORD n=GetModuleFileNameA(NULL,exe,MAX_PATH);
    if (!n || n>=MAX_PATH) return;
    char* slash=strrchr(exe,'\\');
    if (!slash) return;
    slash[1]='\0';
    char ini[MAX_PATH]={0};
    if (_snprintf_s(ini,sizeof(ini),_TRUNCATE,"%sproxy.ini",exe)<0 ||
        _snprintf_s(g_logPath,sizeof(g_logPath),_TRUNCATE,
                    "%sanimation_event_pool_4096.log",exe)<0) return;
    if (!GetPrivateProfileIntA("StaticMapPools","ExpandAnimationEventPool4096",0,ini))
        return;
    Install();
}
bool OC_AnimationEventPool4096_IsActive() {
    return InterlockedCompareExchange(&g_active,1,1)==1;
}
