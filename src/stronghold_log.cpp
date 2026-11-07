// Separate Stronghold-activity logger. Writes to RE\StrongholdRep.log.
// Instruments:
//   * mode bit changes at [0x00cd7fdc + 0x3f0]
//   * flag changes at [0x00cd7fdc + 0x44a] (MP/SP indicator)
//   * state transitions (CALL FUN_0073de27 trace)
//   * menu slot clicks reaching our caves
//   * level load events (PAK path at 0x00cd8310)
//
// Called from:
//   * Patches in memorypatch.cpp (cave entry points)
//   * a per-frame polling thread that watches globals and logs changes

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>

static FILE*            g_log = nullptr;
static CRITICAL_SECTION g_logCs;
static bool             g_csInited = false;
static HANDLE           g_watcher  = nullptr;
static volatile LONG    g_stopFlag = 0;

static void OpenLog() {
    if (g_log) return;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat_s(path, "StrongholdRep.log");
    fopen_s(&g_log, path, "w");
    if (g_log) {
        fprintf(g_log, "=== Stronghold Activity Report ===\n");
        fprintf(g_log, "Logger attached — tracking mode/state/flags/level changes.\n\n");
        fflush(g_log);
    }
}

extern "C" void SHLog(const char* fmt, ...) {
    if (!g_csInited) return;
    EnterCriticalSection(&g_logCs);
    if (!g_log) OpenLog();
    if (g_log) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_log, "[%02d:%02d:%02d.%03d] ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_logCs);
}

// SEH-safe read helpers — main thread wrapping game-memory probes.
static uint32_t SafeU32(uint32_t addr) {
    uint32_t v = 0;
    __try { v = *(uint32_t*)(uintptr_t)addr; }
    __except(EXCEPTION_EXECUTE_HANDLER) { v = 0xDEADBEEF; }
    return v;
}
static uint8_t SafeU8(uint32_t addr) {
    uint8_t v = 0;
    __try { v = *(uint8_t*)(uintptr_t)addr; }
    __except(EXCEPTION_EXECUTE_HANDLER) { v = 0xFF; }
    return v;
}
static void SafeStr(uint32_t addr, char* out, size_t cap) {
    out[0] = '\0';
    __try {
        for (size_t i = 0; i + 1 < cap; ++i) {
            uint8_t c = *(uint8_t*)(uintptr_t)(addr + i);
            out[i] = (char)c;
            if (c == 0) return;
        }
        out[cap - 1] = '\0';
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '?'; out[1] = '\0';
    }
}

// Dump a contiguous window of state memory as hex + ascii.
// `base` is the absolute address; `stateBase` is the state_ptr so we can
// print state-relative offsets in the label.
static void DumpRange(const char* tag, uint32_t stateBase, uint32_t startOff, size_t len) {
    char line[256];
    for (size_t off = 0; off < len; off += 16) {
        char* p = line;
        p += sprintf(p, "%s[state+0x%03X]", tag, (unsigned)(startOff + off));
        uint8_t bytes[16] = {0};
        for (int i = 0; i < 16 && off + i < len; ++i) {
            bytes[i] = SafeU8((uint32_t)(stateBase + startOff + off + i));
            p += sprintf(p, " %02X", bytes[i]);
        }
        *p++ = ' ';
        *p++ = ' ';
        for (int i = 0; i < 16 && off + i < len; ++i) {
            char c = (char)bytes[i];
            *p++ = (c >= 0x20 && c < 0x7f) ? c : '.';
        }
        *p = '\0';
        SHLog("%s", line);
    }
}

