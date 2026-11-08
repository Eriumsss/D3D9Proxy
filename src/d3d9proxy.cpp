#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <new>

#include "d3d9device.h"
#include "memorypatch.h"
#include "retail_fn_trace.h"
#include "retail_heap_extender.h"
#include "static_pool_census.h"
#include "model_registry_2048.h"
#include "texture_registry_4096.h"
#include "effect_registry_1024.h"
#include "gfx_registry_512.h"
#include "animation_binding_cache_512.h"
#include "auxiliary_registry_8192.h"
#include "device_reset_pool_4096.h"
#include "dynamic_buffer_reset_pools_4096.h"
#include "proxy_config.h"
#include "geometry_budget_telemetry.h"

// The frame tracer (F11) sees every draw but none of the code behind it. The
// function tracer is the other half: it traps catalogued retail entry points
// and logs them by name. Inert unless retail_trace.ini says Enabled=1.
extern volatile DWORD g_FrameCounter;

// Real d3d9.dll handle and function
static HMODULE g_RealD3D9 = nullptr;
typedef IDirect3D9* (WINAPI* Direct3DCreate9_t)(UINT SDKVersion);
static Direct3DCreate9_t g_RealDirect3DCreate9 = nullptr;

// Our wrapper IDirect3D9 class
class ProxyIDirect3D9 : public IDirect3D9 {
private:
    IDirect3D9* m_pReal;

public:
    ProxyIDirect3D9(IDirect3D9* pReal) : m_pReal(pReal) {}

    // IUnknown
    HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        *ppvObj = nullptr;
        // Keep COM identity on the proxy. Returning the raw IDirect3D9
        // would bypass CreateDevice and every device hook.
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDirect3D9)) {
            m_pReal->AddRef();
            *ppvObj = static_cast<IDirect3D9*>(this);
            return S_OK;
        }
        return m_pReal->QueryInterface(riid, ppvObj);
    }
    ULONG __stdcall AddRef() override { return m_pReal->AddRef(); }
    ULONG __stdcall Release() override {
        ULONG count = m_pReal->Release();
        if (count == 0) delete this;
        return count;
    }

    // IDirect3D9 - forward all except CreateDevice
    HRESULT __stdcall RegisterSoftwareDevice(void* pInit) override {
        return m_pReal->RegisterSoftwareDevice(pInit);
    }
    UINT __stdcall GetAdapterCount() override { return m_pReal->GetAdapterCount(); }
    HRESULT __stdcall GetAdapterIdentifier(UINT a, DWORD b, D3DADAPTER_IDENTIFIER9* c) override {
        return m_pReal->GetAdapterIdentifier(a, b, c);
    }
    UINT __stdcall GetAdapterModeCount(UINT a, D3DFORMAT b) override {
        return m_pReal->GetAdapterModeCount(a, b);
    }
    HRESULT __stdcall EnumAdapterModes(UINT a, D3DFORMAT b, UINT c, D3DDISPLAYMODE* d) override {
        return m_pReal->EnumAdapterModes(a, b, c, d);
    }
    HRESULT __stdcall GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* b) override {
        return m_pReal->GetAdapterDisplayMode(a, b);
    }
    HRESULT __stdcall CheckDeviceType(UINT a, D3DDEVTYPE b, D3DFORMAT c, D3DFORMAT d, BOOL e) override {
        return m_pReal->CheckDeviceType(a, b, c, d, e);
    }
    HRESULT __stdcall CheckDeviceFormat(UINT a, D3DDEVTYPE b, D3DFORMAT c, DWORD d, D3DRESOURCETYPE e, D3DFORMAT f) override {
        return m_pReal->CheckDeviceFormat(a, b, c, d, e, f);
    }
    HRESULT __stdcall CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE b, D3DFORMAT c, BOOL d, D3DMULTISAMPLE_TYPE e, DWORD* f) override {
        return m_pReal->CheckDeviceMultiSampleType(a, b, c, d, e, f);
    }
    HRESULT __stdcall CheckDepthStencilMatch(UINT a, D3DDEVTYPE b, D3DFORMAT c, D3DFORMAT d, D3DFORMAT e) override {
        return m_pReal->CheckDepthStencilMatch(a, b, c, d, e);
    }
    HRESULT __stdcall CheckDeviceFormatConversion(UINT a, D3DDEVTYPE b, D3DFORMAT c, D3DFORMAT d) override {
        return m_pReal->CheckDeviceFormatConversion(a, b, c, d);
    }
    HRESULT __stdcall GetDeviceCaps(UINT a, D3DDEVTYPE b, D3DCAPS9* c) override {
        return m_pReal->GetDeviceCaps(a, b, c);
    }
    HMONITOR __stdcall GetAdapterMonitor(UINT a) override {
        return m_pReal->GetAdapterMonitor(a);
    }

    // CreateDevice - wrap the device!
    // Also force windowed mode so the editor's D3D9 device doesn't get
    // invalidated (exclusive fullscreen causes device-lost in other D3D9
    // apps on the same GPU).
    HRESULT __stdcall CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pParams,
                                    IDirect3DDevice9** ppDevice) override {
        if (!ppDevice) return D3DERR_INVALIDCALL;
        *ppDevice = nullptr;
        if (pParams) {
            pParams->Windowed = TRUE;
            pParams->FullScreen_RefreshRateInHz = 0;
            // Without a known desktop-compatible backbuffer format, D3D9 fails
            // to create a windowed device. D3DFMT_UNKNOWN tells D3D to use the
            // current desktop format.
            pParams->BackBufferFormat = D3DFMT_UNKNOWN;
        }
        IDirect3DDevice9* pRealDevice = nullptr;
        HRESULT hr = m_pReal->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pParams, &pRealDevice);
        if (SUCCEEDED(hr) && pRealDevice) {
            ProxyIDirect3DDevice9* wrapped = new (std::nothrow) ProxyIDirect3DDevice9(pRealDevice);
            if (!wrapped) {
                pRealDevice->Release();
                return E_OUTOFMEMORY;
            }
            *ppDevice = wrapped;
        }
        else if (pRealDevice) pRealDevice->Release();
        return hr;
    }
};

