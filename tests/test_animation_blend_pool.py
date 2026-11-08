#!/usr/bin/env python3
"""Read-only blend slot probe contracts. Not proof of retail gameplay success."""
import csv
import importlib.util
import re
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
SRC=(ROOT/"src/static_pool_census.cpp").read_text("utf-8")
INI=(ROOT/"src/proxy_config.cpp").read_text("utf-8")
PATCH=(ROOT/"src/memorypatch.cpp").read_text("utf-8")
WORKFLOW=(ROOT/".github/workflows/static-map-pak-preflight.yml").read_text("utf-8")
DOC=(ROOT/"ANIMATION_BLEND_POOL.md").read_text("utf-8")
module_path=ROOT/"tools/analyze_runtime_pressure.py"
spec=importlib.util.spec_from_file_location("analyze_runtime_pressure",module_path)
assert spec and spec.loader
tool=importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

class FixedBlendPool(unittest.TestCase):
    def test_verified_retail_pointer_layout(self):
        for t in (
            "kBlendRootCell = 0x00CD7E7C",
            "kBlendGuardSite = 0x004554B1",
            "kBlendSlotsOffset = 0x9004",
            "kBlendCounterOffset = 0x3E000",
            "kBlendSlotStride = 0xF8",
            "kBlendRetailCapacity = 0x400",
            "(guardWord&0xFFFFu)==0xFE81u",
            "guardLimit==kBlendRetailCapacity",
        ):
            with self.subTest(t=t):
                self.assertIn(t,SRC)
        self.assertEqual(1024*0xF8,0x3E000)
        self.assertIn("static const bool PATCH_29_ENABLED = false;",PATCH)

    def test_monitor_is_read_only_and_race_guarded(self):
        body=SRC[SRC.index("static void PollBlend()"):SRC.index("static void Poll()")]
        for token in (
            "ReadDword(kBlendRootCell,root)",
            "ReadDword((uintptr_t)root+0x0C,arena)",
            "CanRead(slots,kBlendCounterOffset+sizeof(DWORD))",
            "ReadDword(counter,index)",
            "ReadDword(kBlendRootCell,sameRoot)",
            "sameRoot==root",
            "sameArena==arena",
            "index<=kBlendRetailCapacity",
            "g_blendPeak=0",
            '"UNVERIFIED_GUARD"',
            '"RACED"',
            '"OUT_OF_RANGE"',
            '"AT_CAP"',
        ):
            with self.subTest(token=token):
                self.assertIn(token,body)
        for forbidden in (
            "VirtualAlloc(","VirtualProtect(","WriteProcessMemory(",
            "HeapAlloc(","memcpy(","SetWindowsHookEx(","CreateThread(",
            "InterlockedExchange("
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden,body)

    def test_switch_does_not_break_normal_monitor_or_start_new_thread(self):
        self.assertIn('"StaticMapPools","Monitor",0,ini)',SRC)
        self.assertIn('"StaticMapPools","BlendMonitor",0,ini)',SRC)
        self.assertIn("if (!staticOn && !blendOn) return;",SRC)
        self.assertIn("PollBlend(); // valid even while level_object=0",SRC)
        self.assertIn("if (!g_staticMonitorEnabled) return;",SRC)
        self.assertEqual(SRC.count("static DWORD WINAPI Worker("),1)
        self.assertEqual(SRC.count("CreateThread(NULL,0,Worker,NULL,0,NULL)"),1)
        self.assertIn('"BlendMonitor=0\\r\\n"',INI)
        self.assertIn('"Monitor=1\\r\\n"',INI)
        self.assertIn("animation_blend_pool.csv",SRC)

    def test_csv_header_matches_format_specifiers(self):
        body=SRC[SRC.index("static void PollBlend()"):SRC.index("static void Poll()")]
        head=re.search(r"fputs\((.*?)\,f\);",body,re.S)
        self.assertIsNotNone(head)
        fields="".join(re.findall(r'"([^"]*)"',head.group(1))).replace(r"\n","").split(",")
        self.assertEqual(fields,[
            "time_ms","root_ptr","arena_ptr","slot_base","counter_ptr",
            "slot_stride","retail_capacity","next_free_index",
            "peak_next_free_index","guard_limit","valid","status"
        ])
        row=re.search(r"fprintf\(f,(.*?)\,\s*\(unsigned long\)now",body,re.S)
        self.assertIsNotNone(row)
        fmt="".join(re.findall(r'"([^"]*)"',row.group(1)))
        self.assertEqual(len(re.findall(r"%(?:0?[0-9]*)?(?:ll|l)?[Xuds]",fmt)),len(fields))

    def test_analyzer_filters_invalid_blend_and_flags_at_cap(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            va_keys=["time_ms","level_object","valid","minimum_address",
                     "maximum_address","free_bytes","reserved_bytes",
                     "committed_bytes","largest_free_bytes","free_regions",
                     "reserved_regions","committed_regions"]
            va_row=["1000","00000000","1","00010000","7FFEFFFF",
                    "400","100","800","200","1","1","10"]
            with (root/"virtual_address_pressure.csv").open("w",newline="") as f:
                writer=csv.writer(f);writer.writerow(va_keys);writer.writerow(va_row)
            columns=["time_ms","root_ptr","arena_ptr","slot_base","counter_ptr",
                     "slot_stride","retail_capacity","next_free_index",
                     "peak_next_free_index","guard_limit","valid","status"]
            def sample(tick,index,peak,valid,status):
                return [tick,"00001000","00002000","00003000","00040000",
                        248,1024,index,peak,1024,valid,status]
            with (root/"animation_blend_pool.csv").open("w",newline="") as f:
                writer=csv.writer(f)
                writer.writerow(columns)
                writer.writerow(sample(1000,1200,0,0,"OUT_OF_RANGE"))
                writer.writerow(sample(2000,1018,1018,1,"NEAR_CAP"))
                writer.writerow(sample(3000,1024,1024,1,"AT_CAP"))
            result=tool.summarize(root)
            blend=result["blend_pool"]
            self.assertEqual(blend["valid_samples"],2)
            self.assertEqual(blend["invalid_samples"],1)
            self.assertEqual(blend["peak_next_free_index"],1024)
            self.assertEqual(blend["at_capacity_samples"],1)
            self.assertIn("Fixed animation blend",tool.format_report(result))
            self.assertTrue(any("1024-slot" in note for note in result["notes"]))

    def test_docs_ci_and_disabled_default(self):
        self.assertIn("BlendMonitor=1",DOC)
        self.assertIn("BlendMonitor=0",DOC)
        self.assertIn("not an atomic snapshot",DOC)
        self.assertEqual(WORKFLOW.count("'tests/test_animation_blend_pool.py'"),2)
        self.assertIn("test_animation_blend_pool.py -v",WORKFLOW)

if __name__=="__main__":
    unittest.main()
