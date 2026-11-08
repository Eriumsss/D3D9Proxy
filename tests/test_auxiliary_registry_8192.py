#!/usr/bin/env python3
"""Actual x86 auxiliary pool capacity patch fixtures, from recovered Conquest image."""
import importlib.util
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
CPP=(ROOT/"src/auxiliary_registry_8192.cpp").read_text("utf-8")
HDR=(ROOT/"src/auxiliary_registry_8192.h").read_text("utf-8")
PROXY=(ROOT/"src/d3d9proxy.cpp").read_text("utf-8")
BUILD=(ROOT/"build.bat").read_text("utf-8")
CMAKE=(ROOT/"CMakeLists.txt").read_text("utf-8")
SCRIPT=ROOT/"tools/pak_pool_preflight.py"
spec=importlib.util.spec_from_file_location("auxiliary_preflight",SCRIPT)
assert spec and spec.loader
pre=importlib.util.module_from_spec(spec)
sys.modules[spec.name]=pre
spec.loader.exec_module(pre)


def cbytes(name):
    m=re.search(r"\b"+re.escape(name)+r"\[\]\s*=\s*\{([^}]+)\}",CPP,re.S)
    if not m:raise AssertionError("missing x86 source fixture "+name)
    s=re.sub(r"//[^\n]*","",m.group(1))
    return bytes(int(x,16) for x in re.findall(r"\b0x([0-9a-fA-F]{1,2})\b",s))


class AuxiliaryPoolContracts(unittest.TestCase):
    def test_resource_other_exact_constructor_and_thunk(self):
        self.assertEqual(cbytes("kOtherConstructor"),bytes.fromhex("8d b3 f4 00 00 00 e8 e4 44 b9 ff"))
        self.assertEqual(cbytes("kOtherThunk"),bytes.fromhex("e9 85 94 02 00"))
        self.assertEqual(0x876e38+5+struct.unpack("<i",bytes.fromhex("e4 44 b9 ff"))[0],0x40B321)
        self.assertEqual(0x40B321+5+struct.unpack("<i",bytes.fromhex("85 94 02 00"))[0],0x4347AB)

    def test_resource_other_true_2048_reserve_factory(self):
        self.assertEqual(cbytes("kOtherReserve"),bytes.fromhex("51 53 57 bf 00 08 00 00 66 39 7e 02 0f 84 59 00 00 00"))
        self.assertEqual(cbytes("kOtherGuard"),bytes.fromhex("3d 00 08 00 00 0f 8f 18 05 00 00"))
        self.assertIn("0x004347AF,0x00877A1C,0x800,0x1000,0x2000,0x4000",CPP)

    def test_pfield_exact_constructor_and_thunk(self):
        self.assertEqual(cbytes("kPfConstructor"),bytes.fromhex("8d b3 14 01 00 00 e8 c7 45 b9 ff"))
        self.assertEqual(cbytes("kPfThunk"),bytes.fromhex("e9 92 96 05 00"))
        self.assertEqual(0x876e64+5+struct.unpack("<i",bytes.fromhex("c7 45 b9 ff"))[0],0x40B430)
        self.assertEqual(0x40B430+5+struct.unpack("<i",bytes.fromhex("92 96 05 00"))[0],0x464AC7)

    def test_pfield_native_4096_not_magellan_standin_512(self):
        self.assertEqual(cbytes("kPfReserve"),bytes.fromhex("51 53 57 bf 00 10 00 00 66 39 7e 02 0f 84 59 00 00 00"))
        self.assertEqual(cbytes("kPfGuard"),bytes.fromhex(
            "0f bf 8e 14 01 00 00 8d 14 01 81 fa 00 10 00 00 89 4d fc 0f 8f c6 00 00 00"))
        self.assertIn("0x00464ACB,0x00875C07,0x1000,0x2000,0x2000,0x4000",CPP)
        self.assertEqual(pre.TABLES["pfields"][3],4096)

    def test_all_capacity_patches_touch_reserve_and_guard(self):
        for name,low,high in [("resource_other",4096,8192),("pfield",8192,8192)]:
            with self.subTest(name=name):
                self.assertLessEqual(high,32767)
                self.assertTrue(low>0)
        self.assertIn("WriteCap(spec.reserveImm,cap)",CPP)
        self.assertIn("WriteCap(spec.guardImm,cap)",CPP)

    def test_patch_rejects_live_level_unknown_version_and_bad_config(self):
        for needle in ("NoLevelYet() && Exact(spec)",
                       "cap!=spec.lowCapacity && cap!=spec.highCapacity && cap!=spec.ultraCapacity",
                       "if (cap<=spec.oldCapacity || cap>32767)",
                       "InterlockedCompareExchange(&g_once,1,0)",
                       '"ExpandResourceOther4096",0,ini',
                       '"ExpandResourceOther8192",0,ini',
                       '"ExpandPFields8192",0,ini',
                       '"ExpandPFields16384",0,ini',
                       '"ExpandResourceOther16384",0,ini'):
            self.assertIn(needle,CPP)

    def test_protection_flush_rollback_preserved(self):
        for needle in ("PAGE_EXECUTE_READWRITE","VirtualProtect(",
                       "WriteCap(spec.guardImm,spec.oldCapacity)",
                       "WriteCap(spec.reserveImm,spec.oldCapacity)",
                       "FlushInstructionCache(", "originalGuardProtection",
                       "originalReserveProtection"):
            self.assertIn(needle,CPP)

    def test_original_engine_allocator_unique_for_each_registry(self):
        self.assertIn("0x0040B321,kOtherThunk",CPP)
        self.assertIn("0x0040B430,kPfThunk",CPP)
        self.assertNotIn("CreateFileMapping",CPP)
        self.assertNotIn("PAK_WRITE",CPP)

    def test_proxy_build_wiring(self):
        self.assertIn("OC_AuxiliaryRegistry8192_Init();",HDR)
        self.assertIn("OC_AuxiliaryRegistry8192_Init();",PROXY)
        self.assertIn("auxiliary_registry_8192.cpp",BUILD)
        self.assertIn("auxiliary_registry_8192.obj",BUILD)
        self.assertIn("src/auxiliary_registry_8192.cpp",CMAKE)

    def test_pfield_live_census_cap_known_from_real_exe(self):
        census=(ROOT/"src/static_pool_census.cpp").read_text("utf-8")
        self.assertIn('{"pfield",0x114,0x1000 /* verified native x86 */}',census)

    def test_pak_preflight_optional_pfield_high_cap(self):
        self.assertIn('"pfields": ("pfield_info_num", "pfield_info_offset", 0x14, 0x1000)',(ROOT/"tools/pak_pool_preflight.py").read_text("utf-8"))
        self.assertIn('--assume-pfields8192',(ROOT/"tools/pak_pool_preflight.py").read_text("utf-8"))

    def test_docs_do_not_claim_unproven_resource_pak_table(self):
        doc=(ROOT/"AUXILIARY_REGISTRY_8192.md").read_text("utf-8")
        self.assertIn("no invented",doc)
        self.assertIn("0x00464ACB",doc)


if __name__=="__main__":
    unittest.main()
