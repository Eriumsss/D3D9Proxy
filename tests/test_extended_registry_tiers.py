#!/usr/bin/env python3
"""GZF-gated x86 registry capacity tier contracts and synthetic PAK assumptions.

These are offline tests. Only the real Windows x86 game can validate a patch.
"""
from pathlib import Path
import importlib.util
import re
import struct
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
TOOL=ROOT/"tools"/"pak_pool_preflight.py"
spec=importlib.util.spec_from_file_location("pak_tier_preflight",TOOL)
assert spec and spec.loader
pre=importlib.util.module_from_spec(spec)
sys.modules[spec.name]=pre
spec.loader.exec_module(pre)

TIERS=[
    ("model","src/model_registry_2048.cpp",0x400,0x800,0x1000,
     "ExpandModels2048","ExpandModels4096","Install",
     ("WriteNewCap(kModelCtorImm,requestedCap)","WriteNewCap(kFactoryCmpImm,requestedCap)")),
    ("texture","src/texture_registry_4096.cpp",0x800,0x1000,0x2000,
     "ExpandTextures4096","ExpandTextures8192","Apply",
     ("WriteImmediate(kCtorImm,requestedCap)","WriteImmediate(kFactoryImm,requestedCap)")),
    ("effect","src/effect_registry_1024.cpp",0x200,0x400,0x800,
     "ExpandEffects1024","ExpandEffects2048","Install",
     ("StoreImmediate(kReserveImmediate,requestedCap)","StoreImmediate(kFactoryImmediate,requestedCap)"))
]


def synthetic_pak(path,counts):
    hdr=[0]*118
    hdr[2]=0x13
    block=bytearray()
    for table,(count_key,offset_key,stride,_) in pre.TABLES.items():
        count=counts.get(table,0)
        hdr[pre.IDX[count_key]]=count
        hdr[pre.IDX[offset_key]]=len(block)
        block.extend(bytes(stride*count))
    if not block:
        block.extend(bytes(16))
    hdr[pre.IDX["block1_offset"]]=472
    hdr[pre.IDX["block1_size"]]=len(block)
    path.write_bytes(struct.pack("<118I",*hdr)+block)
    return path


class HigherRegistryTiers(unittest.TestCase):
    def test_real_constructor_and_factory_immediates_use_requested_cap(self):
        for label,path,old,lower,higher,low_cfg,high_cfg,func,sites in TIERS:
            source=(ROOT/path).read_text("utf-8")
            with self.subTest(registry=label):
                self.assertIn("kOldCap=0x%X"%old,source)
                self.assertIn("kNewCap=0x%X"%lower,source)
                self.assertIn("kHigherCap=0x%X"%higher,source)
                for expr in sites:
                    self.assertIn(expr,source)
                self.assertIn("requestedCap!=kNewCap && requestedCap!=kHigherCap",source)
                self.assertIn(func+"(high?kHigherCap:kNewCap)",source)
                self.assertIn('GetPrivateProfileIntA("StaticMapPools","'+low_cfg+'",0,ini)',source)
                self.assertIn('GetPrivateProfileIntA("StaticMapPools","'+high_cfg+'",0,ini)',source)
                self.assertIn("if (!high && !low) return;",source)

    def test_all_registry_caps_fit_signed_smallvec_header(self):
        for name,path,old,low,high,*_ in TIERS:
            with self.subTest(name=name):
                self.assertLess(old,low)
                self.assertLess(low,high)
                self.assertLessEqual(high,32767)
        self.assertEqual(3*4096*8,98304)
        self.assertEqual(2*8192*8,131072)
        self.assertEqual(3*2048*8,49152)

    def test_old_opcode_signatures_and_fail_closed_guards_remain(self):
        for _,path,*_ in TIERS:
            data=(ROOT/path).read_text("utf-8")
            with self.subTest(file=path):
                self.assertIn("PAGE_EXECUTE_READWRITE",data)
                self.assertIn("FlushInstructionCache(",data)
                self.assertIn("VirtualProtect(",data)
                self.assertIn("InterlockedCompareExchange(",data)
                self.assertIn("kOldCap",data)
                self.assertIn("requestedCap",data)

    def setUp(self):
        tmp=tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.pak=synthetic_pak(Path(tmp.name)/"large.pak",
            {"models":3500,"textures":6500,"effects":1500})

    def test_retail_caps_reject_merged_counts(self):
        report=pre.inspect_pak(self.pak)
        self.assertTrue(any(s.startswith("models:") for s in report.failures))
        self.assertTrue(any(s.startswith("textures:") for s in report.failures))
        self.assertTrue(any(s.startswith("effects:") for s in report.failures))

    def test_low_caps_still_reject_high_tier_counts(self):
        report=pre.inspect_pak(self.pak,assume_caps={"models":2048,"textures":4096,"effects":1024})
        self.assertEqual(len(report.failures),3)

    def test_high_caps_accept_supported_record_counts(self):
        report=pre.inspect_pak(self.pak,assume_caps={"models":4096,"textures":8192,"effects":2048})
        self.assertEqual(report.failures,[])
        self.assertEqual(report.caps["models"],4096)
        self.assertEqual(report.caps["textures"],8192)
        self.assertEqual(report.caps["effects"],2048)

    def test_cli_high_tier_whatif(self):
        self.assertEqual(pre.main(["--base",str(self.pak),
          "--assume-model4096","--assume-texture8192","--assume-effects2048","--json"]),0)

    def test_cli_high_tiers_override_lower_assumptions(self):
        self.assertEqual(pre.main(["--base",str(self.pak),
          "--assume-model2048","--assume-model4096",
          "--assume-texture4096","--assume-texture8192",
          "--assume-effects1024","--assume-effects2048","--json"]),0)

    def test_new_tier_no_gfx_pfield_patch_claim(self):
        doc=(ROOT/"REGISTRY_CAPACITY_TIERS.md").read_text("utf-8")
        self.assertIn("Neither GFX nor PField is changed",doc)


if __name__=="__main__":
    unittest.main()
