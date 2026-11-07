# Two-world memory residency: normal D3D9 texture VA vault

**Goal:** let a secondary world be kept alongside the main world without
needlessly retaining duplicate CPU texture copies in the retail 32-bit process.

## Source and scope

This is a narrowly scoped port of the proven *offline D3D9 device test*
implementation in DLL/NpcProxy/src/gfx.cpp, documented in
DLL/NpcProxy/PLAN.md §§9.2–9.5. It retains the VaultTexture COM object and
hot/cold/pagefile-backed storage path, but does not introduce any NPC logic,
filtering, LOD override, anti-aliasing, RTX Remix integration, or x64 broker.

The primary GPU copy is D3DPOOL_DEFAULT. While the game edits a texture,
a temporary D3DPOOL_SYSTEMMEM copy holds its pixels. After three seconds,
up to 8 MiB per frame can be cooled to a *pagefile-backed file mapping*,
then its mapped view and system-memory texture are released. A reset or later
LockRect can reconstruct those pixels from the same mapping. Frequently
reopened textures remain hot. When the process has less than 768 MiB of
available virtual address space, eligible texture creation also attempts a
bounded 16 MiB sweep of older idle staging copies, because a second world may
be loading while Present() is paused. This may make loading slower under
pressure; it is deliberately restricted to the calling D3D9 thread.
The original NpcProxy copied texture COM implementation remains the oracle.

This does NOT increase graphics-card VRAM, the EXE's 4 GiB VA ceiling, the
engine's fixed pools, or automatically load a second level/world.
It merely avoids keeping *certain* otherwise duplicated CPU texture data
mapped in the process. World coexistence still needs an owner-aware secondary
world loader and lifetime tests.

## Opt-in config for a disposable game install

Create or edit proxy.ini next to the game executable:

    [WorldResourceMemory]
    TextureVault=0
    Monitor=0

Defaults are both 0. For testing set Monitor=1 first. This writes
world_resource_memory.csv containing used/peak process VA, hot and cold
texture-copy MiB, vault texture count, failures and a GPU memory *estimate*
(the D3D9 driver hint, not actual VRAM residency).

Only after a Windows x86 build, an offline D3D test, and a real-game
graphics/reset test should TextureVault=1 be considered. The original
retail texture-class instruction signature (0x0068D690) and call return
address (0x0068D69A) must match. Unrelated/unsupported textures are
left to Direct3D unchanged. A 256 MiB per-texture cap avoids 32-bit byte
offset overflow. Failures return to the unchanged native creation path.

Do not install DLL/NpcProxy's d3d9.dll next to the OpenConquest D3D9Proxy
d3d9.dll: they are independent implementations with overlapping engine
globals. This feature is inside the main OpenConquest D3D9Proxy only.

## Validation status

Source-level integration and oracle cross-check:
python D3D9Proxy/tests/test_retail_texture_vault.py

The source branch is NOT a claim of a verified executable: no Windows
MSVC x86 compilation, actual D3D9 GPU test, or Conquest two-world runtime
was performed here. Initial configuration must remain disabled until tested.

Note: Direct3D ordinal export mismatch in the older D3D9Proxy .def is a
separate compatibility issue. This patch intentionally does not renumber
the existing Vespucci bridge ordinals or change the DLL's ABI.