// Function pointers for forwarded functions
typedef HRESULT(WINAPI* Direct3DCreate9Ex_t)(UINT, void**);
typedef int(WINAPI* D3DPERF_BeginEvent_t)(D3DCOLOR, LPCWSTR);
typedef int(WINAPI* D3DPERF_EndEvent_t)(void);
typedef void(WINAPI* D3DPERF_SetMarker_t)(D3DCOLOR, LPCWSTR);
typedef void(WINAPI* D3DPERF_SetRegion_t)(D3DCOLOR, LPCWSTR);
typedef BOOL(WINAPI* D3DPERF_QueryRepeatFrame_t)(void);
typedef void(WINAPI* D3DPERF_SetOptions_t)(DWORD);
typedef DWORD(WINAPI* D3DPERF_GetStatus_t)(void);

static Direct3DCreate9Ex_t g_RealDirect3DCreate9Ex = nullptr;
static D3DPERF_BeginEvent_t g_RealD3DPERF_BeginEvent = nullptr;
static D3DPERF_EndEvent_t g_RealD3DPERF_EndEvent = nullptr;
static D3DPERF_SetMarker_t g_RealD3DPERF_SetMarker = nullptr;
static D3DPERF_SetRegion_t g_RealD3DPERF_SetRegion = nullptr;
static D3DPERF_QueryRepeatFrame_t g_RealD3DPERF_QueryRepeatFrame = nullptr;
static D3DPERF_SetOptions_t g_RealD3DPERF_SetOptions = nullptr;
static D3DPERF_GetStatus_t g_RealD3DPERF_GetStatus = nullptr;

