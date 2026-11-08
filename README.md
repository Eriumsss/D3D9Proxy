# D3D9Proxy

Standalone, public **x86 Direct3D 9 proxy** for *The Lord of the Rings: Conquest* (2009). This repository supersedes the original prototype and tracks the D3D9Proxy source formerly kept inside `Eriumsss/OpenConquest/D3D9Proxy/`.

**Development focus:** increase the retail game's **registry and memory pool ceilings** to support larger native levels, including a single Helm's Deep level containing static Isengard assets. The proxy is **not** a level importer, a second-world loader, or RTX Remix.

## Implemented experimental registry capacity patches

| Native registry | Retail cap | Opt-in patch |
| --- | ---: | ---: |
| ModelInfo | 1024 | 2048 or **4096** |
| TextureInfo | 2048 | 4096 or **8192** |
| EffectInfo | 512 | 1024 or **2048** |
| **GFX blocks** | **64** | **256 or 512** |
| **Animation clip/skeleton binding caches (two-level)** | **64 each** | **256 or 512 each** |
| **Resource Other (+0xF4)** | **2048** | **4096 or 8192** |
| **PFields (+0x114)** | **4096** | **8192** |
| **D3D9 device-reset list** | **64** | **1024 or 4096** |
| **Dynamic VB recreate list** | **64** | **1024 or 4096** |
| **Dynamic IB recreate list** | **64** | **1024 or 4096** |

The six registries and separate D3D9 device-reset list are **OFF by default** and gated by original x86 instruction fingerprints. Havok and transient geometry pools are not modified. A successful DLL build or synthetic test does **not** prove an expanded map runs correctly in the game.

```ini
[StaticMapPools]
Monitor=1
ExpandModels2048=0
ExpandModels4096=0
ExpandTextures4096=0
ExpandTextures8192=0
ExpandEffects1024=0
ExpandEffects2048=0
ExpandGfx256=0
ExpandGfx512=0
ExpandAnimationBindingCache256=0
ExpandAnimationBindingCache512=0
ExpandResourceOther4096=0
ExpandResourceOther8192=0
ExpandPFields8192=0
ExpandDeviceResetResources1024=0
ExpandDeviceResetResources4096=0
ExpandDynamicBufferReset1024=0
ExpandDynamicBufferReset4096=0
GeometryBudget=1
BlendMonitor=0
```

Other included modules: read-only pool census, static buffer budget telemetry, optional texture memory vault, native PAK/BIN preflight tools, and retail tracing components. The existing diagnostics and auxiliary modules have been retained from the working OpenConquest proxy rather than discarded during the split.

## Automatic proxy.ini configuration

With the latest DLL, `proxy.ini` is **created automatically beside the game executable on the first `Direct3DCreate9` / `Direct3DCreate9Ex` call**. It is created before registry/pool options are read, in the EXE folder (not the current working directory), using a same-folder temporary file and an atomic non-replacing rename. An existing `proxy.ini` is **never overwritten, edited, or reset**. If the game folder is read-only, Windows will prevent creation; grant normal write access or create the file manually.

All experimental `Expand*` switches start at `0` for safety. To test a verified-executable capacity patch, open the generated file and set the desired high-tier key to `1`; for example `ExpandModels4096=1`. Read-only `Monitor=1` and `GeometryBudget=1` diagnostics are enabled in the template. No map importer, second-world loader, Havok hard limit or RTX Remix is enabled automatically.

The generated file includes current model/texture/effect/GFX/Resource Other/PField/device-reset keys and the future dynamic VB/IB recreate keys. Existing custom `proxy.ini` files are retained verbatim even if missing newer options. `[RetailHeapExtender]` and `[WorldResourceMemory]` experimental switches also default off.

## Build

Requires **Windows**, the **MSVC x86 toolchain** and Windows SDK with D3D9 import libraries.

- Run `build.bat` from the repository root. It emits `out/d3d9.dll`.
- Or configure `cmake -S . -B out/cmake -A Win32` with a compatible Visual Studio generator, followed by `cmake --build out/cmake --config Release`.

On GitHub Actions the full proxy is compiled and linked with MSVC x86 and the read-only Python regression suite runs on Ubuntu. Follow the workflow results before deploying to a disposable game installation.

Do not replace the game's D3D9 DLL blindly if another graphics proxy is already installed. Experimental capacity patches are tied to an exact analyzed Conquest executable variant and will reject unsupported binaries.

## Offline validation

```sh
python -m unittest discover -s tests -p "test_*.py" -v
python tools/pak_pool_preflight.py --help
```

The PAK/BIN tooling is deliberately **read-only**. It checks reference integrity and capacities; it never imports a map or modifies a game asset.

### Repository migration

- Source snapshot: `Eriumsss/OpenConquest` commit `aa17f06a3539adc6fbd6d7d495884d25313cad38`, directory `D3D9Proxy/`.
- The previous standalone repository `main` is backed up at branch `backup/pre-openconquest-sync-20261108`, commit `2f7a067faae5189e795e2751ac01823251735e9b`.
- Old committed CMake build output and binaries were **not** copied into the new source tree.
- The original standalone repository's MIT `LICENSE` is retained.

