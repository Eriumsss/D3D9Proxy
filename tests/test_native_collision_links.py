#!/usr/bin/env python3
"""Synthetic native collision reference validation without requiring game assets."""
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "pak_pool_preflight.py"
spec = importlib.util.spec_from_file_location("collision_preflight_test", SCRIPT)
assert spec and spec.loader
pre = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = pre
spec.loader.exec_module(pre)


def put(b, at, val):
    struct.pack_into("<I", b, at, val)


def make_fixture():
    """Model at 0; two ShapeInfo at 256; three HkShapeInfo at 600."""
    h, b = [0] * 118, bytearray(4096)
    h[2], h[7], h[8] = 0x13, 472, len(b)
    h[44], h[69] = 1, 0
    h[27], h[52], h[77] = 124, 2, 256
    h[28], h[53], h[78] = 80, 3, 600
    put(b, 0, 0xAABBCCDD)
    put(b, 224, 256)
    put(b, 228, 2)
    put(b, 256, 1200)
    put(b, 260, 0)
    put(b, 256+108, 2)
    put(b, 256+112, 600)
    put(b, 380+4, 1)
    put(b, 380+108, 1)
    put(b, 380+112, 760)
    # ShapeExtra: 16 byte header, two u32 entries, last+2 bytes.
    put(b, 1200, 2)
    put(b, 1216, 0)
    put(b, 1220, 4)
    # Convex (normals at 1400, vertices at 1450)
    put(b, 600+32, 5)
    for o, v in [(40,2), (44,1400), (48,3), (52,1450)]:
        put(b, 600+o, v)
    # MOPP (tree at 1500, 4 vertices at 1600, 2 triangles at 1650)
    put(b, 680+32, 6)
    for o, v in [(56,8), (60,1500), (64,4), (68,1600),
                 (72,2), (76,1650)]:
        put(b, 680+o, v)
    put(b, 760+32, 2)  # sphere
    return h, b


