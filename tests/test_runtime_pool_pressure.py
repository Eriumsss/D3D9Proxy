#!/usr/bin/env python3
"""Native read-only runtime pressure census contracts, not a game simulation."""
from pathlib import Path
import re
import unittest

ROOT=Path(__file__).resolve().parents[1]
CPP=(ROOT/"src/static_pool_census.cpp").read_text("utf-8")
PROXY=(ROOT/"src/d3d9proxy.cpp").read_text("utf-8")
INI=(ROOT/"src/proxy_config.cpp").read_text("utf-8")
README=(ROOT/"README.md").read_text("utf-8")
WF=(ROOT/".github/workflows/static-map-pak-preflight.yml").read_text("utf-8")

class RuntimePressureContract(unittest.TestCase):
    def test_original_x86_reset_list_pointer_and_count_cells(self):
        for entry in ('{"device_reset",0x00A41538,0x00A4153C}',
                      '{"dynamic_vb",0x00A41520,0x00A41524}',
                      '{"dynamic_ib",0x00A4152C,0x00A41530}'):
            self.assertIn(entry,CPP)

    def test_binding_registry_exact_native_layout(self):
        for token in ("kAnimBindRegCell=0x00CD7E5C",
                      "kAnimBindRegVtable=0x009C7A1C",
                      "(uintptr_t)reg+0x0C,counts",
                      "(uintptr_t)reg+0x10,base",
                      "(short)(counts&0xffff)","(short)(counts>>16)"):
            self.assertIn(token,CPP)

    def test_inner_map_is_8_byte_pairs_with_second_dword_pointer(self):
        for token in ("const uintptr_t slot=(uintptr_t)base+(uintptr_t)i*8u",
                      "ReadDword(slot+4,map)",
                      "ReadDword((uintptr_t)map,packed)",
                      "ReadDword((uintptr_t)map+4,pairs)",
                      "out.innerPairs+=(unsigned)n"):
            self.assertIn(token,CPP)

    def test_bad_count_invalid_pointer_and_race_guard(self):
        for token in ("count>0x10000 || (count!=0 && base==0)",
                      "!CanRead((uintptr_t)base,(size_t)count*4u)",
                      "count<0 || cap<0 || count>cap",
                      "n<0 || capacity<0 || n>capacity",
                      "n>0 && !CanRead((uintptr_t)pairs,(size_t)n*8u)",
                      "vt!=kAnimBindRegVtable",
                      "++out.invalidInner"):
            self.assertIn(token,CPP)

    def test_observation_does_not_modify_retail_executable_or_arrays(self):
        census=CPP[CPP.index("struct ResetList"):CPP.index("static void Poll()")]
        for bad in ("VirtualProtect(", "VirtualAlloc(", "WriteProcessMemory(",
                    "memcpy((void*)", "InterlockedExchange(",
                    "CreateFileMapping(", "MoveFileExA("):
            self.assertNotIn(bad,census)
        self.assertIn("ReadDword(list.baseCell,base)",census)
        self.assertIn("ReadDword(list.countCell,count)",census)

    def test_no_fake_capacity_from_user_config(self):
        census=CPP[CPP.index("struct ResetList"):CPP.index("static void Poll()")]
        self.assertNotIn("cap=4096",census)
        self.assertIn("4096 is a supported cap, not a proof of the active allocation",census)
        self.assertIn("result.valid=1",census)

    def test_per_level_peaks_reset_and_update_only_from_valid_counts(self):
        for token in ("if (level != g_lastLevel)", "memset(g_resetPeaks,0,sizeof(g_resetPeaks))",
                      "g_clipPeak=0;", "g_innerPairsPeak=0;",
                      "g_latestBinding.valid && g_latestBinding.clips>g_clipPeak",
                      "g_latestBinding.valid && g_latestBinding.innerPairs>g_innerPairsPeak",
                      "g_latestReset[i].valid && g_latestReset[i].count>g_resetPeaks[i]"):
            self.assertIn(token,CPP)

    def test_one_existing_250ms_worker_and_2s_write_throttle(self):
        self.assertIn("static const DWORD kPollMs = 250",CPP)
        self.assertIn("static const DWORD kLogMs = 2000",CPP)
        self.assertEqual(CPP.count("static DWORD WINAPI Worker("),1)
        self.assertEqual(CPP.count("CreateThread(NULL,0,Worker,NULL,0,NULL)"),1)
        self.assertIn("LogPressure(now,levelRaw)",CPP)
        self.assertIn("g_latestBinding=ObserveBindings();",CPP)
        self.assertLess(CPP.index("SamplePressure(); // 250ms readings"),
                        CPP.index("if (g_lastLog && (DWORD)(now-g_lastLog)<kLogMs) return;"))

    def test_csv_is_separate_and_controlled_by_existing_monitor(self):
        self.assertIn('"%sruntime_pool_pressure.csv",exe',CPP)
        self.assertIn('"%sstatic_map_pools.csv",exe',CPP)
        self.assertIn('GetPrivateProfileIntA("StaticMapPools","Monitor",0,ini)',CPP)
        self.assertIn('"Monitor=1\\r\\n"',INI)
        self.assertIn("PressureHeader(f)",CPP)
        self.assertIn('fopen_s(&f,g_pressureOutput,"ab+")',CPP)

    def test_csv_column_and_row_field_counts_match(self):
        m=re.search(r"static void PressureHeader\(FILE\* f\)\s*\{\s*fputs\((.*?)\,f\);",CPP,re.S)
        self.assertIsNotNone(m)
        header="".join(re.findall(r'"([^"]*)"',m.group(1)))
        self.assertEqual(len(header.split(",")),21)
        self.assertIn("invalid_inner_maps",header)
        self.assertIn("dynamic_ib_peak",header)
        row=CPP[CPP.index("static void LogPressure("):CPP.index("static void Poll()")]
        fmt_match=re.search(r"fprintf\(f,(.*?)\,\s*\(unsigned long\)now",row,re.S)
        self.assertIsNotNone(fmt_match)
        fmt="".join(re.findall(r'"([^"]*)"',fmt_match.group(1)))
        self.assertEqual(len(re.findall(r"%(?:0?[0-9]*)?(?:ll|l)?[Xuds]",fmt)),21)

    def test_documentation_disclaims_synthetic_and_sampling_limits(self):
        doc=(ROOT/"RUNTIME_POOL_PRESSURE.md").read_text("utf-8")
        for word in ("sampled","not proof","not the PAK","4096","64",
                     "not an atomic snapshot","level"):
            self.assertIn(word,doc)

    def test_no_new_proxy_init_or_invented_ini_switch(self):
        self.assertEqual(PROXY.count("OC_StaticPoolCensus_Init();"),1)
        self.assertNotIn("OC_RuntimePoolPressure_Init();",PROXY)
        self.assertNotIn("PressureMonitorEnabled",INI)

    def test_ci_and_readme_trigger_and_explain_feature(self):
        self.assertEqual(WF.count("'tests/test_runtime_pool_pressure.py'"),2)
        self.assertEqual(WF.count("'RUNTIME_POOL_PRESSURE.md'"),2)
        self.assertIn("test_runtime_pool_pressure.py -v",WF)
        self.assertIn("runtime_pool_pressure.csv",README)

if __name__=="__main__":
    unittest.main()