`tools/build_retail_symbols.py` and `build_fn_trace.bat` may require an **external analysis corpus/ported engine tree** present in a local OpenConquest workspace. Those external source datasets are not vendored into this public standalone repository.

### Optional Magellan/NpcProxy oracle checks

Some static regression tests compare extracted x86 opcode contracts with the original Magellan C++ port or NpcProxy implementation. To keep this public project independent, those **external-oracle comparisons are skipped** when the private OpenConquest sources are absent; standalone source/patch and PAK tests still execute. To enable all original comparisons, set the `OPENCONQUEST_ROOT` environment variable to a local checkout of `Eriumsss/OpenConquest` (the directory containing `Vespucci/` and `DLL/`). No engine/oracle sources are bundled into this repository.

### New high-capacity runtime tiers

Set `ExpandModels4096=1`, `ExpandTextures8192=1`, and/or `ExpandEffects2048=1` under `[StaticMapPools]`. All are OFF by default; when both low and high flags are enabled for the same registry, high wins. Each tier changes the **constructor reserve and factory guard together**, with the same exact opcode, pre-level, and rollback safeguards as the previous modes. The older options are retained. For documented limitations and exact sites, see [REGISTRY_CAPACITY_TIERS.md](REGISTRY_CAPACITY_TIERS.md). This is real runtime patch code but has **not yet passed a merged-level gameplay test**.

### Isolated GFX Block registry reserve and factory guard

`ExpandGfx256=1` or `ExpandGfx512=1` now patches **only** the GFX level vector's constructor allocation and matching factory early guard. The engine's separate animation binding registry shares the old 64-entry reserve helper, so the patch routes **only GFX's constructor callsite** to a private trampoline that invokes the already-verified native 8-byte pair reserve implementation. The factory's original `JG`, bsearch, qsort, insertion and unload logic remain intact. This patch is exact-image gated and OFF by default; it does **not** load a second world. More: [GFX_REGISTRY_512.md](GFX_REGISTRY_512.md).

### Resource Other and PField native pool expansions

Recovered x86 code proves the **true retail PField reserve is 4096** rather than the 512 temporary stand-in in an older Magellan port. Independent opt-in patch flags now expand the native +0xF4 8-byte resource registry from **2048→4096/8192** and the +0x114 12-byte PField registry from **4096→8192**. Each modifies its own existing allocator reserve immediate and matching factory guard, preserving the original engine allocation/free implementation and exact version gating. More: [AUXILIARY_REGISTRY_8192.md](AUXILIARY_REGISTRY_8192.md).

### Device-reset resource pool (separate from static geometry)

The original 64-pointer list is allocated by the game at 0x00967C27, registered at 0x0068D892 and freed at 0x0096F72D. New opt-in C++ code expands to 1024 or 4096 pointers and guards the formerly unchecked append. If the CRT already allocated 64 slots, it grows the same native allocation through the game's original 0x0067E662 realloc when there are no live entries. It does not modify managed VB/IB or Havok. See DEVICE_RESET_POOL_4096.md for exact machine code and limits. Game runtime validation is pending.

### Dynamic VB/IB native reset-list pool expansion

Two independent 64-pointer arrays ([0x00A41520] for dynamic VB, [0x00A4152C] for dynamic IB) have unchecked original register writes. Opt-in `ExpandDynamicBufferReset1024=1` or `ExpandDynamicBufferReset4096=1` increases BOTH original allocation and registration limits, using the existing native allocator and isolated x86 guard trampolines. Neither changes the generic device-reset resource list, Havok, or managed static geometry buffers. See [DYNAMIC_BUFFER_RESET_POOLS_4096.md](DYNAMIC_BUFFER_RESET_POOLS_4096.md). The automatic `proxy.ini` generator from PR #7 includes both options, disabled by default. Gameplay reset/reload validation is still required.

### Native two-level animation binding cache limit

The retail animation cache contains two independent 64-entry SmallVec8 pair arrays: outer clip-CRC registration and inner skeleton-CRC binding maps. An opt-in x86 patch, `ExpandAnimationBindingCache256=1` or `ExpandAnimationBindingCache512=1`, expands both using the original native `FUN_00EA3A30` reserve implementation, preserving the registry's original `FUN_0089F25C` initialization and existing append/search/sort/unload logic. The separate GFX pool also calls the old `FUN_0040B4CB` allocator, so **only the animation callsite is redirected**; the shared allocator remains unchanged. The animation patch initializes **after the GFX patch preflight**. See [ANIMATION_BINDING_CACHE_512.md](ANIMATION_BINDING_CACHE_512.md). This is not a Havok allocator, not the 1024-slot animation blend pool, and is unverified in actual gameplay.

### Read-only native runtime pool pressure

With [StaticMapPools] Monitor=1, the existing census worker writes runtime_pool_pressure.csv beside static_map_pools.csv. It samples the three native device-reset registration lists, animation clip/skeleton binding counts, per-level high-water marks and validity flags. This does not prove an Expand* patch actually activated. See [RUNTIME_POOL_PRESSURE.md](RUNTIME_POOL_PRESSURE.md) for layouts, fields and limitations.

