# Static map import: retail pool census and verified capacity ceilings

**Target:** import only Isengard's static map contents into Helm's Deep, in
Helm's Deep's existing level. **No second game world, independent level
loader, NPC extension or mission scripts.** The imported data must be native
model/texture/material/geometry/collision/occlusion resources with correct
block offsets, references and lifetime.

## What source actually establishes

Source oracle: `Vespucci/Engine/Magellan/`, in particular:

* `MgLevelObject.cpp`: game level-object constructor, SmallVec8
  `{i16 count, i16 capacity, pointer base}`, registry offsets and reserve calls.
* `MgModelRegistry.cpp`: `FUN_00875d1e` hard guard
  `sorted.count + model_info_num > 0x400` returns without registering;
  pending/committed model entries are 8-byte `{key, pointer}`.
* `MgTextureRegistry.cpp`: `FUN_00876b70` hard guard `0x800`,
  not the same as D3D9 texture memory.
* `MgEffectRegistry.cpp`: `FUN_00877396` hard guard `0x200`.
* `MgLevelConstruct.cpp`: GFX-block registry guard `0x40`.
* `MgGameBuffers.h`: **static** models create VBuffInfo and IBuffInfo
  D3DPOOL_MANAGED buffers through `FUN_0068e091` and `FUN_0068dd41`,
  after model registry, during pending-to-committed `FUN_00876adc`.
  The dynamic geometry pool at `[0x00CD80D4]`, `0x17FD10` bytes and
  `0x164`-byte temporary records, is NOT the static map-model registry.
  Raising its phase counters alone does not permit an extra map.

### Verified initial capacities

| Registry | Level-object offset(s) | Initial capacity | Other guard |
|---|---|---:|---|
| Textures | +0x24,+0x2C | 2048 each | `FUN_00876b70` 2048 |
| Models | +0x8C,+0x94,+0x9C | 1024 each | `FUN_00875d1e` 1024 |
| Effects | +0xFC,+0x104,+0x10C | 512 each | `FUN_00877396` 512 |
| GFX blocks | +0x11C | 64 | `FUN_00875ab8` 64 |
| PFields | +0x114 | **unknown in retail** | `FUN_00875bf7` 4096, NOT its allocated capacity |

These are initial capacities, not proof that importing Isengard exceeds
them. Distinct record keys may deduplicate/rebind. The precise number of
merged ModelInfo records differs from the number of level scene instances.

## New opt-in read-only runtime monitor

In the game's existing `proxy.ini` (beside the EXE):

    [StaticMapPools]
    Monitor=0

Setting 1 enables `static_map_pools.csv`. All output is read-only and
thread independent from rendering: a pinned worker samples 4 times/second
and logs each registry every 2 seconds, including while the renderer is
paused for level loading. Each row reports the current level pointer,
entry count, capacity, per-level observed peak, expected stock capacity,
array pointer and validity flag. It also samples the **separate** dynamic
geometry pool A/C counters as diagnostic only.

Capture three separate runs: untouched Helm's Deep, untouched Isengard,
and a copy of Helm's Deep with the Isengard static assets merged. Compare
the actual counts/peaks. Keep the source files untouched and do the
comparison only on disposable test copies.

## Before any actual retail patch

1. For each over-cap registry, confirm the **retail executable** constructor's
   allocation and all guard sites using the supplied Ghidra GZF, and compare
   with Magellan's address-level notes. Merely increasing `CMP 0x400`
   leaves a 1024-entry array and causes an overwrite.
2. Determine whether the array can be safely reallocated **before registration**.
   A copied `{key,pointer}` record must preserve pointer identity and
   references. The 16-bit signed count/capacity headers impose a separate
   32767-element structural limit.
3. Validate the full native PAK model import transaction: ModelInfo,
   ModelSubData, VBuffInfo, IBuffInfo, BufferInfo, materials, texture keys,
   collision/Havok shapes, occluders, radiosity, BIN slices, and game-mode
   masks. Reject any zeroed/invalid offsets before commit.
4. Assess the overall 32-bit VA budget and GPU buffer residency; the
   experimental texture vault in PR #3 can reduce CPU duplicate texture
   copies but does not expand these registries.
5. Test loading/teardown and a level reload; no leaked/deleted static-model
   resources or ghost depth/occlusion geometry.

**Implementation status:** The opt-in diagnostic monitor and read-only offline
PAK preflight are implemented. No runtime capacity increase, allocator hook,
native import, executable instruction patch or Windows game validation is
claimed. Default OFF.

## Offline PAK capacity preflight (covers early factory aborts)

The live level-object census cannot diagnose a factory which returns BEFORE
registering any entries. This matters for `FUN_00875d1e`: it compares the
incoming `model_info_num` against the 1024-entry guard **before** iterating.
The merged PAK's own table count is therefore a required independent signal.

