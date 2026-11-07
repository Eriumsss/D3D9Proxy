#include "d3d9device.h"
#include "memorypatch.h"
#include "retail_heap_extender.h"
#include "geometry_budget_telemetry.h"
#include "retail_texture_vault.h"
#include <intrin.h>
#include <cstdio>
#include <windows.h>

bool g_WireframeEnabled = false;

// Frame counter for freeze detection - exported for watchdog
volatile DWORD g_FrameCounter = 0;

// ── Single-frame shader constant capture ──
// Press F9 to dump one frame's shader constants to capture_frame.txt
static bool g_CaptureNextFrame = false;
static bool g_Capturing = false;
static FILE* g_CaptureFile = NULL;
static int g_CaptureDrawCall = 0;

// Track current shader/texture state for capture
static IDirect3DVertexShader9* g_curVS = NULL;
static IDirect3DPixelShader9*  g_curPS = NULL;
static IDirect3DBaseTexture9*  g_curTex[16] = {0};
static int g_shaderDumpCount = 0;
static bool g_shadersDumped = false;

// Shader pointer → name mapping (extracted from bytecode #line directives)
#include <map>
#include <string>
static std::map<void*, std::string> g_shaderNames;

// Extract shader name from bytecode's debug info (#line "path/ShaderName.final")
static std::string ExtractShaderName(const DWORD* pFunction) {
    if (!pFunction) return "NULL";
    // Walk bytecodes looking for comment tokens that contain the filename
    const DWORD* p = pFunction;
    int maxScan = 25000; // safety limit
    while (*p != 0x0000FFFF && maxScan-- > 0) {
        // Comment token: opcode 0xFFFE, length in upper 16 bits
        if ((*p & 0xFFFF) == 0xFFFE) {
            DWORD len = (*p >> 16); // DWORD count
            if (len > 2 && len < 1000) {
                // Scan comment payload for path strings
                const char* txt = (const char*)(p + 1);
                int byteLen = len * 4;
                for (int i = 0; i < byteLen - 10; ++i) {
                    // Look for "Mg_" or "MgFP" or "MgVP" pattern
                    if ((txt[i] == 'M' && txt[i+1] == 'g' && (txt[i+2] == '_' || txt[i+2] == 'F' || txt[i+2] == 'V'))) {
                        // Extract until .final or null
                        std::string name;
                        for (int j = i; j < byteLen && txt[j] != '\0' && txt[j] != '.'; ++j)
                            name += txt[j];
                        if (name.size() > 5) return name;
                    }
                }
            }
            p += 1 + len;
        } else {
            p++;
        }
    }
    return "unknown";
}

static void CaptureCheck() {
    if (GetAsyncKeyState(VK_F9) & 1) {
        g_CaptureNextFrame = true;
    }
}

// Track which shaders have had bytecodes dumped (by pointer)
#include <set>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
// ═══════════════════════════════════════════════════════════════════════════
// RETAIL FRAME TRACER (2026-07-31) — retail IS the reference implementation.
// Dump ONE full retail frame and diff it against Vespucci's. Geometry
// fingerprints (verts/prims) are identical across both engines because the
// same assets are drawn, so a draw can be matched exactly between the logs and
// the state feeding it compared register by register.
// F11 in-game => next frame dumped to retail_trace.log NEXT TO THIS DLL.
// Covers geometry AND the post chain: CreateTexture shows HDR/luminance
// formats, SetRenderTarget marks pass boundaries, the tonemap quads appear as
// 2-primitive draws, and the pixel constants around them are the exposure
// parameters.
// ═══════════════════════════════════════════════════════════════════════════
namespace {
FILE*    g_tr = 0; bool g_trArm = false, g_trOn = false; unsigned g_trSeq = 0;
void TrPath(const char* leaf, char* out, size_t cap) {
    HMODULE hm = 0;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&TrPath, &hm);
    char dir[MAX_PATH]; dir[0]=0; GetModuleFileNameA(hm, dir, MAX_PATH);
    char* sl = strrchr(dir, 92); if (sl) *(sl+1)=0; else dir[0]=0;
    _snprintf(out, cap, "%s%s", dir, leaf); out[cap-1]=0;
}
const char* TrFmt(D3DFORMAT f) {
    switch ((int)f) {
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";      case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_A16B16G16R16F: return "A16B16G16R16F(HDR)";
    case D3DFMT_A32B32G32R32F: return "A32B32G32R32F(HDR)";
    case D3DFMT_R16F: return "R16F(lum)";          case D3DFMT_R32F: return "R32F(lum)";
    case D3DFMT_G16R16F: return "G16R16F";         case D3DFMT_A2R10G10B10: return "A2R10G10B10";
    case D3DFMT_D24S8: return "D24S8";             case D3DFMT_D24X8: return "D24X8";
    case D3DFMT_DXT1: return "DXT1";               case D3DFMT_DXT5: return "DXT5";
    default: return "fmt"; }
}
void TrLog(const char* f, ...) {
    if (!g_trOn || !g_tr) return;
    va_list a; va_start(a,f); fprintf(g_tr, "%6u  ", ++g_trSeq);
    vfprintf(g_tr,f,a); fputc('\n',g_tr); va_end(a);
}
bool TrHotVS(UINT r){ return r<=11u || (r>=178u&&r<=191u) || (r>=239u&&r<=246u); }
void TrMarkLoaded(){ char p[MAX_PATH]; TrPath("retail_proxy_loaded.txt",p,sizeof(p));
    FILE* f=fopen(p,"w"); if(f){ fprintf(f,"D3D9Proxy tracer active. F11 dumps one frame.\n"); fclose(f);} }
// ═══════════════════════════════════════════════════════════════════════════
// 08-07 ONE-SHOT FRAME CAPTURE — "catch them all" in a single F11.
//
// THE GAP THIS CLOSES. The old trace logged 1910 draws and 3926 PS-constant
// uploads in the same file, but nothing tied them together: a draw line carried
// only prim counts, so you could never say WHICH shader, WHICH textures or
// WHICH constants produced it. Every feature investigation then needed its own
// bespoke probe. Terrain only fell because its contract happened to be readable
// from the shader .asm instead.
//
// THE FIX IS TO SHADOW THE STATE AND RESOLVE IT AT THE DRAW. We mirror every
// binding the game sets (shaders, decl, streams, indices, all 16 samplers, the
// render states that decide a lane, and the full c0-c255 VS+PS constant files),
// then at each Draw emit ONE record with all of it resolved. Nothing has to be
// correlated by hand afterwards.
//
// WHAT ONE F11 NOW YIELDS:
//   DRAW#   — seq, RT, prim type/count, vert range, VS/PS/decl ids, stride
//   SAMP    — s0..s15 texture pointers for that draw (only the bound ones)
//   RS      — alphablend / zenable / zwrite / zfunc / cull / colorwrite
//   BANKVS/BANKPS — the ENTIRE 256-register constant file, dumped once at frame
//                   end: every light, every matrix, every lane's inputs
// The banks are what make this exhaustive rather than selective — no more
// "hot register" allow-list deciding in advance what we were allowed to learn.
//
// SIZE: ~1910 draws x ~2 lines + 2 x 256 bank rows ~= a few MB of text. Fine
// for a one-shot, and the reason this is F11-gated rather than always-on.
// ═══════════════════════════════════════════════════════════════════════════
IDirect3DBaseTexture9*      g_trTex[16]  = {0};
IDirect3DVertexShader9*     g_trVS       = 0;
IDirect3DPixelShader9*      g_trPS       = 0;
IDirect3DVertexDeclaration9*g_trDecl     = 0;
IDirect3DSurface9*          g_trRT       = 0;
UINT                        g_trStride0  = 0;
IDirect3DVertexBuffer9*     g_trVB0      = 0;
IDirect3DIndexBuffer9*      g_trIB       = 0;
DWORD                       g_trRS[8]    = {0};   // see kTrRSName
float                       g_trVSBank[256*4] = {0};
float                       g_trPSBank[256*4] = {0};
unsigned                    g_trDrawNo   = 0;
// ── WORLD-DRAW SNAPSHOT ─────────────────────────────────────────────────
// Dumping the banks at FRAME END was wrong: the HUD/UI pass runs last and
// rewrites c239-242 to IDENTITY and clears the light registers, so the tail
// state describes the interface, not the world. Measured proof in the first
// one-shot dump: VS c239-242 came out identity and PS c1/c2/c3 came out zero
// on a frame that plainly had a lit 3D scene in it.
// So ALSO snapshot both banks at the first genuine WORLD draw — identified by
// a non-identity viewProj at c239 — and dump that alongside. The snapshot is
// the one to trust for lighting/camera; the frame-end dump is kept because it
// is the honest record of what the UI pass leaves behind.
float                       g_trVSSnap[256*4] = {0};
float                       g_trPSSnap[256*4] = {0};
unsigned                    g_trSnapDraw = 0;   // 0 = never taken
bool TrViewProjIsReal() {
    const float* m = g_trVSBank + 239 * 4;
    // identity (the UI's value) or all-zero (never written) are both 'not world'
    bool ident = (m[0]==1.f&&m[1]==0.f&&m[2]==0.f&&m[3]==0.f);
    bool zero  = true;
    for (int i = 0; i < 16; ++i) if (m[i] != 0.0f) { zero = false; break; }
    return !ident && !zero;
}
const char* kTrRSName[8] = { "ablend","zen","zwrite","zfunc","cull","cw","srcbl","dstbl" };

// ═══════════════════════════════════════════════════════════════════════════
// LANE INVENTORY — every distinct render state the world has used, ALWAYS ON.
//
// WHY THIS EXISTS. A one-shot frame dump only ever contains what the frustum
// happened to hold, so answering 'how does water/sky/lava set up' meant flying
// somewhere it was visible and pressing F11 again — once per feature, with no
// way to know when the set was complete. That is backwards: the game has
// already rendered all of it, we were just throwing the states away between
// frames.
//
// So accumulate instead of sample. A LANE is one distinct (VS, PS, decl)
// triple — in practice one material/shader path: terrain base, terrain L2,
// water, skydome, a particle blend, the HUD. The FIRST draw of each lane
// captures its full context (samplers, render state, and the entire PS
// constant file as it stood for that draw); later draws only bump a counter.
// Runs from launch, costs one linear scan of a small table per draw, and F11
// writes the whole inventory out at once.
//
// The result is a complete census of how the engine configures every visual
// system, gathered by simply PLAYING rather than by aiming a camera.
// ═══════════════════════════════════════════════════════════════════════════
#define TR_MAX_LANES 192
struct TrLane {
    void*    vs; void* ps; void* decl;
    unsigned stride, primType, draws, firstDrawNo;
    void*    tex[16];
    DWORD    rs[8];
    float    psBank[256*4];   // full PS constant file at this lane's first draw
    float    vsCam[8*4];      // VS c239..c246 (viewProj + camera basis)
    char     psName[64];      // shader identity, read from its own bytecode
    char     vsName[64];
};
TrLane   g_trLanes[TR_MAX_LANES];
unsigned g_trLaneCount = 0;
unsigned g_trLaneOverflow = 0;

// Record this draw against its lane, capturing context the first time.
// ── SELF-LABELLING LANES ────────────────────────────────────────────────
// Shader POINTERS are not stable across launches, so a lane dumped today
// cannot be matched to a name harvested yesterday — which is exactly why
// LANE 20 could not be identified. Every D3D9 shader carries its own source
// path in its bytecode (".../shaders/Mg_FP_Terrain3_L2_....final"), and
// IDirect3DPixelShader9::GetFunction hands us that bytecode. So each lane
// now labels ITSELF at capture and the dump is readable on its own terms.
// GetFunction is a resource query, not device state, so it works fine on
// this game's PURE device (unlike every Get*State call).
void TrShaderName(void* shader, char* out, int cap) {
    typedef long (__stdcall* FnGetFunc)(void*, void*, UINT*);
    out[0] = 0;
    if (!shader) return;
    void** vt = *(void***)shader;
    UINT sz = 0;
    if (FAILED(((FnGetFunc)vt[4])(shader, 0, &sz)) || !sz || sz > 65536u) return;
    static unsigned char buf[65536];
    if (FAILED(((FnGetFunc)vt[4])(shader, buf, &sz))) return;
    // find the last path separator before ".final"
    for (UINT i = 0; i + 6 < sz; ++i) {
        if (memcmp(buf + i, ".final", 6) != 0) continue;
        UINT b = i;
        while (b > 0 && buf[b-1] != 0x5c && buf[b-1] != 0x2f) --b;   // 0x5c=backslash 0x2f=slash
        int n = (int)(i - b); if (n <= 0) return; if (n > cap - 1) n = cap - 1;
        memcpy(out, buf + b, n); out[n] = 0;
        return;
    }
}

// Pull the live PS/VS constant files straight off the device. Overwrites the
// mirrored copy, so a register set by a state block still lands in the dump.
void TrReadBackConsts(void* dev, TrLane* L) {
    typedef long (__stdcall* FnGetPSC)(void*, unsigned, float*, unsigned);
    typedef long (__stdcall* FnGetVSC)(void*, unsigned, float*, unsigned);
    if (!dev || !L) return;
    void** vt = *(void***)dev;
    // GetPixelShaderConstantF = slot 111 (0x1bc), GetVertexShaderConstantF = 95 (0x17c)
    // ⛔ DISABLED 08-07. This game creates its device with BehaviorFlags 0x44
    // = HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE, and a PURE device
    // does not track state: every Get* returns zeros. Reading back therefore
    // OVERWROTE the correct mirrored bank with zeros and blanked p1/p2/p3 and
    // every sampler in the dump. The mirror is the only viable capture path
    // here — the game does write these registers directly
    // (SetPixelShaderConstantF(1,n=4) x12, (2,n=2) x34 per frame), so the
    // mirror sees them. Kept as a labelled no-op rather than deleted, so this
    // does not get re-attempted.
    (void)vt; (void)dev; (void)L;
    // ((FnGetPSC)vt[111])(dev, 0, L->psBank, 256);   // PURE DEVICE -> zeros
    // ((FnGetVSC)vt[95]) (dev, 239, L->vsCam, 8);    // PURE DEVICE -> zeros
}