static bool LoadRealD3D9() {
    if (g_RealD3D9) return true;

    // ── RTX REMIX CHAIN-LOAD (2026-08-07) ───────────────────────────────────
    // This proxy used to go straight to the SYSTEM d3d9.dll, which silently
    // cut RTX Remix out of the chain entirely: Remix's client is installed
    // beside the exe as d3d9_remix.dll (renamed so THIS dll can own the
    // d3d9.dll name), and nothing was ever loading it.
    //
    // Honour the contract proxy.ini already documents:
    //     [Remix] Enabled=1  DLLName=d3d9_remix.dll
    // so the chain is  game -> us -> Remix -> .trex runtime. That ordering is
    // what makes the free camera work under RTX as well as in the raster view:
    // we move the camera BEFORE Remix sees it, so Remix needs no camera
    // override of its own (rtx.camera.enableFreeCamera does nothing here).
    //
    // Falls back to the system DLL when Remix is absent or disabled, so a
    // machine without Remix installed behaves exactly as before.
    char path[MAX_PATH];
    path[0] = '\0';
    {
        char ini[MAX_PATH], exeDir[MAX_PATH];
        GetModuleFileNameA(NULL, exeDir, MAX_PATH);
        char* sl = strrchr(exeDir, '\\');
        if (sl) *(sl + 1) = '\0'; else exeDir[0] = '\0';
        _snprintf_s(ini, MAX_PATH, _TRUNCATE, "%sproxy.ini", exeDir);

        // Default ON with Remix's conventional name, so a missing/short
        // proxy.ini still chains correctly rather than silently bypassing.
        UINT enabled = GetPrivateProfileIntA("Remix", "Enabled", 1, ini);
        char dllName[MAX_PATH];
        GetPrivateProfileStringA("Remix", "DLLName", "d3d9_remix.dll",
                                 dllName, MAX_PATH, ini);
        if (enabled) {
            char cand[MAX_PATH];
            _snprintf_s(cand, MAX_PATH, _TRUNCATE, "%s%s", exeDir, dllName);
            if (GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES)
                strcpy_s(path, cand);
        }
    }
    if (path[0] == '\0') {          // no Remix -> the original system path
        GetSystemDirectoryA(path, MAX_PATH);
        strcat_s(path, "\\d3d9.dll");
    }
    g_RealD3D9 = LoadLibraryA(path);
    if (!g_RealD3D9) return false;

    g_RealDirect3DCreate9 = (Direct3DCreate9_t)GetProcAddress(g_RealD3D9, "Direct3DCreate9");
    g_RealDirect3DCreate9Ex = (Direct3DCreate9Ex_t)GetProcAddress(g_RealD3D9, "Direct3DCreate9Ex");
    g_RealD3DPERF_BeginEvent = (D3DPERF_BeginEvent_t)GetProcAddress(g_RealD3D9, "D3DPERF_BeginEvent");
    g_RealD3DPERF_EndEvent = (D3DPERF_EndEvent_t)GetProcAddress(g_RealD3D9, "D3DPERF_EndEvent");
    g_RealD3DPERF_SetMarker = (D3DPERF_SetMarker_t)GetProcAddress(g_RealD3D9, "D3DPERF_SetMarker");
    g_RealD3DPERF_SetRegion = (D3DPERF_SetRegion_t)GetProcAddress(g_RealD3D9, "D3DPERF_SetRegion");
    g_RealD3DPERF_QueryRepeatFrame = (D3DPERF_QueryRepeatFrame_t)GetProcAddress(g_RealD3D9, "D3DPERF_QueryRepeatFrame");
    g_RealD3DPERF_SetOptions = (D3DPERF_SetOptions_t)GetProcAddress(g_RealD3D9, "D3DPERF_SetOptions");
    g_RealD3DPERF_GetStatus = (D3DPERF_GetStatus_t)GetProcAddress(g_RealD3D9, "D3DPERF_GetStatus");

    // AUTO-LOAD DebugOverlay.dll (2026-07-16): it was the ONLY DLL that needed
    // the external Injector.exe, so if the injector wasn't run it silently
    // never loaded — and god mode / raycasts / the whole overlay live in it.
    // We auto-load, and the proxy reliably loads (it IS d3d9.dll), so the
    // overlay now always comes up with the game. Idempotent: LoadLibrary is
    // ref-counted, and the overlay's hooks install-once. Load from next to the
    // exe (where deploy puts it), fall back to a bare name (PATH/cwd search).
    static bool s_overlayLoaded = false;
    if (!s_overlayLoaded) {
        s_overlayLoaded = true;
        char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
        char* slash = strrchr(exe, '\\');
        if (slash) { strcpy_s(slash + 1, MAX_PATH - (slash + 1 - exe), "DebugOverlay.dll");
                     if (!LoadLibraryA(exe)) LoadLibraryA("DebugOverlay.dll"); }
        else LoadLibraryA("DebugOverlay.dll");
    }

    return true;
}

// Both Direct3DCreate9 and Direct3DCreate9Ex must enter this gate. The old
// bool was racy and the Ex path skipped patch initialization entirely.
static INIT_ONCE g_PatchesOnce = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK ProxyApplyPatchesOnce(PINIT_ONCE, PVOID, PVOID*) {
    // On the FIRST D3D9 entry, create the missing config BEFORE any subsystem
    // reads its keys. Never do file I/O inside the Windows loader lock.
    OC_ProxyConfig_Ensure();
    OC_HeapExt_Init(); // opt-in, fingerprinted; no .text changes
    OC_ModelRegistry2048_Init(); // opt-in: guarded registry+factory cap patch, before pool census
    OC_TextureRegistry4096_Init(); // opt-in: two texture vectors and matching factory guard
    OC_EffectRegistry1024_Init(); // opt-in: three FX vectors and the matching factory guard
    OC_GfxRegistry512_Init(); // opt-in: ONLY level GFX vector + matching 64-entry factory guard
    OC_AnimationBindingCache512_Init(); // opt-in: independent clip + skeleton cache cap 64->256/512 AFTER GFX validation
    OC_AuxiliaryRegistry8192_Init(); // opt-in: +0xF4 ResourceOther and +0x114 PField native reserve+guard
    OC_DeviceResetPool4096_Init(); // opt-in: native D3D reset list 64 to 1024/4096, guarded append
    OC_DynamicBufferResetPools4096_Init(); // opt-in: both dynamic VB/IB reset lists 64 to 1024/4096
    OC_StaticPoolCensus_Init(); // opt-in, read-only; samples during level loading
    OC_GeometryBudget_Init(); // opt-in, read-only D3D managed VB/IB pressure and failures
    ApplyMemoryPatches();
    return TRUE;
}
static void EnsureProxyPatches() {
    InitOnceExecuteOnce(&g_PatchesOnce, ProxyApplyPatchesOnce, nullptr, nullptr);
}