```powershell
python D3D9Proxy/tools/pak_pool_preflight.py --base "helmsdeep.PAK" --donor "isengard.PAK" --deep
python D3D9Proxy/tools/pak_pool_preflight.py --base "helmsdeep.PAK" --donor "isengard.PAK" --deep --mask 0x40
python D3D9Proxy/tools/pak_pool_preflight.py --base "helmsdeep.PAK" --donor "isengard.PAK" --merged "helmsdeep_merged.PAK" --deep --json
```

This standalone Python script reads only PC little-endian PAKs. It checks the
472-byte PakHeader, `constx13=0x13`, the Block1 byte span, count+stride
ranges for model/texture/effect/pfield/GFX/VBuffInfo/IBuffInfo records, and
optionally the zlib-decompressed Block1 CRC keys and game-mode masks.\nWith `--deep --mask 0x...` it also reports how many ModelInfo rows\nmatch the selected gamemode. This does NOT relax the factory guard: that\nchecks RAW `model_info_num` before filtering, not the eligible count.
It uses **on-disk** strides; `VBuffInfo` and `IBuffInfo` disk strides
`0x20` and `0x18` differ from some runtime/header structure-size fields.

For two inputs, `naive_append_upper_bound` is a conservative what-if:
a native merge may deduplicate, omit or remap donor records. The **actual
merged PAK** is authoritative. A count beyond a known factory guard is a
definite danger, but a lower count is NOT proof of runtime success: the
level object's sorted registry can already hold previous registrations.
PField retail allocation capacity is currently unknown; the 4096 factory
guard is not a substitute for an allocation bound.

Exit codes: `0` no definitively exceeded guard, `1` at least one known
hard limit exceeded, `2` malformed/inaccessible PAK. The tool never
modifies PAK/BIN or attempts to parse/execute mission scripts.

Validation: GitHub Actions `Static Map PAK Preflight` runs the parser's
Python syntax check and thirteen synthetic PC-PAK tests (valid/uncompressed,
valid/zlib, over-cap, duplicate keys, bad bounds, malformed zlib, combined
capacity estimate, etc.). These tests do **not** verify engine runtime.

## Retail opcode evidence collection (read-only)

When `[StaticMapPools] Monitor=1` is enabled, the D3D9Proxy monitor also
creates `static_map_opcode_windows.txt` next to the game EXE. It records
24 instruction bytes at each model/texture/effect/GFX constructor and
factory-guard address listed below. These are **raw machine-code windows**,
not proofs of instruction boundaries or safe patch sites. Compare them
with the user-supplied Ghidra program and Magellan references before
authoring any runtime edit. The monitor does not change the EXE.

CI additionally compiles `static_pool_census.cpp` with **native Windows MSVC
x86**, independent of the Python preflight tests. This tests the module's
syntax/object generation, not linking the complete d3d9.dll or loading
Helm's Deep.

## Exact runtime pool patch worklist

The capacity increase must be one coherent transaction, not a naked guard
change. Magellan's address references supply the following initial map; **the
retail instruction bytes and other references still need GZF confirmation**.

| Resource | Constructor reserve calls | Registry early guard | Element stride |
|---|---|---|---:|
| Models | `0x00876E17 / 0x00876E22 / 0x00876E2D` | `FUN_00875D1E` at `0x00875D2F`, limit `0x400` | 8 |
| Textures | `0x00876DF8 / 0x00876E00` | `FUN_00876B70`, limit `0x800` | 8 |
| Effects | `0x00876E43 / 0x00876E4E / 0x00876E59` | `FUN_00877396`, limit `0x200` | 8 |
| GFX blocks | `0x00876E6F` | `FUN_00875AB8`, limit `0x40` | 8 |

An increased model registry requires all THREE 8-byte pair vectors to
reserve the higher capacity before registration, plus a matching change
to the factory's early guard. Otherwise the retail insertion helper still
returns -1 at the old capacity, or the factory silently skips all models.
Never substitute `0x00CD80D4`'s frame-transient dynamic geometry pool
for the level's static model registry.

**Status:** runtime registry reallocations and retail guard patching are
NOT implemented. The offline PAK preflight and read-only census are implemented
and opt-in only. No merged Helm's Deep+Isengard runtime test is claimed.


## 2026-11-08: exact-executable capacity patches (supersedes old status paragraphs)

The recovered `ConquestLLC.exe` SHA-256 `3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858` provides exact code for previously unknown reservoirs. The original PField 12-byte record allocator `FUN_00464AC7` **reserves 0x1000 = 4096 entries**, matching its factory `FUN_00875BF7` guard. The `MgLevelObject.cpp` 512-entry reserve is explicitly a **temporary stand-in**, not the native limit. Resource Other at `level+0xF4` reserves 2048 via `FUN_004347AB` and has a matching guard at `0x00877A1B`. Both helpers have a unique level constructor caller. Exact-byte checked, optional runtime patches now increase ResourceOther to **4096/8192**, PField to **8192**, and an independent GFX level registry patch increases **64→256/512** without changing the shared animation map allocator. See `AUXILIARY_REGISTRY_8192.md`, `GFX_REGISTRY_512.md`, and `REGISTRY_CAPACITY_TIERS.md`. All new patches are default OFF; none has a gameplay runtime proof.
