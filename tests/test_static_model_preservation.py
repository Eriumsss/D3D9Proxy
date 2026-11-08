#!/usr/bin/env python3
"""Static donor/host model identity safety: synthetic, read-only PAK tests."""
from __future__ import annotations

import importlib.util
import json
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr
from io import StringIO
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
TOOL=ROOT/"tools"
sys.path.insert(0,str(TOOL))
src=TOOL/"pak_pool_preflight.py"
spec=importlib.util.spec_from_file_location("model_static_gate_test",src)
assert spec and spec.loader
pre=importlib.util.module_from_spec(spec)
sys.modules[spec.name]=pre
spec.loader.exec_module(pre)
from native_model_preservation import snapshot_models,compare_static_model_preservation


HOST=0xAA010001
DONOR=0xBB020002
RENAMED=0xCC030003


def pak_file(path:Path,rows:list[tuple[int,int,bool,int]], *,
             textures:list[tuple[int,int]]|None=None,
             effects:list[tuple[int,int]]|None=None):
    """Minimal native PC PAK with ModelInfo and optional texture/FX tables."""
    h=[0]*118
    b=bytearray()
    h[2]=0x13
    h[7]=472
    h[44]=len(rows)
    h[69]=len(b)
    for key,mask,mesh,asset in rows:
        record=bytearray(256)
        struct.pack_into("<II",record,0,key,mask)
        if mesh:
            struct.pack_into("<5I",record,52,0,0,0,0,1)
        struct.pack_into("<I",record,208,asset)
        b.extend(record)
    h[57],h[82]=len(textures or []),len(b)
    for key,mask in textures or []:
        record=bytearray(0x48)
        struct.pack_into("<II",record,0,key,mask)
        b.extend(record)
    h[60],h[85]=len(effects or []),len(b)
    for key,mask in effects or []:
        record=bytearray(0x10)
        struct.pack_into("<II",record,0,key,mask)
        b.extend(record)
    if not b:
        b.extend(bytes(16))
    h[8]=len(b)
    path.write_bytes(struct.pack("<118I",*h)+b)
    return path


def unpack(path):
    h,_=pre._header(path)
    return h,pre._decode_block1(path,h)


