// Vespucci <-> Game bridge over named pipe \\.\pipe\conquest_bridge
//
// Binary message protocol. Every message is:
//   [u32 msgType][u32 payloadSize][...payload bytes...]
//
// Game → Vespucci message types (streamed every frame or on event):
#pragma once
#include <stdint.h>

#define VESPUCCI_PIPE_NAME "\\\\.\\pipe\\conquest_bridge"

enum VespucciMsgType {
    // ── Game → Vespucci (outgoing) ──
    VB_MSG_HELLO            = 0x0001,  // first msg after connect: game info
    VB_MSG_FRAME_BEGIN      = 0x0100,  // frame header: frameIdx, dt, tick
    VB_MSG_CAMERA           = 0x0101,  // camera view+proj matrices, pos, fov
    VB_MSG_PLAYER           = 0x0102,  // player pos, team, class, health
    VB_MSG_ENTITY_SNAPSHOT  = 0x0103,  // array of entities (pos, team, health)
    VB_MSG_LEVEL_LOADED     = 0x0104,  // level name changed
    VB_MSG_LEVEL_UNLOADED   = 0x0105,
    VB_MSG_FRAME_END        = 0x01FF,  // frame footer

    // ── Vespucci → Game (incoming) ──
    VB_CMD_PING             = 0x1000,  // keepalive
    VB_CMD_SPAWN_ENTITY     = 0x1100,  // spawn unit at position
    VB_CMD_DELETE_ENTITY    = 0x1101,  // delete by pointer/id
    VB_CMD_TELEPORT_PLAYER  = 0x1102,  // move player to position
    VB_CMD_SET_TIMESCALE    = 0x1103,  // slow-mo / pause (float)
    VB_CMD_RELOAD_LEVEL     = 0x1104,  // force reload current level
    VB_CMD_SET_CAMERA       = 0x1105,  // free camera override
    VB_CMD_HIGHLIGHT_ENTITY = 0x1106,  // draw outline around entity ptr
    VB_CMD_DUMP_VS_REGS     = 0x1F00,  // request full c0..c255 snapshot (debug)
    VB_MSG_VS_REGS          = 0x01F0,  // response: 256*16 bytes of float constants
    VB_CMD_READ_MEMORY      = 0x1F01,  // VbReadMemReq payload — SEH-safe read
    VB_MSG_MEMORY_DUMP      = 0x01F1,  // VbMemDump header + raw bytes
};

struct VbReadMemReq {
    uint32_t addr;
    uint32_t size;   // capped at 64 KiB server-side
};

struct VbMemDump {
    uint32_t addr;
    uint32_t size;
    uint32_t ok;      // 1 = full read, 0 = partial/failed (bytes may be 0 here)
    // followed by `size` bytes of payload
};

// Payload structs (all little-endian, packed — explicit layout)
#pragma pack(push, 1)

struct VbMsgHeader {
    uint32_t type;
    uint32_t size;    // size of payload following this header
};

struct VbHello {
    uint32_t protocolVersion;  // = 1
    uint32_t gamePid;
    char     exeName[64];
    char     gameVersion[32];
};

struct VbFrameBegin {
    uint64_t frameIdx;
    float    dt;
    uint32_t tick;  // GetTickCount()
};

struct VbCamera {
    float view[16];        // row-major D3D matrix
    float proj[16];
    float pos[3];          // derived from view inverse
    float fwd[3];          // camera forward vector
    float fovY;            // radians
    uint32_t viewportW;
    uint32_t viewportH;
};

struct VbPlayer {
    float    pos[3];
    float    velocity[3];
    float    health;
    uint32_t team;
    uint32_t creaturePtr;   // address in game memory
};

struct VbEntityEntry {
    uint32_t addr;       // game memory pointer
    float    pos[3];
    float    velocity[3];
    float    health;
    uint32_t team;
    uint32_t flags;      // bit0=isPlayer, bit1=isHero
};

struct VbEntitySnapshot {
    uint32_t count;
    // followed by: count * VbEntityEntry
};

struct VbLevelLoaded {
    char levelName[64];
};

// ── Command payloads (Vespucci → Game) ──
struct VbSpawnEntity {
    char     typeName[32];  // e.g. "CH_orc_swd_all_01"
    float    pos[3];
    float    yaw;
    uint32_t team;
};

struct VbDeleteEntity {
    uint32_t addr;
};

struct VbTeleportPlayer {
    float pos[3];
};

struct VbSetTimescale {
    float scale;  // 1.0=normal, 0.0=paused, 2.0=double
};

struct VbSetCamera {
    float pos[3];
    float fwd[3];
    float fovY;
    uint32_t freeCamActive;  // 1=override game camera
};

#pragma pack(pop)

// ── API ──
#ifdef __cplusplus
extern "C" {
#endif

// Call once on DLL init (non-blocking — spawns I/O thread).
void VespucciBridge_Init();

// Call every frame (cheap — just queues outgoing messages).
void VespucciBridge_OnFrame();

// Hook points from other modules:
void VespucciBridge_OnLevelLoaded(const char* levelName);
void VespucciBridge_OnLevelUnloaded();

// Test if Vespucci is currently connected.
int  VespucciBridge_IsConnected();

// Send a raw message (thread-safe).
void VespucciBridge_Send(uint32_t msgType, const void* payload, uint32_t payloadSize);

// Called from bridge thread when a command message arrives.
// Returns 1 if handled, 0 to let it be logged/dropped.
typedef int (*VespucciCmdHandler_t)(uint32_t msgType, const void* payload, uint32_t payloadSize);
void VespucciBridge_SetCmdHandler(VespucciCmdHandler_t handler);

void VespucciBridge_Shutdown();

#ifdef __cplusplus
}
#endif
