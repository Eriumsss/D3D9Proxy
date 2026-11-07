// ═══════════════════════════════════════════════════════════════════════════
// RETAIL FUNCTION TRACER (2026-08-13)
//
// WHAT IT IS. Every retail function we have catalogued gets one byte of its
// entry point replaced with INT3. A vectored exception handler catches the
// trap, records who was entered / from where / with what, puts the original
// byte back and resumes. The game never knows.
//
// WHY A BREAKPOINT AND NOT A DETOUR. A 5-byte relative JMP has to relocate the
// instructions it overwrites and must not land inside a branch target. Getting
// that wrong on any one of 32,456 entry points is an instant crash, and there
// is no way to audit it at that scale. A single 0xCC needs no relocation, no
// trampoline, no length disassembler, and is an atomic byte store on x86 — so
// it is safe to plant into a running multithreaded game. The price is an
// exception per call, which is why COVERAGE mode (the default) disarms each
// function the first time it fires: after the first hit the cost is zero and
// what you are left with is a complete map of which known functions actually
// executed, in order, with their callers resolved.
//
// THE THREE MODES
//   coverage  one-shot. 32k traps worst case, each paid once. Free afterwards.
//             This is the "what code does this level/menu/spawn touch" tool.
//   count     re-arms via the single-step trap, so you get call counts and the
//             full call sequence. Anything hotter than MaxHitsPerSec is
//             auto-disarmed and marked HOT so the game stays playable.
//   detail    count + ECX (this) + the first N stack dwords + return address.
//
// WHAT NEVER HAPPENS INSIDE THE HANDLER. No allocation, no CRT, no file I/O,
// no locks. The handler writes a fixed record into a lock-free ring and
// returns; the worker thread formats and writes it. A trap can land anywhere,
// including inside malloc or inside the CRT's own locks, so anything else
// would deadlock the game rather than trace it.
//
// SAFETY POSTURE. Disarmed unless retail_trace.ini says Enabled=1, and a
// missing ini is written with the defaults rather than assumed. The image is
// identity-checked (base, size, .text bounds) before a single byte is touched,
// and the tracer names which oracle build it attached to — ConquestEA.exe or
// the patched ConquestLLC.exe — because the two differ in 86 bytes and a
// finding is worthless without knowing which one produced it.
//
// SEE ALSO  tools/build_retail_symbols.py  (bakes retail_symbols.bin)
//           d3d9device.cpp                 (the F11 D3D-side frame tracer)
// ═══════════════════════════════════════════════════════════════════════════
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "retail_fn_trace.h"

#ifndef _M_IX86
#error "The retail function tracer is x86-only: it patches a 32-bit 2009 image \
and reads Eip/Ecx/Esp straight out of CONTEXT."
#endif

namespace {

// ── the baked symbol table, exactly as build_retail_symbols.py writes it ────
#pragma pack(push, 1)
struct RtfHeader {
    char     magic[4];      // "RTSY"
    DWORD    version;       // 1
    DWORD    count;
    DWORD    imageBase;     // 0x00400000
    DWORD    textLo;        // 0x00401000
    DWORD    textHi;        // 0x00970000
    DWORD    subsysCount;
    DWORD    subsysOff;
    DWORD    blobOff;
    DWORD    blobSize;
    DWORD    entryOff;
};
struct RtfSym {
    DWORD    va;
    DWORD    nameOff;       // into the name blob
    DWORD    noteOff;       // into the name blob, 0 = none
    WORD     subsys;        // index into the string table
    WORD     lane;          // ...ditto: the port file, e.g. MgTonemapRuntime
    WORD     topics;        // T_* bits
    BYTE     flags;
    BYTE     byteEA;        // first byte as it stands in ConquestEA.exe
    BYTE     byteLLC;       // ...and in ConquestLLC.exe
    BYTE     reserved;
};
#pragma pack(pop)

enum {
    F_PORTED   = 0x01,      // we have a 1:1 port with a semantic name
    F_ANALYZED = 0x02,      // corpus analyzed/<Subsystem>/
    F_IMPL     = 0x04,      // corpus implemented/<Subsystem>/
    F_REALNAME = 0x08,      // the binary kept a real (mangled) symbol
    F_THUNK    = 0x10,      // thunk_FUN_* adjustor
    F_SUSPECT  = 0x20,      // first byte cannot open a function: likely a
                            // mis-split entry point, off by a byte
    F_UNSAFE   = 0x40       // ⛔ part of exception dispatch — NEVER armed,
                            // no ini key can override it. See ArmOne().
};

// Stories that cut across port files. `Topics=shader` arms the bin reader, the
// CRC lookups, the creation and the per-material bind — the whole path by
// which a shader ends up on the device — without naming a single address.
enum {
    T_TEARDOWN = 0x001, T_HDR    = 0x002, T_EXPOSURE = 0x004, T_DRAIN = 0x008,
    T_SHADER   = 0x010, T_SKY    = 0x020, T_CLOUD    = 0x040, T_FRAME = 0x080,
    T_SPRAY    = 0x100, T_SHADOW = 0x200, T_CONDUCT  = 0x400
};
const struct { const char* name; WORD bit; } kTopics[] = {
    { "teardown", T_TEARDOWN }, { "hdr",     T_HDR     },
    { "exposure", T_EXPOSURE }, { "drain",   T_DRAIN   },
    { "shader",   T_SHADER   }, { "sky",     T_SKY     },
    { "cloud",    T_CLOUD    }, { "frame",   T_FRAME   },
    { "spray",    T_SPRAY    }, { "shadow",  T_SHADOW  },
    { "conduct",  T_CONDUCT  },
};

// ── per-entry runtime state, kept out of the read-only table ───────────────
enum SlotState : BYTE {
    S_OFF = 0,     // not selected by the filter
    S_ARMED,       // 0xCC is planted
    S_HIT,         // coverage mode: fired once, disarmed for good
    S_STEP,        // count mode: momentarily un-patched, mid single-step
    S_HOT,         // exceeded MaxHitsPerSec, disarmed to keep the game alive
    S_SKIP         // entry byte was already 0xCC (padding / bad entry point)
};
struct RtfSlot {
    BYTE   orig;
    BYTE   state;
    WORD   notes;           // bit 0: the worker has already announced the
                            // hot-governor retirement for this function
    volatile LONG hits;
    volatile LONG secHits;
    DWORD  firstMs;
    DWORD  firstFrame;
    DWORD  firstSeq;        // global order of first appearance — this is what
                            // the story report sorts on
};

// ── one trapped call, as the handler records it ────────────────────────────
struct RtfHit {
    DWORD idx;          // index into the symbol table
    DWORD ret;          // return address = the caller
    DWORD tid;
    DWORD frame;
    DWORD ms;
    DWORD ecx;          // `this` for thiscall
    DWORD esp;          // ── the whole reason the log can be a TREE ──
    DWORD arg[4];       // [esp+4 ..] for cdecl/stdcall
    volatile DWORD stamp;   // written LAST; consumer waits on it
};

enum Mode { M_COVERAGE = 0, M_COUNT, M_DETAIL };

// ═══════════════════════════════════════════════════════════════════════════
// STATE
// ═══════════════════════════════════════════════════════════════════════════
const RtfHeader* g_hdr        = 0;
const RtfSym*    g_sym        = 0;
const char*      g_blob       = 0;
const char*      g_subsys[512] = {0};
DWORD            g_subsysN    = 0;
RtfSlot*         g_slot       = 0;
BYTE*            g_symFile    = 0;

RtfHit*          g_ring       = 0;
DWORD            g_ringSize   = 0;      // power of two
DWORD            g_ringMask   = 0;
volatile LONG    g_ringWrite  = 0;
DWORD            g_ringRead   = 0;
volatile LONG    g_dropped    = 0;

volatile LONG    g_secTotal   = 0;      // traps this second, all functions
volatile LONG    g_firstSeq   = 0;      // hands out first-appearance order
volatile LONG    g_armedCount = 0;
volatile LONG    g_isArmed    = 0;      // 0/1 master switch
PVOID            g_veh        = 0;
DWORD            g_tls        = TLS_OUT_OF_INDEXES;
HANDLE           g_worker     = 0;
volatile LONG    g_quit       = 0;
volatile LONG    g_inited     = 0;
volatile unsigned long* g_hostFrame = 0;
DWORD            g_t0         = 0;
bool             g_oracleIsEA = false;  // set by VerifyImage; picks which baked
                                        // entry byte the interlock compares to

FILE*            g_log        = 0;
char             g_dir[MAX_PATH]   = {0};
char             g_iniPath[MAX_PATH] = {0};

// ── configuration (retail_trace.ini) ───────────────────────────────────────
struct Cfg {
    int   enabled;
    int   mode;
    int   armOnLoad;
    char  set[32];              // ported | named | bucketed | all
    char  subsystems[512];      // comma list, empty = any
    char  lanes[512];           // comma list of port files, e.g. MgTonemapRuntime
    char  topics[256];          // comma list of story names, e.g. shader,sky
    char  anchors[1024];        // VAs that print a banner when they run
    int   tree;                 // reconstruct call depth from ESP
    char  include[2048];        // comma list of 0x… VAs, forced on
    char  exclude[2048];        // comma list of 0x… VAs, forced off
    int   skipThunks;
    int   armSuspect;
    int   armCRT;
    int   armDelayMs;
    int   maxTotalHitsPerSec;
    int   armLimit;
    int   maxHitsPerSec;
    int   logArgs;
    int   ringSize;
    int   flushMs;
    int   coverageEverySec;
    char  logFile[64];
    char  coverageFile[64];
    int   keyToggle, keyDump, keyPanic;
} g_cfg;

// ═══════════════════════════════════════════════════════════════════════════
// PATHS + LOGGING (worker thread only — never call these from the handler)
// ═══════════════════════════════════════════════════════════════════════════
void RtfDir() {
    if (g_dir[0]) return;
    HMODULE hm = 0;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&RtfDir, &hm);
    GetModuleFileNameA(hm, g_dir, MAX_PATH);
    char* sl = strrchr(g_dir, '\\');
    if (sl) *(sl + 1) = 0; else g_dir[0] = 0;
}
void RtfPath(const char* leaf, char* out, size_t cap) {
    RtfDir();
    _snprintf(out, cap, "%s%s", g_dir, leaf);
    out[cap - 1] = 0;
}
void Say(const char* fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
}

