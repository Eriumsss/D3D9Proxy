# 1024 EffectInfo registry entries (retail x86 experimental patch)

Only to accommodate more **native static FX definitions** when Isengard
geometry/materials are imported INTO Helm's Deep's existing level. This is
not a second world, mission script loader or NPC pool change. An EffectInfo
registry entry is not a guarantee its FX will render: GameObjs blob refs,
emitter definitions, materials, textures, shaders and downstream runtime
pools must also be valid.

## Ghidra/machine-code proof

Original EXE recovered from `ConquestEA.exe.gzf`, SHA256
`3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858`.
This specific EXE has a custom `.1911` section; the patch is **not**
compatible with arbitrary pristine retail EXEs without exact fingerprint
matching.

```asm
00876E43  CALL 0040B38A      ; level+FC  effect A registry
00876E4E  CALL 0040B38A      ; level+104 effect B
00876E59  CALL 0040B38A      ; level+10C effect C

0040B38A  JMP 00461D2C       ; shared reserve helper
00461D2F  MOV EDI,200h       ; cap = 512, operand at 00461D30

008773A0  MOVSX ECX,WORD PTR [EBX+FCh]
008773A7  LEA EDX,[ECX+EAX]
008773AA  CMP EDX,200h       ; raw FX factory guard, operand at 008773AC
008773B5  JG 0087759F       ; skips all effects when incoming exceeds cap
```

Only the two 4-byte immediate operands change to `0x400` (1024),
after fingerprint validation of both instruction windows, all three
factory caller sites, the reserve thunk and pair-insertion helper. Effect
vectors hold 8-byte entries with signed 16-bit count/capacity fields;
1024 is representable. No PAK/BIN or EXE on-disk edits occur.

The patch fails closed if the game EXE, source instruction fingerprints
or uninitialized level/resource-manager globals do not match. Protection
is acquired for both instruction pages before any write; rollback is
best-effort on write errors. **This is not a globally atomic patch** and
must only run at early Direct3D initialization before level construction.

## Configuration

```ini
[StaticMapPools]
Monitor=1
ExpandModels2048=0
ExpandTextures4096=0
ExpandEffects1024=0
```

Default OFF. `ExpandEffects1024=1` writes `effect_registry_1024.log`
containing ACTIVE/REJECT/FATAL. Test only on a disposable game install
with the analyzed EXE. The three options can be toggled independently,
although combined-map imports may need more than one capacity increase.

## Known boundaries

Effect 512 -> 1024 does NOT increase GFX block (64), the separate animation registry
at level+0xF4 (2048), PField, frame transient geometry, static VB/IB, Havok, occlusion,
texture VRAM or the total 32-bit VA budget. The static map import
dependencies and level unload/reload still require real game validation.

CI status should report separate synthetic PAK tests, exact GZF-derived
opcode fixtures and full MSVC x86 `d3d9.dll` build/link. **No successful
merged level runtime test is claimed.**
