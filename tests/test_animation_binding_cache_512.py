#!/usr/bin/env python3
"""Native x86 two-level animation binding cache 64-to-256/512 patch contracts.

The real PE32 code windows were recovered from ConquestLLC.exe SHA-256
3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858.
No proprietary EXE or Ghidra project is copied into the public repository.
"""
import re
import struct
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
CPP=(ROOT/"src/animation_binding_cache_512.cpp").read_text("utf-8")
HEADER=(ROOT/"src/animation_binding_cache_512.h").read_text("utf-8")
PROXY=(ROOT/"src/d3d9proxy.cpp").read_text("utf-8")
CONFIG=(ROOT/"src/proxy_config.cpp").read_text("utf-8")
BUILD=(ROOT/"build.bat").read_text("utf-8")
CMAKE=(ROOT/"CMakeLists.txt").read_text("utf-8")
WF=(ROOT/".github/workflows/static-map-pak-preflight.yml").read_text("utf-8")


def fixture(name):
    match=re.search(r"\b"+re.escape(name)+r"\[\]\s*=\s*\{([^}]+)\}",CPP,re.S)
    if not match:
        raise AssertionError("missing original x86 machine bytes: "+name)
    without_comments=re.sub(r"//[^\n]*","",match.group(1))
    return bytes(int(word,16) for word in re.findall(r"\b0x([0-9a-fA-F]{1,2})\b",without_comments))


