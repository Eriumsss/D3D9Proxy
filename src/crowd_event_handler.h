// ============================================================================
//  crowd_event_handler.h — THE FUCKING HANDLE THAT MAKES CROWDS LISTEN
//
//  Half 2 of the CrowdProxy architecture. The level.json side defines an
//  entity type "CrowdProxy" with Factory="logic_relay" + Vesp* crowd-target
//  metadata fields. The engine treats those entities as stock logic_relays
//  at runtime and ignores the metadata. THIS module is what gives the
//  metadata teeth. Reads the Vesp* fields, mutates crowd state, makes
//  spearmen vanish on cue.
//
//  Loaded from inside d3d9.dll, ticks once per Present(). The retail game
//  thread never crosses paths with us beyond reading values our hooks
//  parked in its memory. No threads spawned. Game-side allocations
//  untouched. We mutate one u32 (CrowdItem.instNum) and that's enough.
// ============================================================================

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Call once during DLL init. Lightweight — only resolves a couple of static
// addresses and registers the hotkey state. Safe to call before the game
// has actually loaded a level (the per-frame tick handles that gracefully).
void CrowdHandler_Init();

// Call once per D3D9 Present(). Reads input + global game state, mutates
// crowd CrowdItem.instNum when the trigger condition fires. Cheap — every
// call returns within a couple of memory reads if there's nothing to do.
void CrowdHandler_OnFrame();

// Call on DLL unload. Restores any crowd state we mutated so the game
// doesn't leave the level in a half-hidden state on shutdown.
void CrowdHandler_Shutdown();

#ifdef __cplusplus
}
#endif
