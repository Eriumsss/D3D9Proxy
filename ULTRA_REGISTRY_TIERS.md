# Ultra-tier native x86 registry capacities (experimental)

D3D9Proxy only. **No map importer, second game world, NPC changes, RTX Remix, or Havok cap claims.**

Existing exact-byte-gated x86 patches already change *both* the native constructor's registry reserve and the corresponding early factory guard. These additional options use the **same verified code sites and original game allocators**. They extend the whitelisted immediate choices, not arbitrary executable-address writes.

| Native registry | Retail capacity | Previous highest | New optional capacity |
| --- | ---: | ---: | ---: |
| ModelInfo (3 × 8-byte pair vectors) | 1024 | 4096 | **8192** |
| TextureInfo (2 × 8-byte pair vectors) | 2048 | 8192 | **16384** |
| EffectInfo (3 × 8-byte pair vectors) | 512 | 2048 | **4096** |
| GFX block (isolated level registry, 8-byte entries) | 64 | 512 | **1024** |
| Resource Other (`level+0xF4`, 8-byte entries) | 2048 | 8192 | **16384** |
| PField (`level+0x114`, 12-byte entries) | 4096 | 8192 | **16384** |

The native `SmallVec` capacity and count fields are signed 16-bit. **16384 fits; 32768 does not.** Real native array allocations for the largest tier: models 3×8192×8 = 192 KiB, textures 2×16384×8 = 256 KiB, effects 3×4096×8 = 96 KiB, GFX 1024×8 = 8 KiB, Resource Other 16384×8 = 128 KiB, PField 16384×12 = 192 KiB. These are **pair/data-array reservations only**, not payloads, geometry, Havok worlds or 32-bit address-space availability.

Use the following in `proxy.ini` next to the game's EXE, after merging the PR and installing the new MSVC x86 DLL:

```ini
[StaticMapPools]
ExpandModels8192=0
ExpandTextures16384=0
ExpandEffects4096=0
ExpandGfx1024=0
ExpandResourceOther16384=0
ExpandPFields16384=0
```

All options default OFF. Set a single option to 1 to attempt the patch on the **one supported fingerprinted Ghidra/Conquest EXE**, before level construction. If multiple tiers for a registry are enabled, the **largest wins**. Existing low/medium/high settings remain supported and are not reinterpreted.

Factory guards are raised in lockstep with allocation reserves. GFX remains callsite-isolated from the unrelated animation registry's 64-slot shared reserve helper. PField and Resource Other still invoke the original game's unique native reserve routines and matching factory checks. No new heap ownership model or pointer migration is introduced.

The PAK preflight supports the corresponding `--assume-model8192 --assume-texture16384 --assume-effects4096 --assume-gfx1024 --assume-pfields16384` scenario. It is an **offline what-if** and cannot establish that an EXE patch activated. Resource Other is deliberately excluded from PAK assumptions because no one-to-one PAK count mapping is established. Use `Monitor=1` and the original registry patch logs for actual process validation.

**Limits:** unverified real-game runtime, collision/Havok allocation, device-reset stability, scene unload, actual texture GPU usage and 32-bit process VA. This PR does not “solve” the embedded 1024-slot animation blend pool: that pool's counter and adjacent memory are laid out at fixed relative offsets, so merely raising its comparison risks memory corruption.
