// OpenConquest D3D9Proxy - optional retail heap extension.
//
// Source oracle: DLL/NpcProxy/src/npc_game.cpp, NpcGame_HeapUpkeep(),
// kGrowSigs / kGaugeSigs and PLAN.md section 12.2.
// Magellan: MgParseHelpers.cpp (0x67d790, 0x67d7d3, 0x67d8d7,
// 0x67b7f3, 0x67b19d). Preserve the game's bucket allocator and CRT owner.
//
// NOT a replacement for the fixed animation/geometry pools, not x64 memory.
// Defaults OFF. The original 101 MiB arena is NOT resized or freed here.
// These additional blocks must outlive this DLL if the game keeps pointers
// into them. Windows reclaims them when the game process exits.
//
// Do not enable alongside NpcProxy's heap extender. Unknown/foreign arenas
// fail closed even when the DLL name differs, and we recheck under lock.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include "retail_heap_extender.h"

#ifndef _M_IX86
#error Retail heap extender requires x86 MSVC (retail EAX lock convention)
#endif

namespace {
const DWORD kExeBase = 0x00400000;
const DWORD kHeapBase = 0x00CD8104;
const DWORD kHeapEnd  = 0x00CD8108;
const DWORD kHeapTop  = 0x00CD810C;
const DWORD kLock = 0x0067B7F3;
const DWORD kUnlock = 0x0067B19D;
const DWORD kInitialBytes = 0x06500000;       // 101 MiB, NOT 101.5625 MiB
const DWORD kGrowBytes = 64u * 1024u * 1024u;
const DWORD kThreshold = 16u * 1024u * 1024u;
const int kMaxBlocks = 8;

struct Sig { DWORD va; BYTE size; BYTE bytes[16]; };
// Exact retail opcode fingerprints copied from the NpcProxy oracle.
const Sig kGrowSig[] = {
  {0x0067D790,13,{0x56,0xBE,0x00,0x00,0x50,0x06,0x56,0xE8,0xB1,0x12,0xF9,0xFF,0x59}},
  {0x0067D58B,16,{0xFF,0x35,0x04,0x81,0xCD,0x00,0xE8,0x7A,0x15,0xF9,0xFF,0x83,0x25,0x04,0x81,0xCD}},
  {0x0067D8F0,13,{0xF6,0x86,0xB8,0x01,0x00,0x00,0x04,0x0F,0x85,0x1E,0x01,0x00,0x00}},
  {0x0067D95B,14,{0x57,0xE8,0x72,0xFE,0xFF,0xFF,0x3B,0xC3,0x59,0x89,0x45,0xF4,0x75,0x46}},
  {0x0067B7F3,15,{0x8D,0x04,0x85,0x48,0x6D,0xCD,0x00,0x50,0xE8,0xA4,0xFC,0xFF,0xFF,0x59,0xC3}},
  {0x0067B19D,15,{0x51,0x8D,0x04,0x85,0x48,0x6D,0xCD,0x00,0xE8,0x6D,0xFF,0xFF,0xFF,0x59,0xC3}},
  {0x0067E63B,16,{0x6A,0x02,0x58,0xE8,0xB0,0xD1,0xFF,0xFF,0x6A,0x00,0xFF,0x74,0x24,0x10,0x6A,0x00}}
};
const Sig kGaugeSig[] = {
  {0x0067D7D3,16,{0x83,0x3D,0x04,0x81,0xCD,0x00,0x00,0x75,0x05,0xE8,0xAF,0xFF,0xFF,0xFF,0x8B,0x0D}},
  {0x0067D7E3,4,{0x0C,0x81,0xCD,0x00}},
  {0x0067D7ED,6,{0x3B,0x0D,0x08,0x81,0xCD,0x00}}
};

static LONG g_init;
static volatile LONG g_busy;
static volatile LONG g_state;    // 0 disabled, 1 armed, 2 foreign, 3 fault, 4 max blocks
static HANDLE g_worker;
static volatile LONG g_workerEnabled;
static DWORD g_originalBase;
static void* g_chunks[kMaxBlocks];
static int g_chunkCount;
static DWORD g_bytesUsedBefore;
static char g_logPath[MAX_PATH];

void Log(const char* fmt, ...) {
    if (!g_logPath[0]) return;
    FILE* f = fopen(g_logPath, "ab");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02u:%02u:%02u] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap,fmt); vfprintf(f,fmt,ap); va_end(ap);
    fputc('\n',f);
    fclose(f);
}

