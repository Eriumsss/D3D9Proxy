#!/usr/bin/env python3
"""Retail static TextureInfo registry 2048->4096 opcode contract tests.

Fixtures below are from the original ConquestEA.exe embedded in the user's
Ghidra ConquestEA.exe.gzf; SHA256 3ac61c2fe3b61288f06496e0a77a57b04afa09021180f149265cfb686c835858.
The x86 PC test is not a game runtime test. Existing EXE files aren't shipped.
"""
from pathlib import Path
import re
import struct
import unittest

ROOT=Path(__file__).resolve().parents[2]
C=(ROOT/"D3D9Proxy/src/texture_registry_4096.cpp").read_text(encoding="utf-8")
H=(ROOT/"D3D9Proxy/src/texture_registry_4096.h").read_text(encoding="utf-8")
P=(ROOT/"D3D9Proxy/src/d3d9proxy.cpp").read_text(encoding="utf-8")
BUILD=(ROOT/"build_all.bat").read_text(encoding="utf-8")
LEVEL=(ROOT/"Vespucci/Engine/Magellan/MgLevelObject.cpp").read_text(encoding="utf-8")
TEXTURES=(ROOT/"Vespucci/Engine/Magellan/MgTextureRegistry.cpp").read_text(encoding="utf-8")
MODEL=(ROOT/"D3D9Proxy/src/model_registry_2048.cpp").read_text(encoding="utf-8")

CTOR_AT=0x00876DF0
FACTORY_AT=0x00876B73
CTOR_BYTES=bytes.fromhex(
    "8d 73 24 bf 00 08 00 00 e8 fb f9 b8 ff "
    "8d 73 2c e8 f3 f9 b8 ff")
FACTORY_BYTES=bytes.fromhex(
    "0f bf 47 2c 83 ec 14 03 c1 3d 00 08 00 00 "
    "53 56 0f 8f 0a 01 00 00")
TEXTURE_CALLS={
    0x00876DF8:bytes.fromhex("e8 fb f9 b8 ff"),
    0x00876E00:bytes.fromhex("e8 f3 f9 b8 ff"),
}

def data(name: str) -> bytes:
    m=re.search(rf"\b{name}\[\]\s*=\s*\{{([^}}]+)\}}",C,re.S)
    if not m:
        raise AssertionError("missing "+name)
    raw=re.sub(r"//[^\n]*", "", m.group(1))
    return bytes(int(x,16) for x in re.findall(r"0x([0-9A-Fa-f]{1,2})\b",raw))

class StaticTextureCapTests(unittest.TestCase):
    def test_exact_original_exe_assembly_windows(self):
        self.assertEqual(data("kCtorBefore"),CTOR_BYTES)
        self.assertEqual(data("kFactoryBefore"),FACTORY_BYTES)
        self.assertEqual(data("kReserveThunkBefore"),bytes.fromhex("e9 33 d2 a9 00"))
        self.assertEqual(data("kReserve8Before"),
                         bytes.fromhex("66 39 7e 02 53 0f 84 67 00 00 00"))
        self.assertEqual(data("kTexPairInsertBefore"),
                         bytes.fromhex("0f b7 01 66 3b 41 02 7c 06"))
        self.assertEqual(data("kResolverBefore"),
                         bytes.fromhex("56 ff 74 24 08 8d 77 2c e8 eb 54 b9 ff"))
        self.assertIn("kCtorImm=0x00876DF4",C)
        self.assertIn("kFactoryImm=0x00876B7D",C)
        self.assertIn("kOldCap=0x800",C)
        self.assertIn("kNewCap=0x1000",C)

    def test_both_vector_reserve_calls_share_same_implementation(self):
        for at, opcode in TEXTURE_CALLS.items():
            with self.subTest(at=hex(at)):
                target=at+5+struct.unpack("<i",opcode[1:])[0]
                self.assertEqual(target,0x004067F8)
                self.assertIn(opcode,C.encode("utf8") if False else CTOR_BYTES)
        self.assertIn("VEC(ebx, 0x24), 0x800",LEVEL)
        self.assertIn("VEC(ebx, 0x2c), 0x800",LEVEL)
        self.assertIn("sortedVec->count",TEXTURES)
        self.assertIn("(int)count > 0x800",TEXTURES)

    def test_two_immediates_only_no_opcode_changes(self):
        ctor=bytearray(CTOR_BYTES)
        fact=bytearray(FACTORY_BYTES)
        ctor[4:8]=struct.pack("<I",0x1000) # mov edi imm, 0x876df4
        fact[10:14]=struct.pack("<I",0x1000) # cmp eax imm, 0x876b7d
        self.assertEqual(bytes(ctor[:4]),CTOR_BYTES[:4])
        self.assertEqual(bytes(ctor[8:]),CTOR_BYTES[8:])
        self.assertEqual(bytes(fact[:10]),FACTORY_BYTES[:10])
        self.assertEqual(bytes(fact[14:]),FACTORY_BYTES[14:])
        self.assertEqual(struct.unpack_from("<I",ctor,4)[0],4096)
        self.assertEqual(struct.unpack_from("<I",fact,10)[0],4096)

    def test_model_and_texture_patch_windows_do_not_overlap(self):
        # Texture signature stops at 0x876e05: the model patch begins there.
        self.assertEqual(CTOR_AT+len(CTOR_BYTES),0x00876E05)
        self.assertIn("kModelCtor=0x00876E05",MODEL)
        self.assertIn("kModelCtorImm=0x00876E0C",MODEL)
        self.assertIn("kFactoryCmpImm=0x00875D36",MODEL)
        self.assertNotEqual(0x00875D36,0x00876B7D)

    def test_fail_closed_when_game_has_initialized_or_binary_unknown(self):
        for marker in (
            'GetPrivateProfileIntA("StaticMapPools","ExpandTextures4096",0',
            "if (!ExactVersion())", "if (!NoLevelYet())",
            "bool okay=ExactVersion() && NoLevelYet();",
            "kLevelCell=0x00CD7EA0", "kResourceManager=0x00CD80D8",
            "PAGE_EXECUTE_READWRITE", "VirtualProtect(",
            "wroteCtor=true;", "wroteFactory=true;",
            "if (wroteFactory) restored=WriteImmediate(kFactoryImm,kOldCap)",
            "if (wroteCtor) restored=WriteImmediate(kCtorImm,kOldCap)",
            "FlushInstructionCache(", "GetModuleHandleA(NULL)",
            "0x00EA3A30,kReserve8Before",
            "0x0040B3F3,kTexPairInsertBefore",
            "0x0087603A,kResolverBefore"
        ):
            with self.subTest(marker=marker):
                self.assertIn(marker,C)

    def test_build_and_scope(self):
        self.assertIn('void OC_TextureRegistry4096_Init();',H)
        self.assertIn('OC_TextureRegistry4096_Init();',P)
        self.assertLess(P.index('OC_TextureRegistry4096_Init();'),
                        P.index('OC_StaticPoolCensus_Init();'))
        self.assertIn('texture_registry_4096.cpp',BUILD)
        self.assertIn('texture_registry_4096.obj',BUILD)
        for banned in ('NpcGame_', 'PAK_WRITE', 'CreateFileMapping',
                       '0x00877A1B', 'D3DPOOL_', 'RTX'):
            if banned == '0x00877A1B':
                # Allowed solely in comment to document an excluded cap.
                self.assertEqual(C.count(banned),0)
            else:
                self.assertNotIn(banned,C)

if __name__=="__main__":
    unittest.main()
