#!/usr/bin/env python3
"""Read-only Conquest PC PAK capacity preflight for a native static-map merge.

Helm's Deep + Isengard means ONE merged level, NOT two worlds or two loaders.
Uses the 118-u32 PakHeader from Vespucci/Level/LevelReader.h. Model/texture/
effect/GFX guards are from Magellan's registry builders. Does not read/alter
mission scripts and never writes a PAK or BIN.

Exit codes: 0 = no definite over-cap/malformed tables; 1 = known factory cap
exceeded; 2 = invalid input/PAK structure. Pair projections are warnings,
NOT certain failures, because imports can remove/deduplicate records.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

# Support direct CLI and importlib-driven unittest loading.
if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_geometry_links import inspect_model_links
from native_collision_links import inspect_collision_links
from native_collision_provenance import compare_collision_preservation, compare_base_collision_preservation, load_key_map
from native_model_preservation import compare_static_model_preservation

HEADER_FIELDS = 118
HEADER_BYTES = HEADER_FIELDS * 4
MAX_BLOCK1 = 768 * 1024 * 1024
# Header indices (u32, not byte offsets) match LevelReader.h exactly.
IDX = {
    "constx13": 2,
    "vbuff_info_size": 30, "ibuff_info_size": 31,
    "block1_offset": 7, "block1_size": 8, "block1_size_comp": 9,
    "block2_offset": 11, "block2_size": 12, "block2_size_comp": 13,
    "model_info_num": 44, "vbuff_info_num": 55, "ibuff_info_num": 56,
    "texture_info_num": 57, "effect_info_num": 60, "pfield_info_num": 61,
    "gfx_block_info_num": 62,
    "model_info_offset": 69, "vbuff_info_offset": 80, "ibuff_info_offset": 81,
    "texture_info_offset": 82, "effect_info_offset": 85,
    "pfield_info_offset": 86, "gfx_block_info_offset": 87,
}
# Count, Block1 offset, on-disk stride, initial retail registry hard cap.
# VBuffInfo/IBuffInfo are NOT the header's runtime structure sizes!
TABLES = {
    "models": ("model_info_num", "model_info_offset", 0x100, 0x400),
    "textures": ("texture_info_num", "texture_info_offset", 0x48, 0x800),
    "effects": ("effect_info_num", "effect_info_offset", 0x10, 0x200),
    "gfx_blocks": ("gfx_block_info_num", "gfx_block_info_offset", 0x0C, 0x40),
    "pfields": ("pfield_info_num", "pfield_info_offset", 0x14, 0x1000),
    "vertex_infos": ("vbuff_info_num", "vbuff_info_offset", 0x20, None),
    "index_infos": ("ibuff_info_num", "ibuff_info_offset", 0x18, None),
}



# Remaining native Block1 asset record envelopes used by the level constructor
# and collision/material import. These are NOT inner Havok blob validators.
# Columns: table label, PakHeader record-size index, count index, offset index.
# The file's declared record size is used for these tables, following
# Vespucci/Level/PakRebuilder.cpp's EXTRACT(..., size) contract.
NATIVE_TABLE_ENVELOPES = [
    ("obja",17,42,67), ("obj0",18,43,68),
    ("buffer_infos",20,45,70),
    ("mat1",21,46,71), ("mat2",22,47,72),
    ("mat3",23,48,73), ("mat4",24,49,74),
    ("mat_extra",25,50,75),
    ("shape_infos",27,52,77), ("havok_shape_infos",28,53,78),
    ("havok_constraint_data",29,54,79),
    ("animation_infos",33,58,83), ("havok_constraint_infos",34,59,84),
    ("animation_block_infos",38,63,88),
    ("foliage_infos",39,64,89), ("radiosity_infos",40,65,90),
]

def inspect_native_table_envelopes(
    h: tuple[int, ...], failures: list[str]
) -> dict[str, dict[str, int]]:
    """Read-only outer descriptor-table bounds; not inner asset correctness."""
    size=h[IDX["block1_size"]]
    result: dict[str,dict[str,int]]={}
    for name,stride_idx,count_idx,offset_idx in NATIVE_TABLE_ENVELOPES:
        stride,count,offset=h[stride_idx],h[count_idx],h[offset_idx]
        result[name]={"count":count,"declared_stride":stride,"offset":offset}
        if not count:
            continue
        if not stride:
            failures.append(f"{name}: {count} records with zero declared stride")
            continue
        # Huge count*stride multiplication checked by division, not DWORD math.
        if offset>size or count>(size-offset)//stride:
            failures.append(
                f"{name}: Block1 table OOB (offset={offset}, count={count}, "
                f"declared_stride={stride}, Block1={size})")
    return result

class InvalidPak(ValueError):
    pass


def _checked_region(offset: int, length: int, file_size: int, label: str) -> None:
    if offset < HEADER_BYTES or length <= 0 or offset > file_size or length > file_size - offset:
        raise InvalidPak(f"{label}: region [{offset}, {offset + length}) outside file ({file_size} bytes)")


def _header(path: Path) -> tuple[tuple[int, ...], int]:
    with path.open("rb") as fp:
        raw = fp.read(HEADER_BYTES)
    if len(raw) != HEADER_BYTES:
        raise InvalidPak("truncated PAK: 472-byte PC header absent")
    h = struct.unpack("<118I", raw)
    if h[IDX["constx13"]] != 0x13:
        raise InvalidPak("not a supported PC little-endian Conquest PAK (constx13 != 0x13)")
    file_size = path.stat().st_size
    uncomp = h[IDX["block1_size"]]
    comp = h[IDX["block1_size_comp"]]
    if not uncomp or uncomp > MAX_BLOCK1:
        raise InvalidPak(f"Block1 unpacked size invalid or exceeds {MAX_BLOCK1} safety cap: {uncomp}")
    _checked_region(h[IDX["block1_offset"]], comp or uncomp, file_size, "Block1")
    if h[IDX["block2_size"]]:
        _checked_region(h[IDX["block2_offset"]],
                        h[IDX["block2_size_comp"]] or h[IDX["block2_size"]],
                        file_size, "Block2")
    return h, file_size


def _decode_block1(path: Path, h: tuple[int, ...]) -> bytes:
    expected = h[IDX["block1_size"]]
    length = h[IDX["block1_size_comp"]] or expected
    with path.open("rb") as fp:
        fp.seek(h[IDX["block1_offset"]])
        compressed = fp.read(length)
    if len(compressed) != length:
        raise InvalidPak("Block1 truncated after header validation")
    if not h[IDX["block1_size_comp"]]:
        return compressed
    try:
        inflater = zlib.decompressobj()
        # Cap output to expected+1, even for hostile corrupt input.
        out = inflater.decompress(compressed, expected + 1)
        if (len(out) > expected or not inflater.eof or inflater.unconsumed_tail or
                inflater.unused_data):
            raise InvalidPak("Block1 zlib stream truncated, oversized, or has trailing bytes")
        out += inflater.flush()
    except (zlib.error, MemoryError) as exc:
        raise InvalidPak(f"Block1 zlib inflate failed: {exc}") from exc
    if len(out) != expected:
        raise InvalidPak(f"Block1 decoded size {len(out)} != header {expected}")
    return out


@dataclass
class PakReport:
    path: str
    file_size: int
    block1_size: int
    counts: dict[str, int]
    caps: dict[str, int | None]
    failures: list[str]
    warnings: list[str]
    details: dict[str, dict[str, int]]
    geometry: dict[str, object] | None = None
    native_tables: dict[str, dict[str, int]] | None = None
    model_links: dict[str, object] | None = None
    collision_links: dict[str, object] | None = None



def _geometry_preflight(
    h: tuple[int, ...], block1: bytes, failures: list[str], warnings: list[str]
) -> dict[str, object]:
    """Summarize disk VB/IB sizes; payload validation requires model BIN.

    VBuffInfo/IBuffInfo offset fields are per-model BIN ASSET offsets, NOT
    Block2 offsets. See LevelScene.cpp and MgMergeWriteBack.h.
    """
    block2_bytes = h[IDX["block2_size"]]
    totals: dict[str, dict[str, int]] = {}
    for kind, fmt in (("vertex_infos", "vertex"), ("index_infos", "index")):
        count_key, offset_key, stride, _ = TABLES[kind]
        base = h[IDX[offset_key]]
        count = h[IDX[count_key]]
        disk_bytes = len(block1)
        if count > (disk_bytes - base) // stride if base <= disk_bytes else True:
            # Main preflight already reports this malformed table. Never read it.
            totals[kind] = {"count": count, "unreadable_table": 1}
            continue
        created = 0
        empty = 0
        unverified = 0
        cpu_only = 0
        cpu_dup = 0
        bytes_total = 0
        bytes_managed = 0
        highest_unverified_end = 0
        largest = 0
        unexpected_format = 0
        for i in range(count):
            pos = base + i * stride
            buf_size = struct.unpack_from("<I", block1, pos+4)[0]
            if fmt == "vertex":
                data_off = struct.unpack_from("<I", block1, pos+12)[0]
                flags = struct.unpack_from("<I", block1, pos+16)[0]
                is_cpu = bool(flags & 0x40000)
            else:
                bits = struct.unpack_from("<I", block1, pos+8)[0]
                flags = struct.unpack_from("<I", block1, pos+12)[0]
                data_off = struct.unpack_from("<I", block1, pos+16)[0]
                is_cpu = False
                if bits not in (0x10, 0x20):
                    unexpected_format += 1
                if flags == 1:
                    cpu_dup += buf_size
            if not buf_size:
                empty += 1
                continue
            created += 1
            bytes_total += buf_size
            if is_cpu:
                cpu_only += buf_size
            else:
                bytes_managed += buf_size
            if buf_size > largest:
                largest = buf_size
            # BIN-relative, NOT Block2-relative. No payload checks possible
            # until ModelInfo asset key resolves to an actual BIN handle.
            unverified += 1
            highest_unverified_end = max(highest_unverified_end, data_off + buf_size)
        if unexpected_format:
            warnings.append(f"{kind}: {unexpected_format} unexpected index-format values")
        totals[kind] = {
            "count": count, "nonzero": created, "zero_size": empty,
            "unverified_bin_asset_spans": unverified,
            "bytes_requested": bytes_total,
            "bytes_managed_d3d": bytes_managed,
            "bytes_cpu_only": cpu_only,
            "bytes_kept_cpu_index_copies": cpu_dup,
            "largest_buffer_bytes": largest,
            "highest_unverified_bin_end": highest_unverified_end,
        }
    return {
        "block2_uncompressed_bytes_unrelated_to_model_geometry": block2_bytes,
        "vbuff_info_declared_size_not_disk_stride": h[IDX["vbuff_info_size"]],
        "ibuff_info_declared_size_not_disk_stride": h[IDX["ibuff_info_size"]],
        "disk_strides": {"vertex_infos": 0x20, "index_infos": 0x18},
        "buffer_tables": totals,
        "note": "VB/IB offsets index model BIN assets, NOT PAK Block2. Use --model-links and matching --bin-* paths to validate spans. Cumulative bytes are not live GPU residency.",
    }

def inspect_pak(path: Path, deep: bool = False, game_mask: int | None = None,
                assume_caps: dict[str, int] | None = None,
                inspect_geometry: bool = False,
                inspect_native_tables: bool = False,
                inspect_links: bool = False, bin_path: Path | None = None,
                verify_bin_streams: bool = False,
                inspect_collision: bool = False) -> PakReport:
    h, file_size = _header(path)
    counts: dict[str, int] = {}
    caps: dict[str, int | None] = {}
    failures: list[str] = []
    warnings: list[str] = []
    details: dict[str, dict[str, int]] = {}
    block_size = h[IDX["block1_size"]]
    block1 = _decode_block1(path, h) if deep or inspect_geometry or inspect_links or inspect_collision or bin_path is not None else None
    for name, (count_key, offset_key, stride, stock_cap) in TABLES.items():
        cap = (assume_caps or {}).get(name, stock_cap)
        count = h[IDX[count_key]]
        offset = h[IDX[offset_key]]
        counts[name] = count
        caps[name] = cap
        # NEVER use the runtime *_info_size as on-disk stride. Magellan's
        # merge writer proves some of these differ.
        if count and (offset > block_size or count > (block_size - offset) // stride):
            failures.append(
                f"{name}: Block1 table OOB (offset={offset}, count={count}, stride={stride}, Block1={block_size})")
            continue
        if cap is not None and count > cap:
            failures.append(
                f"{name}: raw info count {count} > {'assumed patched' if cap != stock_cap else 'retail'} factory guard {cap}; registration may be skipped")
        if not count or not deep:
            continue
        assert block1 is not None
        keys: set[int] = set()
        zeros = 0
        mode_masks: set[int] = set()
        eligible_for_mask = 0
        zero_mode_masks = 0
        for n in range(count):
            at = offset + n * stride
            key = struct.unpack_from("<I", block1, at)[0]
            keys.add(key)
            if key == 0:
                zeros += 1
            if name in ("models", "textures", "effects"):
                rec_mask = struct.unpack_from("<I", block1, at + 4)[0]
                mode_masks.add(rec_mask)
                if not rec_mask:
                    zero_mode_masks += 1
                if game_mask is not None and (rec_mask & game_mask):
                    eligible_for_mask += 1
        details[name] = {
            "unique_keys": len(keys), "duplicate_keys": count - len(keys),
            "zero_keys": zeros, "distinct_mode_masks": len(mode_masks),
            "zero_mode_masks": zero_mode_masks,
            **({"eligible_for_mask": eligible_for_mask} if game_mask is not None else {}),
        }
        if zeros:
            warnings.append(f"{name}: {zeros} zero CRC keys; investigate imported asset metadata")
        if zero_mode_masks and name in ("models", "textures", "effects"):
            warnings.append(f"{name}: {zero_mode_masks} records have zero gamemode mask and cannot register")
        if count != len(keys):
            warnings.append(
                f"{name}: {count - len(keys)} duplicate CRC keys; raw factory guard still sees {count} records")
    if counts["pfields"] > 0x1000:
        failures.append("pfields: exceeds known builder guard 4096 (actual retail allocation cap unverified)")
    elif counts["pfields"] > 0x200:
        warnings.append("pfields: exceeds Magellan host stand-in 512; retail capacity remains unverified")
    native = (inspect_native_table_envelopes(h, failures)
              if inspect_native_tables else None)
    geo = (_geometry_preflight(h, block1, failures, warnings)
           if inspect_geometry and block1 is not None else None)
    links = (inspect_model_links(h, block1, failures, warnings, bin_path, verify_bin_streams)
             if (inspect_links or bin_path is not None) and block1 is not None else None)
    collision = (inspect_collision_links(h, block1, failures, warnings)
                 if inspect_collision and block1 is not None else None)
    if inspect_geometry and bin_path is None:
        warnings.append("geometry: BIN not supplied; VB/IB payload spans cannot be checked against PAK Block2")
    return PakReport(
        path=str(path), file_size=file_size, block1_size=block_size,
        counts=counts, caps=caps, failures=failures, warnings=warnings,
        details=details, geometry=geo, native_tables=native,
        model_links=links, collision_links=collision,
    )


def projection(base: PakReport, donor: PakReport) -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    for name, (_, _, _, stock_cap) in TABLES.items():
        cap = base.caps[name]
        total = base.counts[name] + donor.counts[name]
        result.append({
            "registry": name, "helm_count": base.counts[name],
            "isengard_count": donor.counts[name],
            "naive_append_upper_bound": total, "retail_guard": stock_cap,
            "assumed_runtime_guard": cap,
            # This is not a certainty: native merge can dedup or omit records,
            # while prior level registry entries can also increase real demand.
            "risk_if_all_appended": (cap is not None and total > cap),
        })
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, type=Path, help="Helm's Deep PAK")
    parser.add_argument("--donor", type=Path, help="Isengard source PAK (static assets only)")
    parser.add_argument("--merged", type=Path, help="actual merged Helm's Deep PAK, if available")
    parser.add_argument("--deep", action="store_true", help="decompress Block1 and validate CRC keys")
    parser.add_argument("--geometry", action="store_true",
                        help="summarize VB/IB allocation bytes (payloads are in model BIN assets, not Block2)")
    parser.add_argument("--model-links", action="store_true",
                        help="validate ModelInfo->mesh-order->BufferInfo->VB/IB Block1 links")
    parser.add_argument("--bin-base", type=Path, help="Helm's Deep BIN, paired with --base")
    parser.add_argument("--bin-donor", type=Path, help="Isengard BIN, paired with --donor")
    parser.add_argument("--bin-merged", type=Path, help="merged Helm's Deep BIN, paired with --merged")
    parser.add_argument("--verify-bin-streams", action="store_true",
                        help="also verify model BIN zlib streams and inflated lengths")
    parser.add_argument("--static-model-preservation", action="store_true",
                        help="fail if any original host ModelInfo key/LOD/mode disappears or explicitly selected donor static model is missing")
    parser.add_argument("--required-donor-model", action="append", default=[], type=lambda s: int(s,0),
                        help="donor ModelInfo CRC explicitly selected for import (repeat, hex 0x...); requires --static-model-preservation")
    parser.add_argument("--model-key-map", type=Path,
                        help="JSON explicit donor CRC -> renamed merged CRC map; requires --static-model-preservation")
    parser.add_argument("--strict-native-metadata", action="store_true",
                        help="fail on zero CRC keys, duplicate keys or zero GameModeMask in merged (or base if no merged)")
    parser.add_argument("--collision-preservation", action="store_true",
                        help="read-only source donor vs merged PAK collision fingerprint comparison")
    parser.add_argument("--collision-key-map", type=Path,
                        help="optional JSON CRC mapping donor model key to renamed merged model key")
    parser.add_argument("--require-base-collision", action="store_true",
                        help="fail if existing Helm's Deep model collision disappears or differs in merged PAK")
    parser.add_argument("--require-donor-collision", action="store_true",
                        help="fail if matching or explicitly mapped donor collision is dropped, changed, or unverifiable")
    parser.add_argument("--collision-links", action="store_true",
                        help="validate ModelInfo -> ShapeInfo -> Havok children and Block1 MOPP/convex payload spans")
    parser.add_argument("--native-tables", action="store_true",
                        help="check outer Block1 bounds for BufferInfo, materials, Havok and other info tables")
    parser.add_argument("--mask", type=lambda s: int(s, 0), help="optional Helm's Deep gamemode mask; requires --deep")
    parser.add_argument("--json", action="store_true", help="machine-readable report")
    parser.add_argument("--assume-pfields16384", action="store_true",
                        help="what-if: PField registry 16384-entry patch ACTIVE (unverified)")
    parser.add_argument("--assume-gfx1024", action="store_true",
                        help="what-if: GFX Block registry 1024-entry patch ACTIVE (unverified)")
    parser.add_argument("--assume-model8192", action="store_true",
                        help="what-if: 8192 ModelInfo registry patch ACTIVE (unverified)")
    parser.add_argument("--assume-texture16384", action="store_true",
                        help="what-if: 16384 TextureInfo registry patch ACTIVE (unverified)")
    parser.add_argument("--assume-effects4096", action="store_true",
                        help="what-if: 4096 EffectInfo registry patch ACTIVE (unverified)")
    parser.add_argument("--assume-pfields8192", action="store_true",
                        help="what-if: PField registry 8192-entry patch ACTIVE (unverified)")
    parser.add_argument("--assume-gfx256", action="store_true",
                        help="what-if: GFX Block registry 256-entry patch ACTIVE (unverified)")
    parser.add_argument("--assume-gfx512", action="store_true",
                        help="what-if: GFX Block registry 512-entry patch ACTIVE (unverified)")
    parser.add_argument("--assume-model4096", action="store_true",
                        help="what-if: 4096 ModelInfo registry patch ACTIVE (unverified)")
    parser.add_argument("--assume-texture8192", action="store_true",
                        help="what-if: 8192 TextureInfo registry patch ACTIVE (unverified)")
    parser.add_argument("--assume-effects2048", action="store_true",
                        help="what-if: 2048 EffectInfo registry patch ACTIVE (unverified)")
    parser.add_argument("--assume-model2048", action="store_true",
                        help="what-if: runtime MODEL registry patch 2048 applied (NOT verified)")
    parser.add_argument("--assume-texture4096", action="store_true",
                        help="what-if: runtime TEXTURE registry patch 4096 applied (NOT verified)")
    parser.add_argument("--assume-effects1024", action="store_true",
                        help="what-if: runtime EFFECT registry patch 1024 applied (NOT verified)")
    args = parser.parse_args(argv)
    if args.static_model_preservation and not (args.donor and args.merged):
        parser.error("--static-model-preservation requires --donor and --merged")
    if args.required_donor_model and not args.static_model_preservation:
        parser.error("--required-donor-model requires --static-model-preservation")
    if args.model_key_map and not args.static_model_preservation:
        parser.error("--model-key-map requires --static-model-preservation")
    if any(key<=0 or key>0xFFFFFFFF for key in args.required_donor_model):
        parser.error("--required-donor-model CRC must be nonzero uint32")
    if args.collision_preservation and not (args.donor and args.merged):
        parser.error("--collision-preservation requires both --donor and --merged")
    if args.collision_key_map and not args.collision_preservation:
        parser.error("--collision-key-map requires --collision-preservation")
    if args.require_donor_collision and not args.collision_preservation:
        parser.error("--require-donor-collision requires --collision-preservation")
    if args.require_base_collision and not args.collision_preservation:
        parser.error("--require-base-collision requires --collision-preservation")
    if args.bin_donor and not args.donor:
        parser.error("--bin-donor requires --donor")
    if args.bin_merged and not args.merged:
        parser.error("--bin-merged requires --merged")
    if args.verify_bin_streams and (not args.bin_base or
            (args.donor and not args.bin_donor) or (args.merged and not args.bin_merged)):
        parser.error("--verify-bin-streams requires BIN for each supplied PAK")
    if args.mask is not None and not (0 <= args.mask <= 0xFFFFFFFF):
        parser.error("--mask must be a 32-bit unsigned value")
    if args.mask is not None and not args.deep:
        parser.error("--mask requires --deep")
    assumed_caps: dict[str, int] = {}
    if args.assume_model2048:
        assumed_caps["models"] = 0x800
    if args.assume_texture4096:
        assumed_caps["textures"] = 0x1000
    if args.assume_effects1024:
        assumed_caps["effects"] = 0x400
    # Match runtime high-tier precedence; this is still only an assumption.
    if args.assume_model4096:
        assumed_caps["models"] = 0x1000
    if args.assume_texture8192:
        assumed_caps["textures"] = 0x2000
    if args.assume_effects2048:
        assumed_caps["effects"] = 0x800
    if args.assume_gfx256:
        assumed_caps["gfx_blocks"] = 0x100
    if args.assume_gfx512:
        assumed_caps["gfx_blocks"] = 0x200
    if args.assume_pfields8192:
        assumed_caps["pfields"] = 0x2000
    # Ultra-tier options win over all lower settings, just like the DLL.
    if args.assume_model8192:
        assumed_caps["models"] = 0x2000
    if args.assume_texture16384:
        assumed_caps["textures"] = 0x4000
    if args.assume_effects4096:
        assumed_caps["effects"] = 0x1000
    if args.assume_gfx1024:
        assumed_caps["gfx_blocks"] = 0x400
    if args.assume_pfields16384:
        assumed_caps["pfields"] = 0x4000
    try:
        def check_one(pak: Path, bin_file: Path | None) -> PakReport:
            return inspect_pak(pak, args.deep or args.collision_preservation or
                               args.static_model_preservation or args.strict_native_metadata,
                               args.mask, assumed_caps,
                               args.geometry, args.native_tables, args.model_links,
                               bin_file, args.verify_bin_streams,
                               args.collision_links or args.collision_preservation)
        inputs: dict[str, PakReport] = {"base": check_one(args.base, args.bin_base)}
        if args.donor:
            inputs["donor"] = check_one(args.donor, args.bin_donor)
        if args.merged:
            inputs["merged"] = check_one(args.merged, args.bin_merged)
    except (InvalidPak, OSError) as exc:
        print(f"PAK PREFLIGHT INVALID: {exc}", file=sys.stderr)
        return 2
    # The old --deep checks report metadata problems as WARN. This explicit
    # opt-in turns just the actual merged target's registration metadata into
    # a hard failure, catching zeroed CRC/ModeMask and duplicate keys before
    # the engine can silently omit invisible models.
    if args.strict_native_metadata:
        target = inputs["merged"] if "merged" in inputs else inputs["base"]
        for name in ("models","textures","effects"):
            info=target.details.get(name,{})
            for field in ("zero_keys","duplicate_keys","zero_mode_masks"):
                count=info.get(field,0)
                if count:
                    target.failures.append(
                        f"STRICT NATIVE METADATA {name}: {count} {field}; "
                        "target ModelInfo/registry may not register reliably"
                    )
    model_preservation: dict[str, object] | None = None
    preservation: dict[str, object] | None = None
    base_preservation: dict[str, object] | None = None
    if args.static_model_preservation:
        try:
            model_key_map=load_key_map(args.model_key_map) if args.model_key_map else None
            def model_b1(path: Path) -> tuple[tuple[int, ...], bytes]:
                h,_=_header(path)
                return h,_decode_block1(path,h)
            host_header,host_blob=model_b1(args.base)
            donor_header,donor_blob=model_b1(args.donor)
            merged_header,merged_blob=model_b1(args.merged)
            model_preservation=compare_static_model_preservation(
                host_header,host_blob,donor_header,donor_blob,
                merged_header,merged_blob,set(args.required_donor_model),model_key_map
            )
        except (InvalidPak,OSError,ValueError) as exc:
            print(f"STATIC MODEL PRESERVATION INVALID: {exc}",file=sys.stderr)
            return 2
    if args.collision_preservation:
        try:
            key_map = load_key_map(args.collision_key_map) if args.collision_key_map else None
            def load_b1(path: Path) -> tuple[tuple[int, ...], bytes]:
                h, _ = _header(path)
                return h, _decode_block1(path, h)
            # Bound peak RAM: each input is a read-only decompressed PAK Block1.
            bh, bb = load_b1(args.base)
            dh, db = load_b1(args.donor)
            mh, mb = load_b1(args.merged)
            preservation = compare_collision_preservation(dh, db, mh, mb, bh, bb, key_map)
            base_preservation = compare_base_collision_preservation(bh, bb, mh, mb)
        except (InvalidPak, OSError, ValueError) as exc:
            print(f"COLLISION PRESERVATION INVALID: {exc}", file=sys.stderr)
            return 2
    payload: dict[str, object] = {
        "scope": "ONE native Helm's Deep level containing static Isengard assets; no mission scripts",
        "files": {name: vars(data) for name, data in inputs.items()},
        "projection": projection(inputs["base"], inputs["donor"]) if "donor" in inputs else [],
        "static_model_preservation": model_preservation,
        "collision_preservation": preservation,
        "base_collision_preservation": base_preservation,
        "assumed_patch_caps_not_verified": assumed_caps,
        "notes": [
            "Header counts are input records, NOT live scene instances or GPU buffers.",
            "VB/IB payload offsets are relative to the MODEL BIN asset, never PAK Block2; provide matching --bin-* paths to prove spans.",
            "Native table envelopes do not validate internal Havok pointers, collision logic or renderer references.",
            "--collision-links checks unrelocated Block1 shape/MOPP spans, not Havok bytecode semantics or in-game collision.",
            "Collision footprint counts ModelInfo-reachable ON-DISK Block1 bytes only; no Havok runtime heap limit or actual loaded shape count is inferred.",
            "--collision-preservation compares matched model CRCs with normalized Block1 data; unmatched donor keys are not automatically expected imports.",
            "--static-model-preservation requires ALL host ModelInfo keys and only EXPLICIT donor CRCs; presence is NOT scene placement or correct BIN contents.",
            "--strict-native-metadata is opt-in and treats zero/duplicate CRC and zero GameModeMask on the target as hard failures.",
            "Conservative paired sums are not a substitute for inspecting the actual merged PAK.",
            "A low runtime registry count does NOT prove success: factory may return before insertion.",
            "Mask-eligible models are separate from RAW ModelInfo counts tested by the early factory guard.",
            "Capacity cannot be increased by patching a CMP without reallocating the associated vectors.",
            "Assumed expanded caps are hypothetical: validate ACTIVE logs plus live runtime census before relying on them.",
        ],
    }
    definite_failure = any(report.failures for report in inputs.values())
    if args.json:
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    else:
        print("Conquest native STATIC MAP pool preflight (read-only)")
        if assumed_caps:
            print("WARNING: ASSUMED patch capacities, NOT proven active in game:", assumed_caps)
        for role, report in inputs.items():
            print(f"\n[{role}] {report.path} | PAK={report.file_size:,} B | Block1={report.block1_size:,} B")
            for name, count in report.counts.items():
                cap = report.caps[name]
                suffix = "unknown cap" if cap is None else f"cap {cap}"
                print(f"  {name:14s} {count:7d} / {suffix}")
            if report.geometry:
                stats = report.geometry["buffer_tables"]
                vb = stats["vertex_infos"]
                ib = stats["index_infos"]
                print("  GEOMETRY VB/IB descriptor bytes (payload source is model BIN asset)")
                print(f"  VB {vb.get('bytes_requested', 0):,} B, IB {ib.get('bytes_requested', 0):,} B requested")
                print(f"  UNVERIFIED DESCRIPTORS VB={vb.get('unverified_bin_asset_spans', 0)} IB={ib.get('unverified_bin_asset_spans', 0)}")
            if report.model_links:
                stats = report.model_links
                print(f"  MODEL LINKS: {stats['distinct_buffer_infos']} BufferInfos; {stats['checked_asset_spans']} BIN geometry spans checked")
            if report.collision_links:
                c = report.collision_links
                print(f"  COLLISION LINKS: models={c['models_with_shapes']} ShapeInfo={c['shape_count']} HkShapeInfo={c['havok_child_count']} checked MOPP spans={c['mopp_spans']}")
                print(f"  COLLISION FOOTPRINT: reachable ShapeInfo={c['reachable_shape_records']} / {c['shape_count']}; HkShapeInfo={c['reachable_havok_children']} / {c['havok_child_count']}")
                print(f"  COLLISION BLOCK1 DISK BYTES: descriptors={c['reachable_descriptor_bytes']:,}; payload raw={c['reachable_payload_raw_bytes']:,}; unique={c['reachable_payload_unique_bytes']:,}; complete={c['payload_footprint_valid']}")
                print(f"  MOPP reachable children={c['reachable_mopp_children']}; orphan shapes={c['unreferenced_shape_records']}; orphan children={c['unreferenced_havok_children']}")
            if report.native_tables:
                print(f"  NATIVE INFO TABLE ENVELOPES: {len(report.native_tables)} inspected")
            for error in report.failures:
                print("  FAIL: " + error)
            for warning in report.warnings:
                print("  WARN: " + warning)
        if model_preservation is not None:
            print("\\n[STATIC MODEL IDENTITY PRESERVATION / read-only]")
            for label in ("base_models","base_preserved","requested_donor_models",
                          "donor_present","strict_failures"):
                print(f"  {label}: {model_preservation[label]}")
            for sample in model_preservation["samples"]:
                print(f"  {sample['status']}: {sample['source_key']} -> {sample['target_key']} ({sample['detail']})")
        if base_preservation is not None:
            host = base_preservation
            print("\n[BASE COLLISION PRESERVATION / read-only]")
            for label in ("base_models_with_collision", "preserved", "dropped_collision",
                          "changed_collision", "missing_host_model", "ambiguous_duplicate_key",
                          "unverifiable"):
                print(f"  {label}: {host[label]}")
            for example in host["samples"]:
                print(f"  {example['status']}: {example['model_key']} ({example['detail']})")
        if preservation is not None:
            audit = preservation
            print("\n[DONOR COLLISION PRESERVATION / read-only]")
            for label in ("donor_models_with_collision", "preserved", "dropped_collision",
                          "changed_collision", "missing_unmapped_target", "missing_mapped_target",
                          "skipped_preexisting_base_key", "unverifiable", "ambiguous_duplicate_key"):
                print(f"  {label}: {audit[label]}")
            for example in audit["samples"]:
                print(f"  {example['status']}: {example['source_key']} -> {example['target_key']} ({example['detail']})")
        if "donor" in inputs:
            print("\n[naive concatenation estimate, NOT merged file]")
            for row in payload["projection"]:
                if row["risk_if_all_appended"]:
                    print(f"  RISK {row['registry']}: {row['naive_append_upper_bound']} > {row['assumed_runtime_guard']} (assumed guard)")
        if "merged" not in inputs:
            print("\nActual merged PAK not supplied: capacity outcome is not confirmed.")
        print("No file was modified.")
    if model_preservation is not None and model_preservation["strict_failures"]:
        definite_failure = True
    if args.require_donor_collision and preservation is not None and preservation["strict_failures"]:
        definite_failure = True
    if args.require_base_collision and base_preservation is not None and base_preservation["strict_failures"]:
        definite_failure = True
    return 1 if definite_failure else 0


if __name__ == "__main__":
    raise SystemExit(main())