// ★ 08-07 READ BACK, DO NOT MIRROR. The bank we assemble from
// SetPixelShaderConstantF is structurally blind to anything applied via a
// D3D9 STATE BLOCK, and this engine sets its lighting that way
// (Mg_0068b899_ApplyStateBlock). Proof from retail_trace_016: terrain LANE 20
// shows p212 = 0.0040 (set directly by the material apply, so the mirror sees
// it) while p1/p2/p3 read ZERO — impossible for a visibly lit scene. Ask the
// device for the live values instead; GetPixelShaderConstantF returns whatever
// is actually bound, no matter who bound it.
void TrLaneNote(void* realDev, unsigned primType) {
    unsigned i;
    for (i = 0; i < g_trLaneCount; ++i) {
        TrLane* L = &g_trLanes[i];
        if (L->vs == (void*)g_trVS && L->ps == (void*)g_trPS && L->decl == (void*)g_trDecl) {
            ++L->draws;
            // ── REFRESH A STALE CAPTURE ───────────────────────────────────
            // A lane banks its constants at its FIRST EVER draw, which for
            // terrain is frame one — before the atmosphere lane has run. Both
            // retail dumps came back with p1/p2/p3 empty for LANE 20 as a
            // result, so they could not arbitrate our ambient/sunCol/sunDir
            // question at all. If this lane was captured with an unlit c1 and
            // lighting has since arrived, re-take the snapshot ONCE.
            {
                const float* had = L->psBank + 1 * 4;
                const float* now = g_trPSBank + 1 * 4;
                const bool wasDark = (had[0]==0.f && had[1]==0.f && had[2]==0.f);
                const bool nowLit  = (now[0]!=0.f || now[1]!=0.f || now[2]!=0.f);
                if (wasDark && nowLit) {
                    memcpy(L->psBank, g_trPSBank, sizeof(L->psBank));
                    memcpy(L->vsCam,  g_trVSBank + 239 * 4, sizeof(L->vsCam));
                    TrShaderName((void*)g_trPS, L->psName, sizeof(L->psName));
                    TrShaderName((void*)g_trVS, L->vsName, sizeof(L->vsName));
                    for (int k = 0; k < 16; ++k) L->tex[k] = (void*)g_trTex[k];
                }
            }
            return;
        }
    }
    if (g_trLaneCount >= TR_MAX_LANES) { ++g_trLaneOverflow; return; }
    {
        TrLane* L = &g_trLanes[g_trLaneCount++];
        int k;
        L->vs = (void*)g_trVS; L->ps = (void*)g_trPS; L->decl = (void*)g_trDecl;
        L->stride = g_trStride0; L->primType = primType;
        L->draws = 1; L->firstDrawNo = g_trDrawNo;
        for (k = 0; k < 16; ++k) L->tex[k] = (void*)g_trTex[k];
        for (k = 0; k < 8;  ++k) L->rs[k]  = g_trRS[k];
        memcpy(L->psBank, g_trPSBank, sizeof(L->psBank));
        memcpy(L->vsCam,  g_trVSBank + 239 * 4, sizeof(L->vsCam));
        TrShaderName((void*)g_trPS, L->psName, sizeof(L->psName));
        TrShaderName((void*)g_trVS, L->vsName, sizeof(L->vsName));
    }
}

// Write the whole inventory. Called when a trace file is open (F11).
void TrDumpLanes() {
    unsigned i; int r, k;
    if (!g_tr) return;
    fprintf(g_tr, "==== LANE INVENTORY: %u distinct (vs,ps,decl) states since launch\n", g_trLaneCount);
    if (g_trLaneOverflow)
        fprintf(g_tr, "     WARNING %u draws hit lanes past the %u cap and were NOT recorded\n",
                g_trLaneOverflow, (unsigned)TR_MAX_LANES);
    for (i = 0; i < g_trLaneCount; ++i) {
        TrLane* L = &g_trLanes[i];
        fprintf(g_tr, "---- LANE %u  vs=%p ps=%p decl=%p stride=%u pt=%u draws=%u first=#%u\n",
                i, L->vs, L->ps, L->decl, L->stride, L->primType, L->draws, L->firstDrawNo);
        fprintf(g_tr, "     NAME vs=%s ps=%s\n",
                L->vsName[0] ? L->vsName : "?", L->psName[0] ? L->psName : "?");
        fprintf(g_tr, "     SAMP");
        for (k = 0; k < 16; ++k) if (L->tex[k]) fprintf(g_tr, " s%d=%p", k, L->tex[k]);
        fprintf(g_tr, "\n     RS  ");
        for (k = 0; k < 8; ++k) fprintf(g_tr, " %s=%u", kTrRSName[k], L->rs[k]);
        fprintf(g_tr, "\n     VS c239..c246 (viewProj + camera basis)\n");
        for (r = 0; r < 8; ++r) {
            const float* v = L->vsCam + r * 4;
            fprintf(g_tr, "       c%-3d = [%12.5f %12.5f %12.5f %12.5f]\n",
                    239 + r, v[0], v[1], v[2], v[3]);
        }
        fprintf(g_tr, "     PS constants (non-zero)\n");
        for (r = 0; r < 256; ++r) {
            const float* v = L->psBank + r * 4;
            if (v[0]==0.0f && v[1]==0.0f && v[2]==0.0f && v[3]==0.0f) continue;
            fprintf(g_tr, "       p%-3d = [%12.5f %12.5f %12.5f %12.5f]\n",
                    r, v[0], v[1], v[2], v[3]);
        }
    }
}

// Emit the fully-resolved record for one draw. Called from both Draw paths.
void TrDrawRecord(const char* kind, int primType, int baseVtx, unsigned minVtx,
                  unsigned numVtx, unsigned startIdx, unsigned prims)
{
    if (!g_trOn || !g_tr) return;
    ++g_trDrawNo;
    if (!g_trSnapDraw && TrViewProjIsReal()) {   // first real world draw
        memcpy(g_trVSSnap, g_trVSBank, sizeof(g_trVSSnap));
        memcpy(g_trPSSnap, g_trPSBank, sizeof(g_trPSSnap));
        g_trSnapDraw = g_trDrawNo;
    }
    TrLog("DRAW#%-5u %-4s rt=%p pt=%d prims=%u base=%d min=%u nverts=%u start=%u "
          "vs=%p ps=%p decl=%p vb=%p ib=%p stride=%u",
          g_trDrawNo, kind, (void*)g_trRT, primType, prims, baseVtx, minVtx,
          numVtx, startIdx, (void*)g_trVS, (void*)g_trPS, (void*)g_trDecl,
          (void*)g_trVB0, (void*)g_trIB, g_trStride0);
    // Samplers — only the bound ones, so terrain's s0/s3/s8/s11 pattern reads
    // straight off the line instead of being reconstructed from a call stream.
    {
        char b[512]; int n = 0; b[0] = 0;
        for (int s = 0; s < 16; ++s)
            if (g_trTex[s] && n < (int)sizeof(b) - 24)
                n += _snprintf(b + n, sizeof(b) - n, "s%d=%p ", s, (void*)g_trTex[s]);
        if (n > 0) TrLog("      SAMP %s", b);
    }
    TrLog("      RS   %s=%u %s=%u %s=%u %s=%u %s=%u %s=0x%x %s=%u %s=%u",
          kTrRSName[0], g_trRS[0], kTrRSName[1], g_trRS[1],
          kTrRSName[2], g_trRS[2], kTrRSName[3], g_trRS[3],
          kTrRSName[4], g_trRS[4], kTrRSName[5], g_trRS[5],
          kTrRSName[6], g_trRS[6], kTrRSName[7], g_trRS[7]);
}

// Dump a whole 256-register constant file. Zero rows are skipped so the useful
// content is not buried — a register absent from the dump IS the finding.
void TrDumpBank(const char* tag, const float* bank) {
    if (!g_tr) return;
    fprintf(g_tr, "==== BANK%s (non-zero registers of c0..c255) ====\n", tag);
    for (int r = 0; r < 256; ++r) {
        const float* v = bank + r * 4;
        if (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 0.0f) continue;
        fprintf(g_tr, "  %s c%-3d = [%12.5f %12.5f %12.5f %12.5f]\n",
                tag, r, v[0], v[1], v[2], v[3]);
    }
}

void TrFrameEdge(){
    if (g_trOn) { g_trOn=false;
        if(g_tr){
            TrDumpLanes();
            if (g_trSnapDraw) {
                fprintf(g_tr, "==== WORLD SNAPSHOT (taken at DRAW#%u) ====\n", g_trSnapDraw);
                TrDumpBank("wVS", g_trVSSnap);
                TrDumpBank("wPS", g_trPSSnap);
            } else {
                fprintf(g_tr, "==== WORLD SNAPSHOT: NEVER TAKEN (no non-identity c239 seen) ====\n");
            }
            fprintf(g_tr, "---- frame-end state (UI pass has overwritten much of this) ----\n");
            TrDumpBank("VS", g_trVSBank);
            TrDumpBank("PS", g_trPSBank);
            fprintf(g_tr,"==== frame end (%u calls, %u draws) ====\n",g_trSeq,g_trDrawNo);
            fclose(g_tr); g_tr=0; } }
    else if (g_trArm) { g_trArm=false;
        // ── ONE FILE PER PRESS ──────────────────────────────────────────
        // This used to always open "retail_trace.log" with mode "w", so every
        // F11 silently DESTROYED the previous capture. Anyone dumping several
        // vantage points (terrain, then water, then sky) kept only the last
        // one and had no way to know the others had gone. Number them instead;
        // the sequence also records the order they were taken in.
        // The counter is per-PROCESS, so a fresh launch used to restart at 001
        // and overwrite every dump from the previous session. Skip past any
        // file that already exists, so captures accumulate across launches.
        static unsigned s_trFileNo = 0;
        char leaf[64]; char p[MAX_PATH];
        for (;;) {
            _snprintf(leaf, sizeof(leaf), "retail_trace_%03u.log", ++s_trFileNo);
            leaf[sizeof(leaf)-1] = 0;
            TrPath(leaf,p,sizeof(p));
            if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) break;
            if (s_trFileNo > 900u) break;   // give up rather than spin
        }
        g_tr=fopen(p,"w"); if(g_tr){ g_trSeq=0; g_trDrawNo=0; g_trSnapDraw=0; g_trOn=true;
            fprintf(g_tr,"==== RETAIL FRAME TRACE #%u (one-shot, resolved per draw) ====\n", s_trFileNo); } }
    // EDGE-triggered (&1), not level-triggered (&0x8000): holding F11 used to
    // re-arm on every frame edge and dump continuously. One press, one file.
    if (GetAsyncKeyState(VK_F11) & 1) g_trArm = true;
}
}  // namespace

// ── LIFECYCLE LOG (always on): teardowns, device resets, level switches ─────
// Separate from the one-frame tracer on purpose. The frame trace answers "what
// did this draw look like"; this answers "what happened to the device and when"
// — shell -> level, level -> level, and every teardown in between. Retail's
// ordering here is the reference for our own load/unload path.
namespace {
FILE* g_lf = 0;
unsigned g_lfFrames = 0, g_lfTexThisPhase = 0;
void LfOpen(){ if(!g_lf){ char p[MAX_PATH]; TrPath("retail_lifecycle.log",p,sizeof(p)); g_lf=fopen(p,"w"); } }
void LfLog(const char* f, ...) {
    LfOpen(); if(!g_lf) return;
    va_list a; va_start(a,f); fprintf(g_lf,"[f%06u] ",g_lfFrames); vfprintf(g_lf,f,a);
    fputc('\n',g_lf); fflush(g_lf); va_end(a);
}
// A level switch shows up as a BURST of resource creation after a quiet spell.
// Report the burst rather than every texture, so the log stays readable.
void LfNoteTexture(){ ++g_lfTexThisPhase; }
void LfFrameTick(){
    ++g_lfFrames;
    if (g_lfTexThisPhase >= 32u) { LfLog("LOAD BURST: %u textures created since last frame  <-- level/shell switch", g_lfTexThisPhase); }
    g_lfTexThisPhase = 0;
}
}  // namespace
static std::set<void*> g_dumpedShaderPtrs;

// ═══════════════════════════════════════════════════════════════════════════
//  ENUM-TO-STRING HELPERS (switch statements returning string literals)
// ═══════════════════════════════════════════════════════════════════════════

