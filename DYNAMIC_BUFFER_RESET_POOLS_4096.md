# Native Dynamic VB/IB Reset Pools: 64 to 1024/4096

This PR contains executable x86 capacity patches for TWO independent Direct3D buffer resource lists. It does not change the editor, any map importer, the generic device-reset list introduced in PR #4, static managed VB/IB records, or Havok.

## Verified source

Original ConquestLLC.exe SHA-256 3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858, checked directly against native text bytes and the existing Magellan C++ port Vespucci/Engine/Magellan/MgGameBuffers.cpp.

| Pool | Native base | Native count | CRT boot | Register | Original capacity | Optional capacity |
| --- | --- | --- | --- | --- | --- | --- |
| Dynamic VB wrappers | 0x00A41520 | 0x00A41524 | 0x00967BE5 | 0x0068DFD3 | 64 pointers | 1024 or 4096 |
| Dynamic IB wrappers | 0x00A4152C | 0x00A41530 | 0x00967C06 | 0x0068DCC1 | 64 pointers | 1024 or 4096 |

Each boot allocates 0x100 bytes (64 * 4 bytes) through the existing native locked realloc 0x0067E662. Both register paths append without bounds checking. Device-lost, device-reset, swap-delete and the CRT destructor all load the same native base pointer and count cells.

## Opt-in capacity extension

The two lists expand in lockstep to 1024 or 4096 pointers each. The implementation:
1. Fingerprints original boot, unchecked append, destruction, release/recreate, atexit and native realloc machine code for BOTH lists; rejects unknown EXEs.
2. Requires no active level/resource manager and count=0 in BOTH lists. It never migrates live D3D9 wrappers.
3. Reallocates already-booted arrays with the SAME original game allocator and publishes the new pointers into their original globals. If CRT has not booted, the patched original boot allocation size handles it.
4. Changes only each original PUSH imm32 byte count in its CRT initializer, and redirects the two register prologues through independent x86 guard stubs. When full or null, an append is skipped instead of overwriting heap memory. Otherwise, the original native append/increment runs unchanged.
5. Leaves the dynamic wrapper flag bit 2 untouched: it also controls D3DLOCK_DISCARD. Unregister scans the list, and safely does nothing when the object was never registered.
6. Restores original code protections and flushes instruction cache. Failed machine-code writes get best-effort rollback. The trampolines stay executable for the process lifetime after successful installation.

The original game has no bounds check for these arrays. If the new capacity is itself exhausted, an overflow wrapper is NOT tracked for device-reset recreation. This prevents heap corruption but is not a correct substitute for provisioning enough capacity.

The VB and IB allocation operands are at 0x00967BE8 and 0x00967C09. Their native register sites are 0x0068DFD3 and 0x0068DCC1. The original register continuation logic, release, recreate, unregister and atexit remain unmodified.

## Enable in proxy.ini (next to the game executable)

    [StaticMapPools]
    ExpandDynamicBufferReset1024=0
    ExpandDynamicBufferReset4096=0

All switches are OFF by default. Set one to 1 only for a matching, supported retail executable. If both are true, the higher 4096 tier wins.

The log dynamic_buffer_reset_pools_4096.log reports ACTIVE, REJECT or FATAL. ACTIVE proves that process-memory patching succeeded, NOT that the game successfully handled device loss/reset.

## Validation boundaries

The 32-bit MSVC DLL must compile and link. Python regression tests compare the original x86 instruction bytes and the isolation and control-flow contracts. Real game startup, device loss/reset and level teardown still require runtime tests. No purported general-purpose Havok pool increase is claimed.
