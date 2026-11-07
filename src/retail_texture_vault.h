#pragma once
// OpenConquest / D3D9Proxy: retail texture address-space vault.
// Source contract: DLL/NpcProxy/src/gfx.cpp (VaultTexture), PLAN.md section 9.
// Holds the GPU copy in D3DPOOL_DEFAULT and cools the CPU copy into a backed,
// unmapped section. This does NOT increase VRAM or create a second game world.
// Opt-in via [WorldResourceMemory] TextureVault=1 in proxy.ini; default OFF.
#include <d3d9.h>

class ProxyIDirect3DDevice9;

struct OCVStats {
    unsigned vaultCount;
    unsigned heldMb;       // cool mapped-file sections (unmapped VA)
    unsigned hotMb;        // active system-memory staging textures
    unsigned relocks;
    unsigned natives;      // fell back to D3DPOOL_MANAGED
    unsigned failures;
    unsigned peakVaMb;
    unsigned usedVaMb;
};

void OCV_OnDeviceCreated(ProxyIDirect3DDevice9* owner, IDirect3DDevice9* real);
void OCV_OnDeviceReleased(IDirect3DDevice9* real);
void OCV_BeforeReset(IDirect3DDevice9* real);
void OCV_AfterReset(IDirect3DDevice9* real, HRESULT resetResult);
void OCV_OnPresent(IDirect3DDevice9* real);

// S_FALSE means not intercepted; the device must forward the call unchanged.
HRESULT OCV_CreateTexture(IDirect3DDevice9* real, UINT w, UINT h, UINT levels,
                          DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                          IDirect3DTexture9** out, HANDLE* shared, void* returnAddress);
HRESULT OCV_SetTexture(IDirect3DDevice9* real, DWORD stage, IDirect3DBaseTexture9* tex);
HRESULT OCV_GetTexture(IDirect3DDevice9* real, DWORD stage, IDirect3DBaseTexture9** out);
HRESULT OCV_UpdateTexture(IDirect3DDevice9* real, IDirect3DBaseTexture9* src,
                          IDirect3DBaseTexture9* dst);
OCVStats OCV_ReadStats();