static const char* RenderStateName(D3DRENDERSTATETYPE s) {
    switch (s) {
    case D3DRS_ZENABLE: return "D3DRS_ZENABLE";
    case D3DRS_FILLMODE: return "D3DRS_FILLMODE";
    case D3DRS_SHADEMODE: return "D3DRS_SHADEMODE";
    case D3DRS_ZWRITEENABLE: return "D3DRS_ZWRITEENABLE";
    case D3DRS_ALPHATESTENABLE: return "D3DRS_ALPHATESTENABLE";
    case D3DRS_LASTPIXEL: return "D3DRS_LASTPIXEL";
    case D3DRS_SRCBLEND: return "D3DRS_SRCBLEND";
    case D3DRS_DESTBLEND: return "D3DRS_DESTBLEND";
    case D3DRS_CULLMODE: return "D3DRS_CULLMODE";
    case D3DRS_ZFUNC: return "D3DRS_ZFUNC";
    case D3DRS_ALPHAREF: return "D3DRS_ALPHAREF";
    case D3DRS_ALPHAFUNC: return "D3DRS_ALPHAFUNC";
    case D3DRS_DITHERENABLE: return "D3DRS_DITHERENABLE";
    case D3DRS_ALPHABLENDENABLE: return "D3DRS_ALPHABLENDENABLE";
    case D3DRS_FOGENABLE: return "D3DRS_FOGENABLE";
    case D3DRS_SPECULARENABLE: return "D3DRS_SPECULARENABLE";
    case D3DRS_FOGCOLOR: return "D3DRS_FOGCOLOR";
    case D3DRS_FOGTABLEMODE: return "D3DRS_FOGTABLEMODE";
    case D3DRS_FOGSTART: return "D3DRS_FOGSTART";
    case D3DRS_FOGEND: return "D3DRS_FOGEND";
    case D3DRS_FOGDENSITY: return "D3DRS_FOGDENSITY";
    case D3DRS_RANGEFOGENABLE: return "D3DRS_RANGEFOGENABLE";
    case D3DRS_STENCILENABLE: return "D3DRS_STENCILENABLE";
    case D3DRS_STENCILFAIL: return "D3DRS_STENCILFAIL";
    case D3DRS_STENCILZFAIL: return "D3DRS_STENCILZFAIL";
    case D3DRS_STENCILPASS: return "D3DRS_STENCILPASS";
    case D3DRS_STENCILFUNC: return "D3DRS_STENCILFUNC";
    case D3DRS_STENCILREF: return "D3DRS_STENCILREF";
    case D3DRS_STENCILMASK: return "D3DRS_STENCILMASK";
    case D3DRS_STENCILWRITEMASK: return "D3DRS_STENCILWRITEMASK";
    case D3DRS_TEXTUREFACTOR: return "D3DRS_TEXTUREFACTOR";
    case D3DRS_WRAP0: return "D3DRS_WRAP0";
    case D3DRS_WRAP1: return "D3DRS_WRAP1";
    case D3DRS_WRAP2: return "D3DRS_WRAP2";
    case D3DRS_WRAP3: return "D3DRS_WRAP3";
    case D3DRS_CLIPPING: return "D3DRS_CLIPPING";
    case D3DRS_LIGHTING: return "D3DRS_LIGHTING";
    case D3DRS_AMBIENT: return "D3DRS_AMBIENT";
    case D3DRS_FOGVERTEXMODE: return "D3DRS_FOGVERTEXMODE";
    case D3DRS_COLORVERTEX: return "D3DRS_COLORVERTEX";
    case D3DRS_LOCALVIEWER: return "D3DRS_LOCALVIEWER";
    case D3DRS_NORMALIZENORMALS: return "D3DRS_NORMALIZENORMALS";
    case D3DRS_DIFFUSEMATERIALSOURCE: return "D3DRS_DIFFUSEMATERIALSOURCE";
    case D3DRS_SPECULARMATERIALSOURCE: return "D3DRS_SPECULARMATERIALSOURCE";
    case D3DRS_AMBIENTMATERIALSOURCE: return "D3DRS_AMBIENTMATERIALSOURCE";
    case D3DRS_EMISSIVEMATERIALSOURCE: return "D3DRS_EMISSIVEMATERIALSOURCE";
    case D3DRS_VERTEXBLEND: return "D3DRS_VERTEXBLEND";
    case D3DRS_CLIPPLANEENABLE: return "D3DRS_CLIPPLANEENABLE";
    case D3DRS_POINTSIZE: return "D3DRS_POINTSIZE";
    case D3DRS_POINTSIZE_MIN: return "D3DRS_POINTSIZE_MIN";
    case D3DRS_POINTSPRITEENABLE: return "D3DRS_POINTSPRITEENABLE";
    case D3DRS_POINTSCALEENABLE: return "D3DRS_POINTSCALEENABLE";
    case D3DRS_POINTSCALE_A: return "D3DRS_POINTSCALE_A";
    case D3DRS_POINTSCALE_B: return "D3DRS_POINTSCALE_B";
    case D3DRS_POINTSCALE_C: return "D3DRS_POINTSCALE_C";
    case D3DRS_MULTISAMPLEANTIALIAS: return "D3DRS_MULTISAMPLEANTIALIAS";
    case D3DRS_MULTISAMPLEMASK: return "D3DRS_MULTISAMPLEMASK";
    case D3DRS_PATCHEDGESTYLE: return "D3DRS_PATCHEDGESTYLE";
    case D3DRS_DEBUGMONITORTOKEN: return "D3DRS_DEBUGMONITORTOKEN";
    case D3DRS_POINTSIZE_MAX: return "D3DRS_POINTSIZE_MAX";
    case D3DRS_INDEXEDVERTEXBLENDENABLE: return "D3DRS_INDEXEDVERTEXBLENDENABLE";
    case D3DRS_COLORWRITEENABLE: return "D3DRS_COLORWRITEENABLE";
    case D3DRS_TWEENFACTOR: return "D3DRS_TWEENFACTOR";
    case D3DRS_BLENDOP: return "D3DRS_BLENDOP";
    case D3DRS_POSITIONDEGREE: return "D3DRS_POSITIONDEGREE";
    case D3DRS_NORMALDEGREE: return "D3DRS_NORMALDEGREE";
    case D3DRS_SCISSORTESTENABLE: return "D3DRS_SCISSORTESTENABLE";
    case D3DRS_SLOPESCALEDEPTHBIAS: return "D3DRS_SLOPESCALEDEPTHBIAS";
    case D3DRS_ANTIALIASEDLINEENABLE: return "D3DRS_ANTIALIASEDLINEENABLE";
    case D3DRS_MINTESSELLATIONLEVEL: return "D3DRS_MINTESSELLATIONLEVEL";
    case D3DRS_MAXTESSELLATIONLEVEL: return "D3DRS_MAXTESSELLATIONLEVEL";
    case D3DRS_ADAPTIVETESS_X: return "D3DRS_ADAPTIVETESS_X";
    case D3DRS_ADAPTIVETESS_Y: return "D3DRS_ADAPTIVETESS_Y";
    case D3DRS_ADAPTIVETESS_Z: return "D3DRS_ADAPTIVETESS_Z";
    case D3DRS_ADAPTIVETESS_W: return "D3DRS_ADAPTIVETESS_W";
    case D3DRS_ENABLEADAPTIVETESSELLATION: return "D3DRS_ENABLEADAPTIVETESSELLATION";
    case D3DRS_TWOSIDEDSTENCILMODE: return "D3DRS_TWOSIDEDSTENCILMODE";
    case D3DRS_CCW_STENCILFAIL: return "D3DRS_CCW_STENCILFAIL";
    case D3DRS_CCW_STENCILZFAIL: return "D3DRS_CCW_STENCILZFAIL";
    case D3DRS_CCW_STENCILPASS: return "D3DRS_CCW_STENCILPASS";
    case D3DRS_CCW_STENCILFUNC: return "D3DRS_CCW_STENCILFUNC";
    case D3DRS_COLORWRITEENABLE1: return "D3DRS_COLORWRITEENABLE1";
    case D3DRS_COLORWRITEENABLE2: return "D3DRS_COLORWRITEENABLE2";
    case D3DRS_COLORWRITEENABLE3: return "D3DRS_COLORWRITEENABLE3";
    case D3DRS_BLENDFACTOR: return "D3DRS_BLENDFACTOR";
    case D3DRS_SRGBWRITEENABLE: return "D3DRS_SRGBWRITEENABLE";
    case D3DRS_DEPTHBIAS: return "D3DRS_DEPTHBIAS";
    case D3DRS_WRAP8: return "D3DRS_WRAP8";
    case D3DRS_WRAP9: return "D3DRS_WRAP9";
    case D3DRS_WRAP10: return "D3DRS_WRAP10";
    case D3DRS_WRAP11: return "D3DRS_WRAP11";
    case D3DRS_WRAP12: return "D3DRS_WRAP12";
    case D3DRS_WRAP13: return "D3DRS_WRAP13";
    case D3DRS_WRAP14: return "D3DRS_WRAP14";
    case D3DRS_WRAP15: return "D3DRS_WRAP15";
    case D3DRS_SEPARATEALPHABLENDENABLE: return "D3DRS_SEPARATEALPHABLENDENABLE";
    case D3DRS_SRCBLENDALPHA: return "D3DRS_SRCBLENDALPHA";
    case D3DRS_DESTBLENDALPHA: return "D3DRS_DESTBLENDALPHA";
    case D3DRS_BLENDOPALPHA: return "D3DRS_BLENDOPALPHA";
    default: return "D3DRS_UNKNOWN";
    }
}

static const char* FogModeName(DWORD v) {
    switch (v) {
    case D3DFOG_NONE: return "D3DFOG_NONE";
    case D3DFOG_EXP: return "D3DFOG_EXP";
    case D3DFOG_EXP2: return "D3DFOG_EXP2";
    case D3DFOG_LINEAR: return "D3DFOG_LINEAR";
    default: return "UNKNOWN";
    }
}

static const char* LightTypeName(DWORD v) {
    switch (v) {
    case D3DLIGHT_POINT: return "D3DLIGHT_POINT";
    case D3DLIGHT_SPOT: return "D3DLIGHT_SPOT";
    case D3DLIGHT_DIRECTIONAL: return "D3DLIGHT_DIRECTIONAL";
    default: return "UNKNOWN";
    }
}

static const char* BlendName(DWORD v) {
    switch (v) {
    case D3DBLEND_ZERO: return "D3DBLEND_ZERO";
    case D3DBLEND_ONE: return "D3DBLEND_ONE";
    case D3DBLEND_SRCCOLOR: return "D3DBLEND_SRCCOLOR";
    case D3DBLEND_INVSRCCOLOR: return "D3DBLEND_INVSRCCOLOR";
    case D3DBLEND_SRCALPHA: return "D3DBLEND_SRCALPHA";
    case D3DBLEND_INVSRCALPHA: return "D3DBLEND_INVSRCALPHA";
    case D3DBLEND_DESTALPHA: return "D3DBLEND_DESTALPHA";
    case D3DBLEND_INVDESTALPHA: return "D3DBLEND_INVDESTALPHA";
    case D3DBLEND_DESTCOLOR: return "D3DBLEND_DESTCOLOR";
    case D3DBLEND_INVDESTCOLOR: return "D3DBLEND_INVDESTCOLOR";
    case D3DBLEND_SRCALPHASAT: return "D3DBLEND_SRCALPHASAT";
    case D3DBLEND_BOTHSRCALPHA: return "D3DBLEND_BOTHSRCALPHA";
    case D3DBLEND_BOTHINVSRCALPHA: return "D3DBLEND_BOTHINVSRCALPHA";
    case D3DBLEND_BLENDFACTOR: return "D3DBLEND_BLENDFACTOR";
    case D3DBLEND_INVBLENDFACTOR: return "D3DBLEND_INVBLENDFACTOR";
    default: return "UNKNOWN";
    }
}

static const char* TextureOpName(DWORD v) {
    switch (v) {
    case D3DTOP_DISABLE: return "D3DTOP_DISABLE";
    case D3DTOP_SELECTARG1: return "D3DTOP_SELECTARG1";
    case D3DTOP_SELECTARG2: return "D3DTOP_SELECTARG2";
    case D3DTOP_MODULATE: return "D3DTOP_MODULATE";
    case D3DTOP_MODULATE2X: return "D3DTOP_MODULATE2X";
    case D3DTOP_MODULATE4X: return "D3DTOP_MODULATE4X";
    case D3DTOP_ADD: return "D3DTOP_ADD";
    case D3DTOP_ADDSIGNED: return "D3DTOP_ADDSIGNED";
    case D3DTOP_ADDSIGNED2X: return "D3DTOP_ADDSIGNED2X";
    case D3DTOP_SUBTRACT: return "D3DTOP_SUBTRACT";
    case D3DTOP_ADDSMOOTH: return "D3DTOP_ADDSMOOTH";
    case D3DTOP_BLENDDIFFUSEALPHA: return "D3DTOP_BLENDDIFFUSEALPHA";
    case D3DTOP_BLENDTEXTUREALPHA: return "D3DTOP_BLENDTEXTUREALPHA";
    case D3DTOP_BLENDFACTORALPHA: return "D3DTOP_BLENDFACTORALPHA";
    case D3DTOP_BLENDTEXTUREALPHAPM: return "D3DTOP_BLENDTEXTUREALPHAPM";
    case D3DTOP_BLENDCURRENTALPHA: return "D3DTOP_BLENDCURRENTALPHA";
    case D3DTOP_PREMODULATE: return "D3DTOP_PREMODULATE";
    case D3DTOP_MODULATEALPHA_ADDCOLOR: return "D3DTOP_MODULATEALPHA_ADDCOLOR";
    case D3DTOP_MODULATECOLOR_ADDALPHA: return "D3DTOP_MODULATECOLOR_ADDALPHA";
    case D3DTOP_MODULATEINVALPHA_ADDCOLOR: return "D3DTOP_MODULATEINVALPHA_ADDCOLOR";
    case D3DTOP_MODULATEINVCOLOR_ADDALPHA: return "D3DTOP_MODULATEINVCOLOR_ADDALPHA";
    case D3DTOP_BUMPENVMAP: return "D3DTOP_BUMPENVMAP";
    case D3DTOP_BUMPENVMAPLUMINANCE: return "D3DTOP_BUMPENVMAPLUMINANCE";
    case D3DTOP_DOTPRODUCT3: return "D3DTOP_DOTPRODUCT3";
    case D3DTOP_MULTIPLYADD: return "D3DTOP_MULTIPLYADD";
    case D3DTOP_LERP: return "D3DTOP_LERP";
    default: return "UNKNOWN";
    }
}

static const char* TextureArgName(DWORD v) {
    // Strip complement/alphareplicate flags for base name
    DWORD base = v & D3DTA_SELECTMASK;
    const char* name = "UNKNOWN";
    switch (base) {
    case D3DTA_DIFFUSE: name = "D3DTA_DIFFUSE"; break;
    case D3DTA_CURRENT: name = "D3DTA_CURRENT"; break;
    case D3DTA_TEXTURE: name = "D3DTA_TEXTURE"; break;
    case D3DTA_TFACTOR: name = "D3DTA_TFACTOR"; break;
    case D3DTA_SPECULAR: name = "D3DTA_SPECULAR"; break;
    case D3DTA_TEMP: name = "D3DTA_TEMP"; break;
    case D3DTA_CONSTANT: name = "D3DTA_CONSTANT"; break;
    }
    return name;
}

static const char* FilterName(DWORD v) {
    switch (v) {
    case D3DTEXF_NONE: return "D3DTEXF_NONE";
    case D3DTEXF_POINT: return "D3DTEXF_POINT";
    case D3DTEXF_LINEAR: return "D3DTEXF_LINEAR";
    case D3DTEXF_ANISOTROPIC: return "D3DTEXF_ANISOTROPIC";
    case D3DTEXF_PYRAMIDALQUAD: return "D3DTEXF_PYRAMIDALQUAD";
    case D3DTEXF_GAUSSIANQUAD: return "D3DTEXF_GAUSSIANQUAD";
    default: return "UNKNOWN";
    }
}

static const char* AddressName(DWORD v) {
    switch (v) {
    case D3DTADDRESS_WRAP: return "D3DTADDRESS_WRAP";
    case D3DTADDRESS_MIRROR: return "D3DTADDRESS_MIRROR";
    case D3DTADDRESS_CLAMP: return "D3DTADDRESS_CLAMP";
    case D3DTADDRESS_BORDER: return "D3DTADDRESS_BORDER";
    case D3DTADDRESS_MIRRORONCE: return "D3DTADDRESS_MIRRORONCE";
    default: return "UNKNOWN";
    }
}

static const char* CmpFuncName(DWORD v) {
    switch (v) {
    case D3DCMP_NEVER: return "D3DCMP_NEVER";
    case D3DCMP_LESS: return "D3DCMP_LESS";
    case D3DCMP_EQUAL: return "D3DCMP_EQUAL";
    case D3DCMP_LESSEQUAL: return "D3DCMP_LESSEQUAL";
    case D3DCMP_GREATER: return "D3DCMP_GREATER";
    case D3DCMP_NOTEQUAL: return "D3DCMP_NOTEQUAL";
    case D3DCMP_GREATEREQUAL: return "D3DCMP_GREATEREQUAL";
    case D3DCMP_ALWAYS: return "D3DCMP_ALWAYS";
    default: return "UNKNOWN";
    }
}