### Diagnosing Alt+Enter / Direct3D9 Reset faults

The CI-built x86 release artifact now includes the **matching** `d3d9.dll`, `d3d9.pdb` and `d3d9.map`. To identify a crash from the DLL's module-relative EIP instead of guessing which reset callback failed, run `python tools/symbolize_proxy_crash.py --map d3d9.map --rva 0xB1B9` using the MAP from the **same** DLL build. The nearest PUBLIC alone is not proof of the precise source instruction; see [CRASH_SYMBOLIZATION.md](CRASH_SYMBOLIZATION.md) for WinDbg PDB steps and a controlled Alt+Enter A/B test. A full match played successfully with expansion options enabled is encouraging, but frequent Alt+Enter causing `0xC0000005` remains a distinct reset-path problem.

### x86 virtual-address pressure and offline triage

With `[StaticMapPools] Monitor=1`, the existing static census worker now writes `virtual_address_pressure.csv` even before a level exists. A read-only `VirtualQuery` scan approximately every two seconds reports process free/reserved/committed VA and the largest contiguous free region. Inspect it alongside registry/reset list CSVs with `python tools/analyze_runtime_pressure.py --game-dir "C:\Games\LOTR Conquest"` or add `--json` for a structured report. The default 128 MiB warning threshold is adjustable, NOT a verified engine limit. See [VIRTUAL_ADDRESS_PRESSURE.md](VIRTUAL_ADDRESS_PRESSURE.md).

### Fixed 1024-slot animation blend pool (read-only monitor)

The retail animation **blend slot array** is separate from the 512-entry
animation clip/skeleton binding caches and the event pool. Its native allocator
uses 1024 consecutive 0xF8-byte records; the next-free counter is immediately
after the last record. Merely increasing its compare immediate would corrupt
the engine's adjacent data. The stock capacity is **not expanded**.

On a disposable test game, use `[StaticMapPools] BlendMonitor=1` in
`proxy.ini` (auto-generated default is `0`). The existing pinned
diagnostic worker writes `animation_blend_pool.csv` every second,
including before a level exists, with guarded counter/guard validity,
a per-arena sampled peak and explicit `NEAR_CAP`/`AT_CAP` statuses.
No new worker, EXE patch, PAK write or Havok change is introduced. The
optional offline VA analyzer will summarize valid blend records if present.
This is **not** a live animation object count or runtime proof.

See [ANIMATION_BLEND_POOL.md](ANIMATION_BLEND_POOL.md) for the mapped native
layout, validation limitations, and the Helm's Deep + Isengard test plan.

### Native collision footprint for combined static maps

Run `tools/pak_pool_preflight.py --collision-links` for each base, donor and
actual merged PAK to obtain **ModelInfo-reachable** ShapeInfo/HkShapeInfo
descriptor counts and distinct Block1 convex/MOPP payload byte spans. The
output separates referenced rows from orphan descriptor rows, avoids double
counting shared disk spans, and marks incomplete/unknown shape layouts.

Pair it with `--collision-preservation --require-base-collision
--require-donor-collision` for collision payload integrity checks. These are
**on-disk measurements**, never Havok runtime pool ceilings or physics proof.
See [COLLISION_FOOTPRINT.md](COLLISION_FOOTPRINT.md).

### Correlate Havok world life with x86 process VA pressure

Opt in with `[StaticMapPools]` `PhysicsWorldMonitor=1` and
`PhysicsVaCorrelation=1` on a disposable test copy. The latter
option defaults **OFF** and triggers additional VirtualQuery passes
only when world monitoring is active. Each row in
`havok_world_va_pressure.csv` contains a validated sampled world
pointer, 32-bit process virtual-address memory counters, process ID,
and a unique launch-session ID. It works before a level exists.

Use `python tools/analyze_havok_va_correlation.py --game-dir PATH`
or add `--json`. Adjacent world events and process VA deltas are
compared **only within the same launch session and process**.
This is process-wide VA, **NOT Havok allocator bytes or leak proof**.
No EXE hook, PAK write or allocator expansion is introduced.
See [HAVOK_VA_CORRELATION.md](HAVOK_VA_CORRELATION.md).

### Strict static ModelInfo import validation (opt-in)

`tools/pak_pool_preflight.py` now supports
`--static-model-preservation --required-donor-model 0xCRC` to reject
merged PAKs that lose **any original Helm's Deep ModelInfo identity**
(including collisionless models), drop original GameModeMask bits, or
lose all LOD meshes for selected imported Isengard models. Only explicitly
named donor CRCs are required; the tool does not pretend the full donor
world or mission was imported. `--model-key-map` maps renamed donor CRCs.

Use `--strict-native-metadata` to turn zero CRC keys, duplicate CRC keys
and zero GameModeMask counts in the **target PAK** into hard failures,
rather than the usual warnings. Companion `--model-links` with matching
BINs and `--collision-preservation` remain independent checks.
No in-game changes, patches or PAK writes occur.
See [STATIC_MODEL_PRESERVATION.md](STATIC_MODEL_PRESERVATION.md).
