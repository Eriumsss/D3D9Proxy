#!/usr/bin/env python3
"""Native D3D device-reset 64-slot pool expansion x86 regression contracts."""
from pathlib import Path
import re
import struct
import unittest

ROOT=Path(__file__).resolve().parents[1]
CODE=(ROOT/"src/device_reset_pool_4096.cpp").read_text("utf-8")
PROXY=(ROOT/"src/d3d9proxy.cpp").read_text("utf-8")
BUILD=(ROOT/"build.bat").read_text("utf-8")
CMAKE=(ROOT/"CMakeLists.txt").read_text("utf-8")


def fixture(name):
    m=re.search(r"\b"+re.escape(name)+r"\[\]\s*=\s*\{([^}]+)\}",CODE,re.S)
    if not m:
        raise AssertionError("Missing code fingerprint "+name)
    block=re.sub(r"//[^\n]*","",m.group(1))
    return bytes(int(h,16) for h in re.findall(r"\b0x([0-9a-fA-F]{1,2})\b",block))


class DeviceResetNativePool(unittest.TestCase):
    def test_exact_boot_allocates_original_256_bytes(self):
        self.assertEqual(fixture("kBootBytesBefore"),bytes.fromhex(
            "6a 00 68 00 01 00 00 6a 00 e8 2d 6a d1 ff "
            "68 2d f7 96 00 a3 38 15 a4 00"))
        self.assertEqual(struct.unpack_from("<I",fixture("kBootBytesBefore"),3)[0],256)
        self.assertIn("kBootBytesImm=0x00967C2A",CODE)

    def test_unrelated_256_byte_global_allocation_is_untouched(self):
        self.assertEqual(fixture("kSiblingBootBefore"),bytes.fromhex(
            "6a 00 68 00 01 00 00 6a 00 e8 4e 6a d1 ff"))
        self.assertIn("Match(kSiblingBoot,kSiblingBootBefore",CODE)
        self.assertNotIn("Store(kSiblingBoot,",CODE)

    def test_exact_original_unbounded_append(self):
        self.assertEqual(fixture("kAppendBefore"),bytes.fromhex(
            "a1 3c 15 a4 00 8b 0d 38 15 a4 00"))
        self.assertEqual(fixture("kAppendAfter"),bytes.fromhex(
            "89 3c 81 ff 05 3c 15 a4 00 8a 47 50 5f c3"))
        self.assertIn("kAppendOriginal=0x0068D8AE",CODE)
        self.assertIn("kAppendReturn=0x0068D8B7",CODE)

    def test_original_unregister_and_reset_walk_reuse_global_pointer(self):
        self.assertEqual(fixture("kUnregisterBefore"),bytes.fromhex(
            "56 8b f1 e8 b7 fe ff ff 85 c0 75 36"))
        self.assertEqual(fixture("kResetWalkBefore"),bytes.fromhex(
            "a1 3c 15 a4 00 56 8b 35 38 15 a4 00"))

    def test_native_atexit_free_owns_reallocated_base(self):
        self.assertEqual(fixture("kAtexitBefore"),bytes.fromhex(
            "ff 35 38 15 a4 00 83 25 3c 15 a4 00 00 "
            "e8 64 ee d0 ff"))
        self.assertIn("InterlockedExchange((volatile LONG*)(uintptr_t)kBaseCell",CODE)
        self.assertNotIn("VirtualFree((LPVOID)original,",CODE)

    def test_original_game_allocator_realloc_verified(self):
        self.assertEqual(fixture("kReallocBefore"),bytes.fromhex(
            "51 56 6a 02 58 e8 87 d1 ff ff ff 74 24 14"))
        self.assertIn("typedef void* (__cdecl *NativeRealloc)(void*,DWORD,DWORD)",CODE)
        self.assertIn("expanded=fn((void*)(uintptr_t)original,bytes,0)",CODE)

    def test_two_tiers_1024_4096_default_off(self):
        for token in ("kCap1024=0x400","kCap4096=0x1000",
                      '"ExpandDeviceResetResources1024",0,ini',
                      '"ExpandDeviceResetResources4096",0,ini',
                      "if (!high && !low) return",
                      "Install(high?kCap4096:kCap1024)"):
            self.assertIn(token,CODE)

    def test_guard_trampoline_branches_to_correct_offsets(self):
        stub=bytes.fromhex(
            "8b 0d 38 15 a4 00 85 c9 74 11 "
            "a1 3c 15 a4 00 3d 00 10 00 00 73 05 "
            "e9 00 00 00 00 c6 47 51 00 e9 00 00 00 00")
        self.assertEqual(len(stub),36)
        self.assertEqual(10+struct.unpack_from("b",stub,9)[0],27)
        self.assertEqual(22+struct.unpack_from("b",stub,21)[0],27)
        self.assertIn("Put32(stub+16,cap)",CODE)
        self.assertIn("PutRel32(stub+23,",CODE)
        self.assertIn("PutRel32(stub+32,",CODE)

    def test_original_append_is_resumed_not_reimplemented(self):
        self.assertIn("PutRel32(jmp+1,kAppend+5",CODE)
        self.assertIn("PutRel32(stub+23,",CODE)
        self.assertIn("kAppendOriginal=0x0068D8AE",CODE)
        self.assertIn("kAppendReturn=0x0068D8B7",CODE)
        self.assertIn("0xC6,0x47,0x51,0x00",CODE)

    def test_pre_level_no_live_entries_gate(self):
        for x in ("return count==0","originalBase2==originalBase",
                  "VersionMatches() && ReadState(originalBase2,count2)",
                  "if (good) good=ResizeExistingList(originalBase,desiredBytes)"):
            self.assertIn(x,CODE)

    def test_protection_and_rollback(self):
        for token in ("PAGE_EXECUTE_READWRITE", "PAGE_EXECUTE_READ",
                      "FlushInstructionCache(", "VirtualFree(stub,0,MEM_RELEASE)",
                      "Store(kBootBytesImm,kBootBytesBefore+3,4)",
                      "Store(kAppend,kAppendBefore", "g_executableStub=stub"):
            self.assertIn(token,CODE)

    def test_build_link_integration(self):
        header=(ROOT/"src/device_reset_pool_4096.h").read_text("utf-8")
        self.assertIn("OC_DeviceResetPool4096_Init();",header)
        self.assertIn("OC_DeviceResetPool4096_Init();",PROXY)
        self.assertIn("device_reset_pool_4096.cpp",BUILD)
        self.assertIn("device_reset_pool_4096.obj",BUILD)
        self.assertIn("src/device_reset_pool_4096.cpp",CMAKE)

    def test_docs_distinguish_havok_and_managed_buffers(self):
        text=(ROOT/"DEVICE_RESET_POOL_4096.md").read_text("utf-8")
        for word in ("Havok","D3DPOOL_MANAGED","0x00967C27",
                     "0x0068D8A3","4096","REJECT"):
            self.assertIn(word,text)


if __name__=="__main__":
    unittest.main()
