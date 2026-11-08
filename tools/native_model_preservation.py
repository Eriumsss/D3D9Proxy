#!/usr/bin/env python3
"""Read-only ModelInfo identity preservation for a single native static-map PAK.

This is a conservative on-disk identity/visibility gate. It does NOT prove that
scene instances were transplanted, GPU buffers are usable, Havok works, or the
retail engine registered the model. Run --model-links with matching BINs too.
"""
from __future__ import annotations

import struct

MODEL_STRIDE = 256
MAX_SAMPLES = 25


def snapshot_models(header: tuple[int, ...], block1: bytes) -> dict[int, list[dict[str, object]]]:
    count, start = header[44], header[69]
    if not count:
        return {}
    if start > len(block1) or count > (len(block1) - start) // MODEL_STRIDE:
        raise ValueError("ModelInfo table outside decoded Block1")
    result: dict[int, list[dict[str, object]]] = {}
    for index in range(count):
        at = start + index * MODEL_STRIDE
        key, mask = struct.unpack_from("<2I", block1, at)
        asset_key = struct.unpack_from("<I", block1, at + 208)[0]
        mesh_ranges = [struct.unpack_from("<5I", block1, at+52+20*lod)
                       for lod in range(4)]
        # Deliberately classify presence, not identical geometry.
        has_mesh = any(fields[4] > fields[0] for fields in mesh_ranges)
        result.setdefault(key, []).append({
            "index": index, "key": key, "gamemode_mask": mask,
            "has_mesh": has_mesh, "asset_key": asset_key,
        })
    return result


def compare_static_model_preservation(
    base_h: tuple[int, ...], base_block: bytes,
    donor_h: tuple[int, ...], donor_block: bytes,
    merged_h: tuple[int, ...], merged_block: bytes,
    required_donor: set[int] | None = None,
    key_map: dict[int, int] | None = None,
) -> dict[str, object]:
    """Require all original host ModelInfo CRCs and EXPLICIT donor imports.

    Absence of an unlisted donor CRC is not a failure: only selected donor
    statics were requested for this native level, not its whole mission.
    """
    base = snapshot_models(base_h, base_block)
    donor = snapshot_models(donor_h, donor_block)
    merged = snapshot_models(merged_h, merged_block)
    required_donor = required_donor or set()
    key_map = key_map or {}

    out: dict[str, object] = {
        "base_models": sum(map(len, base.values())),
        "requested_donor_models": len(required_donor),
        "base_preserved": 0, "donor_present": 0,
        "missing_base": 0, "missing_donor": 0,
        "missing_donor_source": 0, "ambiguous_key": 0,
        "lost_base_mesh": 0, "lost_donor_mesh": 0,
        "dropped_base_mask_bits": 0, "zero_donor_mask": 0,
        "zero_source_key": 0, "zero_merged_asset_key": 0,
        "issues": [], "samples": [],
        "note": "ModelInfo CRC/LOD presence/host mode bits only; companion BIN and object placements are separate",
    }
    def problem(status: str, source: int, target: int, detail: str) -> None:
        out[status] += 1
        out["issues"].append(f"{status}: 0x{source:08X} -> 0x{target:08X}: {detail}")
        if len(out["samples"]) < MAX_SAMPLES:
            out["samples"].append({
                "status":status,"source_key":f"0x{source:08X}",
                "target_key":f"0x{target:08X}","detail":detail,
            })

    def unique(table: dict[int, list[dict[str, object]]], key: int):
        rows = table.get(key, [])
        return rows[0] if len(rows)==1 else None

    for key, rows in base.items():
        if key == 0:
            problem("zero_source_key",key,key,"base ModelInfo identity cannot be proven for zero CRC")
            continue
        if len(rows)!=1 or len(merged.get(key,[]))>1:
            problem("ambiguous_key",key,key,"base/merged CRC duplicates prevent unique host comparison")
            continue
        target = unique(merged,key)
        if target is None:
            problem("missing_base",key,key,"original Helm's Deep model absent from merged PAK")
            continue
        source = rows[0]
        rejected = False
        if source["has_mesh"] and not target["has_mesh"]:
            problem("lost_base_mesh",key,key,"host model lost all recorded LOD mesh windows")
            rejected = True
        if int(target["gamemode_mask"]) & int(source["gamemode_mask"]) != int(source["gamemode_mask"]):
            problem("dropped_base_mask_bits",key,key,"merged ModelInfo removes original host gamemode bits")
            rejected = True
        if target["has_mesh"] and not target["asset_key"]:
            problem("zero_merged_asset_key",key,key,"rendered host ModelInfo has zero BIN asset key")
            rejected = True
        if not rejected:
            out["base_preserved"] += 1

    for key in sorted(required_donor):
        target_key = key_map.get(key,key)
        if key == 0 or target_key == 0:
            problem("zero_source_key",key,target_key,"selected donor/destination CRC must be nonzero")
            continue
        sources = donor.get(key,[])
        targets = merged.get(target_key,[])
        if not sources:
            problem("missing_donor_source",key,target_key,"selected donor CRC absent from donor PAK")
            continue
        if len(sources)!=1 or len(targets)>1:
            problem("ambiguous_key",key,target_key,"donor/merged CRC duplicates prevent unique import check")
            continue
        if not targets:
            problem("missing_donor",key,target_key,"selected static donor ModelInfo absent from merged PAK")
            continue
        source, target = sources[0], targets[0]
        rejected = False
        if source["has_mesh"] and not target["has_mesh"]:
            problem("lost_donor_mesh",key,target_key,"imported donor model lost its LOD mesh windows")
            rejected = True
        if not target["gamemode_mask"]:
            problem("zero_donor_mask",key,target_key,"imported donor has zero GameModeMask; cannot register")
            rejected = True
        if target["has_mesh"] and not target["asset_key"]:
            problem("zero_merged_asset_key",key,target_key,"imported rendered ModelInfo has zero BIN asset key")
            rejected = True
        if not rejected:
            out["donor_present"] += 1
    out["strict_failures"] = len(out["issues"])
    return out
