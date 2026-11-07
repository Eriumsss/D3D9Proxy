// Vespucci <-> Game bridge (Phase 1+: multi-client).
//
// Named-pipe server that accepts up to N concurrent clients (editor + probe +
// any other tool). Each client gets its own I/O thread and outgoing queue.
// Broadcast messages (FRAME_BEGIN, CAMERA, PLAYER, ENTITY_SNAPSHOT, etc.) are
// fanned out to every connected client.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <deque>
#include <vector>
#include <string>

#include "vespucci_bridge.h"

// Live camera state from d3d9device.cpp (only captures the world-path batched
// write to c239..c246, so HUD can't clobber it).
extern "C" {
    extern float g_VbWorldCam[8 * 4];
    extern float g_VbAllRegs[256 * 4];
}

// SEH-wrapped byte copy (must be outside the anonymous namespace / no C++ objects
// inside the function body). Reads up to `n` bytes from `src` into `dst`;
// returns the number of bytes successfully copied before an AV (or `n` on full).
static uint32_t SafeMemcpyFromGame(uint8_t* dst, const uint8_t* src, uint32_t n) {
    uint32_t got = 0;
    __try {
        for (uint32_t i = 0; i < n; ++i) {
            dst[i] = src[i];
            got = i + 1;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // got holds the count before the AV.
    }
    return got;
}

namespace {

// ── Shared state ─────────────────────────────────────────────────────────
HANDLE               g_acceptThread = nullptr;
volatile LONG        g_stopFlag     = 0;
volatile LONG        g_clientCount  = 0;

VespucciCmdHandler_t g_cmdHandler   = nullptr;

uint64_t             g_frameIdx     = 0;
DWORD                g_lastTick     = 0;

FILE*                g_log          = nullptr;
CRITICAL_SECTION     g_logCs;
bool                 g_logCsInited  = false;

// ── Per-client state ─────────────────────────────────────────────────────
struct ClientCtx {
    HANDLE                              pipe;
    HANDLE                              thread;
    CRITICAL_SECTION                    outCs;
    std::deque<std::vector<uint8_t>>    outQueue;
    volatile LONG                       alive;
};

// List of active clients, for broadcast.
CRITICAL_SECTION                  g_clientsCs;
bool                              g_clientsCsInited = false;
std::vector<ClientCtx*>           g_clients;

constexpr int kMaxClients = 4;

// ── Logging ──────────────────────────────────────────────────────────────
void BridgeLog(const char* fmt, ...) {
    if (!g_log || !g_logCsInited) return;
    va_list ap;
    va_start(ap, fmt);
    EnterCriticalSection(&g_logCs);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logCs);
    va_end(ap);
}

void OpenLog() {
    if (g_log) return;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat_s(path, "bridge.log");
    fopen_s(&g_log, path, "w");
    BridgeLog("[bridge] log opened");
}

// ── Message framing helpers ─────────────────────────────────────────────
std::vector<uint8_t> BuildFrame(uint32_t msgType, const void* payload, uint32_t size) {
    std::vector<uint8_t> buf(sizeof(VbMsgHeader) + size);
    VbMsgHeader hdr = { msgType, size };
    memcpy(buf.data(), &hdr, sizeof(hdr));
    if (payload && size) memcpy(buf.data() + sizeof(hdr), payload, size);
    return buf;
}

void EnqueueToClient(ClientCtx* c, const std::vector<uint8_t>& frame) {
    EnterCriticalSection(&c->outCs);
    if (c->outQueue.size() > 512) c->outQueue.pop_front();  // slow-client cap
    c->outQueue.push_back(frame);
    LeaveCriticalSection(&c->outCs);
}

// Broadcast to every connected client (and drop on each client's queue).
void BroadcastMessage(uint32_t msgType, const void* payload, uint32_t size) {
    if (!g_clientsCsInited) return;
    std::vector<uint8_t> frame = BuildFrame(msgType, payload, size);

    EnterCriticalSection(&g_clientsCs);
    for (ClientCtx* c : g_clients) {
        if (InterlockedCompareExchange(&c->alive, 0, 0))
            EnqueueToClient(c, frame);
    }
    LeaveCriticalSection(&g_clientsCs);
}

// ── Reads ────────────────────────────────────────────────────────────────
bool ReadExact(HANDLE h, void* buf, DWORD n) {
    DWORD total = 0, got = 0;
    while (total < n) {
        if (!ReadFile(h, (char*)buf + total, n - total, &got, nullptr)) return false;
        if (got == 0) return false;
        total += got;
    }
    return true;
}

bool ReadFrame(HANDLE h, std::vector<uint8_t>& payload, uint32_t& msgType) {
    VbMsgHeader hdr = {};
    if (!ReadExact(h, &hdr, sizeof(hdr))) return false;
    msgType = hdr.type;
    payload.clear();
    if (hdr.size == 0) return true;
    if (hdr.size > 16u * 1024u * 1024u) return false;
    payload.resize(hdr.size);
    return ReadExact(h, payload.data(), hdr.size);
}

bool WriteFrame(HANDLE h, const uint8_t* data, DWORD size) {
    DWORD w = 0;
    if (!WriteFile(h, data, size, &w, nullptr)) return false;
    return w == size;
}

// ── HELLO ────────────────────────────────────────────────────────────────
void SendHelloTo(ClientCtx* c) {
    VbHello hello = {};
    hello.protocolVersion = 1;
    hello.gamePid         = GetCurrentProcessId();

    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* base = strrchr(exe, '\\');
    base = base ? base + 1 : exe;
    strncpy_s(hello.exeName, sizeof(hello.exeName), base, _TRUNCATE);
    strncpy_s(hello.gameVersion, sizeof(hello.gameVersion), "LOTR:Conquest", _TRUNCATE);

    EnqueueToClient(c, BuildFrame(VB_MSG_HELLO, &hello, sizeof(hello)));
}

// ── Per-client I/O thread ───────────────────────────────────────────────
DWORD WINAPI ClientThread(LPVOID arg) {
    ClientCtx* c = static_cast<ClientCtx*>(arg);
    InterlockedExchange(&c->alive, 1);
    InterlockedIncrement(&g_clientCount);
    BridgeLog("[bridge] client thread start (count=%ld)",
              InterlockedCompareExchange(&g_clientCount, 0, 0));

    SendHelloTo(c);

    while (!InterlockedCompareExchange(&g_stopFlag, 0, 0)) {
        // Drain outbound.
        for (;;) {
            std::vector<uint8_t> msg;
            EnterCriticalSection(&c->outCs);
            if (!c->outQueue.empty()) {
                msg = std::move(c->outQueue.front());
                c->outQueue.pop_front();
            }
            LeaveCriticalSection(&c->outCs);
            if (msg.empty()) break;
            if (!WriteFrame(c->pipe, msg.data(), (DWORD)msg.size())) {
                BridgeLog("[bridge] client write failed err=%lu", GetLastError());
                goto disconnect;
            }
        }

        // Non-blocking peek for inbound.
        DWORD avail = 0;
        if (!PeekNamedPipe(c->pipe, nullptr, 0, nullptr, &avail, nullptr)) {
            BridgeLog("[bridge] client peek failed err=%lu", GetLastError());
            break;
        }
        if (avail > 0) {
            std::vector<uint8_t> payload;
            uint32_t mt = 0;
            if (!ReadFrame(c->pipe, payload, mt)) {
                BridgeLog("[bridge] client read failed");
                break;
            }
            if (mt == VB_CMD_PING) {
                EnqueueToClient(c, BuildFrame(VB_CMD_PING,
                                              payload.empty() ? nullptr : payload.data(),
                                              (uint32_t)payload.size()));
            } else if (mt == VB_CMD_DUMP_VS_REGS) {
                EnqueueToClient(c, BuildFrame(VB_MSG_VS_REGS, g_VbAllRegs, sizeof(g_VbAllRegs)));
            } else if (mt == VB_CMD_READ_MEMORY && payload.size() >= sizeof(VbReadMemReq)) {
                VbReadMemReq req;
                memcpy(&req, payload.data(), sizeof(req));
                uint32_t capped = req.size > 65536u ? 65536u : req.size;

                std::vector<uint8_t> out(sizeof(VbMemDump) + capped);
                VbMemDump hdr = { req.addr, capped, 0 };
                uint32_t got = SafeMemcpyFromGame(out.data() + sizeof(hdr),
                                                   reinterpret_cast<const uint8_t*>(req.addr),
                                                   capped);
                hdr.size = got;
                hdr.ok   = (got == capped) ? 1u : 0u;
                memcpy(out.data(), &hdr, sizeof(hdr));
                // Only send the header + actually-read bytes (trim tail).
                out.resize(sizeof(VbMemDump) + got);
                EnqueueToClient(c, BuildFrame(VB_MSG_MEMORY_DUMP, out.data(), (uint32_t)out.size()));
                BridgeLog("[bridge] READ_MEMORY addr=0x%08X req=%u got=%u",
                          req.addr, capped, got);
            } else {
                VespucciCmdHandler_t h = g_cmdHandler;
                int handled = 0;
                if (h) handled = h(mt, payload.empty() ? nullptr : payload.data(),
                                   (uint32_t)payload.size());
                if (!handled)
                    BridgeLog("[bridge] unhandled cmd type=0x%04X size=%u",
                              mt, (unsigned)payload.size());
            }
        } else {
            Sleep(1);
        }
    }

disconnect:
    InterlockedExchange(&c->alive, 0);
    FlushFileBuffers(c->pipe);
    DisconnectNamedPipe(c->pipe);
    CloseHandle(c->pipe);
    c->pipe = INVALID_HANDLE_VALUE;

    // Unregister from the broadcast list.
    EnterCriticalSection(&g_clientsCs);
    for (auto it = g_clients.begin(); it != g_clients.end(); ++it) {
        if (*it == c) { g_clients.erase(it); break; }
    }
    LeaveCriticalSection(&g_clientsCs);

    InterlockedDecrement(&g_clientCount);
    BridgeLog("[bridge] client thread exit (remaining=%ld)",
              InterlockedCompareExchange(&g_clientCount, 0, 0));

    DeleteCriticalSection(&c->outCs);
    // Self-owned: close our thread handle when we exit.
    HANDLE selfThread = c->thread;
    delete c;
    if (selfThread) CloseHandle(selfThread);
    return 0;
}

// ── Accept thread (listens for new clients) ─────────────────────────────
DWORD WINAPI AcceptThread(LPVOID) {
    OpenLog();
    BridgeLog("[bridge] accept thread started");

    while (!InterlockedCompareExchange(&g_stopFlag, 0, 0)) {
        HANDLE p = CreateNamedPipeA(
            VESPUCCI_PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            kMaxClients,
            64 * 1024,
            64 * 1024,
            0,
            nullptr);
        if (p == INVALID_HANDLE_VALUE) {
            BridgeLog("[bridge] CreateNamedPipe failed err=%lu", GetLastError());
            Sleep(2000);
            continue;
        }

        BridgeLog("[bridge] pipe instance ready, waiting for client");
        BOOL ok = ConnectNamedPipe(p, nullptr);
        DWORD err = GetLastError();
        if (!ok && err != ERROR_PIPE_CONNECTED) {
            BridgeLog("[bridge] ConnectNamedPipe failed err=%lu", err);
            CloseHandle(p);
            Sleep(500);
            continue;
        }

        // Spawn a per-client thread that takes ownership of the handle.
        ClientCtx* c = new ClientCtx();
        c->pipe  = p;
        c->alive = 0;
        InitializeCriticalSection(&c->outCs);

        EnterCriticalSection(&g_clientsCs);
        g_clients.push_back(c);
        LeaveCriticalSection(&g_clientsCs);

        c->thread = CreateThread(nullptr, 0, ClientThread, c, 0, nullptr);
        if (!c->thread) {
            BridgeLog("[bridge] CreateThread(client) failed");
            EnterCriticalSection(&g_clientsCs);
            for (auto it = g_clients.begin(); it != g_clients.end(); ++it) {
                if (*it == c) { g_clients.erase(it); break; }
            }
            LeaveCriticalSection(&g_clientsCs);
            DeleteCriticalSection(&c->outCs);
            CloseHandle(p);
            delete c;
            continue;
        }
    }

    BridgeLog("[bridge] accept thread exiting");
    if (g_log) { fclose(g_log); g_log = nullptr; }
    return 0;
}

}  // anonymous namespace

