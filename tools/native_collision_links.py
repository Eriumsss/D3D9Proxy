#!/usr/bin/env python3
"""Native Conquest PC PAK collision topology preflight (read-only).

Verified against LevelReader.h, MgCollisionTypes.h, PakRebuilder.cpp and
MgShapeBuilder.cpp. Every offset here is an UNRELOCATED Block1 byte offset:
ModelInfo+224 -> ShapeInfo[124], ShapeInfo+112 -> HkShapeInfo[80].
HkShapeInfo kind 5/6 variable data also resides in Block1, NOT model BIN.
At runtime the loader relocates those offsets to pointers and overwrites
ShapeInfo+112 with an hkpShape*. Never interpret runtime memory with this code.
Valid ranges do NOT prove Havok MOPP semantics, collision correctness, or an
in-game static level import.
"""
from __future__ import annotations

import struct

MODEL_STRIDE = 256
SHAPE_STRIDE = 124
HK_STRIDE = 80


def _u32(b: bytes, off: int) -> int:
    return struct.unpack_from("<I", b, off)[0]


def _in_bytes(offset: int, length: int, total: int) -> bool:
    return offset >= 0 and length >= 0 and offset <= total and length <= total - offset


def _in_table(offset: int, count: int, base: int, available: int, stride: int) -> bool:
    return (offset >= base and (offset - base) % stride == 0 and
            (offset - base) // stride <= available and
            count <= available - (offset - base) // stride)


def _unique_bytes(spans: list[tuple[int, int]]) -> int:
    """Count the union of validated Block1 [start,end) byte ranges."""
    last = total = 0
    for start, end in sorted(spans):
        if end <= last:
            continue
        total += end - max(start, last)
        last = end
    return total


def inspect_collision_links(h: tuple[int, ...], block1: bytes,
                            failures: list[str], warnings: list[str]) -> dict[str, object]:
    """Validate disk collision parent/child links and data envelopes."""
    total = len(block1)
    mc, mb = h[44], h[69]
    sc, sb, ss = h[52], h[77], h[27]
    kc, kb, ks = h[53], h[78], h[28]
    report: dict[str, object] = {
        "model_count": mc, "shape_count": sc, "havok_child_count": kc,
        "models_with_shapes": 0, "model_shape_refs": 0,
        "shape_havok_refs": 0, "extra_spans": 0,
        "convex_spans": 0, "mopp_spans": 0,
        "unknown_rigid_kinds": 0, "unknown_havok_kinds": 0,
        "reachable_shape_records": 0, "unreferenced_shape_records": 0,
        "reachable_havok_children": 0, "unreferenced_havok_children": 0,
        "reachable_mopp_children": 0, "reachable_convex_children": 0,
        "reachable_descriptor_bytes": 0,
        "reachable_payload_raw_bytes": 0, "reachable_payload_unique_bytes": 0,
        "reachable_payload_bytes_by_kind": {},
        "payload_footprint_valid": False,
        "scope": "Referenced unique on-disk Block1 records/byte spans only; no Havok runtime heap allocation estimate",
    }
    if mc and not _in_bytes(mb, mc * MODEL_STRIDE, total):
        failures.append("collision: ModelInfo table outside Block1")
        return report
    if sc and ss != SHAPE_STRIDE:
        failures.append(f"collision: ShapeInfo disk stride {ss} != {SHAPE_STRIDE}")
        return report
    if kc and ks != HK_STRIDE:
        failures.append(f"collision: HkShapeInfo disk stride {ks} != {HK_STRIDE}")
        return report
    if sc and not _in_bytes(sb, sc * SHAPE_STRIDE, total):
        failures.append("collision: ShapeInfo table outside Block1")
        return report
    if kc and not _in_bytes(kb, kc * HK_STRIDE, total):
        failures.append("collision: HkShapeInfo table outside Block1")
        return report

    # Count only the collision data reachable through ModelInfo -> ShapeInfo
    # -> HkShapeInfo. Unreferenced table rows must not be treated as live assets.
    reachable_shapes: set[int] = set()
    shape_children: dict[int, range] = {}
    extras: dict[int, tuple[int, int]] = {}
    payloads: dict[int, list[tuple[str, int, int]]] = {}
    child_kinds: dict[int, int] = {}

    for mi in range(mc):
        at = mb + MODEL_STRIDE * mi
        off, count = struct.unpack_from("<2I", block1, at + 224)
        if count == 0:
            continue  # count-gated: a stale offset alone is not a live link
        report["models_with_shapes"] += 1
        if not _in_table(off, count, sb, sc, SHAPE_STRIDE):
            failures.append(f"ModelInfo[{mi}] key=0x{_u32(block1, at):08X}: "
                            f"invalid shape_offset/count ({off}, {count})")
        else:
            report["model_shape_refs"] += count
            first = (off - sb) // SHAPE_STRIDE
            reachable_shapes.update(range(first, first + count))

    for si in range(sc):
        at = sb + si * SHAPE_STRIDE
        extra, kind = struct.unpack_from("<2I", block1, at)
        child_count, child_off = struct.unpack_from("<2I", block1, at + 108)
        if kind > 5:
            report["unknown_rigid_kinds"] += 1
            if report["unknown_rigid_kinds"] <= 5:
                warnings.append(f"ShapeInfo[{si}]: unknown rigid-body kind {kind}")
        if child_count:
            if not _in_table(child_off, child_count, kb, kc, HK_STRIDE):
                failures.append(f"ShapeInfo[{si}]: invalid hk_shape_offset/count "
                                f"({child_off}, {child_count})")
            else:
                report["shape_havok_refs"] += child_count
                first = (child_off - kb) // HK_STRIDE
                shape_children[si] = range(first, first + child_count)
        # Kind-0 ShapeInfo 'extra' is a separate Block1 blob, not child geometry.
        # PakRebuilder reads [4 x u32 header][num x u32][last_value + 2 bytes].
        if kind == 0 and extra:
            if not _in_bytes(extra, 16, total):
                failures.append(f"ShapeInfo[{si}]: extra header outside Block1")
                continue
            n = _u32(block1, extra)
            if n == 0:
                warnings.append(f"ShapeInfo[{si}]: empty extra record (reader skips)")
                continue
            if not _in_bytes(extra + 16, n * 4, total):
                failures.append(f"ShapeInfo[{si}]: extra values outside Block1")
                continue
            last = _u32(block1, extra + 16 + 4 * (n - 1))
            if not _in_bytes(extra, 16 + n * 4 + last + 2, total):
                failures.append(f"ShapeInfo[{si}]: extra payload outside Block1")
            else:
                report["extra_spans"] += 1
                extras[si] = (extra, 16 + n * 4 + last + 2)

    for ki in range(kc):
        at = kb + ki * HK_STRIDE
        kind = _u32(block1, at + 32)
        child_kinds[ki] = kind
        if kind > 6:
            report["unknown_havok_kinds"] += 1
            if report["unknown_havok_kinds"] <= 5:
                warnings.append(f"HkShapeInfo[{ki}]: unknown child kind {kind}")
            continue

        def validate(label: str, count: int, offset: int, unit: int) -> None:
            if not count:
                return  # zero-sized data may carry stale offset
            if offset == 0 or not _in_bytes(offset, count * unit, total):
                failures.append(
                    f"HkShapeInfo[{ki}] kind {kind}: {label} payload outside Block1 "
                    f"(off={offset}, count={count}, stride={unit})")
            else:
                payloads.setdefault(ki, []).append((label, offset, count * unit))
                if kind == 5:
                    report["convex_spans"] += 1
                elif kind == 6:
                    report["mopp_spans"] += 1

        if kind == 5:
            normal_n, normal_off, vert_n, vert_off = struct.unpack_from(
                "<4I", block1, at + 40)
            validate("convex normals", normal_n, normal_off, 16)
            # Convex data can contain additional padding vertices.
            validate("convex vertices", vert_n, vert_off, 12)
        elif kind == 6:
            tree_n, tree_off, vert_n, vert_off, tri_n, tri_off = (
                struct.unpack_from("<6I", block1, at + 56))
            validate("MOPP bytecode", tree_n, tree_off, 1)
            validate("MOPP vertices", vert_n, vert_off, 12)
            validate("MOPP indices", tri_n, tri_off, 6)  # 3 x u16 per triangle
    reachable_children: set[int] = set()
    for shape in reachable_shapes:
        reachable_children.update(shape_children.get(shape, ()))
    report["reachable_shape_records"] = len(reachable_shapes)
    report["unreferenced_shape_records"] = sc - len(reachable_shapes)
    report["reachable_havok_children"] = len(reachable_children)
    report["unreferenced_havok_children"] = kc - len(reachable_children)
    report["reachable_mopp_children"] = sum(child_kinds.get(i) == 6 for i in reachable_children)
    report["reachable_convex_children"] = sum(child_kinds.get(i) == 5 for i in reachable_children)
    # These are disk descriptor bytes, NOT Havok heap allocations.
    report["reachable_descriptor_bytes"] = (
        len(reachable_shapes) * SHAPE_STRIDE + len(reachable_children) * HK_STRIDE
    )
    by_kind = {
        "shape_extra": 0,
        "convex normals": 0, "convex vertices": 0,
        "MOPP bytecode": 0, "MOPP vertices": 0, "MOPP indices": 0,
    }
    spans: list[tuple[int, int]] = []
    for si in reachable_shapes:
        if si in extras:
            start, length = extras[si]
            by_kind["shape_extra"] += length
            spans.append((start, start + length))
    for ki in reachable_children:
        for label, start, length in payloads.get(ki, ()):
            by_kind[label] += length
            spans.append((start, start + length))
    report["reachable_payload_bytes_by_kind"] = by_kind
    report["reachable_payload_raw_bytes"] = sum(by_kind.values())
    report["reachable_payload_unique_bytes"] = _unique_bytes(spans)
    # Even one invalid or opaque reachable descriptor invalidates footprint
    # completeness. Keep the numeric measurements for triage, but NEVER
    # present them as proof that all imported collision data is understood.
    report["payload_footprint_valid"] = (
        not failures and
        all(child_kinds.get(i, 0) <= 6 for i in reachable_children) and
        all(_u32(block1, sb + si * SHAPE_STRIDE + 4) <= 5 for si in reachable_shapes)
    )
    if report["unknown_rigid_kinds"] > 5:
        warnings.append("collision: additional unknown rigid kinds omitted")
    if report["unknown_havok_kinds"] > 5:
        warnings.append("collision: additional unknown Havok kinds omitted")
    return report
