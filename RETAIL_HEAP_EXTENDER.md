# D3D9Proxy retail heap extension (experimental, disabled by default)

Development branch: fix/d3d9proxy-retail-arena-guard-20261107

## Source of truth

This is an adaptation of the existing NpcProxy implementation, **not** a new
allocator. Read:
- DLL/NpcProxy/PLAN.md §12.2
- DLL/NpcProxy/src/npc_game.cpp: NpcGame_HeapUpkeep, kGrowSigs, kGaugeSigs
- Vespucci/Engine/Magellan/MgParseHelpers.cpp and the 0x67d8d7 path
- The Ghidra ConquestEA.exe program database: instruction bytes and references

The original engine allocates 0x06500000 bytes (101 MiB) with CRT malloc at
0x0067d790. Its original pointer at 0x00CD8104 must never be changed: the
engine's destructor eventually frees that original pointer. Under game heap
lock #2 (0x0067b7f3 / 0x0067b19d), the engine's current grow cursor and end
can be changed to a newly VirtualAlloc'd 64 MiB block, retaining old chunks.
At most eight extra chunks = 613 MiB total nominal arena supply.

## Enabling only for a dedicated test game copy

In the directory containing the EXE create or edit proxy.ini:

    [RetailHeapExtender]
    Enabled=0

Keep **0** until the x86 MSVC build and a retail runtime test pass. Change to 1
only on a disposable/test game copy. If NpcProxy's separate extender is also
injected or otherwise active, do not enable this one. The fingerprint/arena
guard detects many conflicts but does NOT make simultaneous dual ownership
a supported configuration.

When enabled, a worker samples every 100 ms, including during renderless level
loading, and the D3D EndScene hook samples once more per frame. Both paths
serialize through g_busy and the game allocator's original lock. There is no
EXE .text modification and no expansion of animation event / blend-slot / dynamic
geometry record pools. Additional chunks are retained until process exit.
The opt-in worker pins the proxy module to avoid executing code from an
unloaded DLL.

Log: retail_heap_extender.log beside the EXE, if writable.
On mismatched executable fingerprints or a foreign arena owner, disable and
continue running with the engine's original behavior.

## Validation status

Source code has been integrated into the D3D9Proxy branch. Static regression
checks: python D3D9Proxy/tests/test_retail_heap_extender.py

NOT YET VALIDATED: MSVC x86 linking, executable runtime, 64-bit OS VA
fragmentation, game shutdown, D3D9Ex interface wrapping, or real memory
pressure under AI/geometry/texture workloads. Do not claim an offline test
proves retail allocator correctness.

Risk note: IPC x64 broker (in earlier experiment packages) is a separate
mechanism and is NOT needed by or called from this engine heap extender.