static const char* CullName(DWORD v) {
    switch (v) {
    case D3DCULL_NONE: return "D3DCULL_NONE";
    case D3DCULL_CW: return "D3DCULL_CW";
    case D3DCULL_CCW: return "D3DCULL_CCW";
    default: return "UNKNOWN";
    }
}

static const char* FillName(DWORD v) {
    switch (v) {
    case D3DFILL_POINT: return "D3DFILL_POINT";
    case D3DFILL_WIREFRAME: return "D3DFILL_WIREFRAME";
    case D3DFILL_SOLID: return "D3DFILL_SOLID";
    default: return "UNKNOWN";
    }
}

// Helper: is this render state a color (ARGB)?
static bool IsColorState(D3DRENDERSTATETYPE s) {
    return s == D3DRS_FOGCOLOR || s == D3DRS_AMBIENT || s == D3DRS_TEXTUREFACTOR || s == D3DRS_BLENDFACTOR;
}

// Helper: is this render state a float stored as DWORD?
static bool IsFloatState(D3DRENDERSTATETYPE s) {
    return s == D3DRS_FOGSTART || s == D3DRS_FOGEND || s == D3DRS_FOGDENSITY ||
           s == D3DRS_POINTSIZE || s == D3DRS_POINTSIZE_MIN || s == D3DRS_POINTSIZE_MAX ||
           s == D3DRS_POINTSCALE_A || s == D3DRS_POINTSCALE_B || s == D3DRS_POINTSCALE_C ||
           s == D3DRS_TWEENFACTOR || s == D3DRS_SLOPESCALEDEPTHBIAS || s == D3DRS_DEPTHBIAS ||
           s == D3DRS_ADAPTIVETESS_X || s == D3DRS_ADAPTIVETESS_Y ||
           s == D3DRS_ADAPTIVETESS_Z || s == D3DRS_ADAPTIVETESS_W ||
           s == D3DRS_MINTESSELLATIONLEVEL || s == D3DRS_MAXTESSELLATIONLEVEL;
}