class CollisionReferences(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.path = Path(tmp.name)/"collision.pak"
        self.h, self.b = make_fixture()

    def inspect(self):
        self.path.write_bytes(struct.pack("<118I", *self.h) + self.b)
        return pre.inspect_pak(self.path, inspect_collision=True)

    def assert_bad(self, fragment):
        self.assertTrue(any(fragment in x for x in self.inspect().failures),
                        msg=fragment)

    def test_good_reference_chain(self):
        report = self.inspect()
        self.assertFalse(report.failures)
        c = report.collision_links
        self.assertEqual(c["models_with_shapes"], 1)
        self.assertEqual(c["model_shape_refs"], 2)
        self.assertEqual(c["shape_havok_refs"], 3)
        self.assertEqual(c["extra_spans"], 1)
        self.assertEqual(c["convex_spans"], 2)
        self.assertEqual(c["mopp_spans"], 3)

    def test_reachable_collision_footprint_is_disk_bytes_not_havok_heap(self):
        r = self.inspect()
        self.assertFalse(r.failures)
        c = r.collision_links
        self.assertTrue(c["payload_footprint_valid"])
        self.assertEqual(c["reachable_shape_records"], 2)
        self.assertEqual(c["reachable_havok_children"], 3)
        self.assertEqual(c["unreferenced_shape_records"], 0)
        self.assertEqual(c["unreferenced_havok_children"], 0)
        self.assertEqual(c["reachable_convex_children"], 1)
        self.assertEqual(c["reachable_mopp_children"], 1)
        self.assertEqual(c["reachable_descriptor_bytes"], 2 * 124 + 3 * 80)
        self.assertEqual(c["reachable_payload_bytes_by_kind"]["shape_extra"], 30)
        self.assertEqual(c["reachable_payload_bytes_by_kind"]["convex normals"], 32)
        self.assertEqual(c["reachable_payload_bytes_by_kind"]["convex vertices"], 36)
        self.assertEqual(c["reachable_payload_bytes_by_kind"]["MOPP bytecode"], 8)
        self.assertEqual(c["reachable_payload_bytes_by_kind"]["MOPP vertices"], 48)
        self.assertEqual(c["reachable_payload_bytes_by_kind"]["MOPP indices"], 12)
        self.assertEqual(c["reachable_payload_raw_bytes"], 166)
        self.assertEqual(c["reachable_payload_unique_bytes"], 166)

    def test_orphan_collision_rows_not_assumed_loaded(self):
        put(self.b, 228, 0)  # no ModelInfo -> ShapeInfo link
        r = self.inspect()
        c = r.collision_links
        self.assertFalse(r.failures)
        self.assertEqual(c["reachable_shape_records"], 0)
        self.assertEqual(c["unreferenced_shape_records"], 2)
        self.assertEqual(c["reachable_havok_children"], 0)
        self.assertEqual(c["unreferenced_havok_children"], 3)
        self.assertEqual(c["reachable_payload_unique_bytes"], 0)

    def test_shared_child_is_not_double_counted(self):
        put(self.b, 380 + 112, 600)  # shape 1 reuses child 0
        r = self.inspect()
        c = r.collision_links
        self.assertFalse(r.failures)
        self.assertEqual(c["shape_havok_refs"], 3)  # links remain 2 + 1
        self.assertEqual(c["reachable_havok_children"], 2)
        self.assertEqual(c["unreferenced_havok_children"], 1)
        self.assertEqual(c["reachable_descriptor_bytes"], 2 * 124 + 2 * 80)

    def test_overlapping_payload_spans_are_union_counted(self):
        put(self.b, 600 + 52, 1400)  # convex vertices overlap 32 normal bytes
        r = self.inspect()
        c = r.collision_links
        self.assertFalse(r.failures)
        self.assertEqual(c["reachable_payload_raw_bytes"], 166)
        self.assertEqual(c["reachable_payload_unique_bytes"], 134)

    def test_invalid_payload_marks_footprint_incomplete(self):
        put(self.b, 680 + 60, 4090)  # declared MOPP 8 bytes exceeds Block1
        r = self.inspect()
        self.assertTrue(r.failures)
        self.assertFalse(r.collision_links["payload_footprint_valid"])

    def test_model_shape_alignment(self):
        put(self.b, 224, 257)
        self.assert_bad("invalid shape_offset/count")

    def test_model_shape_count_overflow(self):
        put(self.b, 228, 3)
        self.assert_bad("invalid shape_offset/count")

    def test_shape_havok_alignment(self):
        put(self.b, 256+112, 601)
        self.assert_bad("invalid hk_shape_offset/count")

    def test_shape_havok_count_overflow(self):
        put(self.b, 256+108, 5)
        self.assert_bad("invalid hk_shape_offset/count")

    def test_convex_normals_outside(self):
        put(self.b, 600+44, 4090)
        self.assert_bad("convex normals payload")

    def test_convex_vertices_outside(self):
        put(self.b, 600+52, 4090)
        self.assert_bad("convex vertices payload")

    def test_mopp_bytecode_outside(self):
        put(self.b, 680+60, 4090)
        self.assert_bad("MOPP bytecode payload")

    def test_mopp_vertices_outside(self):
        put(self.b, 680+68, 4090)
        self.assert_bad("MOPP vertices payload")

    def test_mopp_indices_outside(self):
        put(self.b, 680+76, 4090)
        self.assert_bad("MOPP indices payload")

    def test_extra_header_outside(self):
        put(self.b, 256, 4090)
        self.assert_bad("extra header outside")

    def test_extra_values_outside(self):
        put(self.b, 1200, 0xFFFFFFFF)
        self.assert_bad("extra values outside")

    def test_extra_payload_outside(self):
        put(self.b, 1220, 0xFFFFFFF0)
        self.assert_bad("extra payload outside")

    def test_wrong_hk_stride(self):
        self.h[28] = 40
        self.assert_bad("HkShapeInfo disk stride")

    def test_wrong_shape_stride(self):
        self.h[27] = 16
        self.assert_bad("ShapeInfo disk stride")

    def test_truncated_shape_table(self):
        self.h[77] = 4000
        self.assert_bad("ShapeInfo table outside")

    def test_zero_count_ignores_stale_offset(self):
        put(self.b, 228, 0)
        put(self.b, 256+108, 0)
        put(self.b, 256+112, 0xFFFFFFFF)
        r = self.inspect()
        self.assertFalse(r.failures)
        self.assertEqual(r.collision_links["models_with_shapes"], 0)
        self.assertEqual(r.collision_links["shape_havok_refs"], 1)

    def test_zero_count_tables_allow_unused_offsets_outside_block1(self):
        # Unlike a live reference, an empty table is not dereferenced.
        self.h[52], self.h[53] = 0, 0
        self.h[77], self.h[78] = 0xFFFFFFF0, 0xFFFFFFF0
        put(self.b, 228, 0)
        report = self.inspect()
        self.assertFalse(report.failures)
        self.assertEqual(report.collision_links["shape_count"], 0)
        self.assertEqual(report.collision_links["havok_child_count"], 0)

    def test_empty_model_table_allows_unused_offset_outside_block1(self):
        self.h[44], self.h[69] = 0, 0xFFFFFFF0
        report = self.inspect()
        self.assertFalse(report.failures)
        self.assertEqual(report.collision_links["model_count"], 0)

    def test_unknown_child_kind_warning(self):
        put(self.b, 760+32, 0xBAD)
        r = self.inspect()
        self.assertFalse(r.failures)
        self.assertTrue(any("unknown child kind" in x for x in r.warnings))

    def test_opt_in_only(self):
        put(self.b, 224, 257)
        self.path.write_bytes(struct.pack("<118I", *self.h)+self.b)
        r = pre.inspect_pak(self.path)
        self.assertFalse(r.failures)
        self.assertIsNone(r.collision_links)

    def test_cli_codes(self):
        self.path.write_bytes(struct.pack("<118I", *self.h)+self.b)
        self.assertEqual(pre.main(["--base",str(self.path),"--collision-links","--json"]),0)
        put(self.b,224,257)
        self.path.write_bytes(struct.pack("<118I", *self.h)+self.b)
        self.assertEqual(pre.main(["--base",str(self.path),"--collision-links"]),1)


if __name__ == "__main__":
    unittest.main()
