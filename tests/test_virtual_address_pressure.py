#!/usr/bin/env python3
"""Synthetic tests only. These validate the offline log parser, not game use."""
import csv
import importlib.util
import io
import json
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
SRC=(ROOT/"src/static_pool_census.cpp").read_text("utf-8")
MODULE=ROOT/"tools/analyze_runtime_pressure.py"
spec=importlib.util.spec_from_file_location("analyze_runtime_pressure",MODULE)
assert spec and spec.loader
tool=importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

VA_KEYS=["time_ms","level_object","valid","minimum_address",
         "maximum_address","free_bytes","reserved_bytes","committed_bytes",
         "largest_free_bytes","free_regions","reserved_regions","committed_regions"]

def write_csv(path,fields,records):
    with path.open("w",newline="",encoding="utf-8") as f:
        writer=csv.DictWriter(f,fieldnames=fields)
        writer.writeheader()
        writer.writerows(records)

def vm(tick=1000,level="00000000",valid=1,largest=300*tool.MIB):
    return dict(time_ms=str(tick),level_object=level,valid=str(valid),
                minimum_address="00010000",maximum_address="7FFEFFFF",
                free_bytes=str(600*tool.MIB),reserved_bytes=str(100*tool.MIB),
                committed_bytes=str(1300*tool.MIB),largest_free_bytes=str(largest),
                free_regions="5",reserved_regions="4",committed_regions="70")

