#pragma once
// Opt-in, exact x86 native dynamic VB/IB reset-list capacity (2 x 64 -> 2 x 1024/4096).
// These are distinct from the generic device-reset resource list and Havok.
void OC_DynamicBufferResetPools4096_Init();