class StaticModelPreservation(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root=Path(temp.name)
        self.base=pak_file(self.root/"base.pak",[(HOST,3,True,0xC0FFEE01)])
        self.donor=pak_file(self.root/"donor.pak",[(DONOR,1,True,0xC0FFEE02)])
        self.merged=pak_file(self.root/"merged.pak",[
            (HOST,3,True,0xC0FFEE01),(DONOR,1,True,0xC0FFEE02)
        ])

    def audit(self, keys=None, mapping=None):
        bh,bb=unpack(self.base);dh,db=unpack(self.donor);mh,mb=unpack(self.merged)
        return compare_static_model_preservation(bh,bb,dh,db,mh,mb,keys,mapping)

    def run_cli(self,*flags):
        with redirect_stdout(StringIO()),redirect_stderr(StringIO()):
            return pre.main(["--base",str(self.base),
                             "--donor",str(self.donor),
                             "--merged",str(self.merged),*flags])

    def test_native_valid_host_and_selected_donor(self):
        r=self.audit({DONOR})
        self.assertEqual(r["strict_failures"],0)
        self.assertEqual(r["base_preserved"],1)
        self.assertEqual(r["donor_present"],1)
        self.assertEqual(self.run_cli("--static-model-preservation",
                                     "--required-donor-model",hex(DONOR)),0)

    def test_missing_base_collisionless_model_is_failure(self):
        pak_file(self.base,[(HOST,3,False,0)])
        pak_file(self.merged,[(DONOR,1,True,0xC0FFEE02)])
        r=self.audit({DONOR})
        self.assertEqual(r["missing_base"],1)
        self.assertEqual(self.run_cli("--static-model-preservation",
                                     "--required-donor-model",hex(DONOR)),1)

    def test_missing_selected_donor_rejected_but_unlisted_donor_not_required(self):
        pak_file(self.merged,[(HOST,3,True,0xC0FFEE01)])
        self.assertEqual(self.audit()["strict_failures"],0)
        self.assertEqual(self.audit({DONOR})["missing_donor"],1)
        self.assertEqual(self.run_cli("--static-model-preservation"),0)
        self.assertEqual(self.run_cli("--static-model-preservation",
                                     "--required-donor-model",hex(DONOR)),1)

    def test_missing_donor_source_and_duplicate_target(self):
        self.assertEqual(self.audit({0xDD030001})["missing_donor_source"],1)
        pak_file(self.merged,[(HOST,3,True,0xC0FFEE01),
                              (DONOR,1,True,0xC0FFEE02),
                              (DONOR,1,True,0xC0FFEE02)])
        self.assertEqual(self.audit({DONOR})["ambiguous_key"],1)

    def test_donor_crc_already_host_is_not_fake_import_proof(self):
        pak_file(self.donor,[(HOST,3,True,0xC0FFEE03)])
        # The merged level still has host CRC, but that alone does not
        # establish the donor's separate model/geometry was imported.
        self.assertEqual(self.audit({HOST})["target_already_host"],1)
        self.assertEqual(self.run_cli("--static-model-preservation",
                                     "--required-donor-model",hex(HOST)),1)

    def test_renamed_selected_donor_requires_explicit_map(self):
        pak_file(self.merged,[(HOST,3,True,0xC0FFEE01),
                              (RENAMED,7,True,0xC0FFEE02)])
        self.assertEqual(self.audit({DONOR})["missing_donor"],1)
        self.assertEqual(self.audit({DONOR},{DONOR:RENAMED})["strict_failures"],0)
        map_path=self.root/"key-map.json"
        map_path.write_text(json.dumps({hex(DONOR):hex(RENAMED)}))
        self.assertEqual(self.run_cli("--static-model-preservation",
                                     "--required-donor-model",hex(DONOR),
                                     "--model-key-map",str(map_path)),0)

    def test_host_mask_and_mesh_are_preserved(self):
        pak_file(self.merged,[(HOST,1,False,0),(DONOR,1,True,0xC0FFEE02)])
        r=self.audit({DONOR})
        self.assertEqual(r["dropped_base_mask_bits"],1)
        self.assertEqual(r["lost_base_mesh"],1)
        self.assertEqual(r["strict_failures"],2)

    def test_required_donor_mesh_and_mask_must_work(self):
        pak_file(self.merged,[(HOST,3,True,0xC0FFEE01),
                              (DONOR,0,False,0)])
        r=self.audit({DONOR})
        self.assertEqual(r["lost_donor_mesh"],1)
        self.assertEqual(r["zero_donor_mask"],1)

    def test_imported_rendered_zero_asset_key_is_failure(self):
        pak_file(self.merged,[(HOST,3,True,0xC0FFEE01),
                              (DONOR,1,True,0)])
        self.assertEqual(self.audit({DONOR})["zero_merged_asset_key"],1)

    def test_zero_source_key_and_truncated_model_table_rejected(self):
        pak_file(self.base,[(0,3,False,0)])
        self.assertEqual(self.audit()["zero_source_key"],1)
        bh,bb=unpack(self.base)
        malformed=list(bh)
        malformed[44]=1000000
        with self.assertRaises(ValueError):
            snapshot_models(tuple(malformed),bb)

    def test_opt_in_strict_target_metadata_has_real_exit_code(self):
        pak_file(self.merged,[
            (0,0,False,0),(HOST,3,False,0),(HOST,3,False,0)
        ],textures=[(0,0)],effects=[(0,0)])
        # Default behavior is backward compatible: warnings, no hard fail.
        self.assertEqual(self.run_cli("--deep"),0)
        self.assertEqual(self.run_cli("--strict-native-metadata"),1)
        out=StringIO()
        with redirect_stdout(out):
            self.assertEqual(pre.main(["--base",str(self.merged),
                                       "--strict-native-metadata","--json"]),1)
        data=json.loads(out.getvalue())
        self.assertTrue(any("STRICT NATIVE METADATA models" in x
                            for x in data["files"]["base"]["failures"]))

    def test_require_flags_validate_user_input(self):
        bad=[
            ("--required-donor-model",hex(DONOR)),
            ("--model-key-map","whatever.json"),
            ("--static-model-preservation",),
            ("--static-model-preservation","--required-donor-model","0"),
            ("--static-model-preservation","--required-donor-model","0x100000000"),
        ]
        for flags in bad:
            with self.subTest(flags=flags):
                with redirect_stderr(StringIO()),self.assertRaises(SystemExit) as ctx:
                    pre.main(["--base",str(self.base),*flags])
                self.assertEqual(ctx.exception.code,2)

    def test_read_only_and_full_bin_validation_can_be_composed(self):
        original=[p.read_bytes() for p in (self.base,self.donor,self.merged)]
        self.run_cli("--static-model-preservation","--required-donor-model",hex(DONOR),
                     "--native-tables","--deep")
        self.assertEqual([p.read_bytes() for p in (self.base,self.donor,self.merged)],
                         original)
        src=(TOOL/"pak_pool_preflight.py").read_text("utf-8")
        self.assertIn('if args.static_model_preservation',src)
        self.assertIn('if args.strict_native_metadata',src)
        self.assertIn('"static_model_preservation": model_preservation',src)
        wf=(ROOT/".github/workflows/static-map-pak-preflight.yml").read_text("utf-8")
        self.assertEqual(wf.count("'tests/test_static_model_preservation.py'"),2)
        self.assertIn("test_static_model_preservation.py -v",wf)


if __name__=="__main__":
    unittest.main()
