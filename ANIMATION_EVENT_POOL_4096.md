# Native animation EVENT pool: experimental 1024 → 4096

**NOT retail-validated.** This is an opt-in x86 memory patch in
`Eriumsss/D3D9Proxy`, not a level importer, not the 1024-slot animation
**blend** arena, not Havok, and not RTX Remix. Leave the flag **OFF**
on your primary game installation.

## Source-verified native ownership

In `Eriumsss/OpenConquest`, reconstructed Magellan C++:

| Original x86 address | Reconstructed behavior |
| --- | --- |
| `FUN_009555DF` | Static constructor builds `0x400` records of stride `0x30` using `FUN_00404C2F` |
| `FUN_00404C2F` | Native `__stdcall PoolInit(hdr, stride, count)`, returns header, uses original `0x0067E610` allocator then `0x00404C6C` freelist linker |
| `FUN_0096E44C` | Registered atexit destructor `delete[]` for current `[0x00CD9970]` base |
| `FUN_0088AA78` | PostAnimEvent, `CMP [0x00CD9974],0x400` at `0x0088AA85`; computes `uint16_t` record index; full sentinel `0xFFFF` |
| `FUN_00404CBB` and `FUN_0042E090` | Return events by record index to the freelist, decrement live count |
| `MgAnimEventDrain.inc` | Consumer indexes `base + uint16_index * 0x30`; a `>=0x400` guard in the **reconstruction** is a host safety addition, not a verified retail boundary |

The real native pool has three adjacent global fields:
`freeHead = [0x00CD996C]`, `base = [0x00CD9970]`,
`live = [0x00CD9974]`. The separate blend-slot `FUN_0045549C`
owns 1024 records of stride `0xF8` and must **never** be modified by
this feature.

## Opt-in usage

The new `proxy.ini` generator writes this setting **OFF**.
Only on a disposable game copy with the exact supported retail EXE,
set:

```ini
[StaticMapPools]
ExpandAnimationEventPool4096=1
EventPoolMonitor=1
```

Read `animation_event_pool_4096.log` beside the game's EXE.
`ACTIVE` means the new native allocation, owner pointer and single
producer guard were verified/published at startup. `REJECT` means
the install was not completed. `FATAL` means stop the game and revert
the DLL/test copy; do **not** keep playing.

`animation_event_pool.csv` reports `stock_capacity=1024` for
reference, plus `effective_capacity=4096` and
`expanded_active=1` **only** when the native module verified
activation. `UNVERIFIED_RETAIL_GUARD` is an invalid sample, not
proof of an unknown patched capacity. Offline:

```powershell
python tools/analyze_runtime_pressure.py --game-dir "C:\Games\LOTR Conquest" --json
```

## Install transaction and safeguards

1. Requires the original x86 image base `0x00400000`, exact original
   PostAnimEvent CMP bytes, a neighboring native pool instruction window,
   and a verified *relative call* from the game's own
   `FUN_009555DF` static constructor to `FUN_00404C2F`.
2. Requires `[0x00CD7EA0]` level and `[0x00CD80D8]` resource manager
   both null. The **entire original 1024-entry freelist** must contain
   exactly the original arena's 1024 aligned, unique entries, with
   `live==0`.
3. Calls the game's original `FUN_00404C2F(temp,0x30,0x1000)` and checks
   that the newly allocated 192 KiB arena has **4096 distinct, aligned
   freelist nodes**, all linked, no cycles and no external pointers.
4. Protects **only** the 4-byte immediate in the verified original
   `CMP` at `0x0088AA8B`, rechecks original idle state and expected
   globals, publishes the new base/head and patches the count guard from
   `0x400` to `0x1000`. The code reads the published state back and
   restores executable page protection. There are no new detours or hooks.
5. Once active, old legacy event pool crash/freelist checks use the
   verified 4096 capacity; without verified activation, they remain at
   1024 and cannot be fooled by an unrelated external guard write.
6. At process exit the game's **original registered atexit destructor**
   sees the new base, which came from its original native allocator.
   The original 48 KiB pool is deliberately quarantined, not freed in
   flight. This is a **one-time 48 KiB retained allocation per process**,
   not a repeatedly growing per-level leak.

**Important race limitation:** Updating the separate base, freeHead
and guard fields is *not globally atomic*. The pre-world idle, zero
live-count and fully-free original freelist checks make the startup
transition plausible, but cannot prove another game thread will never
enter PostAnimEvent in that window. If a concurrent mutation is seen,
the module rejects or emits `FATAL`. A correct experiment still
requires retail load/unload/reload and event saturation testing.

## Required real-game test matrix

1. Baseline unmodified proxy `ExpandAnimationEventPool4096=0`:
   launch, one full match, exit, reenter, then repeat while monitoring.
2. Opt-in `4096=1` on the **same** executable: check `ACTIVE`,
   `effective_capacity=4096`, pointer stability and no freelist
   corruption across multiple matches and level loads.
3. Trigger high event usage, monitor peaks above 1024 with no
   `0xFFFF` index entering a consumer, and test repeated Alt+Enter.
4. Only then test a native merged Helm's Deep + Isengard static map
   and correlate with model/texture/GFX and Havok logs.

The Python tests and Windows MSVC x86 build check source contracts,
native calling-convention compilation and CSV handling. They do NOT
execute the retail binary or prove its real `FUN_00404C2F` callsite
matches the expected image. The feature remains **experimental** until
the exact executable and the real game pass the matrix.
