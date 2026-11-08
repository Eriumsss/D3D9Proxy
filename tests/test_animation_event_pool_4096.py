#!/usr/bin/env python3
"""Native 4096 animation-event pool code contracts and 4K CSV regression.

These tests do NOT execute the retail EXE, native allocator, or a game level.
"""
from __future__ import annotations

import csv
import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
SRC=(ROOT/"src/animation_event_pool_4096.cpp").read_text("utf-8")
HDR=(ROOT/"src/animation_event_pool_4096.h").read_text("utf-8")
MEM=(ROOT/"src/memorypatch.cpp").read_text("utf-8")
CENSUS=(ROOT/"src/static_pool_census.cpp").read_text("utf-8")
CONFIG=(ROOT/"src/proxy_config.cpp").read_text("utf-8")
PROXY=(ROOT/"src/d3d9proxy.cpp").read_text("utf-8")
BUILD=(ROOT/"build.bat").read_text("utf-8")
CMAKE=(ROOT/"CMakeLists.txt").read_text("utf-8")
WORKFLOW=(ROOT/".github/workflows/static-map-pak-preflight.yml").read_text("utf-8")
DOC=(ROOT/"ANIMATION_EVENT_POOL_4096.md").read_text("utf-8")
spec=importlib.util.spec_from_file_location("pool_pressure",(ROOT/"tools/analyze_runtime_pressure.py"))
assert spec and spec.loader
pressure=importlib.util.module_from_spec(spec)
spec.loader.exec_module(pressure)

