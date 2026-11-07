#!/usr/bin/env python3
"""Read-only source -> merged collision preservation audit for Conquest PC PAK.

Compare *on-disk* ModelInfo CRC keys in a donor level to the merged level.
The comparison hashes ShapeInfo, HkShapeInfo and Block1 convex/MOPP payloads,
excluding only the Block1 address fields that legally move on reflow.

Never confuse Havok Block1 data with model VB/IB payloads in companion BIN.
Never apply to relocated runtime structures. This cannot prove executable
Havok/MOPP correctness, transplanted instances, or script-free level loading.

Same-key matching is conservative: models absent from the merge are not
assumed imported. An optional explicit CRC mapping supports renamed models.
"""
from __future__ import annotations

import hashlib
import json
import struct
from pathlib import Path

from native_collision_links import _in_bytes, _in_table

MODEL_SIZE = 256
SHAPE_SIZE = 124
HK_SIZE = 80
MAX_SAMPLES = 20


def _u32(blob: bytes, at: int) -> int:
    return struct.unpack_from("<I", blob, at)[0]


def _get_models(h: tuple[int, ...], block: bytes) -> list[tuple[int, int, int]]:
    count, off = h[44], h[69]
    if count and not _in_bytes(off, count * MODEL_SIZE, len(block)):
        raise ValueError("ModelInfo table outside decoded Block1")
    return [( _u32(block, off + i * MODEL_SIZE),
              _u32(block, off + i * MODEL_SIZE + 224),
              _u32(block, off + i * MODEL_SIZE + 228) )
            for i in range(count)]


def _digest_for_model(h: tuple[int, ...], block: bytes, shape_off: int,
                      shape_count: int) -> str:
    """Stable if and only if known offset fields were merely relocated."""
    sb, sc, ss = h[77], h[52], h[27]
    kb, kc, ks = h[78], h[53], h[28]
    total = len(block)
    if ss != SHAPE_SIZE or ks != HK_SIZE:
        raise ValueError("unsupported on-disk ShapeInfo/HkShapeInfo stride")
    if not _in_table(shape_off, shape_count, sb, sc, SHAPE_SIZE):
        raise ValueError("model ShapeInfo reference is invalid")
    out = hashlib.sha256(b"OpenConquest/collision-preservation/v1")
    out.update(struct.pack("<I", shape_count))

    def feed(label: bytes, data: bytes) -> None:
        out.update(struct.pack("<I", len(label)))
        out.update(label)
        out.update(struct.pack("<Q", len(data)))
        out.update(data)

    def payload(kind: int, label: bytes, off: int, count: int, stride: int) -> None:
        if count and (not off or not _in_bytes(off, count * stride, total)):
            raise ValueError(f"kind {kind}: {label.decode()} payload outside Block1")
        feed(label, block[off:off + count * stride] if count else b"")

    for si in range(shape_count):
        at = shape_off + si * SHAPE_SIZE
        raw = bytearray(block[at:at + SHAPE_SIZE])
        kind = _u32(raw, 4)
        if kind > 5:
            raise ValueError(f"ShapeInfo[{si}] unknown kind {kind}")
        extra = _u32(raw, 0)
        child_count, child_off = struct.unpack_from("<2I", raw, 108)
        if kind == 0:
            raw[0:4] = b"\0" * 4  # ShapeInfo.offset -> extra blob
        raw[112:116] = b"\0" * 4  # child pointer reflow
        feed(b"ShapeInfo", raw)
        if kind == 0 and extra:
            if not _in_bytes(extra, 16, total):
                raise ValueError("ShapeInfo extra header outside Block1")
            n = _u32(block, extra)
            if n and not _in_bytes(extra + 16, n * 4, total):
                raise ValueError("ShapeInfo extra value table outside Block1")
            length = 0 if not n else 16 + n * 4 + _u32(block, extra + 16 + (n - 1) * 4) + 2
            if not _in_bytes(extra, length, total):
                raise ValueError("ShapeInfo extra payload outside Block1")
            payload(0, b"shape-extra", extra, length, 1)
        else:
            feed(b"shape-extra", b"")
        if child_count:
            if ks != HK_SIZE or not _in_table(child_off, child_count, kb, kc, HK_SIZE):
                raise ValueError("HkShapeInfo child reference outside native table")
        feed(b"child-count", struct.pack("<I", child_count))
        for ci in range(child_count):
            ci_at = child_off + ci * HK_SIZE
            hk = bytearray(block[ci_at:ci_at + HK_SIZE])
            hk_kind = _u32(hk, 32)
            if hk_kind > 6:
                raise ValueError(f"HkShapeInfo[{ci}] unknown kind {hk_kind}")
            if hk_kind == 5:
                n1, p1, n2, p2 = struct.unpack_from("<4I", hk, 40)
                hk[44:48] = b"\0" * 4
                hk[52:56] = b"\0" * 4
                feed(b"HkShapeInfo", hk)
                payload(5, b"convex-normals", p1, n1, 16)
                payload(5, b"convex-vertices", p2, n2, 12)
            elif hk_kind == 6:
                tn, tp, vn, vp, fn, fp = struct.unpack_from("<6I", hk, 56)
                for atoff in (60, 68, 76):
                    hk[atoff:atoff+4] = b"\0" * 4
                feed(b"HkShapeInfo", hk)
                payload(6, b"mopp-bytecode", tp, tn, 1)
                payload(6, b"mopp-vertices", vp, vn, 12)
                payload(6, b"mopp-triangle-indices", fp, fn, 6)
            else:
                feed(b"HkShapeInfo", hk)
    return out.hexdigest()