// ═══════════════════════════════════════════════════════════════════════════
// CONFIG
// ═══════════════════════════════════════════════════════════════════════════
// A missing ini is WRITTEN, not assumed. Dropping this DLL into a game folder
// must never start planting breakpoints on its own — the operator opts in.
const char* kDefaultIni =
"; ══════════════════════════════════════════════════════════════════════════\r\n"
"; retail_trace.ini — RETAIL FUNCTION TRACER\r\n"
";\r\n"
"; Traps catalogued retail functions with INT3 and logs every entry, resolved\r\n"
"; through retail_symbols.bin (32,456 entry points; 1,680 of them carrying the\r\n"
"; semantic name a 1:1 port earned, plus the file:line we ported them at).\r\n"
";\r\n"
"; Nothing is armed while Enabled=0. This file was created with the safe\r\n"
"; defaults on first run.\r\n"
"; ══════════════════════════════════════════════════════════════════════════\r\n"
"[trace]\r\n"
"Enabled=0\r\n"
"\r\n"
"; coverage = one-shot per function (cheap, complete \"what ran\" map)\r\n"
"; count    = re-arm after every hit: call counts + full call order\r\n"
"; detail   = count, plus ECX(this), the return address and stack args\r\n"
"Mode=coverage\r\n"
"\r\n"
"; ported   = only the 1,680 functions we have decoded and named  (RECOMMENDED)\r\n"
"; named    = ported + the 4,724 that kept a real symbol in the binary\r\n"
"; bucketed = named + everything the corpus filed under a subsystem (~19k)\r\n"
"; all      = every catalogued entry point (32,456)\r\n"
"Set=ported\r\n"
"\r\n"
"; Comma-separated subsystem filter, e.g. Animation,EntitySystem,LevelLoader\r\n"
"; (empty = no subsystem filter). Names come from the corpus foldering.\r\n"
"Subsystems=\r\n"
"\r\n"
"; ── STORY MODE ───────────────────────────────────────────────────────────\r\n"
"; Lanes and Topics SELECT instead of narrowing: if either is set, Set= and\r\n"
"; Subsystems= are ignored and you get exactly that story, whole.\r\n"
";\r\n"
"; A LANE is the file we ported the function in, so it names a subsystem with\r\n"
"; no address list to maintain:\r\n"
";   MgTonemapRuntime  the HDR/exposure chain  (AdaptClear BlurLevel\r\n"
";                     BrightPass LumAdapt Run BuildQuad BuildMaterials)\r\n"
";   MgProgramRegistry how the shader bin is read and shaders get bound\r\n"
";                     (ShaderBinName FillFromBin StageBytecodes CreateShaders\r\n"
";                      RegistryPopulate FindPSByCrc FindVSByCrc\r\n"
";                      MaterialCurrentVS/PS RegistryTeardown)\r\n"
";   MgAtmosphere      dome, clouds, scattering  (DomeInit DomeUpdate\r\n"
";                     DomeSceneOpen CloudBuild CloudBindTextures\r\n"
";                     CloudScrollAdvance ScatterPrecompute AtmosphereEval)\r\n"
";   MgDrainPass MgFrameSeed MgFrameConduct MgGameBuffers MgRenderScene\r\n"
";   MgMaterialApply MgOcclusion MgWaterRuntime MgSprayRuntime ...\r\n"
"; Full list: the `lane` column of retail_symbols.txt\r\n"
"Lanes=\r\n"
"\r\n"
"; A TOPIC cuts ACROSS lanes, because a story does not respect file bounds:\r\n"
";   teardown  every shutdown/destroy/free/release/reset path   (206 fns)\r\n"
";   hdr       tonemap, bloom, bright pass, blur                   (9)\r\n"
";   exposure  luminance reduction + eye adaptation metering       (8)\r\n"
";   drain     the drain pass, its buckets, sort and scratch      (24)\r\n"
";   shader    bin read -> CRC lookup -> create -> stage -> bind  (28)\r\n"
";   sky       dome, atmosphere, scattering, haze                 (53)\r\n"
";   cloud     cloud build, textures, scroll                      (13)\r\n"
";   frame     frame begin/seed/conduct/clock/present             (72)\r\n"
"Topics=\r\n"
"\r\n"
"; Print a banner line when these run, so a frame's phases are findable by eye.\r\n"
"; e.g. Anchors=0x00689578,0x0070eb12  (tonemap Run, dome Update)\r\n"
"Anchors=\r\n"
"\r\n"
"; Reconstruct call depth from ESP and indent the log into a call tree. This is\r\n"
"; what turns a list of names into a readable order. Costs nothing.\r\n"
"Tree=1\r\n"
"\r\n"
"; Always/never arm these, whatever Set and Subsystems say. Comma-separated\r\n"
"; VAs, e.g. Include=0x0068e383,0x006a578e\r\n"
"Include=\r\n"
"Exclude=\r\n"
"\r\n"
"ArmOnLoad=1          ; arm as soon as the game starts (catches boot + load)\r\n"
"SkipThunks=1         ; skip the 1,872 thunk_FUN_* adjustors\r\n"
"ArmSuspect=0         ; arm entry points whose first byte cannot open a\r\n"
"                     ; function (probably off by one byte — these are the\r\n"
"                     ; only ones that can crash the game)\r\n"
"ArmLimit=0           ; 0 = no cap on how many functions get armed\r\n"
"MaxHitsPerSec=2000   ; count/detail: disarm anything hotter than this\r\n"
"LogArgs=4            ; detail: how many stack dwords to print (0-4)\r\n"
"RingSize=65536       ; hit records buffered between worker flushes\r\n"
"FlushMs=100          ; how often the worker drains the ring to the log\r\n"
"CoverageEverySec=30  ; auto-write the coverage table (0 = only on demand)\r\n"
"\r\n"
"LogFile=retail_calls.log\r\n"
"CoverageFile=retail_coverage.csv\r\n"
"\r\n"
"; Hotkeys, held with CTRL+ALT. Virtual-key codes: T=0x54 D=0x44 X=0x58\r\n"
"KeyToggle=0x54       ; arm / disarm\r\n"
"KeyDump=0x44         ; write the coverage table now\r\n"
"KeyPanic=0x58        ; disarm everything and restore every byte\r\n";

int IniInt(const char* key, int def) {
    return (int)GetPrivateProfileIntA("trace", key, def, g_iniPath);
}
void IniStr(const char* key, const char* def, char* out, DWORD cap) {
    GetPrivateProfileStringA("trace", key, def, out, cap, g_iniPath);
}

void LoadConfig() {
    RtfPath("retail_trace.ini", g_iniPath, MAX_PATH);
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = fopen(g_iniPath, "wb");
        if (f) { fwrite(kDefaultIni, 1, strlen(kDefaultIni), f); fclose(f); }
    }
    g_cfg.enabled       = IniInt("Enabled", 0);
    g_cfg.armOnLoad     = IniInt("ArmOnLoad", 1);
    g_cfg.skipThunks    = IniInt("SkipThunks", 1);
    g_cfg.armSuspect    = IniInt("ArmSuspect", 0);
    g_cfg.armCRT        = IniInt("ArmCRT", 0);
    g_cfg.armDelayMs    = IniInt("ArmDelayMs", 0);
    g_cfg.maxTotalHitsPerSec = IniInt("MaxTotalHitsPerSec", 60000);
    g_cfg.armLimit      = IniInt("ArmLimit", 0);
    g_cfg.maxHitsPerSec = IniInt("MaxHitsPerSec", 2000);
    g_cfg.logArgs       = IniInt("LogArgs", 4);
    g_cfg.ringSize      = IniInt("RingSize", 65536);
    g_cfg.flushMs       = IniInt("FlushMs", 100);
    g_cfg.coverageEverySec = IniInt("CoverageEverySec", 30);
    g_cfg.keyToggle     = IniInt("KeyToggle", 0x54);
    g_cfg.keyDump       = IniInt("KeyDump", 0x44);
    g_cfg.keyPanic      = IniInt("KeyPanic", 0x58);

    char mode[32];
    IniStr("Mode", "coverage", mode, sizeof(mode));
    g_cfg.mode = !_stricmp(mode, "count")  ? M_COUNT
               : !_stricmp(mode, "detail") ? M_DETAIL
                                           : M_COVERAGE;

    g_cfg.tree = IniInt("Tree", 1);
    IniStr("Set",         "ported", g_cfg.set,        sizeof(g_cfg.set));
    IniStr("Subsystems",  "",       g_cfg.subsystems, sizeof(g_cfg.subsystems));
    IniStr("Lanes",       "",       g_cfg.lanes,      sizeof(g_cfg.lanes));
    IniStr("Topics",      "",       g_cfg.topics,     sizeof(g_cfg.topics));
    IniStr("Anchors",     "",       g_cfg.anchors,    sizeof(g_cfg.anchors));
    IniStr("Include",     "",       g_cfg.include,    sizeof(g_cfg.include));
    IniStr("Exclude",     "",       g_cfg.exclude,    sizeof(g_cfg.exclude));
    IniStr("LogFile",     "retail_calls.log",    g_cfg.logFile,      sizeof(g_cfg.logFile));
    IniStr("CoverageFile","retail_coverage.csv", g_cfg.coverageFile, sizeof(g_cfg.coverageFile));

    if (g_cfg.logArgs  < 0) g_cfg.logArgs = 0;
    if (g_cfg.logArgs  > 4) g_cfg.logArgs = 4;
    if (g_cfg.flushMs  < 10) g_cfg.flushMs = 10;
    // Ring size must be a power of two — the handler masks instead of dividing.
    DWORD rs = 1024;
    while (rs < (DWORD)g_cfg.ringSize && rs < (1u << 22)) rs <<= 1;
    g_ringSize = rs;
    g_ringMask = rs - 1;
}

// Is `va` present in a comma-separated list of hex VAs?
bool InVaList(const char* list, DWORD va) {
    if (!list || !*list) return false;
    const char* p = list;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        if (!*p) break;
        DWORD v = (DWORD)strtoul(p, (char**)&p, 0);
        if (v == va) return true;
        while (*p && *p != ',') ++p;
    }
    return false;
}
bool InCsvList(const char* list, const char* item) {
    if (!list || !*list) return false;
    size_t n = strlen(item);
    if (!n) return false;
    const char* p = list;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        const char* s = p;
        while (*p && *p != ',') ++p;
        const char* e = p;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) --e;
        if ((size_t)(e - s) == n && !_strnicmp(s, item, n)) return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
// SYMBOL TABLE
// ═══════════════════════════════════════════════════════════════════════════
const char* SymName(DWORD i) { return g_blob + g_sym[i].nameOff; }
const char* SymNote(DWORD i) { return g_sym[i].noteOff ? g_blob + g_sym[i].noteOff : ""; }
const char* SymStr(WORD s) {
    return (s < g_subsysN && g_subsys[s]) ? g_subsys[s] : "";
}
const char* SymSubsys(DWORD i) { return SymStr(g_sym[i].subsys); }
const char* SymLane(DWORD i)   { return SymStr(g_sym[i].lane); }

// Which topic bits does `list` name? Unknown words are reported, not ignored —
// a typo in Topics= would otherwise silently arm nothing.
WORD ParseTopics(const char* list, char* unknownOut, size_t cap) {
    WORD bits = 0;
    if (unknownOut && cap) unknownOut[0] = 0;
    const char* p = list;
    while (p && *p) {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        const char* s = p;
        while (*p && *p != ',') ++p;
        const char* e = p;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) --e;
        size_t n = (size_t)(e - s);
        if (!n) continue;
        bool hit = false;
        for (size_t t = 0; t < sizeof(kTopics) / sizeof(kTopics[0]); ++t) {
            if (strlen(kTopics[t].name) == n && !_strnicmp(s, kTopics[t].name, n)) {
                bits |= kTopics[t].bit; hit = true; break;
            }
        }
        if (!hit && unknownOut && cap) {
            size_t have = strlen(unknownOut);
            if (have + n + 2 < cap) {
                if (have) unknownOut[have++] = ' ';
                memcpy(unknownOut + have, s, n);
                unknownOut[have + n] = 0;
            }
        }
    }
    return bits;
}

// Exact entry-point lookup. Called from the exception handler, so it must be
// branch-only: no locks, no allocation, no CRT.
int Lookup(DWORD va) {
    if (!g_sym) return -1;
    int lo = 0, hi = (int)g_hdr->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        DWORD m = g_sym[mid].va;
        if (m == va) return mid;
        if (m < va) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}
// Greatest entry <= va — turns a return address into "CALLER+0x..".
int LookupContaining(DWORD va) {
    if (!g_sym || va < g_hdr->textLo || va >= g_hdr->textHi) return -1;
    int lo = 0, hi = (int)g_hdr->count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (g_sym[mid].va <= va) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best;
}

bool LoadSymbols() {
    char path[MAX_PATH];
    RtfPath("retail_symbols.bin", path, MAX_PATH);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        Say("FATAL: retail_symbols.bin not found next to the DLL (%s)", path);
        Say("       build it with tools\\build_retail_symbols.py");
        return false;
    }
    DWORD size = GetFileSize(h, 0), got = 0;
    if (size < sizeof(RtfHeader) || size == INVALID_FILE_SIZE) {
        CloseHandle(h); Say("FATAL: retail_symbols.bin is truncated"); return false;
    }
    g_symFile = (BYTE*)VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_symFile) { CloseHandle(h); Say("FATAL: out of memory"); return false; }
    ReadFile(h, g_symFile, size, &got, 0);
    CloseHandle(h);
    if (got != size) { Say("FATAL: short read on retail_symbols.bin"); return false; }

    const RtfHeader* hd = (const RtfHeader*)g_symFile;
    if (memcmp(hd->magic, "RTSY", 4) || hd->version != 5) {
        Say("FATAL: retail_symbols.bin is v%u, this tracer needs v5 — rerun "
            "tools\\build_retail_symbols.py", hd->version);
        return false;
    }
    if (hd->entryOff + (DWORD)sizeof(RtfSym) * hd->count > size ||
        hd->blobOff + hd->blobSize > size || hd->subsysOff >= size) {
        Say("FATAL: retail_symbols.bin is malformed"); return false;
    }
    g_hdr  = hd;
    g_sym  = (const RtfSym*)(g_symFile + hd->entryOff);
    g_blob = (const char*)(g_symFile + hd->blobOff);

    const char* p = (const char*)(g_symFile + hd->subsysOff);
    g_subsysN = *(const DWORD*)p; p += 4;
    if (g_subsysN > 512) g_subsysN = 512;
    for (DWORD i = 0; i < g_subsysN; ++i) { g_subsys[i] = p; p += strlen(p) + 1; }

    g_slot = (RtfSlot*)VirtualAlloc(0, sizeof(RtfSlot) * hd->count,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_slot) { Say("FATAL: out of memory for slot state"); return false; }

    Say("symbols  : %u entries, %u subsystems, %u KB",
        hd->count, g_subsysN, size / 1024);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// IMAGE IDENTITY
