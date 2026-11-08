# Strict native static-model preservation before a merged PAK boots

This is a **read-only** validation gate for one Helm's Deep level containing
selected Isengard *static models*. No NPC, mission script, second-world
loader, Havok mutation, engine patch, or PAK/BIN writing is performed.

## The problem this catches

The old `--deep` preflight warned about zero CRC keys, duplicate keys and
zero GameModeMask entries, but did not reject them. A merged PAK could appear
to have enough model/texture/effect registry capacity while models are
invisible or cannot register. Collision-preservation checks only compare
models with collision: losing a **collisionless visible model** was not a
hard failure.

The new checks are opt-in so existing diagnostics keep their behavior.

## Strict source and explicit import requirements

For all host Helm's Deep models and the *explicitly selected* imported donor
models, this scans native 256-byte ModelInfo rows from Block1.

- Every host ModelInfo CRC must remain in the merged target, even if it
  has no collision. A missing key or ambiguous duplicate is an error.
- For host models, original GameModeMask bits cannot be removed, and a model
  with LOD mesh windows cannot become completely meshless.
- For selected Isengard donor CRCs, the specified target CRC must exist
  **uniquely** in merged PAK; a visible donor model must not lose all LOD
  mesh windows, must have nonzero GameModeMask and a nonzero model BIN asset
  key if it has render meshes.
- Donor models **not explicitly selected** are not presumed imported. Import
  means selected static assets, not the donor's whole mission.
- An explicitly selected donor CRC that maps to a key **already present in
  the host** is rejected as unverifiable: the host's existing model cannot
  prove that a separate donor model was imported. Use an actual distinct
  imported destination CRC where appropriate.
- The optional key-map JSON is `{"0xDONOR_CRC": "0xMERGED_CRC"}`
  for models intentionally renamed when imported.
- These are identity, mask and mesh-presence assertions, **not** evidence
  the scene instance exists, its transform is correct, GPU geometry is
  populated, or that runtime model registration succeeded.

Run in your local D3D9Proxy checkout:

```powershell
python tools/pak_pool_preflight.py `
  --base "HelmsDeep.pak" --donor "Isengard.pak" `
  --merged "HelmsDeep_Merged.pak" `
  --static-model-preservation `
  --required-donor-model 0xAABBCCDD `
  --strict-native-metadata --native-tables --deep `
  --model-links --collision-links --json
```

Replace `0xAABBCCDD` with a **real selected Isengard ModelInfo CRC**
from your actual files. Repeat `--required-donor-model` for each
requested asset. When renaming, add `--model-key-map key-map.json`.
To validate model geometry payload references, provide matching
`--bin-base`, `--bin-donor`, and `--bin-merged`, together with
`--verify-bin-streams`. If collision must survive too, add
`--collision-preservation --require-base-collision
--require-donor-collision` (plus its optional collision CRC map).

The `--strict-native-metadata` switch turns **zero CRC, duplicate CRC
and zero GameModeMask** counts into failures for ModelInfo/TextureInfo/
EffectInfo on the **merged target**, or on `--base` when `--merged`
is absent. In stock `--deep` mode they remain warnings for compatibility.
Any malformed or zeroed imported record is rejected before you give it to
the retail game. The tool reports JSON and returns exit code 1 for failures
and exit code 2 for bad arguments or malformed input.

## Limits

The check cannot prove scene placement, selected instances, textures,
materials, shader/FX, Havok allocation/free, or 32-bit VA availability.
CRC identity is not a cryptographic proof of equal geometry bytes. The
existing `--model-links` and matching BIN stream validation cover the
separate pointer and stream topology; collision-preservation verifies
readable MOPP/ShapeInfo payloads only for relevant models.

Do not interpret a passing preflight as a valid retail merged map. Run a
disposable game copy after offline checks and log runtime failures.
