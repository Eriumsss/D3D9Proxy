# Native Resource Other and PField registry capacity patches

Source: verified original `ConquestLLC.exe` PE SHA256 `3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858`, cross-referenced to existing `Vespucci/Engine/Magellan/MgLevelObject.cpp` and `MgLevelConstruct.cpp`. Original executable data and Ghidra archives are not redistributed.

## Actual native pool facts

| Registry | SmallVec header | Entry stride | Retail reserve and factory guard | Opt-in replacement |
|---|---|---|---:|---:|
| Resource Other | `level+0xF4` | 8 bytes | 2048 | 4096 / 8192 |
| PField | `level+0x114` | 12 bytes | **4096** | 8192 |

The Magellan port had a provisional `Mg_Reserve12(...,0x200)` placeholder because the original PField helper was not in its corpus. The **actual executable** has `MOV EDI,0x1000` at `0x00464ACA`. The PField factory compares against `0x1000` at `0x00875C05`. This corrects the old misconception that PField's allocation was 512.

### Exact sites patched

- Resource Other: reserve helper `FUN_004347AB` at `0x004347AB`, 8-byte x86 records, MOV EDI imm32 operand at `0x004347AF`. Factory CMP EAX imm32 operand at `0x00877A1C`. Constructor `0x00876E38` calls the thunk `0x0040B321`; the recovered image has **one direct caller** for this helper. Its expected code includes `51 53 57 BF 00 08 00 00`.
- PField: reserve helper `FUN_00464AC7` at `0x00464AC7`, 12-byte x86 records, MOV EDI imm32 operand at `0x00464ACB`. Factory CMP EDX imm32 operand at `0x00875C07`. Constructor `0x00876E64` calls thunk `0x0040B430`; the recovered image has **one direct caller** for this helper. Expected original code includes `51 53 57 BF 00 10 00 00`.

Both patches change **only two 32-bit immediate operands** each. Native reserve implementations still allocate through the original engine allocator and free old arrays with the original engine free. No external allocator, replacement array layout or PAK writer is used.

All original instruction windows (constructor CALL context, thunk JMP, helper prologue, factory guard and conditional jump) are checked before enabling any write. Level and resource manager global pointers must be zero, and both instruction pages must be protected before writes. Failed writes cause best-effort rollback; original protections are restored. These edits are not globally cross-thread atomic.

### INI

```ini
[StaticMapPools]
ExpandResourceOther4096=0
ExpandResourceOther8192=0
ExpandPFields8192=0
```

All flags OFF by default. If both Resource Other flags are enabled, 8192 takes precedence. The PField patch goes from 4096 directly to 8192. Since the SmallVec capacity is signed 16-bit, 8192 is within range. Allocated pair-array sizes are only 64 KiB for 8192 8-byte Resource Other entries, and 96 KiB for 8192 12-byte PField entries; this does not increase virtual address space or Havok budget.

Both produce `auxiliary_registry_8192.log` with per-registry ACTIVE/REJECT/FATAL messages. Compare actual live registry capacities using `static_map_pools.csv` with `Monitor=1`.

```powershell
python tools/pak_pool_preflight.py --base helmsdeep.PAK --merged merged.PAK --assume-pfields8192 --assume-gfx512 --json
```

The PAK preflight now knows that the original PField factory guard is 4096. The `level+0xF4` runtime registry does not map one-to-one to an independently verified on-disk PAK header count, so there is deliberately **no invented `--assume-resourceother` offline count flag**. Use live pool census for it.

No claim of running a real Helm's Deep + Isengard merged PAK or proving allocation stability is made.