//
// Byte-patching the wrong image is the one failure mode with no recovery, so
// the module is checked against the table's own header before anything is
// armed. The EA/LLC discriminator is free while we are here: the two oracle
// builds differ in 86 bytes, and 0x00401aad is one of them — retail EA has
// `push 0 / call Sleep` there, the patched LLC has it NOPed out.
// ═══════════════════════════════════════════════════════════════════════════
bool VerifyImage() {
    HMODULE exe = GetModuleHandleA(0);
    DWORD base = (DWORD)(DWORD_PTR)exe;
    if (base != g_hdr->imageBase) {
        Say("REFUSING TO ARM: exe loaded at 0x%08X, table was built for 0x%08X",
            base, g_hdr->imageBase);
        return false;
    }
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)exe;
    IMAGE_NT_HEADERS* nt   = (IMAGE_NT_HEADERS*)((BYTE*)exe + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE) {
        Say("REFUSING TO ARM: main module is not a PE image"); return false;
    }
    DWORD textEnd = base + nt->OptionalHeader.BaseOfCode +
                           nt->OptionalHeader.SizeOfCode;
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(exe, exePath, MAX_PATH);
    Say("image    : %s", exePath);
    Say("           base=0x%08X  code=0x%08X..0x%08X  timestamp=0x%08X",
        base, base + nt->OptionalHeader.BaseOfCode, textEnd,
        nt->FileHeader.TimeDateStamp);

    if (base + nt->OptionalHeader.BaseOfCode > g_hdr->textLo ||
        textEnd + 0x1000 < g_hdr->textHi) {
        Say("REFUSING TO ARM: .text 0x%08X..0x%08X does not cover the table's "
            "0x%08X..0x%08X — wrong executable",
            base + nt->OptionalHeader.BaseOfCode, textEnd,
            g_hdr->textLo, g_hdr->textHi);
        return false;
    }

    BYTE b = *(BYTE*)0x00401aad;
    g_oracleIsEA = (b == 0x6A);
    const char* oracle = (b == 0x6A) ? "ConquestEA.exe (clean 2009 retail)"
                       : (b == 0x90) ? "ConquestLLC.exe (community-patched)"
                                     : "UNRECOGNISED build";
    Say("oracle   : %s   [0x00401aad]=0x%02X", oracle, b);
    if (b != 0x6A && b != 0x90) {
        Say("REFUSING TO ARM: this is neither oracle build — the entry-point "
            "table cannot be trusted against it");
        return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// ARM / DISARM
// ═══════════════════════════════════════════════════════════════════════════
DWORD g_oldProtect = 0;
bool  g_textWritable = false;
BYTE  g_wantTopics = 0;         // parsed once from Topics=

bool MakeTextWritable() {
    if (g_textWritable) return true;
    SIZE_T len = g_hdr->textHi - g_hdr->textLo;
    if (!VirtualProtect((LPVOID)g_hdr->textLo, len, PAGE_EXECUTE_READWRITE,
                        &g_oldProtect)) {
        Say("FATAL: VirtualProtect on .text failed (%u)", GetLastError());
        return false;
    }
    g_textWritable = true;
    return true;
}

bool Selected(DWORD i) {
    const RtfSym& s = g_sym[i];

    // ⛔ THE ONE RULE WITH NO OVERRIDE. A breakpoint tracer runs inside
    // exception dispatch; instrumenting the code that builds and tears down
    // exception frames makes a trap fire while the thread is halfway through
    // registering the handler that would service it. The first live run armed
    // __SEH_prolog4 / __SEH_epilog4, trapped them 2,043 times, and the game
    // died. Include= does not reach past this.
    if (s.flags & F_UNSAFE) return false;

    if (InVaList(g_cfg.exclude, s.va)) return false;
    if (InVaList(g_cfg.include, s.va)) return true;

    // The CRT is 875 functions of memcpy / calloc / printf internals: enormous
    // call volume, no information about how the ENGINE runs. Off by default.
    if (!g_cfg.armCRT && !_stricmp(SymSubsys(i), "CRT")) return false;
    if (g_cfg.skipThunks && (s.flags & F_THUNK)) return false;
    if (!g_cfg.armSuspect && (s.flags & F_SUSPECT)) return false;

    // ── STORY MODE ─────────────────────────────────────────────────────────
    // Lanes/Topics SELECT rather than narrow. Asking for `Topics=shader` means
    // "show me how a shader gets from the bin onto the device" — you want that
    // whole path, and dropping half of it because it fell outside Set= would
    // hide the middle of the story.
    if (g_wantTopics || g_cfg.lanes[0]) {
        if (g_wantTopics && (s.topics & g_wantTopics)) return true;
        if (g_cfg.lanes[0] && InCsvList(g_cfg.lanes, SymLane(i))) return true;
        return false;
    }

    bool set;
    if (!_stricmp(g_cfg.set, "all"))            set = true;
    else if (!_stricmp(g_cfg.set, "bucketed"))  set = (s.flags & (F_PORTED | F_REALNAME | F_IMPL | F_ANALYZED)) != 0;
    else if (!_stricmp(g_cfg.set, "named"))     set = (s.flags & (F_PORTED | F_REALNAME)) != 0;
    else                                        set = (s.flags & F_PORTED) != 0;
    if (!set) return false;

    if (g_cfg.subsystems[0] && !InCsvList(g_cfg.subsystems, SymSubsys(i)))
        return false;
    return true;
}

// A manual arm starts a NEW capture window: everything that already fired is
// un-retired so it can be seen again. Without this, arming a second time in
// coverage mode gives an almost-empty trace — every interesting function fired
// once during boot and stayed retired, which is useless for "snapshot what
// happens when I walk into this level".
// Keep every closed window. retail_story.txt is always the latest, but each
// window is also frozen as retail_story_NN.txt -- otherwise opening a second
// capture silently destroys the first one you spent a playthrough getting.
DWORD g_window = 0;

void SnapshotWindow() {
    char src[MAX_PATH], dst[MAX_PATH], leaf[64];
    ++g_window;
    _snprintf(leaf, sizeof(leaf), "retail_story_%02u.txt", g_window);
    RtfPath("retail_story.txt", src, MAX_PATH); RtfPath(leaf, dst, MAX_PATH);
    CopyFileA(src, dst, FALSE);
    _snprintf(leaf, sizeof(leaf), "retail_coverage_%02u.csv", g_window);
    RtfPath(g_cfg.coverageFile, src, MAX_PATH); RtfPath(leaf, dst, MAX_PATH);
    CopyFileA(src, dst, FALSE);
    Say("-- window %u frozen as retail_story_%02u.txt / retail_coverage_%02u.csv --",
        g_window, g_window, g_window);
}

void RtfRearmFresh() {
    if (!g_slot || !g_hdr) return;
    DWORD woke = 0;
    for (DWORD i = 0; i < g_hdr->count; ++i) {
        RtfSlot& sl = g_slot[i];
        if (sl.state == S_HIT || sl.state == S_HOT) { sl.state = S_OFF; ++woke; }
        sl.hits = 0; sl.secHits = 0;
        sl.firstMs = 0; sl.firstFrame = 0; sl.firstSeq = 0; sl.notes = 0;
    }
    InterlockedExchange(&g_firstSeq, 0);
    Say("NEW WINDOW: %u previously-retired functions re-armed; counters cleared. "
        "Everything below this line is a fresh capture.", woke);
}

void RtfArmImpl() {
    if (InterlockedCompareExchange(&g_isArmed, 1, 0) != 0) return;
    if (!MakeTextWritable()) { InterlockedExchange(&g_isArmed, 0); return; }

    DWORD armed = 0, skipped = 0, filtered = 0, mismatch = 0, shown = 0;
    for (DWORD i = 0; i < g_hdr->count; ++i) {
        RtfSlot& sl = g_slot[i];
        if (sl.state == S_HIT || sl.state == S_HOT) continue;   // stay retired
        if (!Selected(i)) { sl.state = S_OFF; ++filtered; continue; }
        if (g_cfg.armLimit && (int)armed >= g_cfg.armLimit) { sl.state = S_OFF; continue; }

        DWORD va = g_sym[i].va;
        if (va < g_hdr->textLo || va >= g_hdr->textHi) { sl.state = S_OFF; continue; }
        BYTE orig = *(volatile BYTE*)va;
        if (orig == 0xCC) {          // alignment padding, or a bogus entry point
            sl.state = S_SKIP; ++skipped; continue;
        }
        // ── THE INTERLOCK ──────────────────────────────────────────────────
        // Live memory must still hold the byte the image on disk has. If it
        // does not, something already patched this function — memorypatch.cpp
        // detours ~20 of them with 5-byte JMPs — and planting 0xCC on top
        // would corrupt that hook and take the game with it. Refuse, and name
        // the first few so the collision is visible instead of mysterious.
        BYTE expect = g_oracleIsEA ? g_sym[i].byteEA : g_sym[i].byteLLC;
        if (orig != expect) {
            sl.state = S_SKIP;
            ++mismatch;
            if (shown < 12) {
                Say("  NOT ARMED  0x%08X %-44s live=%02X expected=%02X "
                    "(already hooked or patched)", va, SymName(i), orig, expect);
                ++shown;
            }
            continue;
        }
        sl.orig  = orig;
        sl.state = S_ARMED;
        *(volatile BYTE*)va = 0xCC;
        ++armed;
    }
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)g_hdr->textLo,
                          g_hdr->textHi - g_hdr->textLo);
    InterlockedExchange(&g_armedCount, (LONG)armed);

    const char* modeName = g_cfg.mode == M_COVERAGE ? "coverage"
                         : g_cfg.mode == M_COUNT    ? "count" : "detail";
    if (g_wantTopics || g_cfg.lanes[0])
        Say("ARMED    : %u functions  (mode=%s  topics=%s  lanes=%s)  [story mode]",
            armed, modeName, g_cfg.topics, g_cfg.lanes);
    else
        Say("ARMED    : %u functions  (mode=%s  set=%s%s%s)",
            armed, modeName, g_cfg.set,
            g_cfg.subsystems[0] ? "  subsystems=" : "", g_cfg.subsystems);
    Say("           filtered=%u  padding=%u  already-patched=%u  tree=%s",
        filtered, skipped, mismatch, g_cfg.tree ? "on" : "off");

    // Print the roster once. A story trace is only readable if you know up
    // front which functions COULD appear — silence then means "never ran",
    // which is a finding, instead of "maybe I never armed it".
    if ((g_wantTopics || g_cfg.lanes[0]) && armed && armed <= 300) {
        Say("           roster:");
        for (DWORD i = 0; i < g_hdr->count; ++i) {
            if (g_slot[i].state != S_ARMED) continue;
            Say("             0x%08X  %-46s [%s]",
                g_sym[i].va, SymName(i),
                SymLane(i)[0] ? SymLane(i) : SymSubsys(i));
        }
    }
    fflush(g_log);
}

