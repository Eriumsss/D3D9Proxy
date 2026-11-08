#!/usr/bin/env python3
"""Exact recovered Conquest x86 GFX runtime patch contracts. No game EXE shipped."""
import importlib.util
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
CPP=(ROOT/"src/gfx_registry_512.cpp").read_text("utf-8")
HDR=(ROOT/"src/gfx_registry_512.h").read_text("utf-8")
PROXY=(ROOT/"src/d3d9proxy.cpp").read_text("utf-8")
BUILD=(ROOT/"build.bat").read_text("utf-8")
CMAKE=(ROOT/"CMakeLists.txt").read_text("utf-8")
SRC=ROOT/"tools/pak_pool_preflight.py"
spec=importlib.util.spec_from_file_location("gfx_preflight_test",SRC)
assert spec and spec.loader
pre=importlib.util.module_from_spec(spec)
sys.modules[spec.name]=pre
spec.loader.exec_module(pre)


def cbytes(name):
    m=re.search(r"\b"+re.escape(name)+r"\[\]\s*=\s*\{([^}]+)\}",CPP,re.S)
    if not m: raise AssertionError("missing x86 bytes: "+name)
    lines=re.sub(r"//[^\n]*","",m.group(1))
    return bytes(int(x,16) for x in re.findall(r"\b0x([0-9a-fA-F]{1,2})\b",lines))


class GfxBlockCapacity(unittest.TestCase):
    def test_exact_original_ctor_context(self):
        self.assertEqual(cbytes("kCtorExpected"),bytes.fromhex("8d b3 1c 01 00 00 e8 57 46 b9 ff"))
        self.assertEqual(cbytes("kCtorNextExpected"),bytes.fromhex("83 a3 44 0a 00 00 00"))
        self.assertIn("kCtorCall=0x00876E6F",CPP)

    def test_exact_original_factory_and_guard(self):
        self.assertEqual(cbytes("kFactoryExpected"),bytes.fromhex(
            "55 8b ec 51 51 53 0f bf 9e 1c 01 00 00 8d 0c 03 "
            "83 f9 40 89 5d f8 0f 8f ab 00 00 00"))
        self.assertEqual(cbytes("kGuardExpected"),bytes.fromhex("83 f9 40 89 5d f8"))
        self.assertIn("kGuardSite=0x00875AC8",CPP)
        self.assertIn("kGuardResume=0x00875ACE",CPP)

    def test_animation_shared_helper_is_not_mutated(self):
        self.assertEqual(cbytes("kAnimationCallerExpected"),bytes.fromhex("e8 c5 21 b1 ff"))
        self.assertEqual(cbytes("kSharedExpected"),bytes.fromhex("51 53 57 6a 40 5b 66 39 5e 02"))
        self.assertIn("kAnimationSharedCaller=0x008F9301",CPP)
        self.assertIn("EqualCode(kAnimationSharedCaller",CPP)
        self.assertNotIn("Store(kSharedReserve40,",CPP)
        self.assertNotIn("Store(kAnimationSharedCaller,",CPP)

    def test_calls_native_reserve8_with_custom_target(self):
        self.assertEqual(cbytes("kReserve8Expected"),bytes.fromhex(
            "66 39 7e 02 53 0f 84 67 00 00 00"))
        self.assertIn("kReserve8=0x00EA3A30",CPP)
        self.assertIn("PutU32(ctor+9,cap)",CPP)
        for byte_pattern in ("0x9C,0x60","0x8D,0xB3,0x1C,0x01,0x00,0x00",
                             "0xB8,0x30,0x3A,0xEA,0x00","0xFF,0xD0,0x61,0x9D,0xC3"):
            self.assertIn(byte_pattern,CPP)

    def test_factory_trampoline_preserves_flags_for_original_jg(self):
        for part in ("0x81,0xF9,0,0,0,0","0x89,0x5D,0xF8",
                     "PutU32(guard+2,cap)","PutRel32(guard+10,",
                     "kGuardResume","0xE9,0,0,0,0,0x90"):
            self.assertIn(part,CPP)
        self.assertNotIn("Store(kFactoryEntry,",CPP)

    def test_ctor_call_modifies_only_own_caller_not_shared_helper(self):
        rel=(0x0040B4CB-(0x00876E6F+5))&0xFFFFFFFF
        self.assertEqual(b"\xe8"+struct.pack("<I",rel),bytes.fromhex("e8 57 46 b9 ff"))
        rel2=(0x0040B4CB-(0x008F9301+5))&0xFFFFFFFF
        self.assertEqual(b"\xe8"+struct.pack("<I",rel2),bytes.fromhex("e8 c5 21 b1 ff"))
        self.assertIn("PutRel32(ctorCall+1,kCtorCall+5",CPP)

    def test_both_capacity_tiers_are_real_and_default_off(self):
        for needle in ("kCap256=0x100","kCap512=0x200",
                       '"ExpandGfx256",0,ini','"ExpandGfx512",0,ini',
                       "if (!ultra && !high && !low) return",
                       "kCap1024=0x400",
                       '"ExpandGfx1024",0,ini',
                       "Install(ultra?kCap1024:(high?kCap512:kCap256))"):
            self.assertIn(needle,CPP)

    def test_installation_fails_closed_and_rolls_back(self):
        for needle in ("BeforeLevelInit() && ExactRetail()",
                       "PAGE_EXECUTE_READWRITE","PAGE_EXECUTE_READ",
                       "VirtualAlloc(NULL,4096","VirtualFree(stub,0,MEM_RELEASE)",
                       "FlushInstructionCache(",
                       "Store(kGuardSite,kGuardExpected",
                       "Store(kCtorCall,kCtorExpected+6",
                       "g_executableStubs=stub"):
            self.assertIn(needle,CPP)

    def test_build_wiring(self):
        self.assertIn("void OC_GfxRegistry512_Init();",HDR)
        self.assertIn("OC_GfxRegistry512_Init();",PROXY)
        self.assertLess(PROXY.index("OC_EffectRegistry1024_Init();"),
                        PROXY.index("OC_GfxRegistry512_Init();"))
        self.assertLess(PROXY.index("OC_GfxRegistry512_Init();"),
                        PROXY.index("OC_StaticPoolCensus_Init();"))
        self.assertIn("gfx_registry_512.cpp",BUILD)
        self.assertIn("gfx_registry_512.obj",BUILD)
        self.assertIn("src/gfx_registry_512.cpp",CMAKE)

    def test_no_map_import_or_second_world(self):
        for forbidden in ("PAK_WRITE","NpcGame_","LoadSecondWorld","MapImport","RTX Remix"):
            self.assertNotIn(forbidden,CPP)

    def test_docs_differentiate_native_vs_animation_pool(self):
        doc=(ROOT/"GFX_REGISTRY_512.md").read_text("utf-8")
        self.assertIn("0x008F9301",doc)
        self.assertIn("0x00876E6F",doc)
        self.assertIn("0x00875AC8",doc)
        self.assertIn("512 wins",doc)


if __name__=="__main__":
    unittest.main()
