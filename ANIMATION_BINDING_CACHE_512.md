# Native two-level animation binding cache: 64 → 256 / 512

This is an optional, executable x86 runtime capacity expansion for **existing clip/skeleton animation binding cache arrays**. It is **not** a model importer, a second world, Havok memory, dynamic geometry or the 1024-slot fixed-layout blend pool.

## Source-backed original limits

The checked original `ConquestLLC.exe` (SHA-256 `3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858`) and named Magellan port `Vespucci/Engine/Magellan/MgAnTaskExec.inc` agree on this two-level structure:

| Native structure | Function/site | Record structure | Original cap | Optional cap |
| --- | --- | --- | ---: | ---: |
| Outer **clip CRC → skeleton map** registry | `FUN_008F90BC` virtual method, vtable `0x009C7A24`; setup call `0x00917E2B` | 8-byte `{u32 CRC, map*}` at registry+0x0C | 64 | **256 / 512** |
| Inner **skeleton CRC → resolved binding** map | `0x008F9301` calls `FUN_0040B4CB` | 8-byte `{u32 CRC, binding*}` | 64 | **256 / 512** |

The outer reserve method sets the init bit on exit by tail-jumping into `FUN_0089F25C`, which matters during level teardown. The inner reserve helper `FUN_0040B4CB` **also serves the separate GFX block registry** at `0x00876E6F`. Raising its 64 literal globally would change both owners and could interfere with the GFX registry patch.

## Actual C++ x86 patches

`src/animation_binding_cache_512.cpp` patches only two sites after matching exact original x86 code windows and verifying no level, resource manager or binding registry has been initialized:

1. At **`0x008F90BC`**, replace the first complete seven instruction bytes `53 56 6A 40 8B F1 5B` with a JMP plus two NOPs. The trampoline receives `ECX=registry`, uses `ESI=ECX+0x0C` (SmallVec header), loads `EDI=256/512` and calls the **original `FUN_00EA3A30` native SmallVec8 reserve**. After restoring all original general registers and flags, it tail-jumps into original `FUN_0089F25C` (not just RET), preserving the setup flag gate.
2. At **`0x008F9301`**, replace only the original CALL `E8 C5 21 B1 FF` with a CALL to a second trampoline. Here `EBX` is the new skeleton map, verified by original `MOV ESI,EBX` at `0x008F92FF`; this hook invokes the same original SmallVec8 reserve with the new capacity, then returns to the unmodified original append, qsort and registration code.

The shared `FUN_0040B4CB` instruction bytes, the GFX caller, the bsearch/qsort loops, map append, registry append, binding cleanup and free paths remain **unmodified**. This patch runs **after `OC_GfxRegistry512_Init()`**, because GFX's version check expects the original animation CALL byte sequence. It remains compatible with GFX's own isolated constructor hook.

The two patches are opt-in, verified only for the exact x86 image, and executed before level startup. Both code pages are made writable before any edit, instruction cache is flushed, original page protections are restored, and failed writes trigger best-effort rollback. Any trampoline potentially referenced after a failed rollback remains mapped rather than freeing executing memory. This patch is **not cross-thread atomic**; it must not be enabled after the game has created its first animation binding registry.

Both `int16_t` count/capacity values remain unchanged in layout, with 512 well below 32767. Each 512-entry pointer-pair vector reserves 4096 bytes, through the game's own allocation/free and cleanup ownership.

## How to enable

After merging this PR and installing its new `d3d9.dll`, the auto-generated `proxy.ini` beside the game EXE includes:

```ini
[StaticMapPools]
ExpandAnimationBindingCache256=0
ExpandAnimationBindingCache512=0
```

Both OFF by default. Set exactly one to `1` for a verified game EXE. If both are `1`, **512 wins**. The log `animation_binding_cache_512.log` distinguishes ACTIVE, REJECT and FATAL. ACTIVE means the two instruction sites were patched in process memory, **not** that actual animation caching and level reload have been tested.

## Remaining limits and test boundary

This is a cache capacity increase; it cannot increase animation asset formats, pose track count, Havok allocations, process virtual address space or the separate `0xF8`-stride fixed animation blend pool. Its old 1024-slot counter sits directly after a fixed 1024×0xF8 arena, so blindly raising that limit is a memory corruption hazard.

GitHub Python regressions validate the recovered x86 bytes, hook controls, rollback, separate GFX code isolation and build wiring. MSVC x86 CI builds and links the full DLL. Neither can confirm native game startup, actual cache pressure, animation correctness, scene unload or resource teardown; those require a disposable runtime test.