class NativeAnimationBindingCache(unittest.TestCase):
    def test_real_outer_method_is_exact_64_reserve_and_ends_in_gate(self):
        self.assertEqual(fixture("kRegReserveBefore"),
                         bytes.fromhex("53 56 6a 40 8b f1 5b 66 39 5e 0e 74 5b"))
        self.assertEqual(fixture("kRegReserveTail"),
                         bytes.fromhex("8b ce 5e 5b e9 2f 61 fa ff"))
        self.assertIn("kRegReserveMethod=0x008F90BC",CPP)
        self.assertIn("kRegReserveContinuation=0x0089F25C",CPP)
        self.assertEqual((0x008F9128+5+struct.unpack("<i",bytes.fromhex("2f 61 fa ff"))[0]),
                         0x0089F25C)

    def test_real_outer_vtable_and_level_setup_use_original_method(self):
        self.assertEqual(fixture("kBindingVtableBefore"),bytes.fromhex("bc 90 8f 00"))
        self.assertIn("kBindingVtableSlot=0x009C7A24",CPP)
        self.assertEqual(fixture("kBindingSetupBefore"),
                         bytes.fromhex("8b 0d 5c 7e cd 00 8b 01 ff 50 08"))
        self.assertIn("kBindingSetupCaller=0x00917E23",CPP)

    def test_inner_map_call_is_isolated_from_gfx_shared_helper(self):
        self.assertEqual(fixture("kMapCallContextBefore"),
                         bytes.fromhex("8b f3 e8 c5 21 b1 ff 8b 45 0c"))
        self.assertEqual((0x008F9301+5+struct.unpack("<i",bytes.fromhex("c5 21 b1 ff"))[0]),
                         0x0040B4CB)
        self.assertEqual(fixture("kSharedReserve40Before"),
                         bytes.fromhex("51 53 57 6a 40 5b 66 39 5e 02"))
        self.assertIn("kAnimMapCall=0x008F9301",CPP)
        self.assertIn("kSharedReserve40=0x0040B4CB",CPP)
        self.assertNotIn("WriteBytes(kSharedReserve40,",CPP)
        self.assertNotIn("0x00876E6F,k",CPP)

    def test_real_native_generic_reserve8_not_a_new_allocator(self):
        self.assertEqual(fixture("kNativeReserve8Before"),
                         bytes.fromhex("66 39 7e 02 53 0f 84 67 00 00 00"))
        self.assertIn("kNativeReserve8=0x00EA3A30",CPP)
        self.assertIn("0xB8,0x30,0x3A,0xEA,0x00",CPP)
        self.assertNotIn("new BYTE[",CPP)
        self.assertNotIn("WriteBytes(kNativeReserve8,",CPP)

    def test_outer_stub_preserves_registers_flags_and_setup_gate(self):
        for token in ("0x9C,0x60,0x8D,0x71,0x0C",
                      "0xBF,0,0,0,0",
                      "0xFF,0xD0", "0x61,0x9D",
                      "0xB8,0x5C,0xF2,0x89,0x00",
                      "0xFF,0xE0",
                      "Put32(reg+6,cap)",
                      "static_assert(sizeof(reg)==26"):
            self.assertIn(token,CPP)

    def test_inner_stub_restores_state_before_return_to_append(self):
        for token in ("0x9C,0x60,0x8B,0xF3",
                      "0x61,0x9D,0xC3",
                      "Put32(map+5,cap)",
                      "static_assert(sizeof(map)==19"):
            self.assertIn(token,CPP)

    def test_patch_exact_instruction_boundaries_and_rollback(self):
        for token in ("BYTE regJmp[7]={0xE9,0,0,0,0,0x90,0x90}",
                      "BYTE mapCall[5]={0xE8,0,0,0,0}",
                      "PutRel32(regJmp+1,kRegReserveMethod+5",
                      "PutRel32(mapCall+1,kAnimMapCall+5",
                      "kRegReserveMethod,kRegReserveBefore",
                      "kAnimMapCall,kMapCallContextBefore+2",
                      "PAGE_EXECUTE_READWRITE",
                      "FlushInstructionCache(",
                      "g_stubPage=code"):
            self.assertIn(token,CPP)

    def test_pre_level_and_exact_binary_fail_closed(self):
        for token in ("kImageBase=0x00400000",
                      "BytesEqual(kBindingVtableSlot",
                      "BytesEqual(kBindingSetupCaller",
                      "BeforeBindings()",
                      "kBindingRegistryGlobal=0x00CD7E5C",
                      "kLevelGlobal=0x00CD7EA0",
                      "kResourceManagerGlobal=0x00CD80D8",
                      "InterlockedCompareExchange(&g_attempted,1,0)"):
            self.assertIn(token,CPP)

    def test_both_new_options_are_disabled_by_default_and_high_wins(self):
        self.assertIn("kCap256=0x100",CPP)
        self.assertIn("kCap512=0x200",CPP)
        self.assertIn('"ExpandAnimationBindingCache256",0,ini',CPP)
        self.assertIn('"ExpandAnimationBindingCache512",0,ini',CPP)
        self.assertIn("if (!high && !low) return",CPP)
        self.assertIn("Install(high?kCap512:kCap256)",CPP)
        for key in ("ExpandAnimationBindingCache256=0",
                    "ExpandAnimationBindingCache512=0"):
            self.assertIn(key,CONFIG)

    def test_gfx_signature_preflight_runs_before_animation_changes(self):
        self.assertIn('#include "animation_binding_cache_512.h"',PROXY)
        self.assertIn("OC_AnimationBindingCache512_Init();",HEADER)
        self.assertIn("OC_AnimationBindingCache512_Init();",PROXY)
        self.assertLess(PROXY.index("OC_GfxRegistry512_Init();"),
                        PROXY.index("OC_AnimationBindingCache512_Init();"))
        self.assertLess(PROXY.index("OC_AnimationBindingCache512_Init();"),
                        PROXY.index("OC_StaticPoolCensus_Init();"))

    def test_all_three_build_paths_include_native_cpp(self):
        self.assertIn("animation_binding_cache_512.cpp",BUILD)
        self.assertIn("animation_binding_cache_512.obj",BUILD)
        self.assertIn("src/animation_binding_cache_512.cpp",CMAKE)
        self.assertIn("animation_binding_cache_512.cpp geometry_budget_telemetry.cpp",WF)

    def test_ci_and_docs_are_always_present(self):
        self.assertIn("test_animation_binding_cache_512.py -v",WF)
        self.assertEqual(WF.count("'src/animation_binding_cache_512.cpp'"),2)
        self.assertEqual(WF.count("'src/animation_binding_cache_512.h'"),2)
        self.assertEqual(WF.count("'tests/test_animation_binding_cache_512.py'"),2)
        doc=(ROOT/"ANIMATION_BINDING_CACHE_512.md").read_text("utf-8")
        for word in ("0x008F90BC","0x008F9301","0x0040B4CB",
                     "0x00EA3A30","FUN_0089F25C","Havok","512 wins",
                     "not cross-thread atomic"):
            # Docs may use uppercase or lowercase for emphasis.
            self.assertIn(word.lower(),doc.lower())

    def test_no_second_world_or_unsafe_blend_stride_patch(self):
        self.assertNotIn("PAK_WRITE",CPP)
        self.assertNotIn("LoadSecondWorld",CPP)
        self.assertNotIn("WriteBytes(0x0045549c",CPP)


if __name__=="__main__":
    unittest.main()
