# Experimental retail static ModelInfo 2048 patch

Target: Helm's Deep remains ONE level; imported Isengard static models reside
in Helm's Deep's native ModelInfo / material / texture / geometry structures.
No extra level loader, no donor mission scripts, no NPC changes.

## Evidence from the user's actual Ghidra archive

The original file bytes of the archived `ConquestEA.exe` were reconstructed
using Ghidra's "File Bytes" chain, validated with MZ/PE headers and section
bounds. Extracted PE SHA256:
`3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858`

This specific analyzed EXE has an injected `.1911` section: calls to
`0x004067F8` jump to the small-vector reserve implementation at
`0x00EA3A30`. **It must NOT be treated as a fingerprint for all pristine
retail EXEs.** Other variants are expected to fail the byte checks.

Confirmed assembly:

```asm
00876e05  lea esi, [ebx+8Ch]    ; model A vector
00876e0b  mov edi, 400h         ; ONE allocation capacity for all three
00876e17  call 004067f8h       ; model A (reserve8; JMP -> 00ea3a30)
00876e1c  lea esi, [ebx+94h]   ; model B
00876e22  call 004067f8h
00876e27  lea esi, [ebx+9Ch]   ; model C
00876e2d  call 004067f8h

00875d2f  movsx ecx, word ptr [ebx]
00875d32  add ecx, eax
00875d34  cmp ecx, 400h        ; early factory guard
00875d3a  jg 00875e1ch
```

The factory examines **raw** ModelInfo count + current sorted count before
iterating game-mode-filtered rows. Raising only the guard would allow an
out-of-bounds insert into the three 1024-entry allocation buffers. Raising
only the constructor would still cause early abort at 1024.

This patch replaces ONLY two 4-byte immediates (`0x00876E0C` and
`0x00875D36`) from 0x400 to 0x800 after verifying exact original
opcode windows, all three call sites and the EXE's image base. It demands
that both the global level object `[0x00CD7EA0]` and resource manager
`[0x00CD80D8]` be NULL before changing anything. On known failures it
tries to roll back writes and restore code page protections. If the level
was initialized before the first Direct3DCreate9 call, it refuses to run.
The executable file on disk is never modified.

## Opt-in (experimental, test game installation only)

Put in `proxy.ini` beside the EXE:

```ini
[StaticMapPools]
Monitor=1
ExpandModels2048=0
```

Default `0`. **Do not turn to 1 until the complete D3D9Proxy x86 build,
Windows gameplay and PAK testing have been performed.** For a separate
disposable copy set `ExpandModels2048=1` and inspect
`model_registry_2048.log` for ACTIVE or REJECT. If REJECT, do not assume
the capacity has changed. Monitor writes `static_map_pools.csv` and opcode
evidence separately.

The model 2048 cap is NOT a license to import Isengard's mission script or
NPC population. Other caps (texture 2048, effect 512, GFX 64, PField
unknown, Havok, occlusion, vertex/index buffers, 32-bit VA) remain.

**Validation status:** exact byte snapshots from the provided GZF checked
and embedded in offline regression tests. Runtime engine correctness has
NOT been demonstrated. A successful MSVC C++ module compile, on its own,
is NOT a retail game test.
