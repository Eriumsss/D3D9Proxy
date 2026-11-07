#!/usr/bin/env python3
"""Native collision preservation: relocation-proof donor-to-merged checks."""
import importlib.util
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOL))
SRC = TOOL / "pak_pool_preflight.py"
spec = importlib.util.spec_from_file_location("donor_collision_preflight_test", SRC)
assert spec and spec.loader
pak = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = pak
spec.loader.exec_module(pak)
from native_collision_provenance import compare_collision_preservation, compare_base_collision_preservation, load_key_map

KEY = 0xAA012345
OTHER = 0xF01234AB


def put(buf, at, n):
    struct.pack_into("<I", buf, at, n)


def fake_pak(path, *, model_key=KEY, has_collision=True, shift=0,
             kind=6, altered=False, duplicates=False, shape_kind=1):
    """One collision model with relocatable native Block1 pointers, no BIN."""
    b = bytearray(4096)
    h = [0] * 118
    h[2], h[7], h[8] = 0x13, 472, 4096
    h[44], h[69] = 2 if duplicates else 1, 0
    put(b, 0, model_key)
    put(b, 4, 1)
    if duplicates:
        put(b, 256, model_key)  # second model is collisionless
        put(b, 260, 1)
    if has_collision:
        s, hk = 600 + shift, 1000 + shift
        extra, tree, verts, inds = 1350 + shift, 1700 + shift, 1800 + shift, 1900 + shift
        h[27], h[52], h[77] = 124, 1, s
        h[28], h[53], h[78] = 80, 1, hk
        put(b, 224, s)
        put(b, 228, 1)
        put(b, s + 4, shape_kind)
        put(b, s + 108, 1)
        put(b, s + 112, hk)
        if shape_kind == 0:
            put(b, s, extra)
            put(b, extra, 1)
            put(b, extra+16, 3)
            b[extra+20:extra+25] = b"abcde"
        put(b, hk + 32, kind)
        if kind == 6:
            for offset, value in ((56,8),(60,tree),(64,4),(68,verts),(72,2),(76,inds)):
                put(b, hk + offset, value)
            b[tree:tree+8] = b"mopp1234"
            b[verts:verts+48] = bytes(range(48))
            b[inds:inds+12] = bytes(range(12))
            if altered:
                b[tree+3] ^= 0xFF
        elif kind == 5:
            for offset, value in ((40,2),(44,tree),(48,3),(52,verts)):
                put(b, hk + offset, value)
            b[tree:tree+32] = bytes(range(32))
            b[verts:verts+36] = bytes(range(36))
            if altered:
                b[verts+7] ^= 0xFF
        else:
            if altered:
                put(b, hk + 40, 0x12345678) # sphere radius bytes changed
    path.write_bytes(struct.pack("<118I", *h) + b)
    return path


def snap(path):
    h,_ = pak._header(path)
    return h,pak._decode_block1(path,h)


