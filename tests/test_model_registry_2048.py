#!/usr/bin/env python3
"""GZF-derived retail ModelInfo pool patch contract tests (no game EXE shipped).

The fixture is the exact pair of instruction windows from the original
ConquestEA.exe file bytes embedded in the user's Ghidra .gzf archive:
SHA-256 3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858.
This test DOES NOT claim that the Windows game was executed successfully.
"""
from pathlib import Path
import re
import struct
import unittest

ROOT=Path(__file__).resolve().parents[1]
# Optional external Magellan/NpcProxy source oracles. The standalone public
# repository never vendors the private OpenConquest engine tree.
import os
ORACLE_ROOT = Path(os.environ.get("OPENCONQUEST_ROOT", str(ROOT.parent / "OpenConquest")))
def read_oracle(relative):
    path = ORACLE_ROOT / relative
    return path.read_text(encoding="utf-8") if path.is_file() else ""
HAS_ORACLE = all((ORACLE_ROOT / p).is_file() for p in ["Vespucci/Engine/Magellan/MgLevelObject.cpp","Vespucci/Engine/Magellan/MgModelRegistry.cpp"])

SOURCE=(ROOT/"src/model_registry_2048.cpp").read_text(encoding="utf-8")
HEADER=(ROOT/"src/model_registry_2048.h").read_text(encoding="utf-8")
PROXY=(ROOT/"src/d3d9proxy.cpp").read_text(encoding="utf-8")
BUILD=(ROOT/"build.bat").read_text(encoding="utf-8")
MAGELLAN_INIT=read_oracle("Vespucci/Engine/Magellan/MgLevelObject.cpp")
MAGELLAN_MODELS=read_oracle("Vespucci/Engine/Magellan/MgModelRegistry.cpp")

CTOR_AT=0x00876E05
GUARD_AT=0x00875D2F
CTOR_BYTES=bytes.fromhex(
    "8d b3 8c 00 00 00 bf 00 04 00 00 c6 83 88 00 00 00 00 "
    "e8 dc f9 b8 ff 8d b3 94 00 00 00")
GUARD_BYTES=bytes.fromhex(
    "0f bf 0b 03 c8 81 f9 00 04 00 00 0f 8f dc 00 00 00")
CALLS={
    0x00876E17:bytes.fromhex("e8 dc f9 b8 ff"),
    0x00876E22:bytes.fromhex("e8 d1 f9 b8 ff"),
    0x00876E2D:bytes.fromhex("e8 c6 f9 b8 ff"),
}

def c_array(name: str) -> bytes:
    match=re.search(rf"\b{name}\[\]\s*=\s*\{{([^}}]+)\}}",SOURCE,re.S)
    if match is None:
        raise AssertionError(f"missing {name}")
    raw = re.sub(r"//[^\n]*", "", match.group(1))
    return bytes(int(m,16) for m in re.findall(r"0x([0-9a-fA-F]{1,2})\b",raw))

