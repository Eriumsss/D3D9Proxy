#!/usr/bin/env python3
"""Havok/VA joint sampling regression tests (no retail game execution)."""
import csv
import importlib.util
import io
import json
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
CENSUS=(ROOT/"src/static_pool_census.cpp").read_text("utf-8")
CONFIG=(ROOT/"src/proxy_config.cpp").read_text("utf-8")
WORKFLOW=(ROOT/".github/workflows/static-map-pak-preflight.yml").read_text("utf-8")
DOC=(ROOT/"HAVOK_VA_CORRELATION.md").read_text("utf-8")
name=ROOT/"tools/analyze_havok_va_correlation.py"
spec=importlib.util.spec_from_file_location("havok_va",name)
assert spec and spec.loader
tool=importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

A="01DC4B1C00000000"
B="01DC4B1C00000001"
MB=1024*1024


def sample(t, session=A, pid=4510, manager="00000000", world="00000000",
           status="NO_MANAGER", transition="INITIAL_SAMPLE", committed=400,
           largest_free=700, valid_world=1, valid_va=1, loading=0):
    return dict(
        session_id=session,process_id=pid,time_ms=t,level_object="00000000",
        manager_ptr=manager,world_ptr=world,loading_flag=loading,
        world_valid=valid_world,world_status=status,transition=transition,
        va_valid=valid_va,committed_bytes=committed*MB,
        reserved_bytes=100*MB,free_bytes=1100*MB,
        largest_free_bytes=largest_free*MB,free_regions=17,
        reserved_regions=12,committed_regions=35,
    )


def save(folder, rows):
    with (folder/"havok_world_va_pressure.csv").open("w",newline="") as f:
        writer=csv.DictWriter(f,fieldnames=tool.COLUMNS)
        writer.writeheader()
        writer.writerows(rows)


