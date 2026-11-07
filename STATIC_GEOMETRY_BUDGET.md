# Static geometry capacity and budget: Helm's Deep + Isengard

**Scope:** Isengard's static map data is imported **into Helm's Deep's ONE native level**. There is no second world loader, no donor mission scripts, and no NPC-capacity work.

## What the original engine actually does

The model registry's **1024** fixed entry cap is distinct from GPU geometry storage. Magellan `MgGameBuffers.cpp/.h` and the reconstructed original `ConquestEA.exe` show the static model commit path:

1. `ModelInfo` registered into level's model vectors.
2. `VBuffInfo` and `IBuffInfo` descriptors are read from decompressed PAK Block1.
3. Each VB is built at `FUN_0068E091` with `IDirect3DDevice9::CreateVertexBuffer` via the vtable call at `0x0068E0E9` (return PC `0x0068E0EC`). GPU-backed static VB uses `D3DPOOL_MANAGED` and WRITEONLY. Some VBs with `fmt1 & 0x40000` stay CPU-side and do **not** pass through Direct3D.
4. Each IB is built at `FUN_0068DD41`, `IDirect3DDevice9::CreateIndexBuffer` call `0x0068DD74` (return PC `0x0068DD77`). These use `D3DPOOL_MANAGED`; selected IBs also retain a CPU copy.
5. The **dynamic** ring manager at `[0x00CD80D0]` and transient geometry pool `[0x00CD80D4]` have separate capacities, *not* a global static-map VB/IB count.

There is no evidence for a universal fixed **static** vertex/index buffer count to patch. Their practical bottlenecks include 32-bit process virtual address space, D3D9 managed resource duplication, driver memory, corrupted PAK offsets, and failures during `CreateVertexBuffer`/`CreateIndexBuffer`.

## Offline PAK geometry preflight

```powershell
python D3D9Proxy/tools/pak_pool_preflight.py --base helmsdeep.PAK --donor isengard.PAK --merged helmsdeep_merged.PAK --bin-base helmsdeep.BIN --bin-donor isengard.BIN --bin-merged helmsdeep_merged.BIN --deep --geometry --model-links --native-tables --verify-bin-streams --json
```

`--geometry` decompresses **PAK Block1** and reports the native VBuffInfo/IBuffInfo record counts and cumulative requested bytes. It **DOES NOT validate payload offsets against PAK Block2**. That previous claim in PR #8 was incorrect: the native model loader's `VBuffInfo.offset` and `IBuffInfo.offset` are relative to each model's **BIN asset payload**, not PAK Block2. `Block2` holds unrelated level data such as language/sub-block payloads. The preflight labels geometry-only spans *unverified*, never falsely rejects them for exceeding Block2.

`--model-links` follows `ModelInfo -> mesh_order (model-relative, low 30 bits) -> BufferInfo -> VBuffInfo/IBuffInfo` and verifies that each reference lands on the **correct 32/24-byte aligned Block1 descriptor**. With matching `--bin-base / --bin-donor / --bin-merged`, it additionally checks the paired BIN header/20-byte handles, respects `vdata_num` (models vs textures), and validates each referenced buffer's `offset + size` within that **model handle's declared uncompressed asset size**. Optional `--verify-bin-streams` checks referenced compressed model asset zlib lengths with a 256 MiB per-asset safety bound. These checks do not rewrite bytes or imply in-game success.

**Critical:** The PC disk strides are **32 bytes for VBuffInfo and 24 for IBuffInfo**, even though the PAK header's `vbuff_info_size`/`ibuff_info_size` may be **56/52**. Those header fields must **not** drive the disk-table cursor. See `Vespucci/Level/PakRebuilder.cpp` and `MgGameBuffers.cpp`. The validator does not change a PAK/BIN, run a mission, decompress Block2's full payload, or prove the original geometry is visually correct.

With `--native-tables`, the tool additionally checks the **outer Block1 record-table envelopes** for BufferInfo, Mat1–Mat4, MatExtra, ShapeInfo, HavokShapeInfo, HavokConstraintData/Info, foliage, animation and radiosity metadata. For tables of unknown on-disk format, it uses the PAK header's declared record stride, matching the native writer's `EXTRACT` operation. This catches out-of-range table relocations; it does **not** validate Havok MOPP internals or material pointer semantics.

The merged PAK can pass all registry-cap checks yet still contain an out-of-range VB/IB BIN-asset reference. This is a distinct error from running out of pool capacity. Invalid offsets require fixing the **native asset import/write transaction**, not adding more heap.

## Runtime geometry-budget telemetry (read-only)

Set the following in `proxy.ini` beside the executable on a disposable copy:

```ini
[StaticMapPools]
Monitor=1
GeometryBudget=1
ExpandModels2048=0
ExpandTextures4096=0
ExpandEffects1024=0
```

`GeometryBudget=0` by default. When set to 1, D3D9Proxy records successful/failed VB/IB allocations, bytes requested, pool class (`D3DPOOL_MANAGED`/`DEFAULT`/`SYSTEMMEM`), and the subcounts from the two *verified static-model call sites*. Process total/free virtual memory and current level pointer are sampled once per second into `static_geometry_budget.csv`, **even while no frames are rendered**.

