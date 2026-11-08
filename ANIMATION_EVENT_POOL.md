# Animation EVENT pool: stock capacity, memory safety, and remaining expansion work

## Verified distinction

The *retail animation-event pool* at `[0x00CD9970]` is a separate object
from the *animation BLEND-slot array* at
`[[0x00CD7E7C]+0x0C]+0x9004`. They **both** have a stock limit of
`0x400 = 1024`, but their records are incompatible.

| Native object | Stock layout | Source authority |
| --- | --- | --- |
| Animation event records | 1024 × 0x30 = 0xC000 bytes | `MgAgSpineEval.inc`, `MgAnimEventDrain.inc`, native `PostAnimEvent` at `0x0088AA78` |
| Animation blend slots | 1024 × 0xF8 = 0x3E000 bytes | `MgAgBlendEval.inc`, `FUN_0045549C` |
| Animation binding clip/skeleton caches | 64 each, extended separately to 256/512 | `animation_binding_cache_512.cpp` |

Native event globals: `[0x00CD996C]` free-list head,
`[0x00CD9970]` pool base pointer, `[0x00CD9974]` count.
The `CMP DWORD PTR [0x00CD9974], 0x400` at `0x0088AA85`
has original bytes `81 3D 74 99 CD 00 00 04 00 00`.
Event indices are 16-bit, with `0xFFFF` as full sentinel in
`PostAnimEvent`. This is not proof of an existing 4096-slot backing arena.

## Old 4096-slot claim was incorrect

The legacy `memorypatch.cpp` held `POOL_MAX_ENTRIES = 0x1000`
while the actual stock event pool has 1024 × 0x30 slots.
That made its freelist validator accept addresses outside the
stock arena. It also labeled `g_EventPoolNewLimit = 0x400`
as "4096", despite `0x400` being **1024**.

More seriously, legacy Patch 6 attempted to publish a new arena and
only patch ONE event bound, without established lifetime/ownership
and teardown of the game's original allocation. Legacy Patch 29
targets `0x004554B1`, the completely different BLEND-slot allocator.
Changing its compare to 4096 without relocating the fixed 1024-slot
array would write through adjacent memory.

**Both legacy patches remain disabled** and now carry C++ compile-time
safety assertions that forbid simply flipping either hardcoded bool.
This PR **does not** introduce or activate a 4096-event expansion.

## Read-only evidence collection

In a disposable game copy, add:

```ini
[StaticMapPools]
EventPoolMonitor=1
```

The generated default is `0`; the existing INI is never modified.
This uses the **existing** pinned read-only worker and can run without
`Monitor`, `BlendMonitor`, or `PhysicsWorldMonitor` enabled.
Output: `animation_event_pool.csv` beside the game's EXE.

The sample validates the exact known retail CMP opcode, the three
globals, all 1024 × 0x30 bytes being readable, the free-head
range/alignment, and stable rereads. It logs used-count and observed
peak with statuses `OK`, `NEAR_STOCK_CAP`, `AT_STOCK_CAP`,
`NO_POOL` or invalid reasons like `UNVERIFIED_RETAIL_GUARD`,
`BAD_FREE_HEAD`, `COUNT_EXCEEDS_STOCK`, `UNREADABLE_ARENA`
and `RACED`. A changed opcode does NOT cause a "4096 ACTIVE" claim.

Also logs the arena's `VirtualQuery` allocation base and
`MEM_IMAGE`/`MEM_PRIVATE` region type. **Those do not prove the
allocator/free owner**, which must be established from native init
and teardown before swapping the pool pointer.

```powershell
python tools/analyze_runtime_pressure.py --game-dir "C:\Games\LOTR Conquest" --json
```

The analyzer includes the event pool if its CSV exists, alongside
the separate blend pool and address-space pressure.

## What a genuine safe 4096-slot expansion still requires

1. Original event pool allocator constructor, exact allocated arena
   lifetime and teardown/deallocator reconstructed from original x86
   game instructions.
2. Full producer/free-list/consumer and 16-bit index bounds audit,
   including all hard-coded 1024 comparisons, `0xFFFF` sentinel,
   and count-vs-free semantics.
3. A pre-level, before-first-use, **atomic** redirection of allocation,
   every relevant native bound and ownership/free path, with full
   instruction fingerprint verification and rollback before publish.
4. Retail enter/exit/reload stress tests, event saturation, memory
   and heap ownership checks, and Alt+Enter checks on a disposable
   game build.

Do not confuse a full count with allocator failure while a reusable
free-list may exist. Do not guess Havok capacities from these records.

The source and synthetic tests in this PR establish the *correct
stock bounds* and improve fault detection. **The event pool is NOT expanded.**
