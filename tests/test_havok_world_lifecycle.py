#!/usr/bin/env python3
"""Source/CSV contract: this is NOT a retail Havok allocator stress test."""
import csv
import importlib.util
import io
import json
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
CPP=(ROOT/"src/static_pool_census.cpp").read_text("utf-8")
INI=(ROOT/"src/proxy_config.cpp").read_text("utf-8")
WORKFLOW=(ROOT/".github/workflows/static-map-pak-preflight.yml").read_text("utf-8")
DOC=(ROOT/"HAVOK_WORLD_LIFECYCLE.md").read_text("utf-8")
spec=importlib.util.spec_from_file_location("havok_analyzer",ROOT/"tools/analyze_havok_world.py")
assert spec and spec.loader
analyzer=importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyzer)


def sample(t, manager="00000000", world="00000000", status="NO_MANAGER",
           transition="INITIAL_SAMPLE", valid="1", loading="0"):
    return {
        "time_ms":str(t), "process_id":"2500",
        "level_object":"00000000", "level_valid":"1",
        "manager_ptr":manager,
        "manager_vtable":"009CAF94" if manager!="00000000" else "00000000",
        "thread_memory_ptr":"00000000", "world_ptr":world,
        "loading_flag":loading, "valid":valid, "status":status,
        "transition":transition,
        "observed_attaches":"0", "observed_detaches":"0",
        "observed_replacements":"0",
    }


def write_csv(folder, records):
    with (folder/"havok_world_lifecycle.csv").open("w",newline="") as f:
        writer=csv.DictWriter(f,fieldnames=analyzer.EXPECTED)
        writer.writeheader()
        writer.writerows(records)


class HavokWorldLifecycleContract(unittest.TestCase):
    def test_native_level_world_ownership_offsets_are_explicit(self):
        for needle in (
            "kPhysicsMgrCell = 0x00CD7ED0",
            "kPhysicsMgrVtable = 0x009CAF94",
            "kPhysicsWorldOffset = 0xFC",
            "kPhysicsThreadMemoryOffset = 0xE8",
            "kPhysicsLoadingFlagOffset = 0x1AD",
            "ReadDword(kPhysicsMgrCell,manager)",
            "ReadDword((uintptr_t)manager+kPhysicsWorldOffset,world)",
            "ReadByte((uintptr_t)manager+kPhysicsLoadingFlagOffset,loading)",
            "sameManager==manager && sameWorld==world",
            "vtable==kPhysicsMgrVtable",
            "BAD_MANAGER_RANGE",
        ):
            with self.subTest(needle=needle):
                self.assertIn(needle,CPP)

    def test_monitor_does_not_write_game_memory_or_install_hooks(self):
        body=CPP[CPP.index("static void PollPhysicsWorld()"):CPP.index("static void Poll()")]
        for forbidden in (
            "VirtualAlloc(","VirtualProtect(","WriteProcessMemory(","memcpy(",
            "CreateThread(","HeapAlloc(","SetWindowsHookEx(","InterlockedExchange("
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden,body)
        self.assertIn("ReadDword(kPhysicsMgrCell,sameManager)",body)
        self.assertIn("ReadDword((uintptr_t)manager+kPhysicsWorldOffset,sameWorld)",body)

    def test_opt_in_alone_uses_existing_worker_and_does_not_break_other_monitors(self):
        self.assertIn('"StaticMapPools","PhysicsWorldMonitor",0,ini)',CPP)
        self.assertIn("if (!staticOn && !blendOn && !physicsOn) return;",CPP)
        self.assertIn("PollPhysicsWorld(); // independent",CPP)
        self.assertLess(CPP.index("PollPhysicsWorld(); // independent"),
                        CPP.index("if (!g_staticMonitorEnabled) return;"))
        self.assertEqual(CPP.count("static DWORD WINAPI Worker("),1)
        self.assertEqual(CPP.count("CreateThread(NULL,0,Worker,NULL,0,NULL)"),1)
        self.assertIn('"PhysicsWorldMonitor=0\\r\\n"',INI)
        self.assertIn('"BlendMonitor=0\\r\\n"',INI)
        self.assertIn("havok_world_lifecycle.csv",CPP)

    def test_csv_column_format_matches_declared_schema(self):
        body=CPP[CPP.index("static void PollPhysicsWorld()"):CPP.index("static void Poll()")]
        for key in analyzer.EXPECTED:
            if key in ("time_ms","process_id","level_object","level_valid",
                       "manager_ptr","manager_vtable","thread_memory_ptr"):
                continue
            self.assertIn(key,body)
        self.assertIn("GetCurrentProcessId()",body)
        self.assertIn('fputs("time_ms,process_id,level_object,level_valid,manager_ptr,manager_vtable,"',body)

    def test_parser_reports_world_edges_without_fabricating_havok_limits(self):
        with tempfile.TemporaryDirectory() as d:
            directory=Path(d)
            rows=[
                sample(1000),
                sample(2000,manager="00201000",world="00202000",
                       status="WORLD_PRESENT",transition="WORLD_APPEARED",loading="1"),
                sample(3000,manager="00201000",world="00000000",
                       status="NO_WORLD",transition="WORLD_DISAPPEARED"),
                sample(4000,manager="00201000",world="00000000",
                       status="RACED",transition="UNKNOWN",valid="0"),
                sample(5000,manager="00000000",world="00202000",
                       status="WORLD_PRESENT",transition="WORLD_APPEARED"),
            ]
            write_csv(directory,rows)
            result=analyzer.summarize(directory)
            self.assertEqual(result["valid_samples"],3)
            self.assertEqual(result["invalid_runtime_samples"],1)
            self.assertEqual(result["malformed_samples"],1)
            self.assertEqual(result["observed_transitions"]["WORLD_APPEARED"],1)
            self.assertEqual(result["observed_transitions"]["WORLD_DISAPPEARED"],1)
            self.assertEqual(result["loading_with_world_samples"],1)
            self.assertTrue(any("NOT proof of a leak" in note for note in result["notes"]))

    def test_bad_schema_and_no_valid_rows_fail_closed(self):
        with tempfile.TemporaryDirectory() as d:
            directory=Path(d)
            with self.assertRaises(FileNotFoundError):
                analyzer.summarize(directory)
            (directory/"havok_world_lifecycle.csv").write_text("bogus,x\n1,2\n")
            with self.assertRaises(ValueError):
                analyzer.summarize(directory)
            write_csv(directory,[sample(1000,valid="0",status="RACED",transition="UNKNOWN")])
            with self.assertRaises(ValueError):
                analyzer.summarize(directory)

    def test_json_cli_and_build_wiring(self):
        with tempfile.TemporaryDirectory() as d:
            directory=Path(d)
            write_csv(directory,[sample(1000)])
            out=io.StringIO()
            with redirect_stdout(out):
                code=analyzer.main(["--game-dir",str(directory),"--json"])
            self.assertEqual(code,0)
            self.assertEqual(json.loads(out.getvalue())["valid_samples"],1)
        self.assertEqual(WORKFLOW.count("'tests/test_havok_world_lifecycle.py'"),2)
        self.assertIn("test_havok_world_lifecycle.py -v",WORKFLOW)
        self.assertIn("PhysicsWorldMonitor=1",DOC)
        self.assertIn("not the allocator",DOC.lower())


if __name__=="__main__":
    unittest.main()
