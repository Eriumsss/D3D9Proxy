#!/usr/bin/env python3
"""The native PAK's outer material/shape/Havok table span checks.

Does not parse Havok MOPP data or run a game level.
"""
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

SRC=Path(__file__).resolve().parents[1]/"tools"/"pak_pool_preflight.py"
spec=importlib.util.spec_from_file_location("native_pak_tables",SRC)
assert spec and spec.loader
mod=importlib.util.module_from_spec(spec)
sys.modules[spec.name]=mod
spec.loader.exec_module(mod)

def fake_pak(path:Path, tables:list[tuple[str,int,int,int]], b1_size:int=1024)->Path:
    h=[0]*118
    h[2]=0x13
    h[7]=472
    h[8]=b1_size
    for name,offset,count,stride in tables:
        entry=next(x for x in mod.NATIVE_TABLE_ENVELOPES if x[0]==name)
        h[entry[1]]=stride
        h[entry[2]]=count
        h[entry[3]]=offset
    path.write_bytes(struct.pack("<118I",*h)+bytes(b1_size))
    return path

class NativeTables(unittest.TestCase):
    def setUp(self):
        tmp=tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir=Path(tmp.name)

    def test_valid_material_and_havok_table_descriptors(self):
        f=fake_pak(self.dir/"valid.pak",[
            ("buffer_infos",0,2,356),
            ("mat1",712,4,32),
            ("havok_shape_infos",840,2,20),
            ("shape_infos",880,3,16)])
        report=mod.inspect_pak(f,inspect_native_tables=True)
        self.assertFalse(report.failures)
        self.assertEqual(report.native_tables["buffer_infos"]["count"],2)
        self.assertEqual(report.native_tables["havok_shape_infos"]["declared_stride"],20)

    def test_broken_bufferinfo_range_is_failure(self):
        f=fake_pak(self.dir/"bad_buf.pak",[("buffer_infos",900,2,356)])
        report=mod.inspect_pak(f,inspect_native_tables=True)
        self.assertTrue(any("buffer_infos: Block1 table OOB" in x for x in report.failures))
        self.assertEqual(mod.main(["--base",str(f),"--native-tables"]),1)

    def test_havok_shape_out_of_bounds(self):
        f=fake_pak(self.dir/"bad_shape.pak",[
            ("havok_shape_infos",1020,2,16)])
        report=mod.inspect_pak(f,inspect_native_tables=True)
        self.assertTrue(any("havok_shape_infos: Block1 table OOB" in x
                            for x in report.failures))

    def test_zero_stride_nonempty_table_rejected(self):
        f=fake_pak(self.dir/"bad_zero.pak",[
            ("havok_constraint_data",128,1,0)])
        report=mod.inspect_pak(f,inspect_native_tables=True)
        self.assertTrue(any("zero declared stride" in x for x in report.failures))

    def test_32bit_overflow_not_accepted(self):
        f=fake_pak(self.dir/"bad_wrap.pak",[
            ("mat4",0xFFFFFFF0,0x80000000,8)])
        report=mod.inspect_pak(f,inspect_native_tables=True)
        self.assertTrue(report.failures)

    def test_off_by_default_without_native_flag(self):
        f=fake_pak(self.dir/"bad_optional.pak",[
            ("buffer_infos",900,2,356)])
        self.assertFalse(mod.inspect_pak(f).failures)

if __name__=="__main__":
    unittest.main()
