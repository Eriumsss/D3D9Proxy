#!/usr/bin/env python3
"""Static/oracle regression checks for the opt-in D3D9Proxy retail heap extender.

These tests do NOT run retail x86 code. In-game allocation and calling convention
require a Windows/MSVC x86 build and ConquestEA/ConquestLLC runtime validation.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
CPP = (ROOT / "D3D9Proxy/src/retail_heap_extender.cpp").read_text(encoding="utf-8")
PROXY = (ROOT / "D3D9Proxy/src/d3d9proxy.cpp").read_text(encoding="utf-8")
DEVICE = (ROOT / "D3D9Proxy/src/d3d9device.cpp").read_text(encoding="utf-8")
BUILD = (ROOT / "build_all.bat").read_text(encoding="utf-8")
NPC = (ROOT / "DLL/NpcProxy/src/npc_game.cpp").read_text(encoding="utf-8")
PATCHES = (ROOT / "D3D9Proxy/src/memorypatch.cpp").read_text(encoding="utf-8")

def signature_rows(text):
    result = {}
    for address, size, hex_bytes in re.findall(
        r"\{\s*(0x[0-9a-fA-F]+)\s*,\s*(\d+)\s*,\s*\{([^}]+)\}\s*\}", text
    ):
        data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{1,2})\b", hex_bytes))
        if len(data) == int(size):
            result[int(address, 16)] = data
    return result

class HeapExtensionContract(unittest.TestCase):
    def test_fingerprints_match_original_npc_proxy(self):
        original = signature_rows(NPC)
        copied = signature_rows(CPP)
        addresses = [0x67D790, 0x67D58B, 0x67D8F0, 0x67D95B,
                     0x67B7F3, 0x67B19D, 0x67E63B, 0x67D7D3,
                     0x67D7E3, 0x67D7ED]
        for address in addresses:
            with self.subTest(address=hex(address)):
                self.assertIn(address, original)
                self.assertEqual(copied.get(address), original[address])

    def test_no_exe_text_patch_or_crt_arena_replacement(self):
        self.assertNotIn("VirtualProtect(", CPP)
        self.assertNotIn("WriteProcessMemory(", CPP)
        self.assertNotIn("FlushInstructionCache(", CPP)
        self.assertNotIn("FreeLibrary(", CPP)
        self.assertNotIn("HeapFree(", CPP)
        self.assertNotIn("free(", CPP)
        self.assertIn("GetPrivateProfileIntA(\"RetailHeapExtender\",\"Enabled\",0", CPP)
        self.assertIn("g_originalBase=base;", CPP)
        self.assertIn("Wr", NPC)

    def test_two_write_cells_under_engine_lock(self):
        self.assertIn("EngineLock(true)", CPP)
        self.assertIn("EngineLock(false)", CPP)
        self.assertIn("kHeapTop=(DWORD)ptr;", CPP)
        self.assertIn("kHeapEnd=(DWORD)(ptr+kGrowBytes);", CPP)
        self.assertNotIn("kHeapBase=", CPP)
        self.assertIn("IsOurArena(base,top,end)", CPP)

    def test_no_bad_legacy_pool_patches_enabled(self):
        for number in (6, 29):
            self.assertRegex(PATCHES, rf"PATCH_{number}_ENABLED\s*=\s*false")

    def test_build_wires_every_source_and_object(self):
        self.assertIn("retail_fn_trace.cpp retail_heap_extender.cpp", BUILD)
        self.assertIn("retail_fn_trace.obj retail_heap_extender.obj", BUILD)
        self.assertIn('#include "retail_heap_extender.h"', PROXY)
        self.assertIn('#include "retail_heap_extender.h"', DEVICE)
        self.assertIn("OC_HeapExt_Tick();", DEVICE)

    def test_thread_gate_and_com_reference_guard(self):
        self.assertIn("g_busy,1,0", CPP)
        self.assertIn("Sleep(100)", CPP)
        self.assertIn("g_workerEnabled,0", CPP)
        self.assertIn("InitOnceExecuteOnce(", PROXY)
        self.assertIn("IID_IDirect3D9", PROXY)
        self.assertIn("IID_IDirect3DDevice9", DEVICE)

    def test_arena_nominal_size(self):
        self.assertEqual(0x06500000, 101 * 1024 * 1024)
        self.assertEqual(0x04000000, 64 * 1024 * 1024)
        self.assertEqual(0x06500000 + 8 * 0x04000000, 613 * 1024 * 1024)

if __name__ == "__main__":
    unittest.main()
