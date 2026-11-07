# GFX Block registry: 64 → 256 / 512 (native x86 code patch)

## Ground truth, not a speculative constant change

The original recovered `ConquestLLC.exe` in the supplied RE archive has SHA-256 `3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858`. The executable image matches the existing Ghidra/Magellan source oracle. Its original bytes were checked at the actual instruction addresses, and **no executable or game asset file is copied into this public repository**.

`Vespucci/Engine/Magellan/MgLevelObject.cpp` shows that the constructor initializes the level's GFX Block registry at `level+0x11C` with a 64-entry, 8-byte `{u32 key, record*}` pair array. `Vespucci/Engine/Magellan/MgLevelConstruct.cpp` reproduces the real `FUN_00875AB8` factory, including the 64-entry early guard, the insert-or-replace loop and the sorted registry.

### Why not patch the shared reserve helper?

`FUN_0040B4CB` is called at **both** `0x00876E6F` (level GFX) and `0x008F9301` (animation skeleton binding map). It allocates EXACTLY 64 pairs. Replacing its internal `0x40` globally would alter the animation binding cache, which the GFX expansion must not do.

The patch therefore uses two narrow, exact-byte guarded sites:

1. **GFX constructor**: at `0x00876E6F`, original bytes `E8 57 46 B9 FF` call `0x0040B4CB`. Replace only that CALL with another CALL to a dedicated executable thunk. The thunk preserves all caller GPRs and EFLAGS, computes `ESI = EBX+0x11C`, places **256 or 512** in `EDI`, calls the original `0x00EA3A30` 8-byte pair reserve and returns. It uses the engine's original allocation, free, base-pointer update and count clamping. The animation CALL stays unchanged.
2. **GFX factory guard**: original byte sequence at `0x00875AC8` is `83 F9 40 89 5D F8`: `CMP ECX,64; MOV [EBP-8],EBX`. Redirect **exactly these six bytes** through a tiny executable trampoline that executes `CMP ECX,imm32`, replays the MOV, and returns to `0x00875ACE`. The original `JG`, bsearch, qsort, insertion and unload stay byte-identical.

The constructor's caller and the factory's full prologue, the animation caller, the shared helper and native reserve8 implementation are fingerprinted first. Both edits are protected before either is written. Patches reject unknown EXEs and initialized managers. Attempted writes roll back on failure, and the trampoline allocation stays alive for the process's lifetime after installation. The patches are **not cross-thread atomic**; enable only before first level construction.

### Configuration

```ini
[StaticMapPools]
ExpandGfx256=0
ExpandGfx512=0
```

Both OFF by default. If both switches are true, **512 wins**. 256 reserves 2048 bytes, 512 reserves 4096 bytes for the GFX pair array. No mission scripts, new map loader, NPC changes, textures, or Havok patches.

Logs: `gfx_registry_512.log`. Successful patch reports `ACTIVE`; unknown executable or failed preflight reports `REJECT`. Confirm actual `gfx_block` vector capacity in `static_map_pools.csv` with `Monitor=1`.

For hypothetical PAK checks (no proof of game runtime):

```powershell
python tools/pak_pool_preflight.py --base helmsdeep.PAK --merged merged.PAK --assume-gfx512 --json
```

**Not validated in the live game yet.** GitHub x86 build and static/synthetic checks prove only build and implementation contracts. A merged level must still be run and unloaded without corruption before enabling by default.
