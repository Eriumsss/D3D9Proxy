#!/usr/bin/env python3
"""Read-only PC Conquest ModelInfo -> BufferInfo -> VBuff/IBuff -> BIN checks.

Disk layouts: Vespucci/Level/LevelReader.h, LevelScene.cpp and
Vespucci/Engine/Magellan/MgMergeWriteBack.h. Not a Havok/MOPP validator.
"""
from __future__ import annotations

import struct
import zlib
from pathlib import Path

BIN_HEADER_SIZE = 172
BIN_HANDLE_SIZE = 20
MODEL_STRIDE = 256
VB_STRIDE = 32
IB_STRIDE = 24
MAX_ASSET = 256 * 1024 * 1024


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _valid_span(offset: int, size: int, limit: int) -> bool:
    return offset <= limit and size <= limit - offset


def read_model_bin(bin_path: Path, failures: list[str], warnings: list[str]) -> tuple[dict[int, tuple[int, int, int, int]], dict[str, int]]:
    """Parse model-section BIN handles; no payloads decompressed by default.

    Matching LevelReader::ParseBin, the last model-section duplicate wins.
    Texture-section handles may share CRC keys and MUST NOT shadow models.
    """
    file_size = bin_path.stat().st_size
    with bin_path.open("rb") as fp:
        header = fp.read(BIN_HEADER_SIZE)
        if len(header) != BIN_HEADER_SIZE or _u32(header, 0) != 6:
            failures.append("BIN: missing PC little-endian 172-byte header/magic 6")
            return {}, {"file_size": file_size, "model_handles": 0}
        count = _u32(header, 20)
        base = _u32(header, 24)
        model_count = _u32(header, 32)
        if model_count > count or not _valid_span(base, count * BIN_HANDLE_SIZE, file_size):
            failures.append(f"BIN: handle table OOB or vdata_num > asset_handle_num (offset={base}, handles={count}, models={model_count}, size={file_size})")
            return {}, {"file_size": file_size, "model_handles": 0}
        fp.seek(base)
        rows = fp.read(count * BIN_HANDLE_SIZE)
    handles: dict[int, tuple[int, int, int, int]] = {}
    duplicates = 0
    corrupt = 0
    for i in range(count):
        key, offset, size, packed, kind = struct.unpack_from("<5I", rows, i * BIN_HANDLE_SIZE)
        stored = packed if packed else size
        if not _valid_span(offset, stored, file_size):
            corrupt += 1
            if corrupt <= 8:
                failures.append(f"BIN handle[{i}]: stored asset slice OOB (offset={offset}, size={stored}, BIN={file_size})")
        if i < model_count:
            if key in handles:
                duplicates += 1
            handles[key] = (offset, size, packed, kind)
    if corrupt > 8:
        failures.append(f"BIN: {corrupt} malformed handle file spans; first 8 shown")
    if duplicates:
        warnings.append(f"BIN: {duplicates} duplicate MODEL handle keys; last wins, matching LevelReader")
    return handles, {"file_size": file_size, "total_handles": count,
                     "model_handles": model_count, "model_key_duplicates": duplicates}


def _verify_asset_stream(path: Path, entry: tuple[int, int, int, int]) -> str | None:
    """Validate optional model zlib stream in bounded memory, as reader does."""
    offset, size, packed, _kind = entry
    if size > MAX_ASSET or packed > MAX_ASSET:
        return f"asset exceeds {MAX_ASSET} byte safety cap"
    if not packed:
        return None
    with path.open("rb") as fp:
        fp.seek(offset)
        compressed = fp.read(packed)
    if len(compressed) != packed:
        return "truncated stored bytes"
    try:
        dec = zlib.decompressobj()
        data = dec.decompress(compressed, size + 1)
        if len(data) > size or not dec.eof or dec.unconsumed_tail or dec.unused_data:
            return "truncated/oversized/trailing compressed stream"
        data += dec.flush()
    except (zlib.error, MemoryError) as exc:
        return f"zlib decompression failed: {exc}"
    if len(data) != size:
        return f"inflated length {len(data)} != handle size {size}"
    return None