class CollisionPreservation(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.donor = fake_pak(self.root/"donor.pak")
        self.base = fake_pak(self.root/"base.pak",model_key=OTHER,has_collision=False)
        self.merged = fake_pak(self.root/"merged.pak",shift=150)

    def audit(self, key_map=None):
        a,ab = snap(self.donor)
        b,bb = snap(self.base)
        c,cb = snap(self.merged)
        return compare_collision_preservation(a,ab,c,cb,b,bb,key_map)

    def host_audit(self):
        b, bb = snap(self.base)
        m, mb = snap(self.merged)
        return compare_base_collision_preservation(b, bb, m, mb)

    def test_host_collision_survives_reflow(self):
        fake_pak(self.base, model_key=KEY, has_collision=True)
        self.assertEqual(self.host_audit()["preserved"], 1)

    def test_host_collision_can_be_dropped(self):
        fake_pak(self.base, model_key=KEY, has_collision=True)
        fake_pak(self.merged, model_key=KEY, has_collision=False)
        self.assertEqual(self.host_audit()["dropped_collision"], 1)

    def test_host_collision_bytecode_mutation(self):
        fake_pak(self.base, model_key=KEY, has_collision=True)
        fake_pak(self.merged, model_key=KEY, shift=150, altered=True)
        self.assertEqual(self.host_audit()["changed_collision"], 1)

    def test_host_missing_model_is_failure(self):
        fake_pak(self.base, model_key=OTHER, has_collision=True)
        self.assertEqual(self.host_audit()["missing_host_model"], 1)

    def test_host_duplicate_model_key_unverifiable(self):
        fake_pak(self.base, model_key=KEY, has_collision=True)
        fake_pak(self.merged, model_key=KEY, shift=150, duplicates=True)
        self.assertEqual(self.host_audit()["ambiguous_duplicate_key"], 1)

    def test_host_collisionless_model_is_not_required(self):
        r = self.host_audit()
        self.assertEqual(r["base_models_with_collision"], 0)
        self.assertEqual(r["strict_failures"], 0)

    def test_host_unknown_child_kind_is_unverifiable(self):
        fake_pak(self.base, model_key=KEY, has_collision=True)
        fake_pak(self.merged, model_key=KEY, kind=0xBAD)
        self.assertEqual(self.host_audit()["unverifiable"], 1)

    def test_strict_host_cli_rejects_dropped_collision(self):
        fake_pak(self.base, model_key=KEY, has_collision=True)
        fake_pak(self.merged, model_key=KEY, has_collision=False)
        args=["--base",str(self.base),"--donor",str(self.donor),
              "--merged",str(self.merged),"--collision-preservation"]
        self.assertEqual(pak.main(args), 0)
        self.assertEqual(pak.main(args + ["--require-base-collision"]), 1)

    def test_strict_host_cli_accepts_reflow(self):
        fake_pak(self.base, model_key=KEY, has_collision=True)
        args=["--base",str(self.base),"--donor",str(self.donor),
              "--merged",str(self.merged),"--collision-preservation",
              "--require-base-collision","--json"]
        self.assertEqual(pak.main(args), 0)

    def test_host_flag_needs_comparison_mode(self):
        with self.assertRaises(SystemExit) as err:
            pak.main(["--base",str(self.base),"--require-base-collision"])
        self.assertEqual(err.exception.code, 2)

    def test_preserved_renumbered_block1_pointers(self):
        r=self.audit()
        self.assertEqual(r["preserved"],1)
        self.assertEqual(r["strict_failures"],0)

    def test_zeroed_imported_collision_is_detected(self):
        fake_pak(self.merged,has_collision=False)
        r=self.audit()
        self.assertEqual(r["dropped_collision"],1)
        self.assertEqual(r["strict_failures"],1)

    def test_mutated_mopp_bytecode_is_detected(self):
        fake_pak(self.merged,shift=150,altered=True)
        self.assertEqual(self.audit()["changed_collision"],1)

    def test_mutated_convex_vertices_is_detected(self):
        fake_pak(self.donor,kind=5)
        fake_pak(self.merged,shift=150,kind=5,altered=True)
        self.assertEqual(self.audit()["changed_collision"],1)

    def test_preserved_convex_despite_relocation(self):
        fake_pak(self.donor,kind=5)
        fake_pak(self.merged,shift=150,kind=5)
        self.assertEqual(self.audit()["preserved"],1)

    def test_shape_kind0_extra_relocation(self):
        fake_pak(self.donor,shape_kind=0)
        fake_pak(self.merged,shift=150,shape_kind=0)
        self.assertEqual(self.audit()["preserved"],1)

    def test_donor_extra_blob_mutation(self):
        fake_pak(self.donor,shape_kind=0)
        fake_pak(self.merged,shift=150,shape_kind=0)
        data=bytearray(self.merged.read_bytes())
        data[472+1350+150+22] ^= 1
        self.merged.write_bytes(data)
        self.assertEqual(self.audit()["changed_collision"],1)

    def test_donor_only_unimported_key_is_informational(self):
        fake_pak(self.merged,model_key=OTHER)
        r=self.audit()
        self.assertEqual(r["missing_unmapped_target"],1)
        self.assertEqual(r["strict_failures"],0)

    def test_model_in_base_skipped_automatically(self):
        fake_pak(self.base,model_key=KEY)
        fake_pak(self.merged,has_collision=False)
        r=self.audit()
        self.assertEqual(r["skipped_preexisting_base_key"],1)
        self.assertEqual(r["strict_failures"],0)

    def test_explicit_map_matches_renamed_import(self):
        fake_pak(self.merged,model_key=OTHER,shift=150)
        r=self.audit({KEY:OTHER})
        self.assertEqual(r["preserved"],1)

    def test_explicit_map_missing_target_is_failure(self):
        fake_pak(self.merged,model_key=OTHER)
        r=self.audit({KEY:0xDEADBEEF})
        self.assertEqual(r["missing_mapped_target"],1)
        self.assertEqual(r["strict_failures"],1)

    def test_explicit_map_overrides_preexisting_base_key_skip(self):
        fake_pak(self.base,model_key=KEY,has_collision=False)
        fake_pak(self.merged,has_collision=False)
        r=self.audit({KEY:KEY})
        self.assertEqual(r["dropped_collision"],1)

    def test_duplicate_model_key_is_ambiguous_not_preserved(self):
        fake_pak(self.merged,shift=150,duplicates=True)
        r=self.audit()
        self.assertEqual(r["ambiguous_duplicate_key"],1)
        self.assertEqual(r["preserved"],0)

    def test_unverifiable_mopp_does_not_claim_match(self):
        data=bytearray(self.merged.read_bytes())
        put(data,472+1000+150+60,0xFFFFFFF0)
        self.merged.write_bytes(data)
        r=self.audit()
        self.assertEqual(r["unverifiable"],1)
        self.assertEqual(r["preserved"],0)

    def test_unknown_havok_kind_unverifiable(self):
        fake_pak(self.merged,shift=150,kind=0xFFFF)
        self.assertEqual(self.audit()["unverifiable"],1)

    def test_zero_collision_source_not_a_required_import(self):
        fake_pak(self.donor,has_collision=False)
        r=self.audit()
        self.assertEqual(r["donor_models_with_collision"],0)
        self.assertEqual(r["strict_failures"],0)

    def test_unknown_mapping_source_is_unverifiable(self):
        self.assertEqual(self.audit({0x9999:OTHER})["unverifiable"],1)

    def test_mapping_json_loads_hex_keys(self):
        f=self.root/"map.json"
        f.write_text('{"0xAA012345":"0xF01234AB"}')
        self.assertEqual(load_key_map(f),{KEY:OTHER})

    def test_invalid_mapping_json_rejected(self):
        f=self.root/"map.json"
        f.write_text('{"0xAA012345": 4294967296}')
        with self.assertRaisesRegex(ValueError, "uint32"):
            load_key_map(f)

    def test_cli_plain_report_and_exit_status(self):
        args=["--base",str(self.base),"--donor",str(self.donor),
              "--merged",str(self.merged),"--collision-preservation"]
        self.assertEqual(pak.main(args+["--require-donor-collision"]),0)
        fake_pak(self.merged,has_collision=False)
        self.assertEqual(pak.main(args),0)  # report-only, no strict exit
        self.assertEqual(pak.main(args+["--require-donor-collision"]),1)

    def test_cli_explicit_map_and_json(self):
        fake_pak(self.merged,model_key=OTHER,shift=150)
        f=self.root/"map.json"
        f.write_text(json.dumps({hex(KEY):hex(OTHER)}))
        args=["--base",str(self.base),"--donor",str(self.donor),
              "--merged",str(self.merged),"--collision-preservation",
              "--collision-key-map",str(f),"--require-donor-collision","--json"]
        self.assertEqual(pak.main(args),0)

    def test_cli_missing_donor_is_argument_error(self):
        with self.assertRaises(SystemExit) as e:
            pak.main(["--base",str(self.base),"--collision-preservation"])
        self.assertEqual(e.exception.code,2)


if __name__ == "__main__":
    unittest.main()