// ── Public API ──────────────────────────────────────────────────────────
extern "C" void VespucciBridge_Init() {
    if (g_acceptThread) return;

    InitializeCriticalSection(&g_clientsCs);
    g_clientsCsInited = true;
    InitializeCriticalSection(&g_logCs);
    g_logCsInited = true;

    g_lastTick = GetTickCount();
    g_acceptThread = CreateThread(nullptr, 0, AcceptThread, nullptr, 0, nullptr);
}

extern "C" void VespucciBridge_Shutdown() {
    InterlockedExchange(&g_stopFlag, 1);

    // Force-close all client pipes to unblock their threads.
    EnterCriticalSection(&g_clientsCs);
    for (ClientCtx* c : g_clients) {
        HANDLE h = c->pipe;
        if (h != INVALID_HANDLE_VALUE) {
            CancelIoEx(h, nullptr);
            DisconnectNamedPipe(h);
        }
    }
    LeaveCriticalSection(&g_clientsCs);

    if (g_acceptThread) {
        WaitForSingleObject(g_acceptThread, 3000);
        CloseHandle(g_acceptThread);
        g_acceptThread = nullptr;
    }

    if (g_clientsCsInited) {
        DeleteCriticalSection(&g_clientsCs);
        g_clientsCsInited = false;
    }
    if (g_logCsInited) {
        DeleteCriticalSection(&g_logCs);
        g_logCsInited = false;
    }
}