def inspect_model_links(
    h: tuple[int, ...], block1: bytes, failures: list[str], warnings: list[str],
    bin_path: Path | None = None, verify_streams: bool = False,
) -> dict[str, object]:
    """Prove Block1 info-ref topology and, when BIN supplied, asset spans.

    All model reference offsets are Block1 absolute BYTE offsets, not array
    indices. The mesh-order entries are model-RELATIVE BufferInfo indices,
    with 2 lane bits stripped. Vertex/index payload offsets are relative to
    the model's BIN *inflated* asset, NEVER PAK Block2.
    """
    model_count, model_base = h[44], h[69]
    bi_count, bi_base, bi_stride = h[45], h[70], h[20]
    vb_count, vb_base = h[55], h[80]
    ib_count, ib_base = h[56], h[81]
    size = len(block1)
    report: dict[str, object] = {
        "models": model_count, "models_with_meshes": 0,
        "mesh_slots": 0, "distinct_buffer_infos": 0, "checked_vb_refs": 0,
        "checked_ib_refs": 0, "checked_asset_spans": 0,
        "bin_asset_validation": "not_checked" if bin_path is None else "declared_inflated_size",
    }
    if not _valid_span(model_base, model_count * MODEL_STRIDE, size):
        failures.append("model_links: ModelInfo table exceeds Block1")
        return report
    if bi_count and (bi_stride < 268 or not _valid_span(bi_base, bi_count * bi_stride, size)):
        failures.append(f"model_links: BufferInfo table stride/range invalid (stride={bi_stride})")
        return report
    if not _valid_span(vb_base, vb_count * VB_STRIDE, size) or not _valid_span(ib_base, ib_count * IB_STRIDE, size):
        failures.append("model_links: VBuffInfo/IBuffInfo table exceeds Block1")
        return report
    if verify_streams and bin_path is None:
        failures.append("model_links: --verify-bin-streams requires a matching BIN")
        return report
    handles: dict[int, tuple[int, int, int, int]] = {}
    if bin_path is not None:
        handles, bin_info = read_model_bin(bin_path, failures, warnings)
        report["bin"] = bin_info
    seen_bi: set[int] = set()
    seen_assets: set[int] = set()
    stream_checked: set[int] = set()
    for mi in range(model_count):
        at = model_base + mi * MODEL_STRIDE
        key = _u32(block1, at)
        bi_start = _u32(block1, at + 12)
        mesh_order = _u32(block1, at + 48)
        bi_num = _u32(block1, at + 132)
        asset_key = _u32(block1, at + 208)
        slots: set[int] = set()
        bad_lods = False
        for lod in range(4):
            fields = struct.unpack_from("<5I", block1, at + 52 + lod * 20)
            if any(fields[j] > fields[j + 1] for j in range(4)):
                failures.append(f"ModelInfo[{mi}] key=0x{key:08X}: LOD{lod} non-monotonic mesh window {fields}")
                bad_lods = True
                continue
            if fields[4] > fields[0]:
                if fields[4] - fields[0] > size // 4 or not _valid_span(mesh_order, fields[4] * 4, size):
                    failures.append(f"ModelInfo[{mi}] key=0x{key:08X}: LOD{lod} mesh-order slice OOB")
                    bad_lods = True
                    continue
                slots.update(range(fields[0], fields[4]))
        if bad_lods and not slots:
            continue
        if not slots:
            continue
        report["models_with_meshes"] += 1
        report["mesh_slots"] += len(slots)
        if bi_num == 0:
            failures.append(f"ModelInfo[{mi}] key=0x{key:08X}: nonempty LOD with zero BufferInfo count")
            continue
        if bi_stride < 268 or bi_count == 0:
            failures.append(f"ModelInfo[{mi}] key=0x{key:08X}: nonempty LOD requires a valid BufferInfo table")
            continue
        if bi_start < bi_base or (bi_start - bi_base) % bi_stride != 0:
            failures.append(f"ModelInfo[{mi}] key=0x{key:08X}: BufferInfo base not aligned within table ({bi_start})")
            continue
        local_base = (bi_start - bi_base) // bi_stride
        if local_base > bi_count or bi_num > bi_count - local_base:
            failures.append(f"ModelInfo[{mi}] key=0x{key:08X}: BufferInfo slice outside declared table (start={local_base}, count={bi_num})")
            continue
        asset = handles.get(asset_key) if bin_path is not None else None
        if bin_path is not None and asset is None:
            failures.append(f"ModelInfo[{mi}] key=0x{key:08X}: missing model BIN handle key=0x{asset_key:08X}")
        if asset is not None and verify_streams and asset_key not in stream_checked:
            stream_checked.add(asset_key)
            problem = _verify_asset_stream(bin_path, asset)
            if problem:
                failures.append(f"ModelInfo[{mi}]: model BIN asset 0x{asset_key:08X}: {problem}")
        seen_assets.add(asset_key)
        model_bis = set()
        for slot in sorted(slots):
            idx = _u32(block1, mesh_order + slot * 4) & 0x3FFFFFFF
            if idx >= bi_num:
                failures.append(f"ModelInfo[{mi}] slot[{slot}]: BufferInfo relative index {idx} >= {bi_num}")
                continue
            model_bis.add(local_base + idx)
        for bi_idx in sorted(model_bis):
            seen_bi.add(bi_idx)
            pos = bi_base + bi_idx * bi_stride
            pointers = [(0, "vertex", "v0"), (4, "vertex", "v1"),
                        (8, "vertex", "v2"), (260, "index", "i")]
            for field_off, kind, label in pointers:
                off = _u32(block1, pos + field_off)
                if off in (0, 0xFFFFFFFF):
                    continue
                target_base = vb_base if kind == "vertex" else ib_base
                target_count = vb_count if kind == "vertex" else ib_count
                stride = VB_STRIDE if kind == "vertex" else IB_STRIDE
                if off < target_base or (off - target_base) % stride or (off - target_base) // stride >= target_count:
                    failures.append(f"ModelInfo[{mi}] BufferInfo[{bi_idx}].{label}: invalid Block1 {kind} pointer {off}")
                    continue
                if kind == "vertex":
                    report["checked_vb_refs"] += 1
                else:
                    report["checked_ib_refs"] += 1
                data_off = _u32(block1, off + (12 if kind == "vertex" else 16))
                data_size = _u32(block1, off + 4)
                if asset is not None:
                    report["checked_asset_spans"] += 1
                    if not _valid_span(data_off, data_size, asset[1]):
                        failures.append(f"ModelInfo[{mi}] BufferInfo[{bi_idx}].{label}: BIN asset payload OOB (off={data_off}, size={data_size}, inflated={asset[1]})")
    report["distinct_buffer_infos"] = len(seen_bi)
    report["distinct_model_asset_keys"] = len(seen_assets)
    if bin_path is None:
        warnings.append("model_links: BIN not supplied; model payload geometry spans UNVERIFIED (Block2 is not the vertex/index source)")
    if bin_path is not None and verify_streams:
        report["bin_asset_validation"] = "inflated_size_and_zlib_stream"
        report["verified_model_asset_streams"] = len(stream_checked)
    return report