class VirtualAddressTelemetry(unittest.TestCase):
    def test_uses_64bit_span_and_native_virtual_query(self):
        self.assertIn("static VaPressure MeasureVirtualAddressPressure()",SRC)
        self.assertIn("GetSystemInfo(&info)",SRC)
        self.assertIn("VirtualQuery((LPCVOID)current,&region,sizeof(region))",SRC)
        self.assertIn("const unsigned __int64 endExclusive=",SRC)
        self.assertIn("const unsigned __int64 rawEnd=begin+",SRC)
        self.assertIn("next=rawEnd<endExclusive?rawEnd:endExclusive",SRC)
        for key in ("MEM_FREE","MEM_RESERVE","MEM_COMMIT","largestFreeBytes"):
            self.assertIn(key,SRC)

    def test_readonly_and_no_rewrite_of_game_regions(self):
        va=SRC[SRC.index("struct VaPressure"):SRC.index("static void Poll()")]
        for forbidden in ("VirtualAlloc(","VirtualFree(","VirtualProtect(",
                          "WriteProcessMemory(","InterlockedExchange("):
            self.assertNotIn(forbidden,va)
        self.assertIn("return stats; // valid=0",va)
        self.assertIn("stats.valid=1;",va)

    def test_scans_before_level_creation_without_unbounded_250ms_cost(self):
        no_level=SRC[SRC.index("if (!level) {"):SRC.index("Cell cells[kRegistryCount]")]
        self.assertIn("LogVirtualAddressPressure(now,0)",no_level)
        self.assertIn("(DWORD)(now-g_lastLog)>=kLogMs",no_level)
        self.assertIn('LogVirtualAddressPressure(now,levelRaw)',SRC)
        self.assertIn('"%svirtual_address_pressure.csv",exe',SRC)
        self.assertEqual(SRC.count("static DWORD WINAPI Worker("),1)

    def test_va_csv_schema_is_twelve_fields(self):
        csv=SRC[SRC.index("static void LogVirtualAddressPressure("):SRC.index("static void Poll()")]
        self.assertIn("time_ms,level_object,valid,minimum_address,maximum_address,",csv)
        self.assertIn("free_bytes,reserved_bytes,committed_bytes,largest_free_bytes,",csv)
        self.assertIn("free_regions,reserved_regions,committed_regions",csv)
        self.assertIn("stats.largestFreeBytes,",csv)
        self.assertIn("stats.valid",csv)

    def test_parser_handles_prelevel_and_flags_low_contiguous_free(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            write_csv(root/"virtual_address_pressure.csv",VA_KEYS,[
                vm(1000,largest=300*tool.MIB),
                vm(3000,"00123000",largest=90*tool.MIB),
                vm(5000,"00123000",largest=55*tool.MIB),
            ])
            result=tool.summarize(root,128)
            self.assertEqual(result["pre_level_samples"],1)
            self.assertEqual(result["valid_samples"],3)
            self.assertEqual(result["latest"]["largest_free_bytes"],55*tool.MIB)
            self.assertEqual(result["worst_largest_free"]["time_ms"],5000)
            self.assertTrue(any("threshold" in s for s in result["notes"]))

    def test_parser_drops_partial_corrupted_rows(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            broken=vm(1000,valid=0)
            write_csv(root/"virtual_address_pressure.csv",VA_KEYS,[
                broken,vm(2000),{**vm(3000),"largest_free_bytes":"not_an_int"}
            ])
            result=tool.summarize(root)
            self.assertEqual(result["valid_samples"],1)
            self.assertEqual(result["invalid_samples"],2)

    def test_no_file_or_no_valid_data_fail_explicitly(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            with self.assertRaises(FileNotFoundError):
                tool.summarize(root)
            write_csv(root/"virtual_address_pressure.csv",VA_KEYS,[vm(valid=0)])
            with self.assertRaises(ValueError):
                tool.summarize(root)

    def test_registry_peaks_warn_only_on_observed_capacity(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            write_csv(root/"virtual_address_pressure.csv",VA_KEYS,[vm()])
            fields=["registry","valid","entry_count","capacity","peak_count"]
            write_csv(root/"static_map_pools.csv",fields,[
                dict(registry="model_a",valid="1",entry_count="900",capacity="1024",peak_count="960"),
                dict(registry="model_b",valid="0",entry_count="2000",capacity="1024",peak_count="2000"),
                dict(registry="gfx_block",valid="1",entry_count="30",capacity="64",peak_count="40")
            ])
            result=tool.summarize(root)
            self.assertIn("model_a",result["registry_peaks"])
            self.assertNotIn("model_b",result["registry_peaks"])
            self.assertAlmostEqual(result["registry_peaks"]["model_a"]["utilization"],0.9375)
            self.assertTrue(any("model_a" in x for x in result["notes"]))

    def test_reset_and_binding_peaks_are_not_fabricated_caps(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            write_csv(root/"virtual_address_pressure.csv",VA_KEYS,[vm()])
            fields=["device_reset_valid","device_reset_peak","dynamic_vb_valid",
                    "dynamic_vb_peak","dynamic_ib_valid","dynamic_ib_peak",
                    "bind_valid","clip_peak","inner_pairs_peak"]
            write_csv(root/"runtime_pool_pressure.csv",fields,[
                dict(device_reset_valid="1",device_reset_peak="40",
                     dynamic_vb_valid="1",dynamic_vb_peak="70",
                     dynamic_ib_valid="0",dynamic_ib_peak="999",
                     bind_valid="1",clip_peak="41",inner_pairs_peak="450")
            ])
            result=tool.summarize(root)
            self.assertEqual(result["reset_list_peaks"]["dynamic_vb"],70)
            self.assertNotIn("dynamic_ib",result["reset_list_peaks"])
            self.assertEqual(result["binding_peaks"]["inner_pairs_peak"],450)
            self.assertIn("actual allocated cap unknown",tool.format_report(result))

    def test_cli_json_and_threshold_are_user_configurable(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            write_csv(root/"virtual_address_pressure.csv",VA_KEYS,[vm()])
            out=io.StringIO()
            with redirect_stdout(out):
                status=tool.main(["--game-dir",str(root),"--json","--warn-largest-free-mib","512"])
            self.assertEqual(status,0)
            result=json.loads(out.getvalue())
            self.assertEqual(result["threshold_mib"],512)
            self.assertEqual(result["latest"]["free_bytes"],600*tool.MIB)

    def test_errors_return_nonzero_instead_of_success(self):
        with tempfile.TemporaryDirectory() as d:
            error=io.StringIO()
            with redirect_stderr(error):
                status=tool.main(["--game-dir",d])
            self.assertEqual(status,2)
            self.assertIn("pressure analysis unavailable",error.getvalue())

if __name__=="__main__":
    unittest.main()
