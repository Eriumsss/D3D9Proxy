# Native D3D9 device-reset resource pool 64 -> 1024 / 4096

This is a genuine fixed-size runtime pointer list, not the ModelInfo/TextureInfo registry, Havok pool, or static managed VB/IB budget. The proxy does not import maps or load another world.

## Verified native x86 evidence

Original ConquestLLC.exe SHA-256: 3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858. The recovered executable was inspected byte-for-byte against the named Magellan C++ port, particularly Vespucci/Engine/Magellan/MgEntityMgr.cpp. Proprietary game binaries are not included in this public repository.

- Boot 0x00967C27: PUSH 0, PUSH 0x100, PUSH 0; CALL 0x0067E662. The CRT allocates 256 bytes for 64 32-bit pointers into global [0x00A41538].
- Append 0x0068D8A3: load count [0x00A4153C] and base [0x00A41538]. At 0x0068D8AE it writes [base+count*4] without checking the count. The 65th entry overwrites the end of the original allocation.
- Unregister 0x0068D299 and reset/rebuild 0x0068D861: both re-read the global base and count, rather than using a compiled-in 64-element array.
- CRT atexit function 0x0096F72D: frees the very same global pointer through the game's original allocator.
- Unrelated sibling initializer 0x00967C06: also allocates 0x100, but into a DIFFERENT global [0x00A4152C]. This patch never modifies it.

## Runtime patch

With a supported exact x86 image and no active level, the module offers two opt-in capacities: 1024 pointers (4096 bytes) and 4096 pointers (16384 bytes). Both options default OFF. Higher wins when both selected.

1. Verify the original boot, append, continuation, unregister, recreate, sibling and atexit machine code fingerprints, plus the native realloc implementation.
2. Refuse to edit if level/resource manager is initialized or the list has any live entries. No migrating live device resources.
3. Allocate a private guarded-code trampoline RW, then switch it to RX. Before each append, it reads the native base and count. If either the base is NULL or the count reaches capacity, it clears the object's registered flag [+0x51] and returns through the original code, preventing heap corruption. Otherwise, it jumps to the unmodified original append and increment.
4. If the list already booted with 64 slots, use the original game's 0x0067E662 realloc(oldPtr, requestedBytes, 0), then publish the returned pointer into the original global. Thus the original CRT owner remains responsible for freeing it. If boot has not yet run, the patched allocation operand is used when the original initializer runs.
5. Modify only boot-size imm32 at 0x00967C2A and the original 11-byte append loads at 0x0068D8A3. Restore original code page permissions, flush instruction cache, and attempt rollback on any failed write.

The trampoline allocation stays alive for the process lifetime after successful hook installation. This does not globally suspend threads: enable only in a clean process before level construction. If the list fills at 1024 or 4096, an additional resource will not be registered for future device resets. That is preferable to corrupting the heap, but it is NOT proof the game will tolerate exceeding the list. Monitor real usage and size for the workload.

## proxy.ini

    [StaticMapPools]
    ExpandDeviceResetResources1024=0
    ExpandDeviceResetResources4096=0

Both flags default to zero. The log is device_reset_pool_4096.log. ACTIVE confirms successful process-memory code installation, REJECT indicates unsupported or already active state.

## Verification boundaries

The CI x86 build and synthetic byte regression tests are NOT a gameplay test. Full device-loss/recreate, level reload and resource teardown should be verified on a disposable game copy.

This patch is not the Havok memory allocator, not GPU residency and not the dynamic geometry pool. The Magellan/Havok SDK uses multiple allocation classes and world configuration fields. No singular authoritative Havok hard-cap instruction has yet been verified for a corresponding expansion.