// Forward decl from filehook.cpp
extern "C" void InstallFileHooks();

// Forward decls from vespucci_bridge.cpp
extern "C" void VespucciBridge_Init();
extern "C" void VespucciBridge_Shutdown();

// Forward decls from stronghold_log.cpp
extern "C" void SHLog_Init();
extern "C" void SHLog_Shutdown();

// Exported Direct3DCreate9
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
    // Apply memory patches on first D3D call (safer than DllMain)
    // File hooks were already installed in DllMain (earlier, to catch mutex).
    EnsureProxyPatches();

    if (!LoadRealD3D9() || !g_RealDirect3DCreate9) return nullptr;

    IDirect3D9* pReal = g_RealDirect3DCreate9(SDKVersion);
    if (!pReal) return nullptr;

    ProxyIDirect3D9* wrapped = new (std::nothrow) ProxyIDirect3D9(pReal);
    if (!wrapped) { pReal->Release(); return nullptr; }
    return wrapped;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D) {
    EnsureProxyPatches();
    if (!LoadRealD3D9() || !g_RealDirect3DCreate9Ex) return E_FAIL;
    return g_RealDirect3DCreate9Ex(SDKVersion, (void**)ppD3D);
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name) {
    if (!LoadRealD3D9() || !g_RealD3DPERF_BeginEvent) return 0;
    return g_RealD3DPERF_BeginEvent(color, name);
}

int WINAPI D3DPERF_EndEvent(void) {
    if (!LoadRealD3D9() || !g_RealD3DPERF_EndEvent) return 0;
    return g_RealD3DPERF_EndEvent();
}

void WINAPI D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name) {
    if (LoadRealD3D9() && g_RealD3DPERF_SetMarker)
        g_RealD3DPERF_SetMarker(color, name);
}

void WINAPI D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name) {
    if (LoadRealD3D9() && g_RealD3DPERF_SetRegion)
        g_RealD3DPERF_SetRegion(color, name);
}

BOOL WINAPI D3DPERF_QueryRepeatFrame(void) {
    if (!LoadRealD3D9() || !g_RealD3DPERF_QueryRepeatFrame) return FALSE;
    return g_RealD3DPERF_QueryRepeatFrame();
}

void WINAPI D3DPERF_SetOptions(DWORD options) {
    if (LoadRealD3D9() && g_RealD3DPERF_SetOptions)
        g_RealD3DPERF_SetOptions(options);
}

DWORD WINAPI D3DPERF_GetStatus(void) {
    if (!LoadRealD3D9() || !g_RealD3DPERF_GetStatus) return 0;
    return g_RealD3DPERF_GetStatus();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            // Install file/mutex hooks IMMEDIATELY — before game's startup
            // checks its single-instance mutex. Don't do heavy work here
            // (loader lock), but IAT patching is safe.
            InstallFileHooks();
            // Spin up Vespucci bridge (I/O on its own thread — DllMain-safe).
            VespucciBridge_Init();
            // Start Stronghold activity logger (separate file + watcher thread).
            SHLog_Init();
            // Retail function tracer. Stamping every logged call with the D3D
            // frame number is what lets it be read alongside retail_trace.log:
            // "these 40 functions ran during the frame that drew the dome".
            RTF_SetFrameCounter((volatile unsigned long*)&g_FrameCounter);
            RTF_Init();
            break;
        case DLL_PROCESS_DETACH:
            RTF_Shutdown();
            SHLog_Shutdown();
            VespucciBridge_Shutdown();
            CleanupMemoryPatcher();
            if (g_RealD3D9) FreeLibrary(g_RealD3D9);
            break;
    }
    return TRUE;
}