Classification requires the original x86 callsite opcodes `FF 51 68` and `FF 51 6C` at the exact GZF-mapped addresses; unknown executables still get total VB/IB call counts but **no asserted static classification**. The existing D3D9 calls and return values are forwarded unchanged. No COM buffer wrappers or mutation of executable instructions or native asset records.

**These counters are cumulative**. Buffer `Release()` is *not* intercepted, so totals **do not measure live VRAM residency**. To compare pressure, record three clean processes: untouched Helm's Deep, untouched Isengard, and a merged level under the same game settings, then compare successful managed bytes, failures, VA free, and the pool census.

## Shared GFX helper: do NOT patch blindly

The 64-entry GFX registry uses `0x0040B4CB` (the helper contains `PUSH 0x40; POP EBX`). The original EXE has **at least two direct call sites** to that helper: `0x00876E6F` (level GFX vector) and `0x008F9301` (a different code path). Replacing the global `0x40` constant would affect both callers. An independent GFX patch needs verified call-site semantics and matching factory guard/allocation treatment; this PR makes **no GFX capacity change**.

## Remaining limits and validation

The experimental model/texture/effect patches from PRs #5–7 remain opt-in and are **not** proven safe on a loaded merged level. Havok collision shapes, model subdata, occlusion/depth, radiosity, materials, texture keys, PAK/BIN companion assets, and the native editor's transactional save still require independent verification.

The GitHub Actions suite runs synthetic Block1 descriptor and paired PAK/BIN model-reference tests and compiles/links the complete 32-bit D3D9Proxy. Passing offline tests **does not prove** an in-game Helm's Deep + Isengard import.


## Read-only native collision reference validation

Use `--collision-links` on each PC PAK, ideally alongside `--native-tables --model-links` and the matching model BIN. The opt-in check follows `ModelInfo.shape_offset/shape_num` → the 124-byte `ShapeInfo[]` → `ShapeInfo.hk_shape_offset/hk_shape_num` → the 80-byte `HkShapeInfo[]`, then checks the declared Block1 spans for kind-0 shape extra data, kind-5 convex normals/vertices, and kind-6 MOPP bytecode/vertices/triangle indices. This specifically catches misaligned or out-of-bounds references caused by native table relocation during a merge.

The verifier deliberately does **not** decode MOPP bytecode, execute Havok, check collision response, or read runtime pointers. In an unrelocated PAK, collision payload offsets are **Block1-relative**; D3D vertex/index payload offsets are model **BIN-asset-relative**. These are different storage systems. No import/write operation is performed.

Example: `python D3D9Proxy/tools/pak_pool_preflight.py --base helmsdeep.PAK --donor isengard.PAK --merged merged.PAK --deep --geometry --model-links --collision-links --native-tables --json`.


## Collision preservation across a donor → merged PAK (read-only)

The native merge path `Vespucci/Engine/Magellan/MgMergeWriteBack.h` explicitly writes `ModelInfo+224/+228 = 0` for its newly merged model: **that writer produces collisionless models**. This is a verified risk for that path, NOT proof that every level import path strips collision. Even pristine-looking `ShapeInfo` table envelopes will not identify a missing shape when `shape_num=0`.

`--collision-preservation` requires `--base`, `--donor` and `--merged`. For donor model CRC keys not already present in the base, it compares matching merged models by a **reflow-invariant SHA-256** of the native collision shape chain. It masks only known offset fields in `ShapeInfo` and `HkShapeInfo` and includes kind-0 extra data, kind-5 convex normals/vertices, kind-6 MOPP bytecode/verts/indices. It reports `preserved`, `dropped_collision`, `changed_collision`, `missing_unmapped_target`, `unverifiable` and duplicate-key ambiguity. Donor CRC keys missing from the merged file are **informational**, as not every source model is necessarily imported. Pre-existing base keys are skipped unless explicitly mapped, to avoid blaming donor differences for retained base geometry.

For imported models renamed to fresh CRC keys, supply `--collision-key-map mapping.json` with `{"0xSOURCE_CRC":"0xTARGET_CRC"}`. With `--require-donor-collision`, matched models that lost or altered collision, and missing explicitly mapped targets, cause exit code 1. Without that flag this comparison is report-only, although malformed individual PAKs still fail other requested validations. This is a static artifact comparison; Havok runtime, world instances, occlusion, and the actual physics response remain unverified.

Example:

```powershell
python D3D9Proxy/tools/pak_pool_preflight.py `
  --base helmsdeep.PAK --donor isengard.PAK --merged helmsdeep_merged.PAK `
  --collision-preservation --collision-key-map imported_models.json `
  --require-donor-collision --native-tables --geometry --json
```

### Host-world collision regression guard

The same `--collision-preservation` run now audits the **original Helm's Deep** models as well. It compares base-to-merged native collision fingerprints by model CRC and reports `base_collision_preservation`: preserved, dropped, changed, missing-model, ambiguous-CRC, or unverifiable. Use `--require-base-collision` to fail the CLI if existing base collision is lost, altered, or cannot be verified. This is independent of donor model selection and intentionally requires an actual merged PAK; the goal is to preserve the host level while importing only donor static assets.
