#pragma once
// Original retail clip -> skeleton binding SmallVec8 caches: 64 -> 256/512.
// Exact x86 pre-level, native allocator + flags-gate preservation. OFF by default.
void OC_AnimationBindingCache512_Init();
