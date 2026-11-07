# Larger native registry ceilings (experimental)

The public D3D9Proxy now contains optional **new x86 executable patches** for static native model/texture/effect registry capacity, in addition to the previous lower tiers. The target remains **ONE native game level**; the proxy is not the editor, donor map importer, a second world, or RTX Remix.

| Registry | Retail allocation/guard | Existing opt-in tier | New high tier | Pair vectors | Entry stride |
|---|---:|---:|---:|---:|---:|
| ModelInfo | 1024 | 2048 | **4096** | 3 | 8 bytes |
| TextureInfo | 2048 | 4096 | **8192** | 2 | 8 bytes |
| EffectInfo | 512 | 1024 | **2048** | 3 | 8 bytes |

These are **real registry array reserves**, not geometry, GPU residency, Havok, occlusion, PAK records, or overall 32-bit process VA. The high-tier pair-array bytes are 3×4096×8 = 96 KiB (model), 2×8192×8 = 128 KiB (texture), and 3×2048×8 = 48 KiB (effect). They remain below the signed 16-bit small-vector capacity 32767, but in-game effectiveness is unverified.

## Configuration

```ini
[StaticMapPools]
Monitor=1
ExpandModels2048=0
ExpandModels4096=0
ExpandTextures4096=0
ExpandTextures8192=0
ExpandEffects1024=0
ExpandEffects2048=0
```

Every switch defaults OFF. Either low or high may be enabled; when both options in a registry are set, **the higher tier takes precedence**. No generic arbitrary size is accepted from the INI. Existing function entry points keep their names for compatibility.

### Actual x86 patch sites

- **Models:** `MOV EDI, imm32` operand `0x00876E0C` controls the reserve of three 8-byte pair arrays. Factory `CMP ECX, imm32` operand `0x00875D36` gets the identical capacity.
- **Textures:** `MOV EDI, imm32` operand `0x00876DF4` controls two pair arrays. Factory `CMP EAX, imm32` operand `0x00876B7D` gets the identical capacity.
- **Effects:** reserve helper `MOV EDI, imm32` operand `0x00461D30` feeds three pair vectors. Factory `CMP EDX, imm32` operand `0x008773AC` gets the identical capacity.

Instruction fingerprints and addresses are inherited from the archived Ghidra ConquestEA.exe snapshot SHA256 `3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858`, which includes a custom `.1911` section. They do **NOT** apply to all retail binaries. Only exact known machine code and an uninitialized level/resource manager are accepted. The EXE file is never modified; memory patch sites are temporarily page-writable, then the previous protection is restored. On errors, attempted immediate writes are rolled back best-effort. These patches are not cross-thread atomic: they must run before any level construction.

```powershell
python tools/pak_pool_preflight.py --base helm.pak --donor isengard.pak --merged merged.pak --assume-model4096 --assume-texture8192 --assume-effects2048 --json
```

The `--assume-*` flags are **hypothetical** and do not prove successful patch initialization. Confirm `ACTIVE` logs in `model_registry_2048.log`, `texture_registry_4096.log`, `effect_registry_1024.log`, then compare actual registry counts and capacities.

## Other outstanding pool ceilings

GFX block registry starts at 64. Its `0x40` reserve is shared with another callsite, so patching that global helper blindly would affect an unrelated data structure. PField factory's early raw-count guard is 4096 but the retail allocation size is **not** proven by the Magellan stand-in; it is not safe to label it 4096 or simply increase the guard. Neither GFX nor PField is changed in this tier patch.

No Helm's Deep + Isengard runtime evidence is claimed until a live merged level loads, registers these entries, and reloads without memory corruption.