void RtfDisarmImpl() {
    if (InterlockedCompareExchange(&g_isArmed, 0, 1) != 1) return;
    DWORD restored = 0;
    for (DWORD i = 0; i < g_hdr->count; ++i) {
        RtfSlot& sl = g_slot[i];
        if (sl.state != S_ARMED && sl.state != S_STEP) continue;
        DWORD va = g_sym[i].va;
        if (*(volatile BYTE*)va == 0xCC) { *(volatile BYTE*)va = sl.orig; ++restored; }
        sl.state = S_OFF;
    }
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)g_hdr->textLo,
                          g_hdr->textHi - g_hdr->textLo);
    InterlockedExchange(&g_armedCount, 0);
    Say("DISARMED : %u bytes restored", restored);
    fflush(g_log);
}

// ═══════════════════════════════════════════════════════════════════════════
// THE HANDLER
//
// Runs on whichever game thread hit the breakpoint. Everything here is plain
// loads/stores and one binary search — see the header banner for why.
// ═══════════════════════════════════════════════════════════════════════════
void RecordHit(DWORD idx, CONTEXT* ctx) {
    LONG seq = InterlockedIncrement(&g_ringWrite) - 1;
    RtfHit* r = &g_ring[(DWORD)seq & g_ringMask];
    r->stamp = 0;                      // claim the slot before filling it
    r->idx   = idx;
    r->tid   = GetCurrentThreadId();
    r->ms    = GetTickCount() - g_t0;
    r->frame = g_hostFrame ? *g_hostFrame : 0;
    r->ecx   = ctx->Ecx;
    r->esp   = ctx->Esp;
    const DWORD* sp = (const DWORD*)ctx->Esp;
    r->ret   = sp[0];                  // entry point: [esp] is the return address
    if (g_cfg.mode == M_DETAIL) {
        for (int a = 0; a < 4; ++a) r->arg[a] = sp[1 + a];
    } else {
        r->arg[0] = r->arg[1] = r->arg[2] = r->arg[3] = 0;
    }
    r->stamp = (DWORD)seq + 1;         // release: publish the record
}

// ═══════════════════════════════════════════════════════════════════════════
// FAULT REPORTER — make the next crash name itself
//
// The 08-13 run died with a 101 MB log whose last line was cut off mid-write,
// and no fault record anywhere: the log told us everything about what RAN and
// nothing about what BROKE. Our handler already sees every exception in the
// process before anybody else, so it is the cheapest possible place to answer
// "what faulted, where, and what had this thread just entered".
//
// This is the one place that does I/O from the handler, deliberately: the
// process is being torn down, so a buffered write would be lost. It uses raw
// WriteFile — never the CRT, whose lock the faulting thread may already hold.
// ═══════════════════════════════════════════════════════════════════════════
volatile LONG g_faultsLogged = 0;
DWORD g_tlsLast = TLS_OUT_OF_INDEXES;    // last function this thread entered

char* FHex(char* p, DWORD v, int digits) {
    for (int i = digits - 1; i >= 0; --i) {
        int nyb = (v >> (i * 4)) & 0xF;
        *p++ = (char)(nyb < 10 ? '0' + nyb : 'a' + nyb - 10);
    }
    return p;
}
char* FStr(char* p, const char* s, char* end) {
    while (s && *s && p < end - 1) *p++ = *s++;
    return p;
}

bool IsFatalLooking(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return true;
    default:
        return false;
    }
}

DWORD g_lastFaultCode = 0, g_lastFaultAddr = 0, g_lastFaultEsp = 0, g_firstFaultEsp = 0;
volatile LONG g_faultRepeat = 0;

void ReportFault(EXCEPTION_POINTERS* ep) {
    const DWORD thisCode = ep->ExceptionRecord->ExceptionCode;
    const DWORD thisAddr = (DWORD)(DWORD_PTR)ep->ExceptionRecord->ExceptionAddress;
    const DWORD thisEsp  = ep->ContextRecord->Esp;

    // The SAME fault repeating with a FALLING stack pointer is not 32 separate
    // problems, it is one: the handler for the fault is faulting again at the
    // same instruction, each round pushing another dispatch frame until the
    // stack runs out. Say that once, instead of printing it 32 times.
    if (thisCode == g_lastFaultCode && thisAddr == g_lastFaultAddr) {
        LONG n = InterlockedIncrement(&g_faultRepeat);
        if (n != 8) { g_lastFaultEsp = thisEsp; return; }
        char rbuf[512]; char* rp = rbuf; char* rend = rbuf + sizeof(rbuf);
        rp = FStr(rp, "   ^^ the SAME fault has now repeated ", rend);
        rp = FHex(rp, (DWORD)n + 1, 2);
        rp = FStr(rp, " times with ESP falling 0x", rend);
        rp = FHex(rp, g_firstFaultEsp, 8);
        rp = FStr(rp, " -> 0x", rend);
        rp = FHex(rp, thisEsp, 8);
        rp = FStr(rp, ".\r\n   ^^ THAT IS EXCEPTION RECURSION: the handler for this fault"
                      " faults again at the same\r\n"
                      "   ^^ instruction, so each round pushes another dispatch frame"
                      " until the stack dies.\r\n"
                      "   ^^ The ORIGINAL fault above is the one to fix; everything"
                      " after it is fallout.\r\n\r\n", rend);
        char rpath[MAX_PATH];
        RtfPath("retail_fault.txt", rpath, MAX_PATH);
        HANDLE rh = CreateFileA(rpath, FILE_APPEND_DATA, FILE_SHARE_READ, 0,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        if (rh != INVALID_HANDLE_VALUE) {
            DWORD w = 0; SetFilePointer(rh, 0, 0, FILE_END);
            WriteFile(rh, rbuf, (DWORD)(rp - rbuf), &w, 0); CloseHandle(rh);
        }
        return;
    }
    g_lastFaultCode = thisCode; g_lastFaultAddr = thisAddr;
    g_firstFaultEsp = g_lastFaultEsp = thisEsp;
    InterlockedExchange(&g_faultRepeat, 0);

    if (InterlockedIncrement(&g_faultsLogged) > 32) return;

    char buf[1024];
    char* p = buf;
    char* end = buf + sizeof(buf);
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const DWORD addr = (DWORD)(DWORD_PTR)ep->ExceptionRecord->ExceptionAddress;

    p = FStr(p, "FAULT code=0x", end);       p = FHex(p, code, 8);
    p = FStr(p, " at 0x", end);              p = FHex(p, addr, 8);
    p = FStr(p, " tid=", end);               p = FHex(p, GetCurrentThreadId(), 4);
    p = FStr(p, " eip=0x", end);             p = FHex(p, ep->ContextRecord->Eip, 8);
    p = FStr(p, " esp=0x", end);             p = FHex(p, ep->ContextRecord->Esp, 8);

    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        p = FStr(p, (ep->ExceptionRecord->ExceptionInformation[0] ? " WRITE to 0x"
                                                                 : " READ from 0x"), end);
        p = FHex(p, (DWORD)ep->ExceptionRecord->ExceptionInformation[1], 8);
    }

    int fi = LookupContaining(addr);
    if (fi >= 0) {
        p = FStr(p, "\r\n   in  ", end);
        p = FStr(p, SymName((DWORD)fi), end);
        p = FStr(p, "+0x", end);
        p = FHex(p, addr - g_sym[fi].va, 4);
        const char* note = SymNote((DWORD)fi);
        if (note[0]) { p = FStr(p, "  (", end); p = FStr(p, note, end);
                       p = FStr(p, ")", end); }
    } else {
        // Outside the game's .text — so name the MODULE instead. "ntdll+0x…"
        // is the difference between a diagnosis and a hex number: a fault in
        // the heap routines means corruption, one in the exception dispatcher
        // means the handler itself is faulting.
        char modName[MAX_PATH];
        HMODULE mod = 0;
        p = FStr(p, "\r\n   in  ", end);
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(DWORD_PTR)addr, &mod) && mod &&
            GetModuleFileNameA(mod, modName, MAX_PATH)) {
            const char* leaf = strrchr(modName, '\\');
            p = FStr(p, leaf ? leaf + 1 : modName, end);
            p = FStr(p, "+0x", end);
            p = FHex(p, addr - (DWORD)(DWORD_PTR)mod, 6);
        } else {
            p = FStr(p, "<no module owns this address>", end);
        }
    }

    // The registers are the diagnosis. `EAX=0x00000014` on a write to
    // [eax+4] says the code is using a COUNT as a POINTER — you cannot tell
    // that from the fault address alone.
    const CONTEXT* c = ep->ContextRecord;
    p = FStr(p, "\r\n   eax=", end); p = FHex(p, c->Eax, 8);
    p = FStr(p, " ebx=", end);       p = FHex(p, c->Ebx, 8);
    p = FStr(p, " ecx=", end);       p = FHex(p, c->Ecx, 8);
    p = FStr(p, " edx=", end);       p = FHex(p, c->Edx, 8);
    p = FStr(p, "\r\n   esi=", end); p = FHex(p, c->Esi, 8);
    p = FStr(p, " edi=", end);       p = FHex(p, c->Edi, 8);
    p = FStr(p, " ebp=", end);       p = FHex(p, c->Ebp, 8);

    // The first bytes of the faulting instruction, so the opcode can be read
    // straight out of the report instead of chased in a disassembler.
    p = FStr(p, "\r\n   bytes at eip:", end);
    if (addr >= g_hdr->textLo && addr + 8 < g_hdr->textHi) {
        for (int b = 0; b < 8; ++b) {
            p = FStr(p, " ", end);
            p = FHex(p, ((const BYTE*)(DWORD_PTR)addr)[b], 2);
        }
    } else {
        p = FStr(p, " <outside the game image>", end);
    }

    // ── WHO CALLED THIS ────────────────────────────────────────────────────
    // Scan the stack for return addresses that land in the game's .text and
    // name them. This matters MOST when the fault is inside a system DLL: on
    // its own, "ntdll+0x6f792" says nothing, but "ntdll, called from
    // Mg_008a467a_RegisterGlobals+0xFD" is the whole answer. The earlier
    // version only scanned when the fault was already in game code — exactly
    // backwards.
    //
    // Return addresses are recognised by what precedes them: a CALL is 2, 3, 5
    // or 6 bytes, so a real return site has a CALL opcode just behind it. That
    // filter is what keeps stale stack garbage out of the chain.
    {
        const DWORD* fsp = (const DWORD*)(DWORD_PTR)c->Esp;
        int shown = 0;
        for (int k = 0; k < 256 && shown < 8; ++k) {
            DWORD cand = fsp[k];
            int ci = LookupContaining(cand);
            if (ci < 0) continue;
            if (cand < g_hdr->textLo + 5 || cand >= g_hdr->textHi) continue;
            const BYTE* q = (const BYTE*)(DWORD_PTR)cand;
            bool isRet = (q[-5] == 0xE8) ||                       // call rel32
                         (q[-2] == 0xFF) || (q[-3] == 0xFF) ||    // call r/m32
                         (q[-6] == 0xFF);
            if (!isRet) continue;
            p = FStr(p, shown ? "\r\n              <- " : "\r\n   call stack: ", end);
            p = FStr(p, SymName((DWORD)ci), end);
            p = FStr(p, "+0x", end);
            p = FHex(p, cand - g_sym[ci].va, 4);
            const char* nt = SymNote((DWORD)ci);
            if (nt[0]) { p = FStr(p, "  (", end); p = FStr(p, nt, end);
                         p = FStr(p, ")", end); }
            ++shown;
        }
        if (!shown) p = FStr(p, "\r\n   call stack: <no game return address on "
                                "this stack - the thread is deep in a system DLL>", end);
    }

    if (g_tlsLast != TLS_OUT_OF_INDEXES) {
        DWORD_PTR last = (DWORD_PTR)TlsGetValue(g_tlsLast);
        if (last) {
            p = FStr(p, "\r\n   last traced entry on this thread: ", end);
            p = FStr(p, SymName((DWORD)last - 1), end);
        }
    }
    p = FStr(p, "\r\n\r\n", end);

    char path[MAX_PATH];
    RtfPath("retail_fault.txt", path, MAX_PATH);
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, 0,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        SetFilePointer(h, 0, 0, FILE_END);
        WriteFile(h, buf, (DWORD)(p - buf), &wrote, 0);
        CloseHandle(h);
    }
}

