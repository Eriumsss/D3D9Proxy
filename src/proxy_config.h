#pragma once
// Called at first Direct3DCreate9 / Direct3DCreate9Ex, before registry patches.
// Creates <game EXE folder>\proxy.ini if missing, preserving existing bytes.
// Returns false on write/path error; registry patches retain their safe defaults.
bool OC_ProxyConfig_Ensure();