class HavokVaCorrelation(unittest.TestCase):
    def test_config_and_existing_single_worker(self):
        self.assertIn('"PhysicsWorldMonitor=0\\r\\n"',CONFIG)
        self.assertIn('"PhysicsVaCorrelation=0\\r\\n"',CONFIG)
        self.assertIn('"StaticMapPools","PhysicsVaCorrelation",0,ini)',CENSUS)
        self.assertIn("const bool physicsVaOn=physicsOn &&",CENSUS)
        self.assertIn("g_physicsVaEnabled=physicsVaOn;",CENSUS)
        self.assertEqual(CENSUS.count("static DWORD WINAPI Worker("),1)
        self.assertEqual(CENSUS.count("CreateThread(NULL,0,Worker,NULL,0,NULL)"),1)

    def test_runtime_samples_have_shared_session_and_race_validation(self):
        block=CENSUS[CENSUS.index("static void LogWorldVaCorrelation("):
                     CENSUS.index("static void PollPhysicsWorld()")]
        for snippet in (
            "MeasureVirtualAddressPressure()",
            "GetCurrentProcessId()",
            "g_physicsVaSession.dwHighDateTime",
            "g_physicsVaSession.dwLowDateTime",
            "ReadDword(kPhysicsMgrCell,checkManager)",
            "ReadDword((uintptr_t)manager+kPhysicsWorldOffset,checkWorld)",
            "RACED_DURING_VA_SCAN",
            "va_valid,",
            "vm.committedBytes",
            "vm.largestFreeBytes",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet,block)
        for forbidden in ("VirtualProtect(","WriteProcessMemory(","VirtualAlloc(",
                          "CreateThread(","SetWindowsHookEx(","memcpy("):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden,block)
        self.assertIn("GetSystemTimeAsFileTime(&g_physicsVaSession)",CENSUS)
        self.assertIn("LogWorldVaCorrelation(now,level,manager,world,loading,valid,status,transition)",CENSUS)

    def test_correlates_own_session_not_timestamps_of_other_launches(self):
        with tempfile.TemporaryDirectory() as td:
            path=Path(td)
            save(path,[
                sample(1000),
                sample(2000,manager="00200000",world="00300000",
                       status="WORLD_PRESENT",transition="WORLD_APPEARED",
                       committed=700,largest_free=500,loading=1),
                sample(3000,manager="00200000",world="00300000",
                       status="WORLD_PRESENT",transition="NONE",committed=820,
                       largest_free=400),
                sample(4000,manager="00200000",status="NO_WORLD",
                       transition="WORLD_DISAPPEARED",committed=500,
                       largest_free=600),
                sample(1000,session=B,pid=4510,committed=200,largest_free=1000),
                sample(2000,session=B,pid=4510,committed=210,largest_free=980),
            ])
            report=tool.summarize(path)
            self.assertEqual(report["session_count"],2)
            run=next(r for r in report["runs"] if r["session_id"]==A)
            self.assertEqual(run["valid_joint_samples"],4)
            self.assertEqual(run["world_present_samples"],2)
            self.assertEqual(run["loading_with_world_samples"],1)
            self.assertEqual(run["peak_process_committed_bytes"],820*MB)
            self.assertEqual(run["smallest_largest_free_bytes"],400*MB)
            self.assertEqual(run["post_unload_vs_baseline_bytes"],100*MB)
            self.assertEqual(len(run["events"]),2)
            self.assertTrue(run["events"][0]["pair_valid"])
            self.assertEqual(run["events"][0]["process_committed_delta_bytes"],300*MB)
            self.assertEqual(run["events"][1]["process_committed_delta_bytes"],-320*MB)
            other=next(r for r in report["runs"] if r["session_id"]==B)
            self.assertEqual(other["events"],[])
            self.assertEqual(other["peak_process_committed_bytes"],210*MB)

    def test_invalid_vm_or_world_breaks_comparison_without_guessing(self):
        with tempfile.TemporaryDirectory() as td:
            path=Path(td)
            save(path,[
                sample(1000),
                sample(1500,status="RACED_DURING_VA_SCAN",transition="UNKNOWN",valid_world=0),
                sample(2000,manager="00200000",world="00300000",
                       status="WORLD_PRESENT",transition="WORLD_APPEARED",committed=500),
                sample(2500,manager="00200000",world="00300000",
                       status="WORLD_PRESENT",transition="NONE",committed=600,valid_va=0),
                sample(3000,manager="00200000",status="NO_WORLD",
                       transition="WORLD_DISAPPEARED",committed=450),
            ])
            r=tool.summarize(path)["runs"][0]
            self.assertEqual(r["invalid_world_samples"],1)
            self.assertEqual(r["invalid_va_samples"],1)
            self.assertEqual(r["continuity_gaps"],2)
            self.assertEqual(len(r["events"]),2)
            self.assertFalse(any(e["pair_valid"] for e in r["events"]))
            self.assertEqual(r["valid_joint_samples"],3)

    def test_oversized_values_and_bad_schema_fail_closed(self):
        with tempfile.TemporaryDirectory() as td:
            path=Path(td)
            with self.assertRaises(FileNotFoundError):
                tool.summarize(path)
            (path/"havok_world_va_pressure.csv").write_text("tick,unknown\n1,2\n")
            with self.assertRaises(ValueError):
                tool.summarize(path)
            bad=sample(1000);bad["session_id"]="ZZBAD";bad["committed_bytes"]=-1
            save(path,[bad])
            with self.assertRaises(ValueError):
                tool.summarize(path)

    def test_cli_and_documentation_and_ci(self):
        with tempfile.TemporaryDirectory() as td:
            path=Path(td);save(path,[sample(1000)])
            out=io.StringIO()
            with redirect_stdout(out):
                rv=tool.main(["--game-dir",str(path),"--json"])
            self.assertEqual(rv,0)
            self.assertEqual(json.loads(out.getvalue())["session_count"],1)
        self.assertIn("PhysicsVaCorrelation=1",DOC)
        self.assertIn("NOT Havok",DOC)
        self.assertEqual(WORKFLOW.count("'tests/test_havok_va_correlation.py'"),2)
        self.assertIn("test_havok_va_correlation.py -v",WORKFLOW)


if __name__=="__main__":
    unittest.main()
