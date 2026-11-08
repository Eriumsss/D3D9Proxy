#!/usr/bin/env python3
"""Event pool native stock-cap contracts, read-only telemetry; no retail test."""
import csv
import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
MEM=(ROOT/"src/memorypatch.cpp").read_text("utf-8")
CENSUS=(ROOT/"src/static_pool_census.cpp").read_text("utf-8")
CONFIG=(ROOT/"src/proxy_config.cpp").read_text("utf-8")
WORKFLOW=(ROOT/".github/workflows/static-map-pak-preflight.yml").read_text("utf-8")
DOC=(ROOT/"ANIMATION_EVENT_POOL.md").read_text("utf-8")
spec=importlib.util.spec_from_file_location("native_pressure",(ROOT/"tools/analyze_runtime_pressure.py"))
assert spec and spec.loader
pressure=importlib.util.module_from_spec(spec)
spec.loader.exec_module(pressure)


def write_csv(path,rows):
    with path.open("w",newline="") as f:
        writer=csv.DictWriter(f,fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)


def event(t,status="OK",used=100,peak=100,valid=1,cap=1024):
    return {
        "time_ms":t,"process_id":3000,
        "pool_base":"00600000","free_head":"00600030",
        "used_count":used,"stock_capacity":cap,"peak_used_count":peak,
        "allocation_base":"00600000","region_type":"00020000",
        "valid":valid,"status":status,
    }


def va_row():
    return {
        "time_ms":1000,"free_bytes":600*1024*1024,
        "reserved_bytes":200*1024*1024,
        "committed_bytes":300*1024*1024,
        "largest_free_bytes":200*1024*1024,
        "free_regions":10,"reserved_regions":10,"committed_regions":15,
        "level_object":"00000000","valid":1,
        "minimum_address":"00010000","maximum_address":"7FFEFFFF",
    }


class NativeAnimEventPool(unittest.TestCase):
    def test_stock_bounds_fix_and_legacy_unsafe_patches_blocked(self):
        self.assertIn("POOL_MAX_ENTRIES = 0x400;",MEM)
        self.assertNotIn("POOL_MAX_ENTRIES = 0x1000;",MEM)
        self.assertIn("g_EventPoolNewLimit = 0x400;",MEM)
        self.assertIn("static const bool PATCH_6_ENABLED = false;",MEM)
        self.assertIn("static const bool PATCH_29_ENABLED = false;",MEM)
        self.assertIn("static_assert(!PATCH_6_ENABLED",MEM)
        self.assertIn("static_assert(!PATCH_29_ENABLED",MEM)
        self.assertIn("FUN_0045549c",MEM)  # blend-slot function, not event allocator
        self.assertIn("0x0088aa8b",MEM.lower())

    def test_guarded_read_only_event_census_is_independent(self):
        for fragment in (
            "kEventPoolBaseCell=0x00CD9970",
            "kEventPoolHeadCell=0x00CD996C",
            "kEventPoolCountCell=0x00CD9974",
            "kEventFactoryGuard=0x0088AA85",
            "kEventStockCapacity=0x400",
            "kEventRecordStride=0x30",
            "0x81,0x3D,0x74,0x99,0xCD,0x00,0x00,0x04,0x00,0x00",
            "PollEventPool();",
            "EventPoolMonitor",
            "animation_event_pool.csv",
            "UNVERIFIED_RETAIL_GUARD",
            "COUNT_EXCEEDS_CAP",
            "BAD_FREE_HEAD",
            "AT_STOCK_CAP",
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment,CENSUS)
        block=CENSUS[CENSUS.index("static void PollEventPool()"):CENSUS.index("static void LogWorldVaCorrelation(")] if CENSUS.index("static void PollEventPool()")<CENSUS.index("static void LogWorldVaCorrelation(") else CENSUS[CENSUS.index("static void PollEventPool()"):CENSUS.index("static void Poll()")]
        for forbidden in ("VirtualAlloc(","VirtualProtect(","WriteProcessMemory(",
                          "CreateThread(","HeapAlloc(","memcpy("):
            self.assertNotIn(forbidden,block)
        self.assertEqual(CENSUS.count("static DWORD WINAPI Worker("),1)

    def test_default_off_without_reconfiguring_other_monitors(self):
        self.assertIn('"EventPoolMonitor=0\\r\\n"',CONFIG)
        self.assertIn('if (!staticOn && !blendOn && !physicsOn && !eventOn) return;',CENSUS)
        self.assertIn("PollBlend();",CENSUS)
        self.assertIn("PollPhysicsWorld();",CENSUS)
        self.assertIn("EventPoolMonitor=1",DOC)

    def test_valid_counts_and_suspicious_guard_do_not_mix(self):
        with tempfile.TemporaryDirectory() as td:
            d=Path(td)
            write_csv(d/"virtual_address_pressure.csv",[va_row()])
            write_csv(d/"animation_event_pool.csv",[
                event(1000),
                event(2000,"AT_STOCK_CAP",1024,1024),
                event(3000,"UNVERIFIED_RETAIL_GUARD",4096,4096,valid=0),
                event(4000,"COUNT_EXCEEDS_CAP",1100,1100,valid=0),
                event(5000,"NEAR_STOCK_CAP",1009,1024)
            ])
            result=pressure.summarize(d)
            ep=result["event_pool"]
            self.assertEqual(ep["valid_samples"],3)
            self.assertEqual(ep["invalid_samples"],2)
            self.assertEqual(ep["peak_used_count"],1024)
            self.assertEqual(ep["at_stock_cap_samples"],1)
            self.assertEqual(ep["unverified_guard_samples"],1)
            self.assertTrue(any("EVENT pool reached" in x for x in result["notes"]))
            self.assertIn("Animation EVENT pool",pressure.format_report(result))

    def test_default_pressure_report_survives_absent_event_log(self):
        with tempfile.TemporaryDirectory() as td:
            d=Path(td)
            write_csv(d/"virtual_address_pressure.csv",[va_row()])
            self.assertEqual(pressure.summarize(d)["event_pool"],{})

    def test_ci_and_doc_wiring(self):
        self.assertEqual(WORKFLOW.count("'tests/test_animation_event_pool.py'"),2)
        self.assertIn("test_animation_event_pool.py -v",WORKFLOW)
        self.assertIn("NOT expanded",DOC)
        self.assertIn("0x004554B1",DOC)


if __name__=="__main__":
    unittest.main()
