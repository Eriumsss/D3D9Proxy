#pragma once
// Opt-in x86 native animation EVENT pool extension (NOT the BLEND pool).
// One-off pre-level pool swap using the original game's PoolInit allocator
// and its registered atexit delete[] owner; no extra EXE hooks.
void OC_AnimationEventPool4096_Init();
bool OC_AnimationEventPool4096_IsActive();