// ═══════════════════════════════════════════════════════════════════════════
//  GAP 1: Full Render State Snapshot
// ═══════════════════════════════════════════════════════════════════════════
static void CaptureRenderStates(IDirect3DDevice9* dev, FILE* f) {
    fprintf(f, "\n[RENDER STATES]\n");
    static const D3DRENDERSTATETYPE states[] = {
        D3DRS_ZENABLE, D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZWRITEENABLE,
        D3DRS_ALPHATESTENABLE, D3DRS_LASTPIXEL, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
        D3DRS_CULLMODE, D3DRS_ZFUNC, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC,
        D3DRS_DITHERENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_FOGENABLE,
        D3DRS_SPECULARENABLE, D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE,
        D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_RANGEFOGENABLE,
        D3DRS_STENCILENABLE, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL,
        D3DRS_STENCILPASS, D3DRS_STENCILFUNC, D3DRS_STENCILREF,
        D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_TEXTUREFACTOR,
        D3DRS_WRAP0, D3DRS_WRAP1, D3DRS_WRAP2, D3DRS_WRAP3,
        D3DRS_CLIPPING, D3DRS_LIGHTING, D3DRS_AMBIENT,
        D3DRS_FOGVERTEXMODE, D3DRS_COLORVERTEX, D3DRS_LOCALVIEWER,
        D3DRS_NORMALIZENORMALS, D3DRS_DIFFUSEMATERIALSOURCE,
        D3DRS_SPECULARMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE,
        D3DRS_EMISSIVEMATERIALSOURCE, D3DRS_VERTEXBLEND, D3DRS_CLIPPLANEENABLE,
        D3DRS_POINTSIZE, D3DRS_POINTSIZE_MIN, D3DRS_POINTSPRITEENABLE,
        D3DRS_POINTSCALEENABLE, D3DRS_POINTSCALE_A, D3DRS_POINTSCALE_B,
        D3DRS_POINTSCALE_C, D3DRS_MULTISAMPLEANTIALIAS, D3DRS_MULTISAMPLEMASK,
        D3DRS_PATCHEDGESTYLE, D3DRS_DEBUGMONITORTOKEN, D3DRS_POINTSIZE_MAX,
        D3DRS_INDEXEDVERTEXBLENDENABLE, D3DRS_COLORWRITEENABLE,
        D3DRS_TWEENFACTOR, D3DRS_BLENDOP, D3DRS_POSITIONDEGREE,
        D3DRS_NORMALDEGREE, D3DRS_SCISSORTESTENABLE, D3DRS_SLOPESCALEDEPTHBIAS,
        D3DRS_ANTIALIASEDLINEENABLE, D3DRS_MINTESSELLATIONLEVEL,
        D3DRS_MAXTESSELLATIONLEVEL, D3DRS_ADAPTIVETESS_X, D3DRS_ADAPTIVETESS_Y,
        D3DRS_ADAPTIVETESS_Z, D3DRS_ADAPTIVETESS_W, D3DRS_ENABLEADAPTIVETESSELLATION,
        D3DRS_TWOSIDEDSTENCILMODE, D3DRS_CCW_STENCILFAIL, D3DRS_CCW_STENCILZFAIL,
        D3DRS_CCW_STENCILPASS, D3DRS_CCW_STENCILFUNC, D3DRS_COLORWRITEENABLE1,
        D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3, D3DRS_BLENDFACTOR,
        D3DRS_SRGBWRITEENABLE, D3DRS_DEPTHBIAS, D3DRS_WRAP8, D3DRS_WRAP9,
        D3DRS_WRAP10, D3DRS_WRAP11, D3DRS_WRAP12, D3DRS_WRAP13, D3DRS_WRAP14,
        D3DRS_WRAP15, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA,
        D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA
    };
    for (int i = 0; i < sizeof(states)/sizeof(states[0]); ++i) {
        DWORD val = 0;
        HRESULT hr = dev->GetRenderState(states[i], &val);
        if (FAILED(hr)) {
            fprintf(f, "%-40s = [QUERY FAILED hr=0x%08X]\n", RenderStateName(states[i]), (unsigned)hr);
            continue;
        }
        if (IsColorState(states[i])) {
            BYTE a = (BYTE)(val >> 24), r = (BYTE)(val >> 16), g = (BYTE)(val >> 8), b = (BYTE)(val);
            fprintf(f, "%-40s = 0x%08X  (A=%3d R=%3d G=%3d B=%3d) (%.3f, %.3f, %.3f, %.3f)\n",
                RenderStateName(states[i]), val, a, r, g, b, r/255.f, g/255.f, b/255.f, a/255.f);
        } else if (IsFloatState(states[i])) {
            float fval = *(float*)&val;
            fprintf(f, "%-40s = 0x%08X  (float: %.6f)\n", RenderStateName(states[i]), val, fval);
        } else if (states[i] == D3DRS_FOGTABLEMODE || states[i] == D3DRS_FOGVERTEXMODE) {
            fprintf(f, "%-40s = %u  (%s)\n", RenderStateName(states[i]), val, FogModeName(val));
        } else if (states[i] == D3DRS_CULLMODE) {
            fprintf(f, "%-40s = %u  (%s)\n", RenderStateName(states[i]), val, CullName(val));
        } else if (states[i] == D3DRS_FILLMODE) {
            fprintf(f, "%-40s = %u  (%s)\n", RenderStateName(states[i]), val, FillName(val));
        } else if (states[i] == D3DRS_SRCBLEND || states[i] == D3DRS_DESTBLEND ||
                   states[i] == D3DRS_SRCBLENDALPHA || states[i] == D3DRS_DESTBLENDALPHA) {
            fprintf(f, "%-40s = %u  (%s)\n", RenderStateName(states[i]), val, BlendName(val));
        } else if (states[i] == D3DRS_ZFUNC || states[i] == D3DRS_ALPHAFUNC ||
                   states[i] == D3DRS_STENCILFUNC || states[i] == D3DRS_CCW_STENCILFUNC) {
            fprintf(f, "%-40s = %u  (%s)\n", RenderStateName(states[i]), val, CmpFuncName(val));
        } else {
            fprintf(f, "%-40s = %u  (0x%08X)\n", RenderStateName(states[i]), val, val);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  GAP 2: Lights and Material
// ═══════════════════════════════════════════════════════════════════════════
static void CaptureLightsAndMaterial(IDirect3DDevice9* dev, FILE* f) {
    fprintf(f, "\n[LIGHTS]\n");
    for (int i = 0; i < 8; ++i) {
        BOOL enabled = FALSE;
        HRESULT hr1 = dev->GetLightEnable(i, &enabled);
        D3DLIGHT9 light;
        memset(&light, 0, sizeof(light));
        HRESULT hr2 = dev->GetLight(i, &light);
        if (FAILED(hr1) && FAILED(hr2)) {
            fprintf(f, "Light[%d]: NOT AVAILABLE\n", i);
            continue;
        }
        if (!enabled) {
            fprintf(f, "Light[%d]: DISABLED\n", i);
            continue;
        }
        fprintf(f, "Light[%d]: ENABLED  Type=%s\n", i, LightTypeName(light.Type));
        fprintf(f, "  Diffuse  = (%.4f, %.4f, %.4f, %.4f)  [%3d, %3d, %3d, %3d]\n",
            light.Diffuse.r, light.Diffuse.g, light.Diffuse.b, light.Diffuse.a,
            (int)(light.Diffuse.r*255), (int)(light.Diffuse.g*255), (int)(light.Diffuse.b*255), (int)(light.Diffuse.a*255));
        fprintf(f, "  Specular = (%.4f, %.4f, %.4f, %.4f)  [%3d, %3d, %3d, %3d]\n",
            light.Specular.r, light.Specular.g, light.Specular.b, light.Specular.a,
            (int)(light.Specular.r*255), (int)(light.Specular.g*255), (int)(light.Specular.b*255), (int)(light.Specular.a*255));
        fprintf(f, "  Ambient  = (%.4f, %.4f, %.4f, %.4f)\n",
            light.Ambient.r, light.Ambient.g, light.Ambient.b, light.Ambient.a);
        fprintf(f, "  Position  = (%.4f, %.4f, %.4f)\n", light.Position.x, light.Position.y, light.Position.z);
        fprintf(f, "  Direction = (%.4f, %.4f, %.4f)\n", light.Direction.x, light.Direction.y, light.Direction.z);
        fprintf(f, "  Range=%.2f  Falloff=%.2f  Atten0=%.4f  Atten1=%.4f  Atten2=%.4f  Theta=%.4f  Phi=%.4f\n",
            light.Range, light.Falloff, light.Attenuation0, light.Attenuation1, light.Attenuation2, light.Theta, light.Phi);
    }

    fprintf(f, "\n[MATERIAL]\n");
    D3DMATERIAL9 mat;
    memset(&mat, 0, sizeof(mat));
    HRESULT hr = dev->GetMaterial(&mat);
    if (SUCCEEDED(hr)) {
        fprintf(f, "Diffuse  = (%.4f, %.4f, %.4f, %.4f)\n", mat.Diffuse.r, mat.Diffuse.g, mat.Diffuse.b, mat.Diffuse.a);
        fprintf(f, "Ambient  = (%.4f, %.4f, %.4f, %.4f)\n", mat.Ambient.r, mat.Ambient.g, mat.Ambient.b, mat.Ambient.a);
        fprintf(f, "Specular = (%.4f, %.4f, %.4f, %.4f)\n", mat.Specular.r, mat.Specular.g, mat.Specular.b, mat.Specular.a);
        fprintf(f, "Emissive = (%.4f, %.4f, %.4f, %.4f)\n", mat.Emissive.r, mat.Emissive.g, mat.Emissive.b, mat.Emissive.a);
        fprintf(f, "Power    = %.4f\n", mat.Power);
    } else {
        fprintf(f, "[QUERY FAILED hr=0x%08X]\n", (unsigned)hr);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  GAP 3: Transform Matrices
// ═══════════════════════════════════════════════════════════════════════════
static void WriteMatrix(FILE* f, const char* name, const D3DMATRIX& m) {
    fprintf(f, "[%s]\n", name);
    fprintf(f, "  [%12.6f %12.6f %12.6f %12.6f]\n", m._11, m._12, m._13, m._14);
    fprintf(f, "  [%12.6f %12.6f %12.6f %12.6f]\n", m._21, m._22, m._23, m._24);
    fprintf(f, "  [%12.6f %12.6f %12.6f %12.6f]\n", m._31, m._32, m._33, m._34);
    fprintf(f, "  [%12.6f %12.6f %12.6f %12.6f]\n", m._41, m._42, m._43, m._44);
}

static void CaptureTransforms(IDirect3DDevice9* dev, FILE* f) {
    fprintf(f, "\n[TRANSFORMS]\n");
    D3DMATRIX mat;

    if (SUCCEEDED(dev->GetTransform(D3DTS_VIEW, &mat))) {
        WriteMatrix(f, "VIEW MATRIX", mat);
        // Extract camera position: inverse of view translation
        // For orthonormal view: camPos = -transpose(R) * T
        float tx = mat._41, ty = mat._42, tz = mat._43;
        float cx = -(mat._11*tx + mat._21*ty + mat._31*tz);
        float cy = -(mat._12*tx + mat._22*ty + mat._32*tz);
        float cz = -(mat._13*tx + mat._23*ty + mat._33*tz);
        fprintf(f, "  CamPos = (%.4f, %.4f, %.4f)\n", cx, cy, cz);
        fprintf(f, "  CamFwd = (%.4f, %.4f, %.4f)\n", mat._13, mat._23, mat._33);
    } else {
        fprintf(f, "[VIEW MATRIX] QUERY FAILED\n");
    }

    if (SUCCEEDED(dev->GetTransform(D3DTS_PROJECTION, &mat))) {
        WriteMatrix(f, "PROJECTION MATRIX", mat);
        // Extract FOV, near, far from projection matrix
        float fovY = atanf(1.0f / mat._22) * 2.0f * (180.0f / 3.14159265f);
        float nearClip = 0, farClip = 0;
        if (mat._33 != 1.0f) {
            nearClip = -mat._43 / mat._33;
            farClip = mat._43 / (1.0f - mat._33);
        }
        fprintf(f, "  FovY = %.2f degrees\n", fovY);
        fprintf(f, "  NearClip = %.4f\n", nearClip);
        fprintf(f, "  FarClip = %.4f\n", farClip);
    } else {
        fprintf(f, "[PROJECTION MATRIX] QUERY FAILED\n");
    }

    if (SUCCEEDED(dev->GetTransform(D3DTS_WORLD, &mat)))
        WriteMatrix(f, "WORLD MATRIX", mat);
    if (SUCCEEDED(dev->GetTransform(D3DTS_TEXTURE0, &mat)))
        WriteMatrix(f, "TEXTURE0 MATRIX", mat);

    // Viewport
    fprintf(f, "\n[VIEWPORT]\n");
    D3DVIEWPORT9 vp;
    memset(&vp, 0, sizeof(vp));
    if (SUCCEEDED(dev->GetViewport(&vp))) {
        float aspect = (vp.Height > 0) ? (float)vp.Width / (float)vp.Height : 0;
        fprintf(f, "X=%u  Y=%u  Width=%u  Height=%u  MinZ=%.4f  MaxZ=%.4f  Aspect=%.4f\n",
            vp.X, vp.Y, vp.Width, vp.Height, vp.MinZ, vp.MaxZ, aspect);
    } else {
        fprintf(f, "[QUERY FAILED]\n");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  GAP 5: Texture Stage States and Sampler States
// ═══════════════════════════════════════════════════════════════════════════
static void CaptureTextureStages(IDirect3DDevice9* dev, FILE* f) {
    fprintf(f, "\n[TEXTURE STAGES]\n");
    for (int stage = 0; stage < 8; ++stage) {
        // Skip stages with no texture bound and COLOROP=DISABLE
        DWORD colorOp = D3DTOP_DISABLE;
        dev->GetTextureStageState(stage, D3DTSS_COLOROP, &colorOp);
        if (!g_curTex[stage] && colorOp == D3DTOP_DISABLE) continue;

        fprintf(f, "\n  Stage[%d]: tex=%p\n", stage, g_curTex[stage]);

        // Texture stage states
        static const D3DTEXTURESTAGESTATETYPE tssTypes[] = {
            D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2,
            D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
            D3DTSS_BUMPENVMAT00, D3DTSS_BUMPENVMAT01, D3DTSS_BUMPENVMAT10, D3DTSS_BUMPENVMAT11,
            D3DTSS_TEXCOORDINDEX, D3DTSS_BUMPENVLSCALE, D3DTSS_BUMPENVLOFFSET,
            D3DTSS_TEXTURETRANSFORMFLAGS, D3DTSS_COLORARG0, D3DTSS_ALPHAARG0,
            D3DTSS_RESULTARG, D3DTSS_CONSTANT
        };
        static const char* tssNames[] = {
            "COLOROP", "COLORARG1", "COLORARG2",
            "ALPHAOP", "ALPHAARG1", "ALPHAARG2",
            "BUMPENVMAT00", "BUMPENVMAT01", "BUMPENVMAT10", "BUMPENVMAT11",
            "TEXCOORDINDEX", "BUMPENVLSCALE", "BUMPENVLOFFSET",
            "TEXTURETRANSFORMFLAGS", "COLORARG0", "ALPHAARG0",
            "RESULTARG", "CONSTANT"
        };
        for (int t = 0; t < 18; ++t) {
            DWORD val = 0;
            if (SUCCEEDED(dev->GetTextureStageState(stage, tssTypes[t], &val))) {
                // Format ops and args as names
                if (tssTypes[t] == D3DTSS_COLOROP || tssTypes[t] == D3DTSS_ALPHAOP)
                    fprintf(f, "    %-24s = %s (%u)\n", tssNames[t], TextureOpName(val), val);
                else if (tssTypes[t] == D3DTSS_COLORARG0 || tssTypes[t] == D3DTSS_COLORARG1 || tssTypes[t] == D3DTSS_COLORARG2 ||
                         tssTypes[t] == D3DTSS_ALPHAARG0 || tssTypes[t] == D3DTSS_ALPHAARG1 || tssTypes[t] == D3DTSS_ALPHAARG2 ||
                         tssTypes[t] == D3DTSS_RESULTARG)
                    fprintf(f, "    %-24s = %s (%u)\n", tssNames[t], TextureArgName(val), val);
                else
                    fprintf(f, "    %-24s = %u  (0x%08X)\n", tssNames[t], val, val);
            }
        }

        // Sampler states
        static const D3DSAMPLERSTATETYPE sampTypes[] = {
            D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW,
            D3DSAMP_BORDERCOLOR, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
            D3DSAMP_MIPFILTER, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAXMIPLEVEL,
            D3DSAMP_MAXANISOTROPY, D3DSAMP_SRGBTEXTURE, D3DSAMP_ELEMENTINDEX,
            D3DSAMP_DMAPOFFSET
        };
        static const char* sampNames[] = {
            "ADDRESSU", "ADDRESSV", "ADDRESSW",
            "BORDERCOLOR", "MAGFILTER", "MINFILTER",
            "MIPFILTER", "MIPMAPLODBIAS", "MAXMIPLEVEL",
            "MAXANISOTROPY", "SRGBTEXTURE", "ELEMENTINDEX",
            "DMAPOFFSET"
        };
        for (int s = 0; s < 13; ++s) {
            DWORD val = 0;
            if (SUCCEEDED(dev->GetSamplerState(stage, sampTypes[s], &val))) {
                if (sampTypes[s] == D3DSAMP_MAGFILTER || sampTypes[s] == D3DSAMP_MINFILTER || sampTypes[s] == D3DSAMP_MIPFILTER)
                    fprintf(f, "    SAMP_%-20s = %s (%u)\n", sampNames[s], FilterName(val), val);
                else if (sampTypes[s] == D3DSAMP_ADDRESSU || sampTypes[s] == D3DSAMP_ADDRESSV || sampTypes[s] == D3DSAMP_ADDRESSW)
                    fprintf(f, "    SAMP_%-20s = %s (%u)\n", sampNames[s], AddressName(val), val);
                else if (sampTypes[s] == D3DSAMP_BORDERCOLOR) {
                    BYTE a=(BYTE)(val>>24), r=(BYTE)(val>>16), g=(BYTE)(val>>8), b=(BYTE)(val);
                    fprintf(f, "    SAMP_%-20s = 0x%08X  (A=%d R=%d G=%d B=%d)\n", sampNames[s], val, a, r, g, b);
                } else
                    fprintf(f, "    SAMP_%-20s = %u\n", sampNames[s], val);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  GAP 6: Active Shader Summary + Full Constant Dump
// ═══════════════════════════════════════════════════════════════════════════
static void CaptureActiveShaders(IDirect3DDevice9* dev, FILE* f) {
    fprintf(f, "\n[ACTIVE SHADERS]\n");

    // VS
    IDirect3DVertexShader9* vs = NULL;
    dev->GetVertexShader(&vs);
    if (vs) {
        std::string name = "unknown";
        std::map<void*,std::string>::iterator it = g_shaderNames.find((void*)vs);
        if (it != g_shaderNames.end()) name = it->second;
        UINT vsSize = 0;
        vs->GetFunction(NULL, &vsSize);
        fprintf(f, "VertexShader: ptr=%p  name=\"%s\"  bytecode=%u bytes\n", vs, name.c_str(), vsSize);
        vs->Release();
    } else {
        fprintf(f, "VertexShader: NULL (fixed-function)\n");
    }

    // PS
    IDirect3DPixelShader9* ps = NULL;
    dev->GetPixelShader(&ps);
    if (ps) {
        std::string name = "unknown";
        std::map<void*,std::string>::iterator it = g_shaderNames.find((void*)ps);
        if (it != g_shaderNames.end()) name = it->second;
        UINT psSize = 0;
        ps->GetFunction(NULL, &psSize);
        fprintf(f, "PixelShader:  ptr=%p  name=\"%s\"  bytecode=%u bytes\n", ps, name.c_str(), psSize);
        ps->Release();
    } else {
        fprintf(f, "PixelShader:  NULL (fixed-function)\n");
    }

    // Dump ALL non-zero VS constants
    fprintf(f, "\n[ALL VS CONSTANTS] (non-zero)\n");
    static float vsBuf[256 * 4];
    if (SUCCEEDED(dev->GetVertexShaderConstantF(0, vsBuf, 256))) {
        for (int i = 0; i < 256; ++i) {
            float x = vsBuf[i*4], y = vsBuf[i*4+1], z = vsBuf[i*4+2], w = vsBuf[i*4+3];
            if (x != 0 || y != 0 || z != 0 || w != 0)
                fprintf(f, "  VS c%d = (%.6f, %.6f, %.6f, %.6f)\n", i, x, y, z, w);
        }
    }

    // Dump ALL non-zero PS constants
    fprintf(f, "\n[ALL PS CONSTANTS] (non-zero)\n");
    static float psBuf[224 * 4];
    if (SUCCEEDED(dev->GetPixelShaderConstantF(0, psBuf, 224))) {
        for (int i = 0; i < 224; ++i) {
            float x = psBuf[i*4], y = psBuf[i*4+1], z = psBuf[i*4+2], w = psBuf[i*4+3];
            if (x != 0 || y != 0 || z != 0 || w != 0)
                fprintf(f, "  PS c%d = (%.6f, %.6f, %.6f, %.6f)\n", i, x, y, z, w);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Shader Bytecode Export Helper
// ═══════════════════════════════════════════════════════════════════════════
static void DumpShaderBytecode(void* pShader, bool isVS) {
    if (!pShader) return;
    if (g_dumpedShaderPtrs.count(pShader)) return; // already dumped
    g_dumpedShaderPtrs.insert(pShader);

    UINT size = 0;
    HRESULT hr;
    if (isVS)
        hr = ((IDirect3DVertexShader9*)pShader)->GetFunction(NULL, &size);
    else
        hr = ((IDirect3DPixelShader9*)pShader)->GetFunction(NULL, &size);

    if (FAILED(hr) || size == 0 || size > 65536) return;

    static BYTE buf[65536]; // static to avoid heap alloc
    if (isVS)
        hr = ((IDirect3DVertexShader9*)pShader)->GetFunction(buf, &size);
    else
        hr = ((IDirect3DPixelShader9*)pShader)->GetFunction(buf, &size);

    if (FAILED(hr)) return;

    char filename[256];
    sprintf(filename, "conquest_%s_%p.%s", isVS ? "vs" : "ps", pShader, isVS ? "vso" : "pso");
    FILE* fout = fopen(filename, "wb");
    if (fout) {
        fwrite(buf, 1, size, fout);
        fclose(fout);
    }
}

ProxyIDirect3DDevice9::ProxyIDirect3DDevice9(IDirect3DDevice9* pReal) : m_pReal(pReal) { TrMarkLoaded(); OCV_OnDeviceCreated(this,pReal); }

// IUnknown
HRESULT __stdcall ProxyIDirect3DDevice9::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;
    // Raw QueryInterface(IDirect3DDevice9) bypasses EndScene/Reset hooks.
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDirect3DDevice9)) {
        m_pReal->AddRef();
        *ppvObj = static_cast<IDirect3DDevice9*>(this);
        return S_OK;
    }
    return m_pReal->QueryInterface(riid, ppvObj);
}
ULONG __stdcall ProxyIDirect3DDevice9::AddRef() { return m_pReal->AddRef(); }
ULONG __stdcall ProxyIDirect3DDevice9::Release() {
    ULONG count = m_pReal->Release();
    if (count == 0) { LfLog("DEVICE RELEASED (refcount 0) <-- shutdown"); OCV_OnDeviceReleased(m_pReal); Overlay_OnReset(); delete this; }
    return count;
}

// IDirect3DDevice9 methods
HRESULT __stdcall ProxyIDirect3DDevice9::TestCooperativeLevel() { return m_pReal->TestCooperativeLevel(); }
UINT __stdcall ProxyIDirect3DDevice9::GetAvailableTextureMem() { return m_pReal->GetAvailableTextureMem(); }
HRESULT __stdcall ProxyIDirect3DDevice9::EvictManagedResources() { return m_pReal->EvictManagedResources(); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetDirect3D(IDirect3D9** ppD3D9) { return m_pReal->GetDirect3D(ppD3D9); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetDeviceCaps(D3DCAPS9* pCaps) { return m_pReal->GetDeviceCaps(pCaps); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) { return m_pReal->GetDisplayMode(iSwapChain, pMode); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParams) { return m_pReal->GetCreationParameters(pParams); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) { return m_pReal->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap); }
void __stdcall ProxyIDirect3DDevice9::SetCursorPosition(int X, int Y, DWORD Flags) { m_pReal->SetCursorPosition(X, Y, Flags); }
BOOL __stdcall ProxyIDirect3DDevice9::ShowCursor(BOOL bShow) { return m_pReal->ShowCursor(bShow); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pParams, IDirect3DSwapChain9** ppChain) { return m_pReal->CreateAdditionalSwapChain(pParams, ppChain); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** ppChain) { return m_pReal->GetSwapChain(iSwapChain, ppChain); }
UINT __stdcall ProxyIDirect3DDevice9::GetNumberOfSwapChains() { return m_pReal->GetNumberOfSwapChains(); }

HRESULT __stdcall ProxyIDirect3DDevice9::Reset(D3DPRESENT_PARAMETERS* pParams) { LfLog("DEVICE RESET  %ux%u fmt=%d windowed=%d  <-- TEARDOWN (all DEFAULT-pool resources die here)", pParams?pParams->BackBufferWidth:0, pParams?pParams->BackBufferHeight:0, pParams?(int)pParams->BackBufferFormat:0, pParams?(int)pParams->Windowed:0);
    OCV_BeforeReset(m_pReal);
    Overlay_OnReset();
    HRESULT hr=m_pReal->Reset(pParams);
    OCV_AfterReset(m_pReal,hr);
    return hr;
}

#include "crowd_event_handler.h"
// Free camera per-frame input. Defined further down (banner at FcPatch); the
// same anonymous namespace, so this forward decl keeps the internal linkage.
namespace { void FcTick(); }

HRESULT __stdcall ProxyIDirect3DDevice9::Present(const RECT* pSrc, const RECT* pDst, HWND hWnd, const RGNDATA* pDirtyRgn) { TrFrameEdge(); LfFrameTick(); FcTick();
    // Per-frame crowd handler tick. Reads F11 input, mutates CrowdItem.instNum
    // for hide/show. Lives in the render thread which is fine because crowd
    // mutations only need to happen between frames, not during one.
    static bool s_crowdInited = false;
    if (!s_crowdInited) { CrowdHandler_Init(); s_crowdInited = true; }
    CrowdHandler_OnFrame();
    OCV_OnPresent(m_pReal);
    return m_pReal->Present(pSrc, pDst, hWnd, pDirtyRgn);
}
HRESULT __stdcall ProxyIDirect3DDevice9::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppSurface) { return m_pReal->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppSurface); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) { return m_pReal->GetRasterStatus(iSwapChain, pRasterStatus); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetDialogBoxMode(BOOL bEnableDialogs) { return m_pReal->SetDialogBoxMode(bEnableDialogs); }
void __stdcall ProxyIDirect3DDevice9::SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP* pRamp) { m_pReal->SetGammaRamp(iSwapChain, Flags, pRamp); }
void __stdcall ProxyIDirect3DDevice9::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) { m_pReal->GetGammaRamp(iSwapChain, pRamp); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateTexture(UINT W, UINT H, UINT Levels, DWORD Usage, D3DFORMAT Fmt, D3DPOOL Pool, IDirect3DTexture9** ppTex, HANDLE* pHandle) { TrLog("CreateTexture %ux%u lv=%u usage=0x%x %s", W, H, Levels, Usage, TrFmt(Fmt)); LfNoteTexture();
    HRESULT hr = OCV_CreateTexture(m_pReal,W,H,Levels,Usage,Fmt,Pool,ppTex,pHandle,_ReturnAddress());
    if (hr == S_FALSE)
        hr = m_pReal->CreateTexture(W, H, Levels, Usage, Fmt, Pool, ppTex, pHandle);
    // Log RT textures (shadow maps, HDR buffers, bloom targets)
    if (Usage & D3DUSAGE_RENDERTARGET) {
        static FILE* initLog = fopen("graphics_init.log", "a");
        if (initLog) { fprintf(initLog, "CreateTexture(RT): %ux%u fmt=%u levels=%u\n", W, H, (UINT)Fmt, Levels); fflush(initLog); }
    }
    return hr;
}
HRESULT __stdcall ProxyIDirect3DDevice9::CreateVolumeTexture(UINT W, UINT H, UINT D, UINT Levels, DWORD Usage, D3DFORMAT Fmt, D3DPOOL Pool, IDirect3DVolumeTexture9** ppTex, HANDLE* pHandle) { return m_pReal->CreateVolumeTexture(W, H, D, Levels, Usage, Fmt, Pool, ppTex, pHandle); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateCubeTexture(UINT EdgeLen, UINT Levels, DWORD Usage, D3DFORMAT Fmt, D3DPOOL Pool, IDirect3DCubeTexture9** ppTex, HANDLE* pHandle) { return m_pReal->CreateCubeTexture(EdgeLen, Levels, Usage, Fmt, Pool, ppTex, pHandle); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateVertexBuffer(UINT Len, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVB, HANDLE* pHandle) {
    HRESULT hr=m_pReal->CreateVertexBuffer(Len,Usage,FVF,Pool,ppVB,pHandle);
    OC_GeometryBudget_VB(Len,Usage,Pool,hr,ppVB&&*ppVB!=nullptr,_ReturnAddress());
    return hr;
}
HRESULT __stdcall ProxyIDirect3DDevice9::CreateIndexBuffer(UINT Len, DWORD Usage, D3DFORMAT Fmt, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIB, HANDLE* pHandle) {
    HRESULT hr=m_pReal->CreateIndexBuffer(Len,Usage,Fmt,Pool,ppIB,pHandle);
    OC_GeometryBudget_IB(Len,Usage,Pool,hr,ppIB&&*ppIB!=nullptr,_ReturnAddress());
    return hr;
}
HRESULT __stdcall ProxyIDirect3DDevice9::CreateRenderTarget(UINT W, UINT H, D3DFORMAT Fmt, D3DMULTISAMPLE_TYPE MS, DWORD MSQ, BOOL Lockable, IDirect3DSurface9** ppSurf, HANDLE* pHandle) {
    HRESULT hr = m_pReal->CreateRenderTarget(W, H, Fmt, MS, MSQ, Lockable, ppSurf, pHandle);
    // Log RT creation for graphics init analysis
    static FILE* initLog = NULL;
    if (!initLog) { initLog = fopen("graphics_init.log", "w"); fprintf(initLog, "=== Graphics Init Log ===\n"); }
    if (initLog) fprintf(initLog, "CreateRenderTarget: %ux%u fmt=%u ms=%u\n", W, H, (UINT)Fmt, (UINT)MS);
    return hr;
}
HRESULT __stdcall ProxyIDirect3DDevice9::CreateDepthStencilSurface(UINT W, UINT H, D3DFORMAT Fmt, D3DMULTISAMPLE_TYPE MS, DWORD MSQ, BOOL Discard, IDirect3DSurface9** ppSurf, HANDLE* pHandle) {
    HRESULT hr = m_pReal->CreateDepthStencilSurface(W, H, Fmt, MS, MSQ, Discard, ppSurf, pHandle);
    static FILE* initLog = fopen("graphics_init.log", "a");
    if (initLog) { fprintf(initLog, "CreateDepthStencil: %ux%u fmt=%u\n", W, H, (UINT)Fmt); fflush(initLog); }
    return hr;
}
HRESULT __stdcall ProxyIDirect3DDevice9::UpdateSurface(IDirect3DSurface9* pSrc, const RECT* pSrcRect, IDirect3DSurface9* pDst, const POINT* pDstPt) { return m_pReal->UpdateSurface(pSrc, pSrcRect, pDst, pDstPt); }
HRESULT __stdcall ProxyIDirect3DDevice9::UpdateTexture(IDirect3DBaseTexture9* pSrc, IDirect3DBaseTexture9* pDst) { return OCV_UpdateTexture(m_pReal,pSrc,pDst); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetRenderTargetData(IDirect3DSurface9* pRT, IDirect3DSurface9* pDst) { return m_pReal->GetRenderTargetData(pRT, pDst); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDst) { return m_pReal->GetFrontBufferData(iSwapChain, pDst); }
HRESULT __stdcall ProxyIDirect3DDevice9::StretchRect(IDirect3DSurface9* pSrc, const RECT* pSrcRect, IDirect3DSurface9* pDst, const RECT* pDstRect, D3DTEXTUREFILTERTYPE Filter) { TrLog("StretchRect(%p -> %p)", (void*)pSrc, (void*)pDst); return m_pReal->StretchRect(pSrc, pSrcRect, pDst, pDstRect, Filter); }
HRESULT __stdcall ProxyIDirect3DDevice9::ColorFill(IDirect3DSurface9* pSurf, const RECT* pRect, D3DCOLOR Color) { return m_pReal->ColorFill(pSurf, pRect, Color); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateOffscreenPlainSurface(UINT W, UINT H, D3DFORMAT Fmt, D3DPOOL Pool, IDirect3DSurface9** ppSurf, HANDLE* pHandle) { return m_pReal->CreateOffscreenPlainSurface(W, H, Fmt, Pool, ppSurf, pHandle); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetRenderTarget(DWORD RTIndex, IDirect3DSurface9* pRT) { if (RTIndex==0) g_trRT=pRT; if (g_trOn) { D3DSURFACE_DESC d; if (pRT && SUCCEEDED(pRT->GetDesc(&d))) TrLog("SetRenderTarget(%u, %p) %ux%u %s", RTIndex, (void*)pRT, d.Width, d.Height, TrFmt(d.Format)); else TrLog("SetRenderTarget(%u, %p)", RTIndex, (void*)pRT); } return m_pReal->SetRenderTarget(RTIndex, pRT); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetRenderTarget(DWORD RTIndex, IDirect3DSurface9** ppRT) { return m_pReal->GetRenderTarget(RTIndex, ppRT); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) { return m_pReal->SetDepthStencilSurface(pNewZStencil); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetDepthStencilSurface(IDirect3DSurface9** ppZStencil) { return m_pReal->GetDepthStencilSurface(ppZStencil); }

// BeginScene - set wireframe here!
HRESULT __stdcall ProxyIDirect3DDevice9::BeginScene() {
    HRESULT hr = m_pReal->BeginScene();
    Overlay_OnBeginScene(m_pReal);
    if (g_WireframeEnabled) {
        m_pReal->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
    }
    CaptureCheck();
    if (g_CaptureNextFrame && !g_Capturing) {
        g_CaptureNextFrame = false;
        g_Capturing = true;
        g_CaptureDrawCall = 0;
        g_CaptureFile = fopen("capture_frame.txt", "w");
        if (g_CaptureFile) fprintf(g_CaptureFile, "=== Frame Capture (F9) ===\n");
    }
    return hr;
}

// EndScene - draw overlay here!
HRESULT __stdcall ProxyIDirect3DDevice9::EndScene() {
    g_FrameCounter++;  // Increment frame counter for freeze detection

    // Process danger state at end of each frame
    DangerOnEndOfFrame();
    // Non-blocking if another thread is extending. Disabled by default.
    OC_HeapExt_Tick();

    if (g_Capturing && g_CaptureFile) {
        // ── Full render state capture (GAPs 1-6) ──
        CaptureRenderStates(m_pReal, g_CaptureFile);
        CaptureLightsAndMaterial(m_pReal, g_CaptureFile);
        CaptureTransforms(m_pReal, g_CaptureFile);
        CaptureTextureStages(m_pReal, g_CaptureFile);
        CaptureActiveShaders(m_pReal, g_CaptureFile);

        fprintf(g_CaptureFile, "\n========================================\n");
        fprintf(g_CaptureFile, "DUMP COMPLETE: %d draw calls, %d shaders known\n",
                g_CaptureDrawCall, (int)g_shaderNames.size());
        fprintf(g_CaptureFile, "========================================\n");
        fclose(g_CaptureFile);
        g_CaptureFile = NULL;
        g_Capturing = false;
        g_shadersDumped = true;
    }

    m_pReal->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    Overlay_OnEndScene(m_pReal);
    return m_pReal->EndScene();
}

HRESULT __stdcall ProxyIDirect3DDevice9::Clear(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) { return m_pReal->Clear(Count, pRects, Flags, Color, Z, Stencil); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) { return m_pReal->SetTransform(State, pMatrix); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) { return m_pReal->GetTransform(State, pMatrix); }
HRESULT __stdcall ProxyIDirect3DDevice9::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) { return m_pReal->MultiplyTransform(State, pMatrix); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetViewport(const D3DVIEWPORT9* pViewport) { return m_pReal->SetViewport(pViewport); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetViewport(D3DVIEWPORT9* pViewport) { return m_pReal->GetViewport(pViewport); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetMaterial(const D3DMATERIAL9* pMaterial) { return m_pReal->SetMaterial(pMaterial); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetMaterial(D3DMATERIAL9* pMaterial) { return m_pReal->GetMaterial(pMaterial); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetLight(DWORD Index, const D3DLIGHT9* pLight) { return m_pReal->SetLight(Index, pLight); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetLight(DWORD Index, D3DLIGHT9* pLight) { return m_pReal->GetLight(Index, pLight); }
HRESULT __stdcall ProxyIDirect3DDevice9::LightEnable(DWORD Index, BOOL Enable) { return m_pReal->LightEnable(Index, Enable); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetLightEnable(DWORD Index, BOOL* pEnable) { return m_pReal->GetLightEnable(Index, pEnable); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetClipPlane(DWORD Index, const float* pPlane) { return m_pReal->SetClipPlane(Index, pPlane); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetClipPlane(DWORD Index, float* pPlane) { return m_pReal->GetClipPlane(Index, pPlane); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
    switch ((int)State) {   // mirrored for the per-draw RS line (kTrRSName)
        case D3DRS_ALPHABLENDENABLE:  g_trRS[0]=Value; break;
        case D3DRS_ZENABLE:           g_trRS[1]=Value; break;
        case D3DRS_ZWRITEENABLE:      g_trRS[2]=Value; break;
        case D3DRS_ZFUNC:             g_trRS[3]=Value; break;
        case D3DRS_CULLMODE:          g_trRS[4]=Value; break;
        case D3DRS_COLORWRITEENABLE:  g_trRS[5]=Value; break;
        case D3DRS_SRCBLEND:          g_trRS[6]=Value; break;
        case D3DRS_DESTBLEND:         g_trRS[7]=Value; break;
        default: break;
    }
    return m_pReal->SetRenderState(State, Value); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) { return m_pReal->GetRenderState(State, pValue); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) { return m_pReal->CreateStateBlock(Type, ppSB); }
HRESULT __stdcall ProxyIDirect3DDevice9::BeginStateBlock() { return m_pReal->BeginStateBlock(); }
HRESULT __stdcall ProxyIDirect3DDevice9::EndStateBlock(IDirect3DStateBlock9** ppSB) { return m_pReal->EndStateBlock(ppSB); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetClipStatus(const D3DCLIPSTATUS9* pClipStatus) { return m_pReal->SetClipStatus(pClipStatus); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetClipStatus(D3DCLIPSTATUS9* pClipStatus) { return m_pReal->GetClipStatus(pClipStatus); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) { return OCV_GetTexture(m_pReal,Stage,ppTexture); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) { if (Stage<16u) g_trTex[Stage]=pTexture; TrLog("SetTexture(%u, %p)", Stage, (void*)pTexture);
    if (Stage < 16) g_curTex[Stage] = pTexture;
    return OCV_SetTexture(m_pReal,Stage,pTexture);
}
HRESULT __stdcall ProxyIDirect3DDevice9::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) { return m_pReal->GetTextureStageState(Stage, Type, pValue); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) { return m_pReal->SetTextureStageState(Stage, Type, Value); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) { return m_pReal->GetSamplerState(Sampler, Type, pValue); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) { return m_pReal->SetSamplerState(Sampler, Type, Value); }
HRESULT __stdcall ProxyIDirect3DDevice9::ValidateDevice(DWORD* pNumPasses) { return m_pReal->ValidateDevice(pNumPasses); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries) { return m_pReal->SetPaletteEntries(PaletteNumber, pEntries); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) { return m_pReal->GetPaletteEntries(PaletteNumber, pEntries); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetCurrentTexturePalette(UINT PaletteNumber) { return m_pReal->SetCurrentTexturePalette(PaletteNumber); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetCurrentTexturePalette(UINT* PaletteNumber) { return m_pReal->GetCurrentTexturePalette(PaletteNumber); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetScissorRect(const RECT* pRect) { return m_pReal->SetScissorRect(pRect); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetScissorRect(RECT* pRect) { return m_pReal->GetScissorRect(pRect); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetSoftwareVertexProcessing(BOOL bSoftware) { return m_pReal->SetSoftwareVertexProcessing(bSoftware); }
BOOL __stdcall ProxyIDirect3DDevice9::GetSoftwareVertexProcessing() { return m_pReal->GetSoftwareVertexProcessing(); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetNPatchMode(float nSegments) { return m_pReal->SetNPatchMode(nSegments); }
float __stdcall ProxyIDirect3DDevice9::GetNPatchMode() { return m_pReal->GetNPatchMode(); }
HRESULT __stdcall ProxyIDirect3DDevice9::DrawPrimitive(D3DPRIMITIVETYPE Type, UINT StartVertex, UINT PrimitiveCount) { TrDrawRecord("DP", (int)Type, 0, 0, 0, StartVertex, PrimitiveCount); TrLaneNote(m_pReal, (unsigned)Type); return m_pReal->DrawPrimitive(Type, StartVertex, PrimitiveCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::DrawIndexedPrimitive(D3DPRIMITIVETYPE Type, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount) { TrDrawRecord("DIP", (int)Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount); TrLaneNote(m_pReal, (unsigned)Type);
    if (g_Capturing && g_CaptureFile) {
        fprintf(g_CaptureFile, "\n--- DrawCall #%d: %u tris, %u verts ---\n", g_CaptureDrawCall++, PrimitiveCount, NumVertices);
        // Resolve shader names from bytecode CTAB
        std::string vsName = "NULL", psName = "NULL";
        if (g_curVS) { std::map<void*,std::string>::iterator it = g_shaderNames.find((void*)g_curVS); if (it != g_shaderNames.end()) vsName = it->second; }
        if (g_curPS) { std::map<void*,std::string>::iterator it = g_shaderNames.find((void*)g_curPS); if (it != g_shaderNames.end()) psName = it->second; }
        fprintf(g_CaptureFile, "  VS=%s  PS=%s\n", vsName.c_str(), psName.c_str());
        for (int s = 0; s < 12; ++s) {
            if (g_curTex[s]) fprintf(g_CaptureFile, "  tex[%d]=%p\n", s, g_curTex[s]);
        }
        // Log key render states
        DWORD fillMode, cullMode, alphaBlend, alphaTest, zWrite;
        m_pReal->GetRenderState(D3DRS_FILLMODE, &fillMode);
        m_pReal->GetRenderState(D3DRS_CULLMODE, &cullMode);
        m_pReal->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
        m_pReal->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
        m_pReal->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
        if (alphaBlend || alphaTest || !zWrite || fillMode != 3)
            fprintf(g_CaptureFile, "  states: fill=%u cull=%u blend=%u atest=%u zw=%u\n",
                    fillMode, cullMode, alphaBlend, alphaTest, zWrite);
    }
    return m_pReal->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);
}
HRESULT __stdcall ProxyIDirect3DDevice9::DrawPrimitiveUP(D3DPRIMITIVETYPE Type, UINT PrimitiveCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) { return m_pReal->DrawPrimitiveUP(Type, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride); }
HRESULT __stdcall ProxyIDirect3DDevice9::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE Type, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) { return m_pReal->DrawIndexedPrimitiveUP(Type, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride); }
HRESULT __stdcall ProxyIDirect3DDevice9::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) { return m_pReal->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateVertexDeclaration(const D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) { return m_pReal->CreateVertexDeclaration(pVertexElements, ppDecl); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) { g_trDecl=pDecl; TrLog("SetVertexDeclaration(%p)", (void*)pDecl); return m_pReal->SetVertexDeclaration(pDecl); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) { return m_pReal->GetVertexDeclaration(ppDecl); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetFVF(DWORD FVF) { return m_pReal->SetFVF(FVF); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetFVF(DWORD* pFVF) { return m_pReal->GetFVF(pFVF); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateVertexShader(const DWORD* pFunction, IDirect3DVertexShader9** ppShader) {
    HRESULT hr = m_pReal->CreateVertexShader(pFunction, ppShader);
    if (SUCCEEDED(hr) && pFunction && ppShader && *ppShader) {
        std::string name = ExtractShaderName(pFunction);
        g_shaderNames[(void*)*ppShader] = name;
        g_shaderDumpCount++;
    }
    return hr;
}
HRESULT __stdcall ProxyIDirect3DDevice9::SetVertexShader(IDirect3DVertexShader9* pShader) { g_trVS=pShader; TrLog("SetVertexShader(%p)", (void*)pShader);
    g_curVS = pShader;
    if (g_Capturing && pShader) DumpShaderBytecode((void*)pShader, true);
    return m_pReal->SetVertexShader(pShader);
}
HRESULT __stdcall ProxyIDirect3DDevice9::GetVertexShader(IDirect3DVertexShader9** ppShader) { return m_pReal->GetVertexShader(ppShader); }
// Cache of the VS registers the bridge needs (from project_shader_registers.md):
//   c239..c242 = g__viewProjMatrix (row-major 4x4)
//   c243       = g__cameraRight
//   c244       = g__cameraUp
//   c245       = g__cameraPos (xyz, w=1)
//
// IMPORTANT: The game's world-render path batches 8 regs (239..246) in one
// SetVertexShaderConstantF call. HUD/UI later writes these same regs with
// zeros via smaller/individual calls. We only snapshot the batched world
// write so HUD can't clobber the camera cache.
extern "C" {
    float g_VbWorldCam[8 * 4] = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,  // c239..c242 viewProj (identity)
        1,0,0,0,                              // c243 right
        0,1,0,0,                              // c244 up
        0,0,0,1,                              // c245 pos
        0,0,0,0                               // c246 unused
    };
    // Full snapshot of c0..c255 so the bridge can find the real camera.
    float g_VbAllRegs[256 * 4] = {0};

    // Exported getter so a CO-LOADED overlay DLL (DebugOverlay/ConquestConsole)
    // can project world->screen using the GAME'S REAL ViewProj instead of the
    // dead fixed-function GetTransform(D3DTS_VIEW/PROJECTION) — the Zero engine
    // is shader-based and never sets those, so GetTransform returns identity
    // and every collision/bbox corner lands in garbage. This returns c239..c242
    // (g__viewProjMatrix, row-major 4x4) as 16 floats; the caller does the
    // row-vector multiply clip = (wx,wy,wz,1) * VP. 2026-07-16.
    __declspec(dllexport) const float* Vb_GetViewProj() { return g_VbWorldCam; }
    // Camera position c245 (xyz) — handy for distance fade / behind checks.
    __declspec(dllexport) const float* Vb_GetCameraPos() { return g_VbWorldCam + 6 * 4; }
}

// ═══════════════════════════════════════════════════════════════════════════
// FREE CAMERA (2026-08-07) — retail-view flycam, and a Remix flycam for free.
//
// WHY HERE AND NOT IN REMIX: Remix's own rtx.camera.enableFreeCamera does
// nothing in this install. This proxy is chain-loaded AHEAD of Remix
// (proxy.ini: d3d9.dll -> d3d9_remix.dll), so a camera moved HERE is the camera
// Remix receives — the flycam therefore works in the raster view AND under RTX,
// with no dependency on Remix detecting anything.
//
// THE MATH IS EXACT, NOT AN APPROXIMATION. The engine hands us the view basis
// outright (banner above g_VbWorldCam): c243 = right, c244 = up, c245 = eye.
// So the game's view matrix V is reconstructable, and with VP = V*P:
//        P      = V^-1 * VP                  (projection, camera-independent)
//        newVP  = V' * P = (V' * V^-1) * VP  (V' = our camera)
// i.e. D = V' * V^-1 and newVP = D * VP. No frustum decomposition, no guessed
// fov/near/far, and zoom or aspect changes ride along automatically because P
// is never reconstructed — only re-based.
//
// Row-vector convention throughout (clip = v * VP), matching the engine's own
// Vb_GetViewProj contract documented above.
//
// LEFT-HANDED, Y-UP: right = up x forward, forward = right x up. Y-up is not an
// assumption — rtx.conf carries rtx.zUp = False for this title.
// ═══════════════════════════════════════════════════════════════════════════
#include <math.h>
namespace {

bool  g_fcOn      = false;   // flycam driving the camera
bool  g_fcNeedSync = true;   // next real camera write seeds our pose
float g_fcPos[3]  = { 0, 0, 0 };
float g_fcYaw     = 0.0f;    // radians, atan2(F.x, F.z)
float g_fcPitch   = 0.0f;    // radians, asin(F.y)
// Base speed in WORLD UNITS/sec. Grounded in a measured value, not a guess: a
// decoded world matrix for this title carried translation (141.76, -1.39,
// 137.73), so levels span hundreds of units and ~30/sec is a walking pace.
float g_fcSpeed   = 30.0f;
double g_fcLastT  = 0.0;

void FcCross(const float* a, const float* b, float* o) {
    o[0] = a[1]*b[2] - a[2]*b[1];
    o[1] = a[2]*b[0] - a[0]*b[2];
    o[2] = a[0]*b[1] - a[1]*b[0];
}
void FcNorm(float* v) {
    float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 1e-8f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}
// C = A * B, row-major, row-vector convention.
void FcMul(const float* A, const float* B, float* C) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += A[i*4+k] * B[k*4+j];
            C[i*4+j] = s;
        }
}
// Our camera's basis from yaw/pitch.
void FcBasis(float* R, float* U, float* F) {
    const float cp = cosf(g_fcPitch), sp = sinf(g_fcPitch);
    const float cy = cosf(g_fcYaw),   sy = sinf(g_fcYaw);
    F[0] = cp*sy; F[1] = sp; F[2] = cp*cy;                 FcNorm(F);
    const float worldUp[3] = { 0.0f, 1.0f, 0.0f };
    FcCross(worldUp, F, R);                                FcNorm(R);
    FcCross(F, R, U);                                      FcNorm(U);
}

// Per-frame input. Called from Present so it ticks once per presented frame.
void FcTick() {
    // F7 toggles. Deliberately not F9/F11 — those are the capture and tracer
    // keys already bound in this file.
    if (GetAsyncKeyState(VK_F7) & 1) {
        g_fcOn = !g_fcOn;
        if (g_fcOn) g_fcNeedSync = true;   // re-seed from the live game camera
    }
    if (!g_fcOn) { g_fcLastT = 0.0; return; }

    // Real elapsed time, so fly speed does not scale with framerate.
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    const double now = (double)c.QuadPart / (double)f.QuadPart;
    if (g_fcLastT == 0.0) { g_fcLastT = now; return; }
    float dt = (float)(now - g_fcLastT);
    g_fcLastT = now;
    if (dt > 0.25f) dt = 0.25f;            // clamp hitches/breakpoints

    if (GetAsyncKeyState(VK_OEM_PLUS)  & 1) g_fcSpeed *= 1.5f;
    if (GetAsyncKeyState(VK_OEM_MINUS) & 1) g_fcSpeed /= 1.5f;
    if (g_fcSpeed < 0.01f)   g_fcSpeed = 0.01f;
    if (g_fcSpeed > 20000.f) g_fcSpeed = 20000.f;

    float R[3], U[3], F[3]; FcBasis(R, U, F);
    float step = g_fcSpeed * dt;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) step *= 5.0f;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) step *= 0.2f;

    // Arrow keys turn. Keyboard-only on purpose: grabbing the mouse would fight
    // the game, which is still simulating and still owns the cursor.
    const float turn = 1.6f * dt;
    if (GetAsyncKeyState(VK_LEFT)  & 0x8000) g_fcYaw   -= turn;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) g_fcYaw   += turn;
    if (GetAsyncKeyState(VK_UP)    & 0x8000) g_fcPitch += turn;
    if (GetAsyncKeyState(VK_DOWN)  & 0x8000) g_fcPitch -= turn;
    const float lim = 1.5533f;             // ~89 deg, keeps the basis from degenerating
    if (g_fcPitch >  lim) g_fcPitch =  lim;
    if (g_fcPitch < -lim) g_fcPitch = -lim;

    // WASD flies along the view; Q/E use WORLD up so "up" stays up when pitched.
    if (GetAsyncKeyState('W') & 0x8000) { g_fcPos[0]+=F[0]*step; g_fcPos[1]+=F[1]*step; g_fcPos[2]+=F[2]*step; }
    if (GetAsyncKeyState('S') & 0x8000) { g_fcPos[0]-=F[0]*step; g_fcPos[1]-=F[1]*step; g_fcPos[2]-=F[2]*step; }
    if (GetAsyncKeyState('D') & 0x8000) { g_fcPos[0]+=R[0]*step; g_fcPos[1]+=R[1]*step; g_fcPos[2]+=R[2]*step; }
    if (GetAsyncKeyState('A') & 0x8000) { g_fcPos[0]-=R[0]*step; g_fcPos[1]-=R[1]*step; g_fcPos[2]-=R[2]*step; }
    if (GetAsyncKeyState('E') & 0x8000) g_fcPos[1] += step;
    if (GetAsyncKeyState('Q') & 0x8000) g_fcPos[1] -= step;
}

// Rewrite a c239..c246 block in place. `blk` is 8 registers: VP, right, up,
// eye, spare. Returns false if the game basis is unusable (then we pass the
// frame through untouched rather than render garbage).
bool FcPatch(float* blk) {
    const float* gR = blk + 4*4;           // c243
    const float* gU = blk + 5*4;           // c244
    const float* gE = blk + 6*4;           // c245
    float R[3] = { gR[0], gR[1], gR[2] };
    float U[3] = { gU[0], gU[1], gU[2] };
    FcNorm(R); FcNorm(U);
    float F[3]; FcCross(R, U, F); FcNorm(F);
    if (!(F[0]*F[0] + F[1]*F[1] + F[2]*F[2] > 0.5f)) return false;   // degenerate

    if (g_fcNeedSync) {                    // seed from wherever the player is
        g_fcPos[0] = gE[0]; g_fcPos[1] = gE[1]; g_fcPos[2] = gE[2];
        g_fcYaw    = atan2f(F[0], F[2]);
        float sy   = F[1]; if (sy > 1.0f) sy = 1.0f; if (sy < -1.0f) sy = -1.0f;
        g_fcPitch  = asinf(sy);
        g_fcNeedSync = false;
    }

    // V^-1 (view->world) for the GAME camera: rotation transposed + eye.
    const float Vinv[16] = {
        R[0], R[1], R[2], 0.0f,
        U[0], U[1], U[2], 0.0f,
        F[0], F[1], F[2], 0.0f,
        gE[0], gE[1], gE[2], 1.0f
    };
    float nR[3], nU[3], nF[3]; FcBasis(nR, nU, nF);
    const float* P = g_fcPos;
    const float dR = -(P[0]*nR[0] + P[1]*nR[1] + P[2]*nR[2]);
    const float dU = -(P[0]*nU[0] + P[1]*nU[1] + P[2]*nU[2]);
    const float dF = -(P[0]*nF[0] + P[1]*nF[1] + P[2]*nF[2]);
    // V' (world->view) for OUR camera.
    const float Vp[16] = {
        nR[0], nU[0], nF[0], 0.0f,
        nR[1], nU[1], nF[1], 0.0f,
        nR[2], nU[2], nF[2], 0.0f,
        dR,    dU,    dF,    1.0f
    };
    float D[16], newVP[16];
    FcMul(Vp, Vinv, D);                    // D = V' * V^-1
    FcMul(D, blk, newVP);                  // newVP = D * VP
    memcpy(blk, newVP, sizeof(newVP));

    // Keep the derived camera registers consistent. Shaders use these for
    // specular, billboarding and fog; leaving them on the old camera makes
    // those effects disagree with the geometry.
    blk[4*4+0] = nR[0]; blk[4*4+1] = nR[1]; blk[4*4+2] = nR[2];
    blk[5*4+0] = nU[0]; blk[5*4+1] = nU[1]; blk[5*4+2] = nU[2];
    blk[6*4+0] = P[0];  blk[6*4+1] = P[1];  blk[6*4+2] = P[2];
    return true;
}

} // namespace

HRESULT __stdcall ProxyIDirect3DDevice9::SetVertexShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) { if (g_trOn) { TrLog("SetVertexShaderConstantF(%u, n=%u)", StartRegister, Vector4fCount); if (pConstantData && TrHotVS(StartRegister)) for (UINT q=0; q<Vector4fCount && q<8u; ++q) TrLog("      c%-3u = [%10.4f %10.4f %10.4f %10.4f]", StartRegister+q, pConstantData[q*4+0], pConstantData[q*4+1], pConstantData[q*4+2], pConstantData[q*4+3]); }
    if (g_Capturing && g_CaptureFile && pConstantData) {
        for (UINT i = 0; i < Vector4fCount; ++i)
            fprintf(g_CaptureFile, "  VS c%u = (%.6f, %.6f, %.6f, %.6f)\n",
                    StartRegister + i, pConstantData[i*4], pConstantData[i*4+1], pConstantData[i*4+2], pConstantData[i*4+3]);
    }

    // Full live cache of c0-c255, continuously overwritten.
    // Bridge can dump this on demand to find which register is the camera.
    if (pConstantData && Vector4fCount > 0) {
        UINT end = StartRegister + Vector4fCount;
        if (end > 256) end = 256;
        UINT regs = end - StartRegister;
        memcpy(g_VbAllRegs + StartRegister * 4, pConstantData, regs * 16);
        // one-shot capture: mirror into the dumped bank (BANKVS at frame end)
        memcpy(g_trVSBank + StartRegister * 4, pConstantData, regs * 16);

        // One-shot: log to RE\c245_hit.log the first time c245 is written
        // with a non-zero value — confirms the proxy is in the live path.
        static bool s_c245Logged = false;
        if (!s_c245Logged && StartRegister <= 245 && end > 245) {
            const float* s = pConstantData + (245 - StartRegister) * 4;
            if (s[0] != 0.0f || s[1] != 0.0f || s[2] != 0.0f) {
                s_c245Logged = true;
                char p[MAX_PATH];
                GetModuleFileNameA(nullptr, p, MAX_PATH);
                char* slash = strrchr(p, '\\');
                if (slash) *(slash + 1) = '\0';
                strcat_s(p, "c245_hit.log");
                FILE* f = nullptr;
                fopen_s(&f, p, "w");
                if (f) {
                    fprintf(f, "c245 first non-zero write: (%.3f, %.3f, %.3f, %.3f)\n"
                               "  via SetVertexShaderConstantF(Start=%u, Count=%u)\n",
                            s[0], s[1], s[2], s[3], StartRegister, Vector4fCount);
                    fclose(f);
                }
            }
        }
    }

    // Snapshot the camera regs c239..c242 (viewProj).
    //
    // 2026-07-16 FIX: the old filter demanded (Start==239 && Count>=7) — i.e.
    // only the one batched 8-register upload. When the game uploads the matrix
    // as its own 4-register write (Start=239, Count=4), the snapshot NEVER
    // fired and g_VbWorldCam kept its IDENTITY initializer. Everything that
    // projected through it (collision boxes, raycast lines) got identity =
    // world coords = flat squares near the origin. Proven from the overlay log:
    // "VP [1 0 0 0 | 0 1 0 0 | 0 0 1 0 | 0 0 0 1]".
    //
    // Now: accept ANY write that COVERS c239..c242, regardless of batching,
    // but reject the HUD/UI zero-clobber (it rewrites these regs with zeros)
    // by requiring the captured matrix to be non-degenerate.
    // One-shot forensics: record the first few writes that touch the c239-242
    // window, so if the capture still misses we can SEE the real upload shape
    // (Start/Count) instead of guessing. Written next to the exe.
    if (pConstantData && StartRegister <= 242 && (StartRegister + Vector4fCount) > 239) {
        static int s_vpWritesLogged = 0;
        if (s_vpWritesLogged < 8) {
            ++s_vpWritesLogged;
            char p[MAX_PATH]; GetModuleFileNameA(nullptr, p, MAX_PATH);
            char* sl = strrchr(p, '\\');
            if (sl) strcpy_s(sl + 1, MAX_PATH - (sl + 1 - p), "vpwrites.log");
            FILE* f = nullptr; fopen_s(&f, p, s_vpWritesLogged == 1 ? "w" : "a");
            if (f) {
                fprintf(f, "SetVSConstF(Start=%u, Count=%u) c239row0=(%.3f %.3f %.3f %.3f)\n",
                        StartRegister, Vector4fCount,
                        (StartRegister <= 239 && StartRegister + Vector4fCount > 239) ? pConstantData[(239 - StartRegister) * 4 + 0] : 0.0f,
                        (StartRegister <= 239 && StartRegister + Vector4fCount > 239) ? pConstantData[(239 - StartRegister) * 4 + 1] : 0.0f,
                        (StartRegister <= 239 && StartRegister + Vector4fCount > 239) ? pConstantData[(239 - StartRegister) * 4 + 2] : 0.0f,
                        (StartRegister <= 239 && StartRegister + Vector4fCount > 239) ? pConstantData[(239 - StartRegister) * 4 + 3] : 0.0f);
                fclose(f);
            }
        }
    }

    if (pConstantData && StartRegister <= 239 && (StartRegister + Vector4fCount) >= 243) {
        const float* src = pConstantData + (239 - StartRegister) * 4;
        // 2026-07-16 (proven from vpwrites.log): the HUD/UI writes IDENTITY to
        // c239-242 with the SAME call shape as the world render (Start=239,
        // Count=8). Last-writer-wins meant the camera was identity by EndScene
        // -> every 3D draw bailed -> nothing visible. An all-zero guard doesn't
        // catch that, so reject identity (and zero) and keep the last REAL
        // camera. A true viewProj is never identity.
        bool allZero = true, isIdentity = true;
        static const float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        for (int i = 0; i < 16; ++i) {
            if (src[i] != 0.0f) allZero = false;
            if (src[i] != I[i]) isIdentity = false;
        }
        if (!allZero && !isIdentity) {
            memcpy(g_VbWorldCam, src, 16 * sizeof(float));       // c239..c242
            // Grab the camera vectors too when the write reaches that far.
            if ((StartRegister + Vector4fCount) >= 247)
                memcpy(g_VbWorldCam + 16, src + 16, 16 * sizeof(float)); // c243..c246
        }
    }

    // ── FREE CAMERA OVERRIDE (banner at FcPatch) ────────────────────────────
    // Substitute our camera into the world-render upload on its way through.
    // Conditions are deliberately narrow:
    //   * the write must COVER c239..c245, so VP *and* the basis are present in
    //     this one call — we never mix a fresh VP with a stale cached basis
    //   * it must be the REAL camera, not the HUD/UI pass. Proven from
    //     vpwrites.log (see the snapshot banner above): the HUD rewrites these
    //     same registers with IDENTITY using the SAME call shape. Patching that
    //     would drag the entire 2D interface through our view matrix.
    // Failing either test we fall through untouched, so with the flycam off
    // this whole block is one comparison and the proxy behaves exactly as before.
    if (g_fcOn && pConstantData && Vector4fCount > 0 &&
        StartRegister <= 239 && (StartRegister + Vector4fCount) >= 246) {
        const float* vp = pConstantData + (239 - StartRegister) * 4;
        static const float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        bool allZero = true, isIdentity = true;
        for (int i = 0; i < 16; ++i) {
            if (vp[i] != 0.0f) allZero = false;
            if (vp[i] != I[i]) isIdentity = false;
        }
        if (!allZero && !isIdentity) {
            // Patch a COPY. pConstantData is the game's buffer and may well be
            // reused or const; writing through it would corrupt engine state.
            float blk[8 * 4];
            memcpy(blk, vp, sizeof(blk));
            if (FcPatch(blk)) {
                float patched[256 * 4];
                UINT n = Vector4fCount; if (n > 256) n = 256;
                memcpy(patched, pConstantData, n * 16);
                memcpy(patched + (239 - StartRegister) * 4, blk, sizeof(blk));
                return m_pReal->SetVertexShaderConstantF(StartRegister, patched, n);
            }
        }
    }

    return m_pReal->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}
HRESULT __stdcall ProxyIDirect3DDevice9::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) { return m_pReal->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetVertexShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) { return m_pReal->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) { return m_pReal->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetVertexShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) { return m_pReal->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) { return m_pReal->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) { if (StreamNumber==0){ g_trVB0=pStreamData; g_trStride0=Stride; } TrLog("SetStreamSource(%u, %p, off=%u, stride=%u)", StreamNumber, (void*)pStreamData, OffsetInBytes, Stride); return m_pReal->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride) { return m_pReal->GetStreamSource(StreamNumber, ppStreamData, pOffsetInBytes, pStride); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetStreamSourceFreq(UINT StreamNumber, UINT Setting) { return m_pReal->SetStreamSourceFreq(StreamNumber, Setting); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetStreamSourceFreq(UINT StreamNumber, UINT* pSetting) { return m_pReal->GetStreamSourceFreq(StreamNumber, pSetting); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetIndices(IDirect3DIndexBuffer9* pIndexData) { g_trIB=pIndexData; TrLog("SetIndices(%p)", (void*)pIndexData); return m_pReal->SetIndices(pIndexData); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetIndices(IDirect3DIndexBuffer9** ppIndexData) { return m_pReal->GetIndices(ppIndexData); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreatePixelShader(const DWORD* pFunction, IDirect3DPixelShader9** ppShader) {
    HRESULT hr = m_pReal->CreatePixelShader(pFunction, ppShader);
    if (SUCCEEDED(hr) && pFunction && ppShader && *ppShader) {
        std::string name = ExtractShaderName(pFunction);
        g_shaderNames[(void*)*ppShader] = name;
        g_shaderDumpCount++;
    }
    return hr;
}
HRESULT __stdcall ProxyIDirect3DDevice9::SetPixelShader(IDirect3DPixelShader9* pShader) { g_trPS=pShader; TrLog("SetPixelShader(%p)", (void*)pShader);
    g_curPS = pShader;
    if (g_Capturing && pShader) DumpShaderBytecode((void*)pShader, false);
    return m_pReal->SetPixelShader(pShader);
}
HRESULT __stdcall ProxyIDirect3DDevice9::GetPixelShader(IDirect3DPixelShader9** ppShader) { return m_pReal->GetPixelShader(ppShader); }
// 08-07 TERRAIN ORACLE. c200-c213 is the terrain UV block — SEVEN row_major
// float2x4 transforms, decoded from the retail shader's own register map
// (Mg_FP_Terrain3_L2_VtxAtm_WPos_Shdw.asm):
//   c200/202 XfmD0,N0   c204/206 XfmD1,N1   c208/210 XfmD2,N2   c212 XfmB
// The pixel shader builds EVERY terrain texcoord from these against the
// interpolated world position (dp4 r1.x, c212, v0 / dp4 r1.y, c213, v0), because
// terrain carries no stored UVs at all. A zero block therefore collapses the
// whole ground to one texel, which is the flat-sand symptom we are chasing in
// the host engine. The generic path below logs only the FIRST register of a
// write, so a single 14-register upload was hiding twelve of the fourteen rows —
// including c212/c213, the base transform. Log the whole window when it lands.
static bool TrTerrainUVWindow(UINT s, UINT n) { return s <= 213u && (s + n) > 200u; }

HRESULT __stdcall ProxyIDirect3DDevice9::SetPixelShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) {
    // one-shot capture: mirror the whole PS constant file (BANKPS at frame end).
    // Unconditional so the bank is already warm when F11 arms mid-frame.
    if (pConstantData && Vector4fCount > 0) {
        UINT e = StartRegister + Vector4fCount; if (e > 256) e = 256;
        if (StartRegister < 256) memcpy(g_trPSBank + StartRegister * 4, pConstantData, (e - StartRegister) * 16);
    }
    if (g_trOn) { TrLog("SetPixelShaderConstantF(%u, n=%u)", StartRegister, Vector4fCount); if (pConstantData && Vector4fCount) TrLog("      p%-3u = [%10.4f %10.4f %10.4f %10.4f]", StartRegister, pConstantData[0], pConstantData[1], pConstantData[2], pConstantData[3]);
        if (pConstantData && TrTerrainUVWindow(StartRegister, Vector4fCount)) {
            static const char* kT3[14] = { "XfmD0.r0","XfmD0.r1","XfmN0.r0","XfmN0.r1",
                                           "XfmD1.r0","XfmD1.r1","XfmN1.r0","XfmN1.r1",
                                           "XfmD2.r0","XfmD2.r1","XfmN2.r0","XfmN2.r1",
                                           "XfmB.r0", "XfmB.r1" };
            for (UINT q = 0; q < Vector4fCount; ++q) {
                const UINT reg = StartRegister + q;
                if (reg < 200u || reg > 213u) continue;
                TrLog("      T3 c%-3u %-9s = [%10.4f %10.4f %10.4f %10.4f]",
                      reg, kT3[reg - 200u],
                      pConstantData[q*4+0], pConstantData[q*4+1],
                      pConstantData[q*4+2], pConstantData[q*4+3]);
            }
        } }
    if (g_Capturing && g_CaptureFile && pConstantData) {
        for (UINT i = 0; i < Vector4fCount; ++i)
            fprintf(g_CaptureFile, "  PS c%u = (%.6f, %.6f, %.6f, %.6f)\n",
                    StartRegister + i, pConstantData[i*4], pConstantData[i*4+1], pConstantData[i*4+2], pConstantData[i*4+3]);
    }
    return m_pReal->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}
HRESULT __stdcall ProxyIDirect3DDevice9::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) { return m_pReal->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetPixelShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) { return m_pReal->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) { return m_pReal->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetPixelShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) { return m_pReal->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) { return m_pReal->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::DrawRectPatch(UINT Handle, const float* pNumSegs, const D3DRECTPATCH_INFO* pRectPatchInfo) { return m_pReal->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo); }
HRESULT __stdcall ProxyIDirect3DDevice9::DrawTriPatch(UINT Handle, const float* pNumSegs, const D3DTRIPATCH_INFO* pTriPatchInfo) { return m_pReal->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo); }
HRESULT __stdcall ProxyIDirect3DDevice9::DeletePatch(UINT Handle) { return m_pReal->DeletePatch(Handle); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) { return m_pReal->CreateQuery(Type, ppQuery); }