static DWORD WINAPI WatcherThread(LPVOID) {
    // Poll key globals every 250ms. Log when anything changes.
    uint32_t prevGameState  = 0;
    uint32_t prevMode       = 0xCAFEBABE;
    uint8_t  prevFlag44a    = 0xFF;
    uint8_t  prevFlag446    = 0xFF;
    uint8_t  prevFlag44e    = 0xFF;
    uint32_t prevStateSlot  = 0;
    char     prevPak[260]   = {0};
    char     prevBin[260]   = {0};

    while (!InterlockedCompareExchange(&g_stopFlag, 0, 0)) {
        uint32_t gs = SafeU32(0x00cd7fdc);
        if (gs && gs != prevGameState) {
            SHLog("[watcher] game_state pointer set: 0x%08X", gs);
            prevGameState = gs;
            // reset per-state-object caches so we log current values once more
            prevMode = 0xCAFEBABE;
            prevFlag44a = prevFlag446 = prevFlag44e = 0xFF;
            prevStateSlot = 0;
        }

        if (gs) {
            uint32_t mode = SafeU32(gs + 0x3f0);
            if (mode != prevMode) {
                const char* name = "?";
                switch (mode) {
                    case 0: name = "NONE"; break;
                    case 1: name = "InstantAction"; break;
                    case 2: name = "Stronghold"; break;
                    case 3: name = "StrongholdVariant"; break;
                }
                SHLog("[watcher] mode [0x3F0] = %u (%s)", mode, name);
                prevMode = mode;
                // On mode change, snapshot surrounding state so we can find
                // region-id / level-index fields.
                SHLog("[dump]   --- mode change snapshot ---");
                DumpRange("[dump] ", gs, 0x1A0, 0x40);   // state machine area [EBX+0x1a8 etc]
                DumpRange("[dump] ", gs, 0x1C0, 0x40);   // candidate level-index zone
                DumpRange("[dump] ", gs, 0x440, 0x30);   // include-flags + transition flags
            }
            uint8_t f44a = SafeU8(gs + 0x44a);
            if (f44a != prevFlag44a) {
                SHLog("[watcher] flag [0x44A] (SP/MP-indicator) = %u", (unsigned)f44a);
                prevFlag44a = f44a;
            }
            uint8_t f446 = SafeU8(gs + 0x446);
            if (f446 != prevFlag446) {
                SHLog("[watcher] flag [0x446] (state-transition route) = %u", (unsigned)f446);
                prevFlag446 = f446;
            }
            uint8_t f44e = SafeU8(gs + 0x44e);
            if (f44e != prevFlag44e) {
                SHLog("[watcher] flag [0x44E] (mode-string selector)   = %u", (unsigned)f44e);
                prevFlag44e = f44e;
            }
        }

        // UI manager state slot — [0x00cd800c + ...]. We log just the pointer.
        uint32_t uiManager = SafeU32(0x00cd800c);
        if (uiManager != prevStateSlot) {
            SHLog("[watcher] UI-manager [0x00CD800C] = 0x%08X", uiManager);
            prevStateSlot = uiManager;
        }

        // Level paths (PAK at 0x00cd8310, BIN at 0x00cd8414).
        char pak[260] = {0}, bin[260] = {0};
        SafeStr(0x00cd8310, pak, sizeof(pak));
        SafeStr(0x00cd8414, bin, sizeof(bin));
        if (strcmp(pak, prevPak) != 0) {
            SHLog("[watcher] PAK path = \"%s\"", pak);
            strcpy_s(prevPak, pak);
            // Snapshot state at every level transition — catches the moment
            // the sequencer decides the "next" level.
            if (gs) {
                SHLog("[dump]   --- pak change snapshot ---");
                DumpRange("[dump] ", gs, 0x1A0, 0x40);
                DumpRange("[dump] ", gs, 0x1C0, 0x40);
                DumpRange("[dump] ", gs, 0x440, 0x30);
            }
        }
        if (strcmp(bin, prevBin) != 0) {
            SHLog("[watcher] BIN path = \"%s\"", bin);
            strcpy_s(prevBin, bin);
        }

        Sleep(250);
    }
    return 0;
}

extern "C" void SHLog_Init() {
    if (g_csInited) return;
    InitializeCriticalSection(&g_logCs);
    g_csInited = true;
    OpenLog();
    SHLog("[init] Stronghold logger live");
    g_watcher = CreateThread(nullptr, 0, WatcherThread, nullptr, 0, nullptr);
}

extern "C" void SHLog_Shutdown() {
    InterlockedExchange(&g_stopFlag, 1);
    if (g_watcher) {
        WaitForSingleObject(g_watcher, 1000);
        CloseHandle(g_watcher);
        g_watcher = nullptr;
    }
    if (g_log) { fclose(g_log); g_log = nullptr; }
    if (g_csInited) {
        DeleteCriticalSection(&g_logCs);
        g_csInited = false;
    }
}
