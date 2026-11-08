# Native Havok world / process VA correlation

This opt-in addition helps diagnose a **single native Helm's Deep level
containing static Isengard geometry**. Its output is process-wide virtual
address space, NOT Havok allocator totals, refcounted-object lifetimes,
collision bodies, a free-list pool capacity, or proof of a memory leak.

## Why the existing CSVs are not enough

`havok_world_lifecycle.csv` identifies observed `hkpWorld*` transitions.
`virtual_address_pressure.csv` measures x86 address-space pressure.
Both files append across launches and use `GetTickCount`. Joining them on
bare milliseconds across runs can produce completely bogus correlations.
Neither has the same exact sampling moment, and the older VA CSV has no
per-process session key.

This change adds one **joint sample** CSV, not a guessed join. The same
existing pinned `static_pool_census.cpp` worker first validates the
physics world pointer, scans the x86 process VA via the existing
`VirtualQuery` loop, and rereads the world owner after the scan to mark
any detected teardown race as invalid. Both observations share one row.

## Configuration

Use a disposable copy of the retail game with a CI-built standalone
D3D9Proxy x86 DLL. Add the following keys to the existing game-folder
`proxy.ini`:

```ini
[StaticMapPools]
PhysicsWorldMonitor=1
PhysicsVaCorrelation=1
```

Both default to `0` in new auto-generated INIs. Missing keys default to
`0`, and existing INIs are not overwritten. `PhysicsVaCorrelation=1`
does nothing without `PhysicsWorldMonitor=1`. No new worker, thread, hook,
EXE patch, PAK/BIN write, or Havok allocator mutation is introduced.
Because scanning the **entire x86 VA** costs extra work, VA correlation
has a *separate opt-in switch*. It runs at most once a second while active.

## Output and session isolation

`havok_world_va_pressure.csv` is written beside the game's EXE and
appends across launches. Every row includes a 64-bit process-start
`FILETIME`-derived `session_id` and `process_id`; these identify each
capture session. The file contains:

`session_id,process_id,time_ms,level_object,manager_ptr,world_ptr,loading_flag,world_valid,world_status,transition,va_valid,committed_bytes,reserved_bytes,free_bytes,largest_free_bytes,free_regions,reserved_regions,committed_regions`.

A pair is only comparable when the **same session/PID** has adjacent,
valid world+VA samples, a monotonic timer, and a gap at most 3500 ms by
default. Invalid or race-detected samples break continuity. The data is
subject to 1-second sampling blind spots and process-wide activity from
unrelated systems.

To summarize:

```powershell
python tools/analyze_havok_va_correlation.py --game-dir "C:\Games\LOTR Conquest"
python tools/analyze_havok_va_correlation.py --game-dir "C:\Games\LOTR Conquest" --json
```

The analyzer reports peak committed VA, smallest largest-free VA region,
world-appear/disappear event intervals, and signed process-committed /
largest-contiguous-free deltas *only* where an adjacent trustworthy pair
exists. It also compares the first pre-world baseline with the last
sampled world disappearance if both exist **in the same session**.

**NOT Havok heap bytes.** Increased process committed VA after level exit
is not proof of an hkPoolMemory leak: the engine, memory caches, rendering,
audio, textures, and level systems can all retain process pages normally.
Likewise, returning to the old committed-VA baseline does not prove
correct Havok teardown. A pointer transition is only an observed edge,
not an exact allocation/free callback.

## Comparison plan

On the same game executable and proxy build:

1. Untouched Helm's Deep: boot, level enter, full match, level exit, reenter.
2. Untouched Isengard: repeat, without mission-script modifications.
3. Disposable Helm's Deep + Isengard static merged PAK/BIN: repeat.

Keep each launch's session ID separate. Compare peak VA and world lifecycle
with the read-only PAK native collision preflight, blend pool, and registry
CSV. If the merge fails before world creation, the VA timeline may still
show the memory pressure at that point. If the world exists throughout,
this does not certify collision, ownership or Havok free-list correctness.

## Validation scope

Python synthetic tests cover sessions, invalid-gap handling, signed
process deltas, schema failures and safe-off configuration. MSVC x86 CI
build verifies the DLL compiles/links. Neither substitutes for real
gameplay with the actual merged native map and matching retail executable.

A future actual hkPoolMemory allocation/free tracker requires decoded,
fingerprinted entry AND return/teardown call paths; this PR deliberately
does NOT install a blind detour at `0x00495C30`.