class ModelPool2048Contract(unittest.TestCase):
    def test_actual_two_retail_opcode_windows(self):
        self.assertEqual(c_array("kConstructorBefore"),CTOR_BYTES)
        self.assertEqual(c_array("kFactoryBefore"),GUARD_BYTES)
        self.assertEqual(c_array("kReserveThunkBefore"),bytes.fromhex("e9 33 d2 a9 00"))
        self.assertEqual(c_array("kReserve8Before"),
                         bytes.fromhex("66 39 7e 02 53 0f 84 67 00 00 00"))
        self.assertEqual(c_array("kPairInsertBefore"),
                         bytes.fromhex("0f b7 01 66 3b 41 02 7c 06"))
        self.assertIn("0x00EA3A30,kReserve8Before",SOURCE)
        self.assertIn("0x004067F8,kReserveThunkBefore",SOURCE)
        self.assertIn("0x00404BF2,kPairInsertBefore",SOURCE)
        self.assertEqual(CTOR_BYTES[6:11],bytes.fromhex("bf 00 04 00 00"))
        self.assertEqual(GUARD_BYTES[5:11],bytes.fromhex("81 f9 00 04 00 00"))
        self.assertIn("kModelCtorImm=0x00876E0C",SOURCE)
        self.assertIn("kFactoryCmpImm=0x00875D36",SOURCE)
        self.assertIn("kNewCap=0x800",SOURCE)
        self.assertIn("kOldCap=0x400",SOURCE)

    @unittest.skipUnless(HAS_ORACLE, "requires Magellan/NpcProxy oracle sources; set OPENCONQUEST_ROOT")
    def test_same_target_reserver_for_all_three_model_vectors(self):
        # The CALLs are relative jumps to the SAME 0x004067f8 thunk.
        for addr, raw in CALLS.items():
            with self.subTest(addr=hex(addr)):
                self.assertEqual(addr+5+struct.unpack("<i",raw[1:])[0],0x004067F8)
                self.assertIn(f"0x{addr:08X}",SOURCE)
        for offset in (0x8C,0x94,0x9C):
            self.assertIn(f"VEC(ebx, 0x{offset:x}), 0x400",MAGELLAN_INIT)
        self.assertIn("sortedVec->count",MAGELLAN_MODELS)
        self.assertIn("(int)count > 0x400",MAGELLAN_MODELS)

    def test_atomicity_and_fails_closed(self):
        for marker in (
            'GetPrivateProfileIntA("StaticMapPools","ExpandModels2048",0',
            "if (!ValidateRetail())", "if (!LevelNotConstructed())",
            "bool good=LevelNotConstructed() && ValidateRetail();",
            "PAGE_EXECUTE_READWRITE",
            "attemptedCtor=true;", "attemptedFactory=true;",
            "restoredCtor=WriteNewCap(kModelCtorImm,kOldCap);",
            "restoredFactory=WriteNewCap(kFactoryCmpImm,kOldCap);",
            "FlushInstructionCache(", "VirtualProtect(",
            "kLevelCell=0x00CD7EA0", "kResourceMgrCell=0x00CD80D8",
            "InterlockedCompareExchange(&g_attempted,1,0)",
        ):
            with self.subTest(marker=marker):
                self.assertIn(marker,SOURCE)
        self.assertIn("nothing changed",SOURCE)

    def test_registry_not_heap_or_missions(self):
        self.assertNotIn("CreateFileMapping",SOURCE)
        self.assertNotIn("NpcGame_",SOURCE)
        self.assertNotIn("0x00CD80D4",SOURCE)
        self.assertNotIn("ApplyMemoryPatches(",SOURCE)
        self.assertNotIn("PAK_WRITE",SOURCE)
        self.assertIn("OC_ModelRegistry2048_Init();",PROXY)
        self.assertIn("model_registry_2048.cpp",BUILD)
        self.assertIn("model_registry_2048.obj",BUILD)
        self.assertIn("void OC_ModelRegistry2048_Init();",HEADER)

    def test_patch_preserves_opcode_and_changes_both_limits(self):
        ctor=bytearray(CTOR_BYTES)
        factory=bytearray(GUARD_BYTES)
        # Only four immediate bytes change, never branch/prologue/call opcodes.
        ctor[7:11]=struct.pack("<I",0x800)
        factory[7:11]=struct.pack("<I",0x800)
        self.assertEqual(ctor[:7],CTOR_BYTES[:7])
        self.assertEqual(ctor[11:],CTOR_BYTES[11:])
        self.assertEqual(factory[:7],GUARD_BYTES[:7])
        self.assertEqual(factory[11:],GUARD_BYTES[11:])
        self.assertEqual(int.from_bytes(ctor[7:11],"little"),2048)
        self.assertEqual(int.from_bytes(factory[7:11],"little"),2048)

if __name__=="__main__":
    unittest.main()
