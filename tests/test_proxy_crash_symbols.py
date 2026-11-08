#!/usr/bin/env python3
import importlib.util
import io
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("symbolize_proxy_crash",ROOT/"tools/symbolize_proxy_crash.py")
tool=importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
WORKFLOW=(ROOT/".github/workflows/static-map-pak-preflight.yml").read_text("utf-8")
BATCH=(ROOT/"build.bat").read_text("utf-8")
MOCK_MAP=""" Start         Length     Name                   Class
 Preferred load address is 10000000
  Address         Publics by Value              Rva+Base       Lib:Object
 0001:00001000 _First 10001000 f one.obj
 0001:0000B000 _Suspect 1000B000 f two.obj
 0001:0000B200 _Next 1000B200 f three.obj
"""

class CrashSymbolizerContract(unittest.TestCase):
    def test_user_exception_rva(self):
        self.assertEqual(0x6DBDB1B9-0x6DBD0000,0xB1B9)

    def test_nearest_symbol_not_exact(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/"d3d9.map"
            p.write_text(MOCK_MAP)
            preferred,before,after=tool.lookup(p,0xB1B9)
            self.assertEqual(preferred,0x10000000)
            self.assertEqual(before[:2],(0xB000,"_Suspect"))
            self.assertEqual(after[:2],(0xB200,"_Next"))
            output=io.StringIO()
            with redirect_stdout(output):
                code=tool.main(["--map",str(p),"--eip","0x6DBDB1B9","--module-base","0x6DBD0000"])
            self.assertEqual(code,0)
            self.assertIn("NOT proof",output.getvalue())

    def test_reject_invalid_and_missing_map(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/"d3d9.map"
            p.write_text("no symbol data")
            with self.assertRaises(ValueError): tool.lookup(p,0xB1B9)
            with self.assertRaises(ValueError): tool.lookup(p,-1)

    def test_full_x86_dll_and_symbols_uploaded_together(self):
        for part in ("/Zi","/DEBUG:FULL","/MAP:","/PDB:","d3d9.map","d3d9.pdb"):
            self.assertIn(part,WORKFLOW)
            self.assertIn(part,BATCH)
        for part in ("src/d3d9.dll","src/d3d9.pdb","src/d3d9.map"):
            self.assertIn(part,WORKFLOW)

    def test_ci_includes_regressions(self):
        self.assertIn("test_proxy_crash_symbols.py -v",WORKFLOW)
        self.assertEqual(WORKFLOW.count("'tests/test_proxy_crash_symbols.py'"),2)

if __name__=="__main__":
    unittest.main()
