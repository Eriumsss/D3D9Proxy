# Read-only native runtime pool-pressure census

This adds live capacity diagnostics to the existing single D3D9Proxy static pool worker. The original registry memory is never modified. No new hook, second world, PAK importer, NPC logic or Havok allocator modification.

## Native source coordinates

Grounded in recovered ConquestLLC.exe (SHA256 3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858), MgGameBuffers.cpp, MgEntityMgr.cpp and MgAnTaskExec.inc.

| List | Base pointer global | Count global | Stock slots |
|---|---|---|---:|
| Device-reset resources | 0x00A41538 | 0x00A4153C | 64 |
| Dynamic vertex buffer recreation | 0x00A41520 | 0x00A41524 | 64 |
| Dynamic index buffer recreation | 0x00A4152C | 0x00A41530 | 64 |

The three globals expose counts and pointer addresses but do NOT expose their current allocated capacities. Thus this logger never reports assumed 1024 or 4096 as a live allocated size. It records valid current count and high-water sampled count.

Animation binding registry pointer: global 0x00CD7E5C; expected vtable 0x009C7A1C. Clip cache header is at registry+0x0C (signed 16-bit count/capacity, u32 base). Pairs are 8-byte clip CRC / map pointer. Each map is an 8-byte signed count/capacity/pointer header for skeleton CRC / binding pointer pairs. The logger reports outer clip count, actual outer capacity from the object header, observed clip peak, aggregate valid inner map count and binding pairs, largest inner map, and inconsistent map samples.

## Usage

Install a combined x86 DLL built from all dependent PRs on a disposable game copy. Keep [StaticMapPools] Monitor=1 in proxy.ini. When the game creates a level, the existing census worker writes static_map_pools.csv and the new runtime_pool_pressure.csv next to the game executable. Exercise Helm's Deep and static Isengard assets in the same level; reload the scene, then compare both outputs.

Every 250 ms the existing worker polls. CSV is written around every 2 seconds. The high-water values are sampled peaks, not guaranteed maxima between samples. The peaks reset when the level object changes.

Field validity: bind_present=0 means no animation registry at that instant; bind_valid=0 means its pointer or layout could not be validated. invalid_inner_maps measures entries which could not be safely read in this snapshot. The three resource lists have separate validity flags. Do not infer actual allocation sizes from these flags or counters.

The read-only code uses VirtualQuery, SEH and bounded inner-map traversal. This is not an atomic snapshot: another game thread can mutate or free a binding map while the worker reads it. Any inconsistent sample must be treated as uncertain.

The CSV is not proof a patch was applied, not the PAK file contents and not a Havok memory report. Consult hook-specific ACTIVE/REJECT/FATAL logs and actual game stability independently. The fixed-layout 1024 × 0xF8 animation blend arena is not modified.

The Python contracts inspect source/CSV schema; Windows MSVC CI checks compilation. Neither demonstrates in-game runtime correctness.
