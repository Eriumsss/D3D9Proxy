// ═══════════════════════════════════════════════════════════════════════════
// ConquestTrace.dll — the retail function tracer with nothing else attached.
//
// WHY A SECOND DLL. d3d9.dll in the game folder belongs to the RTX Remix FFP
// wrapper: it converts the engine's shader draws to fixed-function so Remix
// can see any geometry at all, and two proxies cannot both own that name.
// Overwriting it with our D3D9Proxy silently kills the entire RTX path (learnt
// the hard way, 08-07). The tracer needs no D3D at all — it only wants the
// process — so it ships separately and gets injected, leaving the wrapper in
// place. The same retail_fn_trace.cpp is also compiled INTO D3D9Proxy for the
// runs where our proxy does own d3d9.dll.
//
// Reads retail_trace.ini and writes retail_calls.log / retail_coverage.csv
// NEXT TO THIS DLL, never the working directory.
// ═══════════════════════════════════════════════════════════════════════════
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "retail_fn_trace.h"

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(mod);
        RTF_Init();          // spawns the worker; does nothing else under the
        break;               // loader lock
    case DLL_PROCESS_DETACH:
        RTF_Shutdown();
        break;
    }
    return TRUE;
}
