# Native collision footprint: model-reachable Block1 accounting

The next input-side validation step for a **single Helm's Deep level containing
static Isengard assets**, without donor mission scripts or a second-world loader.

This change measures exactly what the existing `--collision-links` validator
can establish from on-disk PAK data. It **does not measure Havok's runtime
allocator, physics-world bodies, memory residency or engine pool capacity.**
No executable patch, DLL alteration, imported assets or PAK write occurs.

## Command

```shell
python tools/pak_pool_preflight.py \
  --base "HelmsDeep.pak" \
  --donor "Isengard.pak" \
  --merged "HelmsDeep_Merged.pak" \
  --collision-links --collision-preservation \
  --require-base-collision --require-donor-collision \
  --native-tables --deep --json
```

The `--collision-links` branch populates each `files.*.collision_links`
object with:

- `reachable_shape_records`: distinct ShapeInfo records reached through
  ModelInfo+224 (offset/count).
- `reachable_havok_children`: distinct HkShapeInfo records reached from those
  shapes at ShapeInfo+108/+112.
- `unreferenced_shape_records` and `unreferenced_havok_children`: rows in
  native descriptor tables that are **not** reached through the model graph.
  They may be unused on disk; do not assume they're loaded.
- `reachable_descriptor_bytes`: distinct reached ShapeInfo records ×124 plus
  distinct reached HkShapeInfo records ×80.
- `reachable_payload_bytes_by_kind`: on-disk size of referenced extra blobs,
  convex normals/vertices, MOPP bytecode/vertices/triangle indices.
- `reachable_payload_raw_bytes`: sum of those referenced spans.
- `reachable_payload_unique_bytes`: the **union of Block1 byte intervals**
  so repeated/shared spans are counted once, even if multiple descriptor
  fields point to overlapping data.
- `payload_footprint_valid`: false if an input link/payload was malformed
  or the *referenced* shape/child kind was unknown. Treat measurements as
  incomplete if false.

The model/shape/child graph uses the **unrelocated disk offsets** declared in
the native PC PAK. A shape offset with count zero is ignored; a live
count/offset must fall inside the declared table at its proper stride.
Payload sizes are counted only for valid in-Block1 intervals. A bad MOPP
payload range is still a preflight failure and now also makes the footprint
incomplete.

## How to interpret three files

Compare the base, donor and actual merged **PAK files**, rather than adding
assumed model counts and calling that merged world complete.

The merged file should retain the base's referenced collision and the
selected donor's imported collision. The existing
`--collision-preservation` digest check (with
`--require-base-collision` and `--require-donor-collision`) is separate:
a high descriptor count does not prove correct collision topology, bytecode,
or preservation. An absent *unmapped* donor key is only informational; supply
`--collision-key-map` for intentional renames and strict imported-asset
requirements.

Important: the footprint is not the amount of Havok allocation performed at
runtime. Actual hkpShape allocation, MOPP decompression, object ownership,
deallocation, level reload and 32-bit VA fragmentation must be measured
inside the game. **Do not use these byte counts as a guessed hard limit
or raise native Havok pool constants based on them.**

Synthetic fixtures cover valid reachability, orphan records, repeated child
links, overlapping payload spans and damaged MOPP data. They do not reproduce
the game's physics or verify a real merged PAK.