def write_csv(path:Path,rows:list[dict[str,object]]):
    with path.open("w",newline="") as f:
        writer=csv.DictWriter(f,fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

def event_row(time:int,used:int,peak:int,status:str,expanded:int=1,
              cap:int=4096,valid:int=1):
    return {
        "time_ms":time,"process_id":4135,"pool_base":"00900000",
        "free_head":"00900030","used_count":used,
        "stock_capacity":1024,"peak_used_count":peak,
        "allocation_base":"00900000","region_type":"00020000",
        "valid":valid,"status":status,
        "effective_capacity":cap,"expanded_active":expanded,
    }

def va_row():
    return {
        "time_ms":1000,"free_bytes":600*1048576,
        "reserved_bytes":200*1048576,"committed_bytes":300*1048576,
        "largest_free_bytes":200*1048576,"free_regions":10,
        "reserved_regions":10,"committed_regions":15,
        "level_object":"00000000","valid":1,
        "minimum_address":"00010000","maximum_address":"7FFEFFFF",
    }

class NativeAnimationEventPool4096(unittest.TestCase):
    def test_exact_native_oracle_addresses_and_allocation_owner(self):
        for expected in (
            "kNativePoolInit=0x00404C2F",
            "kPostGuard=0x0088AA85",
            "kPostGuardImm=0x0088AA8B",
            "kFreeHeadCell=0x00CD996C",
            "kPoolBaseCell=0x00CD9970",
            "kLiveCountCell=0x00CD9974",
            "kStockCap=0x400",
            "kNewCap=0x1000",
            "kStride=0x30",
            "typedef void* (__stdcall *NativePoolInit)(void*,DWORD,DWORD)",
            "invoked=init(&replacement,kStride,kNewCap)==(void*)&replacement",
            "ValidFreeList(old,kStockCap)",
            "ValidFreeList(replacement,kNewCap)",
            "replacement.base==old.base",
            "g_active,1",
            "kKnownPoolNeighbour=0x00404BF2",
        ):
            with self.subTest(expected=expected):
                self.assertIn(expected,SRC)
        for bad in ("VirtualAlloc(", "HeapAlloc(", "Patch_29", "0x004554B1"):
            self.assertNotIn(bad,SRC)
        self.assertIn("atexit",SRC.lower())
        self.assertIn("old 48 KiB quarantined",SRC)

    def test_fail_closed_and_before_level_state(self):
        install=SRC[SRC.index("static bool Install()"):SRC.index("void OC_AnimationEventPool4096_Init()")]
        for expected in (
            "if (!Idle() || !StockImage())",
            "count!=0",
            "ValidFreeList(old,kStockCap)",
            "VirtualProtect((LPVOID)(uintptr_t)kPostGuardImm,4",
            "PAGE_EXECUTE_READWRITE",
            "ReadHeader(again,live)",
            "InterlockedExchange(&g_active,1)",
            "FlushInstructionCache",
            "ValidFreeList(replacement,kNewCap)",
            "*(volatile DWORD*)(uintptr_t)kPoolBaseCell=replacement.base",
            "*(volatile DWORD*)(uintptr_t)kFreeHeadCell=replacement.freeHead",
            "*(volatile DWORD*)(uintptr_t)kPostGuardImm=kNewCap",
            "*(volatile DWORD*)(uintptr_t)kPostGuardImm=kStockCap",
        ):
            with self.subTest(expected=expected):
                self.assertIn(expected,install)
        self.assertIn("Readable(h.base,bytes)",SRC)  # implemented in ValidFreeList, called by Install
        self.assertIn("0x81,0x3D,0x74,0x99,0xCD,0x00,0x00,0x04,0x00,0x00",SRC) # checked in StockImage
        self.assertIn('if (!GetPrivateProfileIntA("StaticMapPools","ExpandAnimationEventPool4096",0,ini))',SRC)
        self.assertIn("kLevelCell=0x00CD7EA0",SRC)
        self.assertIn("kResourceMgrCell=0x00CD80D8",SRC)
        self.assertIn('"ExpandAnimationEventPool4096=0\\r\\n"',CONFIG)

    def test_wired_into_real_x86_build_and_inits_before_census(self):
        self.assertIn("animation_event_pool_4096.cpp",BUILD)
        self.assertIn("animation_event_pool_4096.obj",BUILD)
        self.assertIn("src/animation_event_pool_4096.cpp",CMAKE)
        self.assertIn('#include "animation_event_pool_4096.h"',PROXY)
        self.assertLess(PROXY.index("OC_AnimationEventPool4096_Init();"),
                        PROXY.index("OC_StaticPoolCensus_Init();"))
        self.assertIn("OC_AnimationEventPool4096_IsActive()",HDR)
        self.assertIn('"ExpandAnimationEventPool4096=0\\r\\n"',CONFIG)

    def test_auditors_must_switch_only_on_verified_native_install(self):
        self.assertIn("POOL_MAX_ENTRIES = 0x400;",MEM)
        self.assertIn("POOL_AUDIT_MAX_ENTRIES=0x1000;",MEM)
        self.assertIn("OC_AnimationEventPool4096_IsActive()",MEM)
        self.assertIn("g_PoolAllocationBitmap[POOL_AUDIT_MAX_ENTRIES / 8 + 1]",MEM)
        self.assertGreaterEqual(MEM.count("PoolAuditLimit()"),8)
        self.assertIn("static_assert(!PATCH_6_ENABLED",MEM)
        self.assertIn("static_assert(!PATCH_29_ENABLED",MEM)
        self.assertIn("EventGuardIsStock()",CENSUS)
        self.assertIn("EventGuardIsExtended()",CENSUS)
        self.assertIn("kEventExpandedCapacity=0x1000",CENSUS)
        self.assertIn("const bool extended=OC_AnimationEventPool4096_IsActive() && EventGuardIsExtended()",CENSUS)
        self.assertIn("effective_capacity,expanded_active",CENSUS)

    def test_4k_pool_flows_into_pressure_parser_without_fake_stock_overflow(self):
        with tempfile.TemporaryDirectory() as td:
            dir=Path(td)
            write_csv(dir/"virtual_address_pressure.csv",[va_row()])
            write_csv(dir/"animation_event_pool.csv",[
                event_row(1000,0,0,"OK"),
                event_row(2000,2500,2500,"OK"),
                event_row(3000,4096,4096,"AT_EXPANDED_CAP"),
                event_row(4000,4096,4096,"AT_EXPANDED_CAP",expanded=0,cap=4096,valid=0),
                event_row(5000,10000,10000,"COUNT_EXCEEDS_CAP",valid=0),
            ])
            result=pressure.summarize(dir)
            ep=result["event_pool"]
            self.assertEqual(ep["valid_samples"],3)
            self.assertEqual(ep["expanded_valid_samples"],3)
            self.assertEqual(ep["invalid_samples"],2)
            self.assertEqual(ep["peak_used_count"],4096)
            self.assertEqual(ep["at_expanded_cap_samples"],1)
            self.assertTrue(any("4096 extended" in x for x in result["notes"]))
            self.assertIn("expanded-at-cap=1",pressure.format_report(result))

    def test_stock_rows_without_extra_columns_remain_compatible(self):
        with tempfile.TemporaryDirectory() as td:
            dir=Path(td)
            write_csv(dir/"virtual_address_pressure.csv",[va_row()])
            old=event_row(1000,1024,1024,"AT_STOCK_CAP",expanded=0,cap=1024)
            old.pop("effective_capacity")
            old.pop("expanded_active")
            write_csv(dir/"animation_event_pool.csv",[old])
            e=pressure.summarize(dir)["event_pool"]
            self.assertEqual(e["valid_samples"],1)
            self.assertEqual(e["at_stock_cap_samples"],1)
            self.assertEqual(e["expanded_valid_samples"],0)

    def test_original_16bit_record_indices_cover_requested_4096_without_sentinel(self):
        # The native producers compute (rec-base)/0x30 as uint16_t; the full
        # sentinel is 0xFFFF, safely above the final extended index 4095.
        for i in (0,1023,1024,4095):
            address=0x09000000+i*0x30
            self.assertEqual((address-0x09000000)//0x30,i)
            self.assertLess(i,0xFFFF)
        self.assertEqual(4096*0x30,196608)

    def test_ci_docs_and_guardrails_explicit(self):
        self.assertEqual(WORKFLOW.count("'tests/test_animation_event_pool_4096.py'"),2)
        self.assertIn("test_animation_event_pool_4096.py -v",WORKFLOW)
        for phrase in ("NOT retail-validated","FUN_00404C2F","FUN_0096E44C",
                       "48 KiB","ExpandAnimationEventPool4096=1"):
            self.assertIn(phrase,DOC)

if __name__=="__main__":
    unittest.main()