def _snapshot(h: tuple[int, ...], block: bytes) -> dict[int, list[dict[str, object]]]:
    """All model keys, including collisionless ones, preserving duplicates."""
    result: dict[int, list[dict[str, object]]] = {}
    for key, offset, count in _get_models(h, block):
        if not count:
            entry: dict[str, object] = {"status": "no_collision", "shape_count": 0}
        else:
            try:
                digest = _digest_for_model(h, block, offset, count)
                entry = {"status": "hashed", "shape_count": count, "sha256": digest}
            except ValueError as exc:
                entry = {"status": "unverifiable", "reason": str(exc),
                         "shape_count": count}
        result.setdefault(key, []).append(entry)
    return result


def load_key_map(path: Path) -> dict[int, int]:
    """JSON {"0xDONOR_CRC":"0xMERGED_CRC"} for intentional model renaming."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"collision key map: unreadable JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise ValueError("collision key map must be a JSON object")
    def key32(value: object) -> int:
        if isinstance(value, bool):
            raise ValueError("CRC keys cannot be booleans")
        try:
            n = int(value, 0) if isinstance(value, str) else int(value)
        except (ValueError, TypeError) as exc:
            raise ValueError(f"invalid CRC key {value!r}") from exc
        if (not isinstance(value, (int, str)) or n < 0 or n > 0xFFFFFFFF):
            raise ValueError(f"CRC key {value!r} must be a uint32")
        return n
    return {key32(k): key32(v) for k, v in data.items()}


def compare_collision_preservation(
    donor_header: tuple[int, ...], donor_block: bytes,
    merged_header: tuple[int, ...], merged_block: bytes,
    base_header: tuple[int, ...], base_block: bytes,
    key_map: dict[int, int] | None = None,
) -> dict[str, object]:
    """Compare only donor collision models. Unknown unimported keys do NOT fail.

    Unmapped donor keys already in the base are ignored; the merged version
    might legitimately preserve the base's definition rather than the donor.
    Explicit key mappings override that skip and require the target key.
    """
    donor = _snapshot(donor_header, donor_block)
    merged = _snapshot(merged_header, merged_block)
    base_keys = {key for key, _, _ in _get_models(base_header, base_block)}
    key_map = key_map or {}
    out: dict[str, object] = {
        "donor_models_with_collision": 0, "preserved": 0,
        "dropped_collision": 0, "changed_collision": 0,
        "missing_unmapped_target": 0, "missing_mapped_target": 0,
        "skipped_preexisting_base_key": 0, "unverifiable": 0,
        "ambiguous_duplicate_key": 0,
        "issues": [], "samples": [],
        "note": "Unmatched donor keys are informational without an explicit key map; exact reflow-invariant Block1 collision payload comparison only",
    }
    issues: list[str] = out["issues"]
    samples: list[dict[str, object]] = out["samples"]
    def note(status: str, source: int, target: int, message: str) -> None:
        if len(samples) < MAX_SAMPLES:
            samples.append({"status": status, "source_key": f"0x{source:08X}",
                            "target_key": f"0x{target:08X}", "detail": message})
        if status in ("dropped_collision", "changed_collision",
                      "missing_mapped_target", "unverifiable", "ambiguous_duplicate_key"):
            issues.append(f"donor 0x{source:08X} -> merged 0x{target:08X}: {status}: {message}")

    for source, source_rows in donor.items():
        source_coll = [row for row in source_rows if row["status"] != "no_collision"]
        if not source_coll:
            continue
        out["donor_models_with_collision"] += 1
        explicit = source in key_map
        target = key_map.get(source, source)
        if source in base_keys and not explicit:
            out["skipped_preexisting_base_key"] += 1
            continue
        target_rows = merged.get(target, [])
        if not target_rows:
            field = "missing_mapped_target" if explicit else "missing_unmapped_target"
            out[field] += 1
            note(field, source, target,
                 "explicit destination CRC absent" if explicit else "not observed; not proof this model was imported")
            continue
        if len(source_rows) != 1 or len(target_rows) != 1:
            out["ambiguous_duplicate_key"] += 1
            note("ambiguous_duplicate_key", source, target, "same CRC occurs multiple times; cannot prove which record is used")
            continue
        sr, tr = source_rows[0], target_rows[0]
        if sr["status"] == "unverifiable" or tr["status"] == "unverifiable":
            out["unverifiable"] += 1
            note("unverifiable", source, target,
                 f"donor={sr.get('reason',sr['status'])}; merged={tr.get('reason',tr['status'])}")
        elif tr["status"] == "no_collision":
            out["dropped_collision"] += 1
            note("dropped_collision", source, target, "donor had shapes; target shape_num is zero")
        elif sr["sha256"] != tr["sha256"]:
            out["changed_collision"] += 1
            note("changed_collision", source, target, "collision shape/child/payload fingerprint differs")
        else:
            out["preserved"] += 1
    for source in key_map:
        if source not in donor:
            out["unverifiable"] += 1
            note("unverifiable", source, key_map[source], "mapped donor CRC not present")
        elif not any(x["status"] != "no_collision" for x in donor[source]):
            out["unverifiable"] += 1
            note("unverifiable", source, key_map[source], "mapped donor model has no collision to compare")
    out["strict_failures"] = out["dropped_collision"] + out["changed_collision"] + out["missing_mapped_target"] + out["unverifiable"] + out["ambiguous_duplicate_key"]
    return out


def compare_base_collision_preservation(
    base_header: tuple[int, ...], base_block: bytes,
    merged_header: tuple[int, ...], merged_block: bytes,
) -> dict[str, object]:
    """Conservatively verify existing base-level model collision survives merge.

    This is separate from donor provenance: the host world must not lose its
    existing collision shapes just because new geometry was appended.
    Model keys absent from merged PAK are reported; duplicate CRC keys are
    ambiguous. No runtime Havok/mission behavior is inferred.
    """
    original = _snapshot(base_header, base_block)
    merged = _snapshot(merged_header, merged_block)
    out: dict[str, object] = {
        "base_models_with_collision": 0, "preserved": 0,
        "dropped_collision": 0, "changed_collision": 0,
        "missing_host_model": 0, "ambiguous_duplicate_key": 0,
        "unverifiable": 0, "samples": [],
        "note": "Existing base CRCs with collision should survive into merged native PAK; duplicate CRCs are ambiguous",
    }
    samples: list[dict[str, object]] = out["samples"]

    def record(status: str, key: int, desc: str) -> None:
        out[status] += 1
        if len(samples) < MAX_SAMPLES:
            samples.append({"status": status, "model_key": f"0x{key:08X}", "detail": desc})

    for key, base_rows in original.items():
        if not any(row["status"] != "no_collision" for row in base_rows):
            continue
        out["base_models_with_collision"] += 1
        dest = merged.get(key, [])
        if not dest:
            record("missing_host_model", key, "base model CRC absent from merged PAK")
            continue
        if len(base_rows) != 1 or len(dest) != 1:
            record("ambiguous_duplicate_key", key, "duplicate source or merged CRC prevents unique comparison")
            continue
        a, b = base_rows[0], dest[0]
        if a["status"] == "unverifiable" or b["status"] == "unverifiable":
            record("unverifiable", key,
                   f"base={a.get('reason',a['status'])}; merged={b.get('reason',b['status'])}")
        elif b["status"] == "no_collision":
            record("dropped_collision", key, "base had collision but merged shape_num is zero")
        elif a["sha256"] != b["sha256"]:
            record("changed_collision", key, "shape/child/Block1 payload fingerprint changed")
        else:
            out["preserved"] += 1
    out["strict_failures"] = sum(out[name] for name in (
        "dropped_collision", "changed_collision", "missing_host_model",
        "ambiguous_duplicate_key", "unverifiable"))
    return out
