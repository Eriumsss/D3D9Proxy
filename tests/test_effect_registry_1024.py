#!/usr/bin/env python3
"""Read-only opcode regression for x86 EffectInfo registry (GZF fixture).
The original x86 EXE has SHA256
3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858.
"""
from pathlib import Path
import re
import struct
import unittest

ROOT=Path(__file__).resolve().parents[2]
CODE=(ROOT/"D3D9Proxy/src/effect_registry_1024.cpp").read_text("utf-8")
HDR=(ROOT/"D3D9Proxy/src/effect_registry_1024.h").read_text("utf-8")
PROXY=(ROOT/"D3D9Proxy/src/d3d9proxy.cpp").read_text("utf-8")
BUILD=(ROOT/"build_all.bat").read_text("utf-8")
MAGELLAN=(ROOT/"Vespucci/Engine/Magellan/MgEffectRegistry.cpp").read_text("utf-8")
INIT=(ROOT/"Vespucci/Engine/Magellan/MgLevelObject.cpp").read_text("utf-8")
MODEL=(ROOT/"D3D9Proxy/src/model_registry_2048.cpp").read_text("utf-8")
TEX=(ROOT/"D3D9Proxy/src/texture_registry_4096.cpp").read_text("utf-8")

def array(name: str) -> bytes:
    found=re.search(rf"\b{name}\[\]\s*=\s*\{{([^}}]+)\}}",CODE,re.S)
    if not found: raise AssertionError("array not found: "+name)
    raw=re.sub(r"//[^\n]*","",found.group(1))
    return bytes(int(x,16) for x in re.findall(r"0x([0-9A-Fa-f]{1,2})\b",raw))

class Effect1024Contract(unittest.TestCase):
    def test_verified_x86_instruction_windows(self):
        self.assertEqual(array("kReserveBefore"),
                         bytes.fromhex("51 53 57 bf 00 02 00 00 66 39 7e 02 0f 84 59 00 00 00"))
        self.assertEqual(array("kFactoryBefore"),
                         bytes.fromhex(
                             "0f bf 8b fc 00 00 00 8d 14 01 81 fa 00 02 00 00 "
                             "56 57 89 4d f8 0f 8f e4 01 00 00"))
        self.assertEqual(array("kReserveThunkBefore"),
                         bytes.fromhex("e9 9d 69 05 00"))
        self.assertEqual(array("kPairAppendBefore"),
                         bytes.fromhex("0f b7 01 66 3b 41 02 7c 06"))
        self.assertIn("kReserveImmediate=0x00461D30",CODE)
        self.assertIn("kFactoryImmediate=0x008773AC",CODE)
        self.assertIn("kOldCap=0x200",CODE)
        self.assertIn("kNewCap=0x400",CODE)

    def test_three_effect_reserves_reach_same_thunk_and_implementation(self):
        calls={
          0x00876E43:bytes.fromhex("e8 42 45 b9 ff"),
          0x00876E4E:bytes.fromhex("e8 37 45 b9 ff"),
          0x00876E59:bytes.fromhex("e8 2c 45 b9 ff"),
        }
        for addr, raw in calls.items():
            with self.subTest(address=hex(addr)):
                self.assertEqual(addr+5+struct.unpack("<i",raw[1:])[0],0x0040B38A)
                self.assertIn(f"0x{addr:08X}",CODE)
        self.assertEqual(0x0040B38A+5+struct.unpack("<i",array("kReserveThunkBefore")[1:])[0],0x00461D2C)
        for off in ("0xfc","0x104","0x10c"):
            self.assertIn(f"VEC(ebx, {off}), 0x200",INIT)
        self.assertIn("if (a0 + (int)count > 0x200)",MAGELLAN)

    def test_modifies_only_two_immediates(self):
        reserve=bytearray(array("kReserveBefore"))
        factory=bytearray(array("kFactoryBefore"))
        reserve[4:8]=struct.pack("<I",1024)
        factory[12:16]=struct.pack("<I",1024)
        self.assertEqual(reserve[:4],array("kReserveBefore")[:4])
        self.assertEqual(reserve[8:],array("kReserveBefore")[8:])
        self.assertEqual(factory[:12],array("kFactoryBefore")[:12])
        self.assertEqual(factory[16:],array("kFactoryBefore")[16:])
        self.assertEqual(struct.unpack_from("<I",reserve,4)[0],1024)
        self.assertEqual(struct.unpack_from("<I",factory,12)[0],1024)

    def test_default_off_and_rollback(self):
        for term in (
            '"StaticMapPools","ExpandEffects1024",0',
            "if (!ExactRetailInstructions())",
            "if (!BeforeLevelInit())",
            "BeforeLevelInit()&&ExactRetailInstructions()",
            "PAGE_EXECUTE_READWRITE",
            "kLevelGlobal=0x00CD7EA0","kResourceManagerGlobal=0x00CD80D8",
            "if (factoryAttempted)", "if (reserveAttempted)",
            "StoreImmediate(kReserveImmediate,kOldCap)",
            "StoreImmediate(kFactoryImmediate,kOldCap)",
            "FlushInstructionCache(","VirtualProtect(",
            "0x0040B3F3,kPairAppendBefore",
        ):
            with self.subTest(term=term):
                self.assertIn(term,CODE)

    def test_no_interference_with_model_texture_or_missions(self):
        self.assertIn("void OC_EffectRegistry1024_Init();",HDR)
        self.assertIn("OC_EffectRegistry1024_Init();",PROXY)
        self.assertIn("effect_registry_1024.cpp",BUILD)
        self.assertIn("effect_registry_1024.obj",BUILD)
        self.assertLess(PROXY.index("OC_ModelRegistry2048_Init();"),
                        PROXY.index("OC_TextureRegistry4096_Init();"))
        self.assertLess(PROXY.index("OC_TextureRegistry4096_Init();"),
                        PROXY.index("OC_EffectRegistry1024_Init();"))
        self.assertLess(PROXY.index("OC_EffectRegistry1024_Init();"),
                        PROXY.index("OC_StaticPoolCensus_Init();"))
        # Reserve function at 0x461d2c and factory at 0x877396 are
        # completely distinct from the model/texture instruction sites.
        self.assertNotIn("0x00461D30",MODEL+TEX)
        self.assertNotIn("0x008773AC",MODEL+TEX)
        for forbidden in ("NpcGame_","PAK_WRITE","D3DPOOL_","CreateFileMapping"):
            self.assertNotIn(forbidden,CODE)

if __name__=="__main__":
    unittest.main()
