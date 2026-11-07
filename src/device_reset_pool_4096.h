#pragma once
// Opt-in, exact-x86 D3D device-reset registration pool: 64 -> 1024/4096.
// Native realloc + bounded append trampoline; does NOT change Havok.
void OC_DeviceResetPool4096_Init();
