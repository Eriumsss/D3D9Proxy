// Overlay + callback registration system for cooperative hooking.
// Other DLLs (ConquestConsole, etc.) can register callbacks instead of
// patching D3D9 vtables — prevents crashes when both DLLs are loaded.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <vector>

#include "d3d9device.h"

// Forward decl from vespucci_bridge.cpp
extern "C" void VespucciBridge_OnFrameDev(IDirect3DDevice9* device);

// ═══════════════════════════════════════════════════════════════════════════
//  Callback registration system
//  External DLLs call RegisterEndSceneCallback/RegisterResetCallback to hook
//  into our D3D pipeline without patching vtables.
// ═══════════════════════════════════════════════════════════════════════════
typedef void (*D3DCallback_t)(IDirect3DDevice9*);
typedef void (*ResetCallback_t)();

static std::vector<D3DCallback_t> g_endSceneCallbacks;
static std::vector<D3DCallback_t> g_beginSceneCallbacks;
static std::vector<ResetCallback_t> g_resetCallbacks;

extern "C" void RegisterEndSceneCallback(D3DCallback_t fn) {
    if (fn) g_endSceneCallbacks.push_back(fn);
}
extern "C" void RegisterBeginSceneCallback(D3DCallback_t fn) {
    if (fn) g_beginSceneCallbacks.push_back(fn);
}
extern "C" void RegisterResetCallback(ResetCallback_t fn) {
    if (fn) g_resetCallbacks.push_back(fn);
}
extern "C" void UnregisterEndSceneCallback(D3DCallback_t fn) {
    for (auto it = g_endSceneCallbacks.begin(); it != g_endSceneCallbacks.end(); ++it)
        if (*it == fn) { g_endSceneCallbacks.erase(it); return; }
}

// Lets external DLLs check if they're running inside D3D9Proxy
extern "C" bool IsD3D9ProxyActive() { return true; }

void Overlay_OnBeginScene(IDirect3DDevice9* device) {
    // Check F4 input for wireframe toggle
    static bool lastF4 = false;
    bool f4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
    if (f4 && !lastF4) {
        g_WireframeEnabled = !g_WireframeEnabled;
    }
    lastF4 = f4;

    // Call all registered BeginScene callbacks (e.g. ConquestConsole)
    for (size_t i = 0; i < g_beginSceneCallbacks.size(); ++i)
        g_beginSceneCallbacks[i](device);
}

void Overlay_OnEndScene(IDirect3DDevice9* device) {
    // Vespucci bridge frame tick — enqueues HELLO/FRAME/CAMERA for the editor.
    // No-op when no editor is connected.
    VespucciBridge_OnFrameDev(device);

    // Call all registered EndScene callbacks (e.g. ConquestConsole overlay)
    for (size_t i = 0; i < g_endSceneCallbacks.size(); ++i)
        g_endSceneCallbacks[i](device);
}

void Overlay_OnReset() {
    // Notify all registered reset callbacks
    for (size_t i = 0; i < g_resetCallbacks.size(); ++i)
        g_resetCallbacks[i]();
}

