# Retail static texture registry 4096 patch (experimental)

**Actual goal:** add Isengard's static map assets *inside* Helm's Deep's
existing native level. It does not load a second world or Isengard missions.
This changes the *engine's keyed TextureInfo registry*, not D3D9 GPU VRAM or
the NpcProxy texture vault.

## Exact Ghidra and Magellan evidence

The user-supplied Ghidra `ConquestEA.exe.gzf` contains original
`ConquestEA.exe` bytes with SHA256
`3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858`.
**This analyzed EXE includes a custom `.1911` section.** Patch refuses
unknown binaries and all unexpected instruction bytes.

| Instruction | Original | Proposed | Reason |
|---|---|---|---|
| `0x00876DF3` (`MOV EDI, imm32`) | `0x800` | `0x1000` | Both texture registry vectors A/B reserve this capacity |
| `0x00876B7C` (`CMP EAX, imm32`) | `0x800` | `0x1000` | Factory early-return cap for raw TextureInfo count |

The 4-byte immediate operands start at `0x00876DF4` and
`0x00876B7D`. The two reserve calls at `0x00876DF8` and
`0x00876E00` both target the original `0x004067F8` reserve8 thunk,
which jumps to `0x00EA3A30`. The inserted texture pair helper
`0x0040B3F3` has its own count-vs-cap guard, so keeping the vector
headers and allocation size in sync matters.

Texture registry arrays live at level-object `+0x24` and `+0x2C`.
The on-disk `TextureInfo` rows live in Block1; raising a registry cap
does NOT repair invalid CRCs, model references, materials or BufferInfo.

The other `CMP EAX,0x800` at `0x00877A1B` belongs to a *different*
animation registry at `level+0xF4`, processed by `FUN_00877A00_BuildAnimations`.
It is intentionally untouched: static map import does not imply importing
Isengard's character animation tables or mission scripts.

## Relationship with PR #5 model patch

The texture constructor opcode fingerprint ends **immediately before**
`0x00876E05`, where the separate model constructor setup starts.
Therefore both patches can be enabled in the same early Direct3DCreate9
initialization without clobbering each other's inspected bytes. Each is
still a separate best-effort two-site write, NOT an atomic engine-wide
transaction. Keep both disabled for normal installations until game tests.

## Opt-in

In `proxy.ini` next to the actual game EXE:

```ini
[StaticMapPools]
Monitor=1
ExpandModels2048=0
ExpandTextures4096=0
```

`ExpandTextures4096=1` requires the exact analyzed EXE variant, all
instruction signatures, `[0x00CD7EA0]==0`,
`[0x00CD80D8]==0` and accessible EXE code pages. It emits
`texture_registry_4096.log`. On mismatch, it refuses to patch.
The executable on disk is unchanged.

## Read-only merged-PAK capacity what-if

The preflight tool still evaluates **stock capacities by default**. To see
what the registry counts would imply **only if both runtime patches were
verified ACTIVE** on the correct ConquestEA variant, add:

```powershell
python D3D9Proxy/tools/pak_pool_preflight.py --base helmsdeep.PAK --donor isengard.PAK --merged helmsdeep_merged.PAK --deep --assume-model2048 --assume-texture4096
```

These flags are **hypothetical**, not a proof that the executable was patched.
The JSON output includes `assumed_patch_caps_not_verified`. An actual merged
PAK containing 3000 TextureInfo entries can be below the assumed 4096 guard
but still be rejected by a stock 2048 guard. Effect/GFX/PField and other
engine limits are NOT relaxed by either flag. The original PAK inputs are
never modified.

## Validation boundaries

The selected CPU-array capacity changes 2048 -> 4096 in two vectors, and
the factory guard also changes 2048 -> 4096. However **this is NOT yet
tested inside a live Conquest game**. Offline GZF opcode tests and native
Windows MSVC x86 DLL link do not prove runtime health. Benchmark native
Helm's Deep, native Isengard and the merged Helm's Deep test PAK, including
model/texture registry counts, texture resolver hits, scene unload/reload,
Havok/occlusion, and 32-bit VA.

This PR is stacked onto the unfinished PR #5, which itself depends on PR #4.
No script, NPC logic, Magellan source, PAK/BIN bytes or x64 broker changes.