LONG CALLBACK RtfVeh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT* ctx = ep->ContextRecord;

    // ── the re-arm leg of count/detail mode ────────────────────────────────
    if (code == EXCEPTION_SINGLE_STEP) {
        if (g_tls == TLS_OUT_OF_INDEXES) return EXCEPTION_CONTINUE_SEARCH;
        DWORD_PTR pending = (DWORD_PTR)TlsGetValue(g_tls);
        if (!pending) return EXCEPTION_CONTINUE_SEARCH;   // not ours
        TlsSetValue(g_tls, 0);
        ctx->EFlags &= ~0x100u;                           // clear TF
        DWORD idx = (DWORD)pending - 1;
        RtfSlot& sl = g_slot[idx];
        if (g_isArmed && sl.state == S_STEP) {
            // Hot-function governor: a function called thousands of times a
            // second costs one exception per call and would take the frame
            // rate with it. Retire it and say so in the coverage table.
            if (g_cfg.maxHitsPerSec > 0 && sl.secHits > g_cfg.maxHitsPerSec) {
                sl.state = S_HOT;
            } else {
                *(volatile BYTE*)g_sym[idx].va = 0xCC;
                sl.state = S_ARMED;
            }
        } else if (sl.state == S_STEP) {
            sl.state = S_OFF;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code != EXCEPTION_BREAKPOINT) {
        // Not ours — but we are the first handler in the process, so this is
        // the only place a fatal fault can still be described. Record it, then
        // get out of the way so the game's own handlers run unchanged.
        if (g_sym && IsFatalLooking(code)) ReportFault(ep);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!g_isArmed || !g_sym) return EXCEPTION_CONTINUE_SEARCH;

    // ── WHICH ADDRESS TRAPPED ──────────────────────────────────────────────
    // Windows reports a user-mode INT3 with ExceptionAddress AND Eip both
    // pointing AT the 0xCC. Other conventions leave Eip one past it, so all
    // three candidates are tried.
    //
    // ⚠ THE BUG THIS REPLACES (2026-08-13, it killed the game). The old code
    // tried `Eip-1` FIRST and only fell back `if (idx < 0)`. At 0x008a406d the
    // byte before is `FUN_008a406c`, a one-instruction function that IS in the
    // catalogue but was NOT armed — so the lookup SUCCEEDED on the wrong
    // function, found it unarmed, and returned CONTINUE_SEARCH. The tracer
    // declined its own breakpoint, the 0xCC stayed planted, and the thread ran
    // back into it. The game's own handler logged
    // `ExceptionCode = 0x80000003 ... (Ignoring debugger exception)`.
    //
    // A hit is therefore never accepted on "this address is catalogued" — only
    // on "this address is one WE planted", which is what sl.state says.
    DWORD cand[3];
    cand[0] = (DWORD)(DWORD_PTR)ep->ExceptionRecord->ExceptionAddress;
    cand[1] = ctx->Eip;
    cand[2] = ctx->Eip - 1;

    DWORD va = 0;
    int idx = -1;
    for (int pass = 0; pass < 2 && idx < 0; ++pass) {
        for (int k = 0; k < 3; ++k) {
            int t = Lookup(cand[k]);
            if (t < 0) continue;
            BYTE st = g_slot[t].state;
            // pass 0 takes only a live breakpoint; pass 1 accepts one we have
            // already retired, which is the benign multi-thread race.
            bool ok = (pass == 0) ? (st == S_ARMED)
                                  : (st == S_HIT || st == S_STEP || st == S_HOT);
            if (ok) { idx = t; va = cand[k]; break; }
        }
    }
    if (idx < 0) return EXCEPTION_CONTINUE_SEARCH;        // genuinely not ours

    RtfSlot& sl = g_slot[idx];
    if (sl.state != S_ARMED) {
        // Another thread beat us to this entry and already restored the byte;
        // the trap is still real, so just rewind onto the recovered opcode.
        if (sl.state == S_HIT || sl.state == S_STEP || sl.state == S_HOT) {
            ctx->Eip = va;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    *(volatile BYTE*)va = sl.orig;      // uncover the real first instruction
    ctx->Eip = va;                      // and re-execute it

    InterlockedIncrement(&sl.hits);
    InterlockedIncrement(&sl.secHits);
    InterlockedIncrement(&g_secTotal);      // feeds the global rate governor
    if (g_tlsLast != TLS_OUT_OF_INDEXES)    // breadcrumb for the fault report
        TlsSetValue(g_tlsLast, (LPVOID)(DWORD_PTR)(idx + 1));
    if (!sl.firstSeq) {
        sl.firstSeq   = (DWORD)InterlockedIncrement(&g_firstSeq);
        sl.firstMs    = GetTickCount() - g_t0;
        sl.firstFrame = g_hostFrame ? *g_hostFrame : 0;
    }
    RecordHit((DWORD)idx, ctx);

    if (g_cfg.mode == M_COVERAGE) {
        sl.state = S_HIT;               // one shot: never pay for it again
    } else {
        sl.state = S_STEP;
        TlsSetValue(g_tls, (LPVOID)(DWORD_PTR)(idx + 1));
        ctx->EFlags |= 0x100u;          // TF: trap after the real instruction
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// ═══════════════════════════════════════════════════════════════════════════
// THE WORKER — all formatting and I/O happens here
// ═══════════════════════════════════════════════════════════════════════════
void FormatSym(DWORD i, char* out, size_t cap) {
    // The lane (the port file) says more than the corpus subsystem for anything
    // we have actually ported, so it wins when both exist.
    const char* lane = SymLane(i);
    const char* tag  = lane[0] ? lane : SymSubsys(i);
    const char* note = SymNote(i);
    if (tag[0] && note[0])
        _snprintf(out, cap, "%s  [%s] (%s)", SymName(i), tag, note);
    else if (tag[0])
        _snprintf(out, cap, "%s  [%s]", SymName(i), tag);
    else
        _snprintf(out, cap, "%s", SymName(i));
    out[cap - 1] = 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// CALL DEPTH FROM ESP — how the log becomes a tree
//
// We only trap ENTRIES, never returns, so there is no exit event to pop a
// shadow stack with. There does not need to be: on x86 the stack pointer at a
// function's entry is strictly LOWER for a callee than for its caller, and
// equal for a sibling called from the same place. So keeping the ESP of each
// open frame and popping every frame whose ESP is >= the new one recovers the
// nesting exactly, for free, with no second hook and no risk.
//
// It also self-heals: longjmp, an exception unwind or a missed frame all just
// pop more entries next time, because the comparison is against live ESP
// rather than a count we are maintaining.
// ═══════════════════════════════════════════════════════════════════════════
const int MAX_TRACE_THREADS = 24;
const int MAX_TRACE_DEPTH   = 48;
struct ThreadStack {
    DWORD tid;
    int   top;
    DWORD esp[MAX_TRACE_DEPTH];
};
ThreadStack g_stacks[MAX_TRACE_THREADS];
int         g_stackCount = 0;

int DepthOf(DWORD tid, DWORD esp) {
    ThreadStack* ts = 0;
    for (int i = 0; i < g_stackCount; ++i)
        if (g_stacks[i].tid == tid) { ts = &g_stacks[i]; break; }
    if (!ts) {
        if (g_stackCount >= MAX_TRACE_THREADS) return 0;   // too many threads
        ts = &g_stacks[g_stackCount++];
        ts->tid = tid; ts->top = 0;
    }
    while (ts->top > 0 && ts->esp[ts->top - 1] <= esp) --ts->top;
    int depth = ts->top;
    if (ts->top < MAX_TRACE_DEPTH) ts->esp[ts->top++] = esp;
    return depth;
}

volatile LONG g_draining = 0;

void DrainRing() {
    // RTF_DumpCoverage is public, so the worker is not guaranteed to be the
    // only drainer. One writer at a time or the log interleaves mid-line.
    if (InterlockedCompareExchange(&g_draining, 1, 0) != 0) return;
    LONG w = g_ringWrite;
    DWORD write = (DWORD)w;
    if (write - g_ringRead > g_ringSize) {
        // A DROP IS NEVER SILENT. A trace with an unannounced hole in it is
        // worse than no trace: you read the gap as "it never ran".
        DWORD lost = (write - g_ringRead) - g_ringSize;
        InterlockedExchangeAdd(&g_dropped, (LONG)lost);
        g_ringRead = write - g_ringSize;
        fprintf(g_log,
                "\n!!!!!! %u RECORDS LOST — the ring wrapped before the worker "
                "drained it.\n"
                "!!!!!! The trace above this line has a HOLE. Raise RingSize "
                "(now %u) or lower FlushMs (now %d), or narrow Topics/Lanes.\n\n",
                lost, g_ringSize, g_cfg.flushMs);
    }
    char sym[256], caller[160];
    while (g_ringRead != write) {
        RtfHit* r = &g_ring[g_ringRead & g_ringMask];
        if (r->stamp != g_ringRead + 1) break;      // producer still writing
        DWORD i = r->idx;

        int ci = LookupContaining(r->ret);
        if (ci >= 0)
            _snprintf(caller, sizeof(caller), "%s+0x%X",
                      SymName((DWORD)ci), r->ret - g_sym[ci].va);
        else
            _snprintf(caller, sizeof(caller), "0x%08X", r->ret);
        caller[sizeof(caller) - 1] = 0;

        FormatSym(i, sym, sizeof(sym));

        // A banner for the anchors, so the phases of a frame are findable by
        // eye instead of by counting lines.
        if (g_cfg.anchors[0] && InVaList(g_cfg.anchors, g_sym[i].va))
            fprintf(g_log, "\n══════ %s ══════  f%u  %u.%03us\n",
                    SymName(i), r->frame, r->ms / 1000, r->ms % 1000);

        int depth = g_cfg.tree ? DepthOf(r->tid, r->esp) : 0;
        if (depth > 20) depth = 20;
        char indent[64];
        for (int d = 0; d < depth; ++d) { indent[d * 2] = ' '; indent[d * 2 + 1] = ' '; }
        indent[depth * 2] = 0;

        if (g_cfg.mode == M_DETAIL) {
            char args[128]; args[0] = 0;
            for (int a = 0; a < g_cfg.logArgs; ++a) {
                char one[24];
                _snprintf(one, sizeof(one), " %08X", r->arg[a]);
                strncat(args, one, sizeof(args) - strlen(args) - 1);
            }
            fprintf(g_log, "f%-6u %5u.%03us t%04X %s%s  <- %s  ecx=%08X args=%s\n",
                    r->frame, r->ms / 1000, r->ms % 1000, r->tid & 0xFFFF,
                    indent, sym, caller, r->ecx, args);
        } else {
            fprintf(g_log, "f%-6u %5u.%03us t%04X %s%s  <- %s\n",
                    r->frame, r->ms / 1000, r->ms % 1000, r->tid & 0xFFFF,
                    indent, sym, caller);
        }
        ++g_ringRead;
    }
    InterlockedExchange(&g_draining, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// THE STORY REPORT — retail_story.txt
//
// The raw log is every call in time order, which is the truth but not yet an
// explanation. This is the same data re-cut three ways so a subsystem can be
// read as a sequence:
//
//   1. EXECUTION ORDER   every function that ran, in order of FIRST appearance
//   2. BY LANE           the same, grouped by the file we ported it in
//   3. BY TOPIC          the same, grouped by story (shader / sky / drain / …)
//
// ★ Each group also lists what NEVER RAN. That is not filler: it is the half
//   of the result that says "this path was armed and the game did not take
//   it", which is a finding. Without it, silence is ambiguous.
// ═══════════════════════════════════════════════════════════════════════════
DWORD* g_order = 0;          // scratch index array, allocated once

int __cdecl BySeq(const void* a, const void* b) {
    DWORD ia = *(const DWORD*)a, ib = *(const DWORD*)b;
    DWORD sa = g_slot[ia].firstSeq, sb = g_slot[ib].firstSeq;
    return sa < sb ? -1 : sa > sb ? 1 : 0;
}

// One group's worth of lines. `match` picks the members; ran ones come out in
// first-appearance order, then the ones that never fired.
void StoryGroup(FILE* f, const char* title, WORD topicBit, WORD laneIdx) {
    DWORD nRan = 0, nIdle = 0;
    for (DWORD i = 0; i < g_hdr->count; ++i) {
        bool member = topicBit ? (g_sym[i].topics & topicBit) != 0
                               : g_sym[i].lane == laneIdx;
        if (!member) continue;
        const RtfSlot& sl = g_slot[i];
        if (sl.state == S_OFF && !sl.hits) continue;    // never armed: not ours
        if (sl.firstSeq) g_order[nRan++] = i; else ++nIdle;
    }
    if (!nRan && !nIdle) return;

    qsort(g_order, nRan, sizeof(DWORD), BySeq);
    fprintf(f, "\n── %s ── %u ran, %u never ran\n", title, nRan, nIdle);
    if (nRan) {
        fprintf(f, "   order   calls  first@frame     time  function\n");
        for (DWORD k = 0; k < nRan; ++k) {
            DWORD i = g_order[k];
            const RtfSlot& sl = g_slot[i];
            fprintf(f, "   %5u  %6ld  %11u  %4u.%03us  %s%s\n",
                    k + 1, sl.hits, sl.firstFrame, sl.firstMs / 1000,
                    sl.firstMs % 1000, SymName(i),
                    sl.state == S_HOT ? "   <- HOT, later calls not logged" : "");
        }
    }
    if (nIdle) {
        fprintf(f, "   NEVER RAN:");
        DWORD shown = 0;
        for (DWORD i = 0; i < g_hdr->count && shown < 60; ++i) {
            bool member = topicBit ? (g_sym[i].topics & topicBit) != 0
                                   : g_sym[i].lane == laneIdx;
            if (!member || g_slot[i].firstSeq) continue;
            if (g_slot[i].state == S_OFF && !g_slot[i].hits) continue;
            fprintf(f, "%s %s", shown ? "," : "", SymName(i));
            ++shown;
        }
        if (shown < nIdle) fprintf(f, ", ... (%u more)", nIdle - shown);
        fputc('\n', f);
    }
}

void WriteStory() {
    if (!g_order) {
        g_order = (DWORD*)VirtualAlloc(0, sizeof(DWORD) * g_hdr->count,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_order) return;
    }
    char path[MAX_PATH];
    RtfPath("retail_story.txt", path, MAX_PATH);
    FILE* f = fopen(path, "w");
    if (!f) return;

    DWORD total = 0;
    for (DWORD i = 0; i < g_hdr->count; ++i) if (g_slot[i].firstSeq) ++total;

    fprintf(f, "═══════════════════════════════════════════════════════════\n");
    fprintf(f, " RETAIL EXECUTION STORY   %u distinct functions ran in %us\n",
            total, (GetTickCount() - g_t0) / 1000);
    fprintf(f, " mode=%s  armed=%ld  dropped=%ld\n",
            g_cfg.mode == M_COVERAGE ? "coverage" :
            g_cfg.mode == M_COUNT ? "count" : "detail",
            g_armedCount, g_dropped);
    fprintf(f, "═══════════════════════════════════════════════════════════\n");

    // 1 — the global order, which is the boot/level narrative
    DWORD n = 0;
    for (DWORD i = 0; i < g_hdr->count; ++i)
        if (g_slot[i].firstSeq) g_order[n++] = i;
    qsort(g_order, n, sizeof(DWORD), BySeq);
    fprintf(f, "\n════ EXECUTION ORDER (first appearance) ════\n");
    fprintf(f, "   order   calls  first@frame     time  lane                  function\n");
    for (DWORD k = 0; k < n; ++k) {
        DWORD i = g_order[k];
        const RtfSlot& sl = g_slot[i];
        const char* lane = SymLane(i)[0] ? SymLane(i) : SymSubsys(i);
        fprintf(f, "   %5u  %6ld  %11u  %4u.%03us  %-20s  %s\n",
                k + 1, sl.hits, sl.firstFrame, sl.firstMs / 1000,
                sl.firstMs % 1000, lane, SymName(i));
    }

    // 2 — by lane (the port file), which is how the code is organised
    fprintf(f, "\n\n════ BY LANE (the file we ported it in) ════\n");
    for (WORD s = 1; s < (WORD)g_subsysN; ++s) {
        bool used = false;
        for (DWORD i = 0; i < g_hdr->count && !used; ++i)
            if (g_sym[i].lane == s && (g_slot[i].firstSeq ||
                g_slot[i].state == S_ARMED || g_slot[i].state == S_HIT)) used = true;
        if (used) StoryGroup(f, SymStr(s), 0, s);
    }

    // 3 — by topic, which is how the subsystem is understood
    fprintf(f, "\n\n════ BY TOPIC (the story it belongs to) ════\n");
    for (size_t t = 0; t < sizeof(kTopics) / sizeof(kTopics[0]); ++t)
        StoryGroup(f, kTopics[t].name, kTopics[t].bit, 0);

    fclose(f);
}

void WriteCoverage() {
    char path[MAX_PATH];
    RtfPath(g_cfg.coverageFile, path, MAX_PATH);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "va,name,subsystem,ported,realname,thunk,state,hits,"
               "first_frame,first_ms,ported_at\n");
    DWORD ran = 0, armed = 0, hot = 0;
    for (DWORD i = 0; i < g_hdr->count; ++i) {
        const RtfSlot& sl = g_slot[i];
        if (sl.state == S_ARMED) ++armed;
        if (sl.state == S_HOT)   ++hot;
        if (!sl.hits) continue;
        ++ran;
        const char* st = sl.state == S_HIT  ? "hit"
                       : sl.state == S_HOT  ? "HOT-disarmed"
                       : sl.state == S_ARMED? "armed"
                       : sl.state == S_STEP ? "stepping" : "off";
        fprintf(f, "0x%08X,%s,%s,%d,%d,%d,%s,%ld,%u,%u,\"%s\"\n",
                g_sym[i].va, SymName(i), SymSubsys(i),
                (g_sym[i].flags & F_PORTED)   ? 1 : 0,
                (g_sym[i].flags & F_REALNAME) ? 1 : 0,
                (g_sym[i].flags & F_THUNK)    ? 1 : 0,
                st, sl.hits, sl.firstFrame, sl.firstMs, SymNote(i));
    }
    fclose(f);

    char status[MAX_PATH];
    RtfPath("retail_trace_status.txt", status, MAX_PATH);
    FILE* s = fopen(status, "w");
    if (s) {
        fprintf(s, "armed=%s mode=%s set=%s\n",
                g_isArmed ? "yes" : "no",
                g_cfg.mode == M_COVERAGE ? "coverage" :
                g_cfg.mode == M_COUNT ? "count" : "detail", g_cfg.set);
        fprintf(s, "functions_armed=%u  functions_that_ran=%u  hot_disarmed=%u\n",
                armed, ran, hot);
        fprintf(s, "records_logged=%u  records_dropped=%ld  uptime=%us\n",
                g_ringRead, g_dropped, (GetTickCount() - g_t0) / 1000);
        fclose(s);
    }
}

bool Chord(int vk) {
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
           (GetAsyncKeyState(VK_MENU)    & 0x8000) &&
           (GetAsyncKeyState(vk) & 1);
}

DWORD WINAPI RtfWorker(LPVOID) {
    // Everything heavy lives here: DllMain only spawns this thread, so the
    // loader lock is long gone by the time we touch files or .text.
    g_t0 = GetTickCount();
    LoadConfig();

    char logPath[MAX_PATH];
    RtfPath(g_cfg.logFile, logPath, MAX_PATH);
    g_log = fopen(logPath, "w");
    if (!g_log) return 0;
    setvbuf(g_log, 0, _IOFBF, 1 << 16);

    // The fault file is opened APPEND from the handler (a dying process cannot
    // afford a buffered rewrite), so it has to be cleared HERE or every run
    // reads as though it inherited the previous run's crash.
    {
        char fp[MAX_PATH];
        RtfPath("retail_fault.txt", fp, MAX_PATH);
        DeleteFileA(fp);
    }

    Say("═══ RETAIL FUNCTION TRACER ═══");
    Say("ini      : %s", g_iniPath);
    if (!g_cfg.enabled) {
        Say("DISABLED : set Enabled=1 in retail_trace.ini to arm.");
        fflush(g_log);
        return 0;
    }
    if (!LoadSymbols()) { fflush(g_log); return 0; }
    if (!VerifyImage()) { fflush(g_log); return 0; }

    if (g_cfg.topics[0]) {
        char bad[256];
        g_wantTopics = ParseTopics(g_cfg.topics, bad, sizeof(bad));
        if (bad[0]) {
            Say("WARNING  : unknown topic(s) ignored: %s", bad);
            Say("           valid: teardown hdr exposure drain shader sky "
                "cloud frame spray shadow conduct");
        }
    }

    g_ring = (RtfHit*)VirtualAlloc(0, sizeof(RtfHit) * g_ringSize,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_ring) { Say("FATAL: ring allocation failed"); fflush(g_log); return 0; }

    g_tls     = TlsAlloc();
    g_tlsLast = TlsAlloc();
    if (g_tls == TLS_OUT_OF_INDEXES && g_cfg.mode != M_COVERAGE) {
        Say("no TLS slot available — falling back to coverage mode");
        g_cfg.mode = M_COVERAGE;
    }

    // First in the chain: we must see our own INT3 before the proxy's crash
    // handler decides it is a fault worth reporting.
    g_veh = AddVectoredExceptionHandler(1, RtfVeh);
    if (!g_veh) { Say("FATAL: AddVectoredExceptionHandler failed"); fflush(g_log); return 0; }

    Say("hotkeys  : CTRL+ALT+0x%02X toggle | CTRL+ALT+0x%02X dump | "
        "CTRL+ALT+0x%02X panic-disarm", g_cfg.keyToggle, g_cfg.keyDump, g_cfg.keyPanic);
    if (g_cfg.armOnLoad) {
        // ArmDelayMs is the bisect: let the game boot un-instrumented, then
        // arm. If it survives the delay and dies the instant we arm, the
        // tracer is the cause; if it dies during the delay, it never was.
        if (g_cfg.armDelayMs > 0) {
            Say("waiting  : ArmDelayMs=%d - the game runs UNINSTRUMENTED until "
                "then. Anything that goes wrong before the ARMED line below is "
                "not the tracer.", g_cfg.armDelayMs);
            fflush(g_log);
            Sleep((DWORD)g_cfg.armDelayMs);
        }
        RtfArmImpl();
    } else {
        Say("waiting  : ArmOnLoad=0, press the toggle chord to arm. NOTHING is "
            "patched until you do - if the game misbehaves now, it is not us.");
    }
    fflush(g_log);

    DWORD lastSec = GetTickCount(), lastCov = lastSec;
    while (!g_quit) {
        Sleep(g_cfg.flushMs);
        DrainRing();
        fflush(g_log);

        DWORD now = GetTickCount();
        if (now - lastSec >= 1000) {
            lastSec = now;

            // ── GLOBAL RATE GOVERNOR ───────────────────────────────────────
            // The per-function governor cannot see the sum. On 08-13 no single
            // function was hot enough to retire, yet the process was taking
            // ~109,000 traps a second across thousands of them — each one a
            // kernel round trip. Degrade to coverage: every armed function
            // then disarms on its next hit, so the rate collapses on its own
            // and the trace stays complete in the way that still fits.
            LONG rate = InterlockedExchange(&g_secTotal, 0);
            if (g_cfg.maxTotalHitsPerSec > 0 && g_cfg.mode != M_COVERAGE &&
                rate > g_cfg.maxTotalHitsPerSec) {
                g_cfg.mode = M_COVERAGE;
                Say("!! RATE      %ld traps/sec across all functions exceeded "
                    "MaxTotalHitsPerSec=%d.", rate, g_cfg.maxTotalHitsPerSec);
                Say("             DEGRADED TO COVERAGE: from here each function "
                    "is logged once more and then disarms. Call COUNTS above "
                    "this line are complete; below it they are first-hit only.");
            }

            for (DWORD i = 0; i < g_hdr->count; ++i) {
                RtfSlot& sl = g_slot[i];
                // The governor is the other way a trace can lose calls, so it
                // announces itself by name the moment it retires something.
                if (sl.state == S_HOT && !(sl.notes & 1)) {
                    sl.notes |= 1;
                    Say("!! HOT       %-46s retired after %ld calls in one "
                        "second (MaxHitsPerSec=%d). Its later calls are NOT in "
                        "this log.", SymName(i), sl.hits, g_cfg.maxHitsPerSec);
                }
                if (sl.secHits) InterlockedExchange(&sl.secHits, 0);
            }
        }
        if (g_cfg.coverageEverySec > 0 &&
            now - lastCov >= (DWORD)g_cfg.coverageEverySec * 1000) {
            lastCov = now;
            WriteCoverage(); WriteStory();
        }

        if (Chord(g_cfg.keyToggle)) {
            if (g_isArmed) {
                RtfDisarmImpl();
                DrainRing(); WriteCoverage(); WriteStory();
                SnapshotWindow();
            } else {
                // Save the window we are about to discard, then open a new one.
                DrainRing(); WriteCoverage(); WriteStory();
                SnapshotWindow();
                RtfRearmFresh();
                RtfArmImpl();
            }
            fflush(g_log);
        }
        if (Chord(g_cfg.keyDump)) {
            DrainRing(); WriteCoverage(); WriteStory();
            SnapshotWindow();
            fflush(g_log);
        }
        if (Chord(g_cfg.keyPanic)) {
            RtfDisarmImpl();
            Say("-- PANIC: everything disarmed --");
            fflush(g_log);
        }
    }
    return 0;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════
extern "C" void RTF_SetFrameCounter(volatile unsigned long* counter) {
    g_hostFrame = counter;
}

extern "C" void RTF_Init(void) {
    if (InterlockedCompareExchange(&g_inited, 1, 0) != 0) return;

    // ── ONE TRACER PER PROCESS ─────────────────────────────────────────────
    // The same code ships twice: compiled into d3d9.dll (the proxy) and as the
    // standalone ConquestTrace.dll. Load both and you get two independent
    // copies with their own globals, each arming the same 1,680 entry points
    // and each opening retail_calls.log with "w".
    //
    // Measured on 2026-08-13: `ARMED: 272  padding=1345` — the second copy read
    // the FIRST copy's 0xCC bytes as though they were alignment padding, and
    // skipped 1,345 functions. The log came out with two threads' lines spliced
    // mid-word. A process-wide named mutex settles who owns the tracing; the
    // loser stays completely inert.
    HANDLE own = CreateMutexA(0, TRUE, "Local\\RetailFnTracer_SingleInstance");
    if (!own || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (own) CloseHandle(own);
        return;                      // another copy owns it — do nothing at all
    }

    // DllMain-safe: spawning a thread under the loader lock is fine, doing
    // anything else here is not.
    DWORD tid = 0;
    g_worker = CreateThread(0, 0, RtfWorker, 0, 0, &tid);
}

extern "C" void RTF_Arm(void)          { if (g_hdr) RtfArmImpl(); }
extern "C" void RTF_Disarm(void)       { if (g_hdr) RtfDisarmImpl(); }
extern "C" void RTF_DumpCoverage(void) { if (g_hdr) { DrainRing(); WriteCoverage(); WriteStory(); } }

// ═══════════════════════════════════════════════════════════════════════════
// OFFLINE FALSIFIER
//
// A tracer that compiles is not a tracer that works, and the game is not
// available to prove it on. So the mechanism proves itself: this builds three
// real x86 functions in an RWX page, points the tracer's own globals at a
// synthetic symbol table describing them, and then runs the SHIPPED arm /
// trap / restore / re-arm / drain code over them. Nothing here is a
// re-implementation — RtfArmImpl, RtfVeh, DrainRing and Selected are the same
// functions the game will run.
//
// The load-bearing check is not "did we count a hit" but "did the function
// still return the right value afterwards": that is the one that proves the
// original byte went back correctly and the instruction was re-executed
// rather than skipped.
// ═══════════════════════════════════════════════════════════════════════════
namespace {

int g_stFail = 0;
void StCheck(bool ok, const char* what, ...) {
    char msg[256];
    va_list a; va_start(a, what);
    _vsnprintf(msg, sizeof(msg), what, a);
    msg[sizeof(msg) - 1] = 0;
    va_end(a);
    if (!ok) ++g_stFail;
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", msg);
    Say("  [%s] %s", ok ? "PASS" : "FAIL", msg);
}

typedef int(__cdecl* StFn)(void);

// Registered BEHIND the tracer's handler. It only ever runs if the tracer
// declined a breakpoint it planted — the exact failure that killed the game on
// 08-13. It repairs the byte so the test reports a clean FAIL instead of
// spinning forever on a live 0xCC.
volatile LONG g_stDeclined = 0;
DWORD         g_stGuardVa   = 0;
BYTE          g_stGuardOrig = 0;

LONG CALLBACK StGuardVeh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD at = (DWORD)(DWORD_PTR)ep->ExceptionRecord->ExceptionAddress;
    if (at != g_stGuardVa && ep->ContextRecord->Eip - 1 != g_stGuardVa)
        return EXCEPTION_CONTINUE_SEARCH;
    InterlockedExchange(&g_stDeclined, 1);
    *(volatile BYTE*)g_stGuardVa = g_stGuardOrig;
    ep->ContextRecord->Eip = g_stGuardVa;
    return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

extern "C" int RTF_SelfTest(void) {
    g_t0 = GetTickCount();
    LoadConfig();

    char logPath[MAX_PATH];
    RtfPath("retail_trace_selftest.log", logPath, MAX_PATH);
    g_log = fopen(logPath, "w");
    printf("=== RETAIL FUNCTION TRACER — SELF TEST ===\n");
    Say("=== RETAIL FUNCTION TRACER — SELF TEST ===");

    // ── PART 1: the real symbol table parses and resolves ──────────────────
    printf("\n-- symbol table --\n"); Say("-- symbol table --");
    bool loaded = LoadSymbols();
    StCheck(loaded, "retail_symbols.bin loads");
    if (loaded) {
        StCheck(g_hdr->count > 30000, "%u entries catalogued", g_hdr->count);
        struct { DWORD va; const char* want; } known[] = {
            { 0x0068e383, "Mg_0068e383_RingFrameRotate" },
            { 0x006a578e, "Mg_006a578e_PostMatReset"    },
            { 0x008f7071, "Mg_008f7071_AnimTick"        },
        };
        for (int k = 0; k < 3; ++k) {
            int i = Lookup(known[k].va);
            StCheck(i >= 0 && !strcmp(SymName((DWORD)i), known[k].want),
                    "0x%08X resolves to %s (note: %s)", known[k].va,
                    i >= 0 ? SymName((DWORD)i) : "<missing>",
                    i >= 0 ? SymNote((DWORD)i) : "");
        }
        // A return address lands INSIDE a function, never on its entry — this
        // is what turns `<- 0x0068e390` into `<- ...RingFrameRotate+0xD`.
        int c = LookupContaining(0x0068e390);
        StCheck(c >= 0 && g_sym[c].va <= 0x0068e390,
                "caller resolution: 0x0068e390 -> %s+0x%X",
                c >= 0 ? SymName((DWORD)c) : "<none>",
                c >= 0 ? 0x0068e390 - g_sym[c].va : 0);

        DWORD ported = 0, named = 0;
        for (DWORD i = 0; i < g_hdr->count; ++i) {
            if (g_sym[i].flags & F_PORTED) ++ported;
            if (g_sym[i].flags & (F_PORTED | F_REALNAME)) ++named;
        }
        StCheck(ported > 1500, "%u ported functions, %u named in total",
                ported, named);
    }

    // ── PART 2: the trap mechanism, on functions we generate ───────────────
    printf("\n-- trap mechanism (synthetic module) --\n");
    Say("-- trap mechanism (synthetic module) --");

    BYTE* page = (BYTE*)VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (!page) { printf("  [FAIL] page alloc\n"); return ++g_stFail; }

    // Three functions, 16 bytes apart:  mov eax, imm32 ; ret
    const int NFN = 3;
    const DWORD kMagic[NFN] = { 0x0000AAAA, 0x0000BBBB, 0x0000CCCC };
    for (int f = 0; f < NFN; ++f) {
        BYTE* p = page + f * 16;
        p[0] = 0xB8;                                   // mov eax, imm32
        memcpy(p + 1, &kMagic[f], 4);
        p[5] = 0xC3;                                   // ret
    }
    FlushInstructionCache(GetCurrentProcess(), page, 4096);

    // Point the tracer at a synthetic table describing exactly those three.
    static RtfHeader synHdr;
    static RtfSym    synSym[NFN + 1];   // +1: the nested-call probe
    static char      synBlob[128];
    static const char* synSubsys = "SelfTest";
    static RtfSlot   synSlot[NFN + 1];

    memset(&synHdr, 0, sizeof(synHdr));
    memcpy(synHdr.magic, "RTSY", 4);
    synHdr.version = 5; synHdr.count = NFN;
    synHdr.imageBase = 0x00400000;
    synHdr.textLo = (DWORD)(DWORD_PTR)page;
    synHdr.textHi = synHdr.textLo + 4096;
    {
        DWORD off = 1; synBlob[0] = 0;
        for (int f = 0; f < NFN; ++f) {
            synSym[f].va      = synHdr.textLo + f * 16;
            synSym[f].nameOff = off;
            off += (DWORD)sprintf(synBlob + off, "SelfTest_fn%d", f) + 1;
            synSym[f].noteOff = 0;
            synSym[f].subsys  = 1;
            synSym[f].lane    = 1;
            synSym[f].topics  = 0;
            synSym[f].flags   = F_PORTED;
            synSym[f].byteEA  = 0xB8;   // the interlock's expected first byte
            synSym[f].byteLLC = 0xB8;
            synSym[f].reserved = 0;
        }
    }
    memset(synSlot, 0, sizeof(synSlot));

    g_hdr = &synHdr; g_sym = synSym; g_blob = synBlob; g_slot = synSlot;
    g_subsys[1] = synSubsys; g_subsysN = 2;
    g_textWritable = false;
    g_oracleIsEA = true;
    strcpy(g_cfg.set, "ported");
    g_cfg.armSuspect = 0;
    g_cfg.subsystems[0] = g_cfg.include[0] = g_cfg.exclude[0] = 0;
    g_cfg.armLimit = 0; g_cfg.maxHitsPerSec = 0;   // 0 = never auto-disarm

    g_ring = (RtfHit*)VirtualAlloc(0, sizeof(RtfHit) * g_ringSize,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_tls  = TlsAlloc();
    g_veh  = AddVectoredExceptionHandler(1, RtfVeh);
    StCheck(g_ring && g_tls != TLS_OUT_OF_INDEXES && g_veh,
            "ring + TLS slot + vectored handler installed");

    StFn fn0 = (StFn)(page + 0), fn1 = (StFn)(page + 16), fn2 = (StFn)(page + 32);

    // --- coverage mode: trap once, then get out of the way ---
    g_cfg.mode = M_COVERAGE;
    RtfArmImpl();
    StCheck(g_armedCount == NFN, "coverage: armed %ld of %d", g_armedCount, NFN);
    StCheck(*(BYTE*)fn0 == 0xCC, "coverage: entry byte replaced with INT3");

    DWORD r0 = (DWORD)fn0();
    StCheck(r0 == kMagic[0],
            "coverage: trapped call still returns 0x%08X (got 0x%08X)",
            kMagic[0], r0);
    StCheck(synSlot[0].hits == 1, "coverage: 1 hit recorded (got %ld)",
            synSlot[0].hits);
    StCheck(*(BYTE*)fn0 == 0xB8, "coverage: original opcode restored");

    for (int i = 0; i < 100; ++i) fn0();
    StCheck(synSlot[0].hits == 1,
            "coverage: 100 further calls cost nothing (hits still %ld)",
            synSlot[0].hits);

    // --- count mode: re-arm through the single-step trap ---
    RtfDisarmImpl();
    memset(synSlot, 0, sizeof(synSlot));
    g_cfg.mode = M_COUNT;
    RtfArmImpl();
    DWORD ok = 0;
    for (int i = 0; i < 50; ++i) if ((DWORD)fn1() == kMagic[1]) ++ok;
    StCheck(ok == 50, "count: all 50 calls returned correctly (%u)", ok);
    StCheck(synSlot[1].hits == 50, "count: 50 hits recorded (got %ld)",
            synSlot[1].hits);
    StCheck(*(BYTE*)fn1 == 0xCC, "count: still armed after the last call");

    // --- the hot governor retires runaway functions ---
    g_cfg.maxHitsPerSec = 10;
    for (int i = 0; i < 40; ++i) fn2();
    StCheck(synSlot[2].state == S_HOT,
            "governor: fn2 auto-disarmed after %ld hits (state=%d)",
            synSlot[2].hits, synSlot[2].state);
    StCheck((DWORD)fn2() == kMagic[2],
            "governor: retired function still returns correctly");
    g_cfg.maxHitsPerSec = 0;

    // --- disarm puts every byte back ---
    RtfDisarmImpl();
    StCheck(*(BYTE*)fn0 == 0xB8 && *(BYTE*)fn1 == 0xB8 && *(BYTE*)fn2 == 0xB8,
            "disarm: all three entry points restored");

    // --- the interlock refuses an entry point somebody else already owns ---
    // This is the check that protects memorypatch.cpp's ~20 detours: if the
    // live byte is not what the image on disk holds, the function is already
    // hooked and planting 0xCC over it would corrupt the other hook.
    memset(synSlot, 0, sizeof(synSlot));
    g_cfg.mode = M_COVERAGE;
    *(BYTE*)fn0 = 0xE9;                       // as if a 5-byte JMP landed here
    RtfArmImpl();
    StCheck(synSlot[0].state == S_SKIP,
            "interlock: pre-patched fn0 refused (state=%d, expected %d)",
            synSlot[0].state, (int)S_SKIP);
    StCheck(synSlot[1].state == S_ARMED && synSlot[2].state == S_ARMED,
            "interlock: the other two still armed");
    StCheck(*(BYTE*)fn0 == 0xE9, "interlock: the foreign patch was left intact");
    RtfDisarmImpl();
    *(BYTE*)fn0 = 0xB8;

    // --- THE DENY LIST OUTRANKS EVERYTHING ---
    // F_UNSAFE marks the exception-dispatch machinery. Arming it is what killed
    // the 08-13 run, so Include= — the strongest opt-in there is — must still
    // not reach it.
    {
        memset(synSlot, 0, sizeof(synSlot));
        g_cfg.mode = M_COVERAGE;
        synSym[1].flags |= F_UNSAFE;
        _snprintf(g_cfg.include, sizeof(g_cfg.include), "0x%08X", synSym[1].va);
        RtfArmImpl();
        StCheck(synSlot[1].state != S_ARMED,
                "deny list: F_UNSAFE refused even with Include= naming it "
                "(state=%d)", synSlot[1].state);
        StCheck(*(BYTE*)fn1 == 0xB8, "deny list: its entry byte was not touched");
        StCheck(synSlot[0].state == S_ARMED,
                "deny list: the rest of the set still armed");
        RtfDisarmImpl();
        g_cfg.include[0] = 0;
        synSym[1].flags &= ~F_UNSAFE;
    }

    // --- the symbol table really does mark the SEH machinery unsafe ---
    // The runtime rule is only worth anything if the table it reads sets the
    // bit, so check the real file, not the synthetic one.
    if (loaded) {
        const RtfHeader* realHdr = (const RtfHeader*)g_symFile;
        const RtfSym* realSym = (const RtfSym*)(g_symFile + realHdr->entryOff);
        const char* realBlob = (const char*)(g_symFile + realHdr->blobOff);
        DWORD nUnsafe = 0; bool sehMarked = false, sehArmable = false;
        for (DWORD i = 0; i < realHdr->count; ++i) {
            const char* nm = realBlob + realSym[i].nameOff;
            bool isSeh = !_strnicmp(nm, "__SEH_", 6);
            if (realSym[i].flags & F_UNSAFE) { ++nUnsafe; if (isSeh) sehMarked = true; }
            else if (isSeh) sehArmable = true;
        }
        StCheck(nUnsafe >= 10 && sehMarked && !sehArmable,
                "deny list: %u functions marked unsafe in the real table, "
                "__SEH_* covered=%d, none left armable=%d",
                nUnsafe, (int)sehMarked, (int)!sehArmable);
    }

    // --- CALL DEPTH: a real nested call, measured on real hardware ---
    // fn3 calls fn0. The tree in the log is built on one claim — that a callee
    // enters with a strictly lower ESP than its caller — so prove it with an
    // actual call rather than trusting the reasoning.
    {
        BYTE* p = page + 48;
        p[0] = 0xE8;                                   // call rel32 -> fn0
        LONG rel = (LONG)((BYTE*)fn0 - (p + 5));
        memcpy(p + 1, &rel, 4);
        p[5] = 0xC3;
        FlushInstructionCache(GetCurrentProcess(), page, 4096);

        synHdr.count = NFN + 1;
        synSym[NFN].va      = synHdr.textLo + 48;
        synSym[NFN].nameOff = synSym[0].nameOff;       // reuse a name; only the
        synSym[NFN].noteOff = 0;                       // ESP relation matters
        synSym[NFN].subsys  = 1; synSym[NFN].lane = 1; synSym[NFN].topics = 0;
        synSym[NFN].flags   = F_PORTED;
        synSym[NFN].byteEA  = 0xE8; synSym[NFN].byteLLC = 0xE8;
        synSym[NFN].reserved = 0;
        memset(synSlot, 0, sizeof(synSlot));

        g_cfg.mode = M_COUNT;
        RtfArmImpl();
        LONG mark = g_ringWrite;
        ((StFn)(page + 48))();
        RtfDisarmImpl();

        DWORD espOuter = 0, espInner = 0;
        for (LONG s = mark; s < g_ringWrite; ++s) {
            RtfHit* h = &g_ring[(DWORD)s & g_ringMask];
            if (h->idx == NFN) espOuter = h->esp;
            if (h->idx == 0)   espInner = h->esp;
        }
        StCheck(espOuter && espInner && espInner < espOuter,
                "tree: callee entered below its caller (outer esp=%08X, "
                "inner=%08X)", espOuter, espInner);
        // And the reconstruction itself, through the shipped DepthOf().
        g_stackCount = 0;
        int d0 = DepthOf(0xBEEF, espOuter);
        int d1 = DepthOf(0xBEEF, espInner);
        int d2 = DepthOf(0xBEEF, espOuter);   // sibling of the outer frame
        StCheck(d0 == 0 && d1 == 1 && d2 == 0,
                "tree: DepthOf gives caller=%d callee=%d sibling=%d "
                "(want 0,1,0)", d0, d1, d2);
        g_stackCount = 0;
        synHdr.count = NFN;
    }

    // --- THE ADJACENT-DECOY REGRESSION (the 08-13 game-killer) -------------
    // Reproduces the field condition exactly: a catalogued but UNARMED entry
    // point sitting one byte below an ARMED one, which is how 0x008a406c
    // (FUN_008a406c, a bare RET) sat under 0x008a406d
    // (Mg_008a406d_LoadNamedScriptChunk). The old handler resolved Eip-1 onto
    // the decoy, decided the trap was not ours, and left its own 0xCC planted.
    //
    // A guard handler registered BEHIND ours catches that: if ours ever
    // declines again, the guard cleans up and the check reports FAIL instead
    // of hanging the process on a live breakpoint.
    {
        BYTE* p2 = page + 128;
        p2[16] = 0xB8; { DWORD m = 0x0000DDDD; memcpy(p2 + 17, &m, 4); } p2[21] = 0xC3;
        p2[15] = 0xC3;                      // the decoy: a one-byte function
        FlushInstructionCache(GetCurrentProcess(), page, 4096);

        static RtfSym  dSym[2];
        static RtfSlot dSlot[2];
        memset(dSym, 0, sizeof(dSym));
        memset(dSlot, 0, sizeof(dSlot));
        dSym[0].va = (DWORD)(DWORD_PTR)(p2 + 15);   // catalogued, NOT ported
        dSym[0].nameOff = synSym[0].nameOff; dSym[0].subsys = 1; dSym[0].lane = 1;
        dSym[0].flags = F_IMPL; dSym[0].byteEA = 0xC3; dSym[0].byteLLC = 0xC3;
        dSym[1].va = (DWORD)(DWORD_PTR)(p2 + 16);   // ported -> armed
        dSym[1].nameOff = synSym[0].nameOff; dSym[1].subsys = 1; dSym[1].lane = 1;
        dSym[1].flags = F_PORTED; dSym[1].byteEA = 0xB8; dSym[1].byteLLC = 0xB8;

        synHdr.count = 2;
        g_sym = dSym; g_slot = dSlot;
        g_cfg.mode = M_COVERAGE;

        g_stDeclined = 0;
        g_stGuardVa  = dSym[1].va;
        g_stGuardOrig = 0xB8;
        PVOID guard = AddVectoredExceptionHandler(0, StGuardVeh);

        RtfArmImpl();
        StCheck(dSlot[1].state == S_ARMED && dSlot[0].state != S_ARMED,
                "decoy: only the ported neighbour armed (armed=%d decoy=%d)",
                dSlot[1].state, dSlot[0].state);

        DWORD rv = (DWORD)((StFn)(p2 + 16))();
        StCheck(!g_stDeclined,
                "decoy: the handler claimed its own breakpoint even with an "
                "unarmed catalogued function one byte below it");
        StCheck(rv == 0x0000DDDD, "decoy: function still returned 0x%08X", rv);
        StCheck(dSlot[1].hits == 1, "decoy: hit recorded (%ld)", dSlot[1].hits);

        RtfDisarmImpl();
        if (guard) RemoveVectoredExceptionHandler(guard);
        g_sym = synSym; g_slot = synSlot; synHdr.count = NFN;
    }

    // --- the story report is written and is not empty ---
    {
        WriteStory();
        char sp[MAX_PATH]; RtfPath("retail_story.txt", sp, MAX_PATH);
        HANDLE h = CreateFileA(sp, GENERIC_READ, FILE_SHARE_READ, 0,
                               OPEN_EXISTING, 0, 0);
        DWORD sz = (h != INVALID_HANDLE_VALUE) ? GetFileSize(h, 0) : 0;
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        StCheck(sz > 200, "story: retail_story.txt written (%u bytes)", sz);
    }

    // --- the records made it to the log ---
    DWORD before = g_ringRead;
    DrainRing();
    StCheck(g_ringRead > before, "drain: %u records formatted to the log",
            g_ringRead - before);
    StCheck(g_dropped == 0, "drain: no records dropped (%ld)", g_dropped);

    RemoveVectoredExceptionHandler(g_veh); g_veh = 0;
    g_hdr = 0; g_sym = 0; g_slot = 0;      // never leave the game pointed here

    printf("\n%s  (%d failed)\n", g_stFail ? "SELFTEST FAILED" : "SELFTEST OK",
           g_stFail);
    Say("%s (%d failed)", g_stFail ? "SELFTEST FAILED" : "SELFTEST OK", g_stFail);
    if (g_log) { fflush(g_log); fclose(g_log); g_log = 0; }
    return g_stFail;
}

extern "C" void RTF_Shutdown(void) {
    if (!g_inited) return;
    InterlockedExchange(&g_quit, 1);
    if (g_worker) {
        WaitForSingleObject(g_worker, 2000);
        CloseHandle(g_worker);
        g_worker = 0;
    }
    if (g_hdr) {
        RtfDisarmImpl();
        DrainRing();
        WriteCoverage();
        WriteStory();
    }
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = 0; }
    if (g_textWritable) {
        DWORD dummy;
        VirtualProtect((LPVOID)g_hdr->textLo, g_hdr->textHi - g_hdr->textLo,
                       g_oldProtect, &dummy);
        g_textWritable = false;
    }
    if (g_log) { fflush(g_log); fclose(g_log); g_log = 0; }
}
