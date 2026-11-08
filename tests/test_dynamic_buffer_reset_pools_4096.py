#!/usr/bin/env python3
"""Original Conquest x86 byte contracts for two dynamic VB/IB reset registries."""
from pathlib import Path
import re
import struct
import unittest

ROOT=Path(__file__).resolve().parents[1]
SRC=(ROOT/"src/dynamic_buffer_reset_pools_4096.cpp").read_text("utf-8")
HEADER=(ROOT/"src/dynamic_buffer_reset_pools_4096.h").read_text("utf-8")
PROXY=(ROOT/"src/d3d9proxy.cpp").read_text("utf-8")
BUILD=(ROOT/"build.bat").read_text("utf-8")
CMAKE=(ROOT/"CMakeLists.txt").read_text("utf-8")


def asm_fixture(name):
    m=re.search(r"\b"+re.escape(name)+r"\[\]\s*=\s*\{([^}]+)\}",SRC,re.S)
    if not m:
        raise AssertionError("missing machine-code signature: "+name)
    cleaned=re.sub(r"//[^\n]*","",m.group(1))
    return bytes(int(s,16) for s in re.findall(r"\b0x([0-9a-fA-F]{1,2})\b",cleaned))


class DynamicBufferResetPools(unittest.TestCase):
    def test_original_vb_boot_is_64_pointers(self):
        boot=asm_fixture("kVBBoot")
        self.assertEqual(boot[:14],bytes.fromhex("6a 00 68 00 01 00 00 6a 00 e8 6f 6a d1 ff"))
        self.assertEqual(struct.unpack_from("<I",boot,3)[0],256)
        self.assertIn("0x00967BE5,kVBBoot",SRC)

    def test_original_ib_boot_is_64_pointers(self):
        boot=asm_fixture("kIBBoot")
        self.assertEqual(boot[:14],bytes.fromhex("6a 00 68 00 01 00 00 6a 00 e8 4e 6a d1 ff"))
        self.assertEqual(struct.unpack_from("<I",boot,3)[0],256)
        self.assertIn("0x00967C06,kIBBoot",SRC)

    def test_original_vb_and_ib_append_have_no_guard(self):
        self.assertEqual(asm_fixture("kVBRegisterPrefix"),bytes.fromhex(
            "a1 24 15 a4 00 8b 0d 20 15 a4 00"))
        self.assertEqual(asm_fixture("kIBRegisterPrefix"),bytes.fromhex(
            "a1 30 15 a4 00 8b 0d 2c 15 a4 00"))
        self.assertEqual(asm_fixture("kVBAppendAndExit"),bytes.fromhex(
            "89 34 81 ff 05 24 15 a4 00 5e c2 04 00"))
        self.assertEqual(asm_fixture("kIBAppendAndExit"),bytes.fromhex(
            "89 34 81 ff 05 30 15 a4 00 5e c2 04 00"))

    def test_native_destructors_own_original_global_pointers(self):
        self.assertEqual(asm_fixture("kVBAtExit")[:6],bytes.fromhex("ff 35 20 15 a4 00"))
        self.assertEqual(asm_fixture("kIBAtExit")[:6],bytes.fromhex("ff 35 2c 15 a4 00"))
        self.assertIn("0x0096F705,kVBAtExit",SRC)
        self.assertIn("0x0096F719,kIBAtExit",SRC)

    def test_original_destroy_uses_pointer_and_count_globals(self):
        self.assertEqual(asm_fixture("kVBUnregister"),bytes.fromhex(
            "a1 20 15 a4 00 8b 0d 24 15 a4 00"))
        self.assertEqual(asm_fixture("kIBUnregister"),bytes.fromhex(
            "a1 2c 15 a4 00 8b 0d 30 15 a4 00"))

    def test_release_and_recreate_reuse_native_globals(self):
        for which in ("VB","IB"):
            self.assertEqual(asm_fixture("k"+which+"Release"),
                             asm_fixture("k"+which+"Recreate"))
        self.assertIn("0x0068DEBC,kVBRelease",SRC)
        self.assertIn("0x0068DB93,kIBRelease",SRC)
        self.assertIn("0x0068DF77,kVBRecreate",SRC)
        self.assertIn("0x0068DC5A,kIBRecreate",SRC)

    def test_native_realloc_bytes_and_signature(self):
        self.assertEqual(asm_fixture("kNativeReallocStart")[:10],bytes.fromhex(
            "51 56 6a 02 58 e8 87 d1 ff ff"))
        self.assertIn("kNativeRealloc = 0x0067E662",SRC)
        self.assertIn("newBase=native((void*)(uintptr_t)original,bytes,0)",SRC)
        self.assertNotIn("VirtualFree((LPVOID)original",SRC)

    def test_original_64_cap_new_1024_and_4096(self):
        for piece in ("kCap1024 = 0x400","kCap4096 = 0x1000",
                      '"ExpandDynamicBufferReset1024",0,ini',
                      '"ExpandDynamicBufferReset4096",0,ini',
                      "if (!high && !low) return",
                      "Install(high?kCap4096:kCap1024)"):
            self.assertIn(piece,SRC)

    def test_both_lists_need_clean_counts_and_inactive_level(self):
        self.assertIn("for (unsigned i=0;i<2;++i)",SRC)
        self.assertIn("g_pools[i].countGlobal!=0",SRC)
        self.assertIn("kResourceManagerCell",SRC)
        self.assertIn("CleanStart(recheck)",SRC)

    def test_trampoline_uses_both_native_globals_and_cap(self):
        for piece in ("PutU32(code+2,p.baseGlobal)","PutU32(code+11,p.countGlobal)",
                      "PutU32(code+16,cap)","PutRel32(code+23,",
                      "PutRel32(code+28,","0x74,0x11","0x73,0x05"):
            self.assertIn(piece,SRC)
        self.assertIn("static_assert(sizeof(code)==32",SRC)

    def test_patch_isolation_from_generic_reset_list(self):
        for needle in ("0x00A41520,0x00A41524", "0x00A4152C,0x00A41530"):
            self.assertIn(needle,SRC)
        self.assertNotIn("0x00A41538",SRC)
        self.assertNotIn("Havok memory cap",SRC)

    def test_original_engine_paths_and_flags_preserved(self):
        for token in ("kVBAppendAndExit","kIBAppendAndExit",
                      "kVBAtExit","kIBAtExit","DISCARD"):
            self.assertIn(token,SRC)
        self.assertNotIn("Store(0x0068DEBC",SRC)
        self.assertNotIn("Store(0x0068DB93",SRC)

    def test_patch_protection_and_rollbacks(self):
        for token in ("PAGE_EXECUTE_READWRITE","PAGE_EXECUTE_READ",
                      "VirtualAlloc(NULL,4096", "FlushInstructionCache(",
                      "Store(g_pools[i].registerAt,g_pools[i].registerSig",
                      "Store(g_pools[i].boot+3,g_pools[i].bootSig+3,4)",
                      "RestoreAll(page)"):
            self.assertIn(token,SRC)

    def test_build_wiring(self):
        self.assertIn("OC_DynamicBufferResetPools4096_Init();",HEADER)
        self.assertIn("OC_DynamicBufferResetPools4096_Init();",PROXY)
        self.assertIn("dynamic_buffer_reset_pools_4096.cpp",BUILD)
        self.assertIn("dynamic_buffer_reset_pools_4096.obj",BUILD)
        self.assertIn("src/dynamic_buffer_reset_pools_4096.cpp",CMAKE)


if __name__=="__main__":
    unittest.main()
