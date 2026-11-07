#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// RETAIL FUNCTION TRACER — trap every retail function we have a name for and
// log it as it runs.
//
// The frame tracer (d3d9device.cpp, F11) sees the game from the D3D side: it
// knows every draw but nothing about the code that produced it. This is the
// other half — it sees the CODE. 32,456 catalogued entry points, 1,680 of them
// carrying the semantic name a 1:1 port earned, all trapped with a one-byte
// INT3 and resolved through a vectored handler.
//
// Init is safe to call from DllMain: everything heavy (loading the symbol
// table, patching .text) happens on the worker thread once the loader lock is
// gone. Nothing is armed unless retail_trace.ini says Enabled=1 — a missing
// ini writes itself with the defaults and leaves the game untouched.
// ═══════════════════════════════════════════════════════════════════════════

extern "C" {

// Start the tracer worker. Idempotent. DllMain-safe.
void RTF_Init(void);

// Stop tracing, restore every patched byte, flush the logs.
void RTF_Shutdown(void);

// Optional: point the tracer at the host's frame counter so every logged call
// carries the frame it happened in. The proxy passes &g_FrameCounter; a
// standalone injection just leaves it null and gets timestamps only.
void RTF_SetFrameCounter(volatile unsigned long* counter);

// Arm / disarm from code (the hotkeys call these).
void RTF_Arm(void);
void RTF_Disarm(void);

// Write the coverage table (what ran, how often, when first seen) right now.
void RTF_DumpCoverage(void);

// OFFLINE FALSIFIER. Runs the real arm / trap / restore / re-arm / drain path
// against three functions this call generates itself, so the mechanism can be
// proven on real hardware without launching the game. Returns the number of
// FAILED checks (0 = everything passed). Only trace_selftest.exe calls this —
// it repoints the tracer's globals at a synthetic table and must never run
// inside the game.
int RTF_SelfTest(void);

}  // extern "C"
