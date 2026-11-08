# Retail Havok world lifecycle: read-only pointer ownership probe

## Purpose

The goal is **one native Helm's Deep level containing static Isengard assets**,
not an RTX Remix layer, not a second-world loader and not a mission-script
import. A merged map can fail during enter, exit or reload even when ModelInfo,
texture and FX registries are far from full.

This opt-in, read-only x86 probe samples the Havok world ownership edge in
the *existing* D3D9Proxy monitor worker. It is **not the allocator** and does
not claim to count Havok allocations, hkpShape objects, collision agents,
cached MOPP bytecode or Havok pool capacity.

## Retail pointer provenance

Reconstructed engine oracle in `Vespucci/Engine/Magellan/MgPhysics.cpp`
and `MgPhysics.h`:

- `[0x00CD7ED0]`: physics-manager pointer, a 0x220-byte game object.
- `[manager + 0x00]`: expected manager vtable `0x009CAF94`.
- `[manager + 0x0E8]`: physics manager's thread-memory pointer, **not**
  a byte counter or an independently known allocation pool.
- `[manager + 0x0FC]`: actual `hkpWorld*`. World create
  `FUN_0085a353` stamps the pointer; destroy
  `FUN_0085acc7` releases it and zeros the slot.
- `[manager + 0x1AD]`: level-loading flag, observed as an unsigned byte.

All these addresses and the manager vtable apply only to the known x86
retail image. The existing executable-base/byte heuristic must pass.
This monitor additionally checks the manager vtable, bounds, page access,
and rereads manager/world pointers to reject some concurrent teardown races.
An unsupported layout produces an **invalid sample**, not an invented zero.

The actual Havok SDK allocation path in the reconstruction is described as
`TlsGetValue([0x00A46A84])` / memory-router accessor
`[0x009700AC]` / allocator function `0x00495C30`.
Those functions are **not hooked by this PR**. A separate, properly
fingerprinted allocation/return/free tracker is required to measure heap
bytes or prove a leak.

## Enable

Install only a matching, CI-built x86 D3D9Proxy DLL in a **disposable**
retail Conquest game copy, with the existing game settings backed up.
In the game's `proxy.ini`:

```ini
[StaticMapPools]
Monitor=1
BlendMonitor=1
PhysicsWorldMonitor=1
```

`PhysicsWorldMonitor=0` in newly generated INI files, and missing keys
default to zero. Existing INI files are never rewritten. This switch also
works when `Monitor=0` and `BlendMonitor=0`, because the existing pinned
worker checks it independently. There is only **one worker thread**.
Sampling occurs approximately once per second, including pre-level loading.

Output beside the EXE: `havok_world_lifecycle.csv`.
Columns:

`time_ms,process_id,level_object,level_valid,manager_ptr,manager_vtable,thread_memory_ptr,world_ptr,loading_flag,valid,status,transition,observed_attaches,observed_detaches,observed_replacements`

Status `WORLD_PRESENT` means a validated non-null `hkpWorld*` at that
sample; `NO_WORLD` means a validated manager with null `hkpWorld*`;
`NO_MANAGER` means no manager was constructed. Other statuses are
incomplete or inconsistent observations and have `valid=0`.

`INITIAL_SAMPLE` is **not** counted as a new world. The only counted
transitions are `WORLD_APPEARED`, `WORLD_DISAPPEARED` and
`WORLD_REPLACED` between adjacent valid samples. A bad/racing sample
breaks continuity. Fast creation/unload cycles can be missed by polling;
the counts are lower bounds on **observed events**, not exact SDK calls.

Analyze the CSV without any VA/registry files:

```powershell
python tools/analyze_havok_world.py --game-dir "C:\Games\LOTR Conquest"
python tools/analyze_havok_world.py --game-dir "C:\Games\LOTR Conquest" --json
```

Keep these independent logs in the same test copy when possible:
`virtual_address_pressure.csv`, `animation_blend_pool.csv`,
`static_map_pools.csv`, `runtime_pool_pressure.csv`.

## Controlled three-run comparison

1. Untouched Helm's Deep: title screen, level enter, a match,
   level exit, reenter. Look for world presence and sampled disappearance.
2. Untouched Isengard: same lifetime path, no imported donor scripts.
3. Disposable native Helm's Deep PAK/BIN with Isengard **static** assets:
   run the same enter/exit/reenter sequence.

Compare loading flag, world pointer transitions and contemporaneous VA and
collision-preflight data. The offline `--collision-links` tool measures
on-disk shapes/MOPP, *not* these runtime Havok objects. Do not infer
allocator exhaustion or world cleanup from either view alone.

## Safety and verification limits

This PR adds **no native writes**, no breakpoints or detours, no SDK hook,
no new worker, no PAK mutation and no allocator pool expansion.
The sampled world pointer may be nonzero right before normal game shutdown;
that is **not proof of a leak**. A zero world pointer is not proof that
Havok has reclaimed every cached block or that physics bodies are gone.

Python source contracts and a Windows MSVC x86 build test compilation and
CSV handling, not actual game behavior. A matching x86 retail executable
and real-world reload runs are required before changing Havok allocations.