extern "C" int VespucciBridge_IsConnected() {
    return InterlockedCompareExchange(&g_clientCount, 0, 0) > 0 ? 1 : 0;
}

extern "C" void VespucciBridge_Send(uint32_t msgType,
                                     const void* payload,
                                     uint32_t payloadSize) {
    BroadcastMessage(msgType, payload, payloadSize);
}

extern "C" void VespucciBridge_SetCmdHandler(VespucciCmdHandler_t handler) {
    g_cmdHandler = handler;
}

extern "C" void VespucciBridge_OnLevelLoaded(const char* levelName) {
    if (!VespucciBridge_IsConnected()) return;
    VbLevelLoaded m = {};
    if (levelName)
        strncpy_s(m.levelName, sizeof(m.levelName), levelName, _TRUNCATE);
    BroadcastMessage(VB_MSG_LEVEL_LOADED, &m, sizeof(m));
}

extern "C" void VespucciBridge_OnLevelUnloaded() {
    if (!VespucciBridge_IsConnected()) return;
    BroadcastMessage(VB_MSG_LEVEL_UNLOADED, nullptr, 0);
}

// Frame tick — called from EndScene callback.
extern "C" void VespucciBridge_OnFrameDev(IDirect3DDevice9* device) {
    if (!VespucciBridge_IsConnected()) return;

    DWORD now = GetTickCount();
    float dt = (now - g_lastTick) * 0.001f;
    if (dt <= 0.0f || dt > 1.0f) dt = 1.0f / 60.0f;
    g_lastTick = now;

    // FRAME_BEGIN
    VbFrameBegin fb = {};
    fb.frameIdx = ++g_frameIdx;
    fb.dt       = dt;
    fb.tick     = now;
    BroadcastMessage(VB_MSG_FRAME_BEGIN, &fb, sizeof(fb));

    // CAMERA (world-path cache, never clobbered by HUD).
    {
        VbCamera cam = {};
        const float* vp = g_VbWorldCam;
        memcpy(cam.view, vp, sizeof(cam.view));
        memcpy(cam.proj, vp, sizeof(cam.proj));

        const float* camRight = g_VbWorldCam + 16;
        const float* camUp    = g_VbWorldCam + 20;
        const float* camPos   = g_VbWorldCam + 24;

        cam.pos[0] = camPos[0]; cam.pos[1] = camPos[1]; cam.pos[2] = camPos[2];

        float rx = camRight[0], ry = camRight[1], rz = camRight[2];
        float ux = camUp[0],    uy = camUp[1],    uz = camUp[2];
        cam.fwd[0] = ry * uz - rz * uy;
        cam.fwd[1] = rz * ux - rx * uz;
        cam.fwd[2] = rx * uy - ry * ux;

        float p11 = vp[5];
        cam.fovY = (p11 != 0.0f) ? (2.0f * atanf(1.0f / p11)) : 1.0f;

        if (device) {
            D3DVIEWPORT9 dvp = {};
            if (SUCCEEDED(device->GetViewport(&dvp))) {
                cam.viewportW = dvp.Width;
                cam.viewportH = dvp.Height;
            }
        }
        BroadcastMessage(VB_MSG_CAMERA, &cam, sizeof(cam));
    }

    // FRAME_END
    BroadcastMessage(VB_MSG_FRAME_END, nullptr, 0);
}

// Convenience: no-device variant.
extern "C" void VespucciBridge_OnFrame() {
    VespucciBridge_OnFrameDev(nullptr);
}