bool Readable(DWORD addr, size_t len) {
    if (!len || addr > 0xFFFFFFFFu - (DWORD)len) return false;
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery((LPCVOID)(uintptr_t)addr, &m, sizeof(m))) return false;
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS|PAGE_GUARD))) return false;
    uintptr_t end=(uintptr_t)m.BaseAddress + m.RegionSize;
    return (uintptr_t)addr + len <= end;
}

bool CheckSignatures() {
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != kExeBase) return false;
    const Sig* groups[2] = {kGrowSig,kGaugeSig};
    const size_t counts[2] = {sizeof(kGrowSig)/sizeof(kGrowSig[0]),
                              sizeof(kGaugeSig)/sizeof(kGaugeSig[0])};
    for (int g=0;g<2;++g) for(size_t i=0;i<counts[g];++i) {
        const Sig& s=groups[g][i];
        if (!Readable(s.va,s.size) || memcmp((const void*)(uintptr_t)s.va,s.bytes,s.size)!=0)
            return false;
    }
    return Readable(kHeapBase,sizeof(DWORD)*3);
}

bool ReadArena(DWORD& base, DWORD& top, DWORD& end) {
    __try {
        base=*(volatile DWORD*)(uintptr_t)kHeapBase;
        end =*(volatile DWORD*)(uintptr_t)kHeapEnd;
        top =*(volatile DWORD*)(uintptr_t)kHeapTop;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool IsOurArena(DWORD base,DWORD top,DWORD end) {
    if (!base || base != g_originalBase || end < top) return false;
    if (g_chunkCount==0) {
        if (base > 0xFFFFFFFFu-kInitialBytes) return false;
        return end==base+kInitialBytes && top>=base && top<=end;
    }
    const DWORD chunk=(DWORD)(uintptr_t)g_chunks[g_chunkCount-1];
    return end==chunk+kGrowBytes && top>=chunk && top<=end;
}

void EngineLock(bool take) {
    const DWORD fn = take ? kLock : kUnlock;
    DWORD savedEsp;
    __asm {
        pushad
        mov savedEsp, esp
        mov eax, 2
        call fn
        mov esp, savedEsp
        popad
    }
}

void Deactivate(LONG reason,const char* why) {
    InterlockedExchange(&g_state,reason);
    // Stop the poller on a permanent fault or a foreign heap owner.
    if (reason != 1) InterlockedExchange(&g_workerEnabled,0);
    Log("DISARMED: %s",why);
}

void TryGrow() {
    DWORD base=0,top=0,end=0;
    if (!ReadArena(base,top,end)) { Deactivate(3,"unreadable arena globals"); return; }
    if (!base || !top || end < top) return; // engine not ready yet
    if (!g_originalBase) {
        if (end < base || end-base != kInitialBytes || top<base || top>end) {
            Deactivate(2,"arena already belongs to another extender or unknown layout");
            return;
        }
        g_originalBase=base;
    }
    if (!IsOurArena(base,top,end)) {
        Deactivate(2,"arena moved by a foreign extender; refusing to compete");
        return;
    }
    if (end-top >= kThreshold) return;
    if (g_chunkCount==kMaxBlocks) {
        Deactivate(4,"eight additional 64-MiB chunks reached");
        return;
    }

    // Reserve OUTSIDE the engine allocator lock, otherwise VirtualAlloc may
    // stall every game's allocation. Only publish after acquiring its lock.
    void* candidate=VirtualAlloc(NULL,kGrowBytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!candidate) { Deactivate(3,"VirtualAlloc(64 MiB) failed"); return; }
    bool locked=false,accepted=false,foreign=false,stillNeeded=false;
    __try {
        __try {
            EngineLock(true);
            locked=true;
            if (!ReadArena(base,top,end) || !IsOurArena(base,top,end)) {
                foreign=true; // never modify another owner's new block
            } else if (end-top < kThreshold) {
                stillNeeded=true;
                // Reject VA wrap and pointer publication beyond x86 space.
                const uintptr_t ptr=(uintptr_t)candidate;
                if (ptr <= 0xFFFFFFFFu-kGrowBytes) {
                    g_bytesUsedBefore += top - (g_chunkCount ?
                        (DWORD)(uintptr_t)g_chunks[g_chunkCount-1] : base);
                    *(volatile DWORD*)(uintptr_t)kHeapTop=(DWORD)ptr;
                    *(volatile DWORD*)(uintptr_t)kHeapEnd=(DWORD)(ptr+kGrowBytes);
                    g_chunks[g_chunkCount++]=candidate;
                    accepted=true;
                }
            }
        } __finally {
            if (locked) EngineLock(false);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Deactivate(3,"exception in engine heap lock or arena publication");
    }
    if (!accepted) {
        // The mod owns the candidate until publication. Do not free on the
        // accepted path: the game may retain pointers into every chunk.
        VirtualFree(candidate,0,MEM_RELEASE);
        if (foreign) Deactivate(2,"foreign heap owner won the lock; no competing writes");
        else if (stillNeeded) Deactivate(3,"candidate address does not fit x86 VA");
        return;
    }
    Log("EXTENDED: block=%d/8 bytes=%lu originalBase=%08lX newTop=%08lX accumulatedMB=%lu",
        g_chunkCount,kGrowBytes,g_originalBase,(DWORD)(uintptr_t)candidate,g_bytesUsedBefore>>20);
}
} // namespace

// Level loading is often renderless. EndScene alone therefore cannot keep
// the engine's arena supplied; the worker watches it without touching D3D.
// Pin this module while the worker executes so FreeLibrary cannot unload the
// code underneath it. This is a process-lifetime service, only when opted in.
DWORD WINAPI HeapWorker(LPVOID) {
    while (InterlockedCompareExchange(&g_workerEnabled,1,1)==1) {
        OC_HeapExt_Tick();
        Sleep(100);
    }
    return 0;
}

void OC_HeapExt_Init() {
    if (InterlockedCompareExchange(&g_init,1,0)!=0) return;
    char exe[MAX_PATH]={0};
    DWORD n=GetModuleFileNameA(NULL,exe,MAX_PATH);
    if (!n || n>=MAX_PATH) return;
    char* slash=strrchr(exe,'\\');
    if (!slash) return;
    slash[1]='\0';
    char ini[MAX_PATH];
    if (_snprintf_s(ini,sizeof(ini),_TRUNCATE,"%sproxy.ini",exe)<0) return;
    if (_snprintf_s(g_logPath,sizeof(g_logPath),_TRUNCATE,
                    "%sretail_heap_extender.log",exe)<0) g_logPath[0]='\0';
    if (!GetPrivateProfileIntA("RetailHeapExtender","Enabled",0,ini))
        return; // OFF BY DEFAULT, never touching the retail heap
    if (!CheckSignatures()) {
        Deactivate(3,"retail fingerprints mismatch (EXE variant, tracer, or existing patch)");
        return;
    }
    InterlockedExchange(&g_state,1);
    Log("ARMED: NpcProxy 64-MiB heap-growth contract; no .text changes; max=8");
    // Starting the worker from Direct3DCreate9 (not DllMain) avoids loader-lock
    // initialization hazards. Pin only after confirming the worker is needed.
    HMODULE self=NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR)(uintptr_t)&OC_HeapExt_Init, &self)) {
        Deactivate(3,"unable to pin the proxy before starting heap worker");
        return;
    }
    InterlockedExchange(&g_workerEnabled,1);
    g_worker=CreateThread(NULL,0,HeapWorker,NULL,0,NULL);
    if (!g_worker) {
        InterlockedExchange(&g_workerEnabled,0);
        Deactivate(3,"unable to start heap worker");
    } else {
        CloseHandle(g_worker); // OS thread keeps running; pin holds module
        g_worker=NULL;
    }
}

void OC_HeapExt_Tick() {
    if (InterlockedCompareExchange(&g_state,1,1)!=1) return;
    if (InterlockedCompareExchange(&g_busy,1,0)!=0) return;
    TryGrow();
    InterlockedExchange(&g_busy,0);
}
