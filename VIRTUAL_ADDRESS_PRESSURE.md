# x86 process virtual-address pressure: read-only monitor and offline triage

This is a genuine process VA diagnostic for one native Helm's Deep level with additional Isengard static assets. A registry expansion is not proof that the 32-bit process can find enough contiguous address space.

## What the runtime DLL records

The same existing static-pool worker uses the read-only Windows VirtualQuery API to traverse all application address-space regions, from GetSystemInfo().lpMinimumApplicationAddress through lpMaximumApplicationAddress inclusive, once about every 2 seconds. It does NOT make a second thread, install a new executable patch or modify engine allocations.

The output file, virtual_address_pressure.csv in the game EXE directory, has:
- time_ms and level_object (8-digit hex pointer, zero before a level exists);
- valid (0 on a partial VM scan, 1 on a complete scan);
- minimum_address and maximum_address from the operating system (hex);
- free_bytes, reserved_bytes and committed_bytes;
- largest_free_bytes: largest contiguous MEM_FREE region seen in the scan;
- free_regions, reserved_regions and committed_regions.

All byte sums and region bounds use 64-bit arithmetic. Scans cannot accidentally wrap when approaching the x86 maximum application address. A failed or malformed VirtualQuery result produces valid=0, never a misleading completed measurement.

Unlike static_map_pools.csv, the new VA log also runs BEFORE the first level object exists. That makes it possible to observe address-space pressure during a loading hang which has not yet created a level object. Registry counter sampling still runs at 250ms; full VA scans only at the two-second log cadence.

MEM_COMMIT is process committed VIRTUAL ADDRESS SPACE and does not equal resident physical RAM or GPU memory. The largest reported free VM region is not a guarantee that any particular engine allocation will succeed.

## Analyze without opening the game

From a checkout of the standalone D3D9Proxy repository, with a game installation that has Monitor=1 enabled:

    python tools/analyze_runtime_pressure.py --game-dir "C:\Games\LOTR Conquest"
    python tools/analyze_runtime_pressure.py --game-dir "C:\Games\LOTR Conquest" --json
    python tools/analyze_runtime_pressure.py --game-dir "C:\Games\LOTR Conquest" --warn-largest-free-mib 256

This reads virtual_address_pressure.csv and, if present, static_map_pools.csv and runtime_pool_pressure.csv. It summarizes latest and worst sampled contiguous free sizes, maximum observed committed VA, pre-level sample count, valid/invalid sample count, registry high-water values versus ACTUAL observed header capacity, and device-reset / animation binding sampled peaks.

The default 128 MiB largest-free warning threshold is an adjustable COMPARISON value, not a verified game memory limit. Configured Expand* options are not used to infer an active patch or allocated capacity. Consult each patch's ACTIVE / REJECT / FATAL log and an actual game run separately.

## Remaining risk

This does not change Havok, any PAK/BIN asset, the native allocation code, pointer size, LAA/PE flags or the static buffer format. In-process VM snapshots can race with other game threads. OS free region sizes and memory map fragmentation are only one potential cause of a load failure. A synthetic Python unit test and a successful x86 MSVC link are NOT proof of game runtime success.
