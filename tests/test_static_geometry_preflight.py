#!/usr/bin/env python3
"""Native PC PAK VB/IB descriptor accounting without BIN payload claims."""
from pathlib import Path
import importlib.util
import struct
import sys
import tempfile
import unittest
import zlib

SCRIPT=Path(__file__).resolve().parents[1]/"tools"/"pak_pool_preflight.py"
spec=importlib.util.spec_from_file_location("pak_pool_preflight_geometry",SCRIPT)
assert spec and spec.loader
pre=importlib.util.module_from_spec(spec)
sys.modules[spec.name]=pre
spec.loader.exec_module(pre)

def make_geom(path:Path, *, vb:list[tuple[int,int,int]]|None=None,
              ib:list[tuple[int,int,int,int]]|None=None,
              b2_size:int=512, compressed:bool=False,
              header_stride_inflated:bool=True)->Path:
    """Synthetic PAK: VBuffInfo[32] / IBuffInfo[24] in decompressed Block1.

    vb tuple: (size, model BIN asset offset, fmt1)
    ib tuple: (size, model BIN asset offset, index bits, keep-CPU flag)
    """
    if vb is None: vb=[(128,0,0),(64,128,0x40000)]
    if ib is None: ib=[(48,192,0x10,1),(32,240,0x20,4)]
    h=[0]*118
    h[2]=0x13
    h[30]=56 if header_stride_inflated else 32
    h[31]=52 if header_stride_inflated else 24
    h[55]=len(vb)
    h[56]=len(ib)
    h[80]=0
    h[81]=len(vb)*32
    buf=bytearray()
    for size,off,fmt in vb:
        row=bytearray(32)
        struct.pack_into("<I",row,4,size)
        struct.pack_into("<I",row,12,off)
        struct.pack_into("<I",row,16,fmt)
        buf+=row
    for size,off,bits,cpu_flag in ib:
        row=bytearray(24)
        struct.pack_into("<I",row,4,size)
        struct.pack_into("<I",row,8,bits)
        struct.pack_into("<I",row,12,cpu_flag)
        struct.pack_into("<I",row,16,off)
        buf+=row
    if not buf: buf+=bytes(16)
    b1=zlib.compress(buf) if compressed else bytes(buf)
    h[7]=472
    h[8]=len(buf)
    h[9]=len(b1) if compressed else 0
    h[11]=472+len(b1)
    h[12]=b2_size
    h[13]=0
    path.write_bytes(struct.pack("<118I",*h)+b1+bytes(b2_size))
    return path

class GeometrySpanTests(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root=Path(temp.name)

    def test_valid_buffers_report_cpu_and_gpu_bytes(self):
        path=make_geom(self.root/"base.pak")
        data=path.read_bytes()
        report=pre.inspect_pak(path,inspect_geometry=True)
        self.assertFalse(report.failures)
        vb=report.geometry["buffer_tables"]["vertex_infos"]
        ib=report.geometry["buffer_tables"]["index_infos"]
        self.assertEqual(vb["bytes_requested"],192)
        self.assertEqual(vb["bytes_cpu_only"],64)
        self.assertEqual(vb["bytes_managed_d3d"],128)
        self.assertEqual(ib["bytes_managed_d3d"],80)
        self.assertEqual(ib["bytes_kept_cpu_index_copies"],48)
        self.assertEqual(vb["unverified_bin_asset_spans"],2)
        self.assertEqual(ib["unverified_bin_asset_spans"],2)
        self.assertEqual(path.read_bytes(),data)

    def test_large_bin_relative_offset_not_compared_to_block2(self):
        path=make_geom(self.root/"bad_vb.pak",vb=[(64,0xFFFFFFF0,0)])
        report=pre.inspect_pak(path,inspect_geometry=True)
        self.assertFalse(report.failures)
        self.assertEqual(report.geometry["buffer_tables"]["vertex_infos"]["unverified_bin_asset_spans"],1)
        self.assertEqual(pre.main(["--base",str(path),"--geometry"]),0)

    def test_index_payload_may_exceed_unrelated_block2(self):
        path=make_geom(self.root/"bad_ib.pak",ib=[(256,400,0x10,0)])
        report=pre.inspect_pak(path,inspect_geometry=True)
        self.assertFalse(report.failures)
        self.assertEqual(report.geometry["buffer_tables"]["index_infos"]["unverified_bin_asset_spans"],1)

    def test_zlib_block1_works_with_geometry(self):
        path=make_geom(self.root/"compressed.pak",compressed=True)
        report=pre.inspect_pak(path,inspect_geometry=True)
        self.assertFalse(report.failures)
        self.assertEqual(report.geometry["disk_strides"],
                         {"vertex_infos":32,"index_infos":24})

    def test_disk_strides_not_header_declared_sizes(self):
        path=make_geom(self.root/"inflated_sizes.pak",header_stride_inflated=True)
        report=pre.inspect_pak(path,inspect_geometry=True)
        self.assertEqual(report.geometry["vbuff_info_declared_size_not_disk_stride"],56)
        self.assertEqual(report.geometry["ibuff_info_declared_size_not_disk_stride"],52)
        self.assertEqual(report.geometry["buffer_tables"]["vertex_infos"]["count"],2)
        self.assertFalse(report.failures)

    def test_unexpected_index_format_warning_is_not_capacity_failure(self):
        path=make_geom(self.root/"fmt.pak",ib=[(32,192,0x65,0)])
        report=pre.inspect_pak(path,inspect_geometry=True)
        self.assertTrue(any("unexpected index-format" in x for x in report.warnings))
        self.assertFalse(report.failures)

    def test_zero_sized_buffers_are_reported_without_crashing(self):
        path=make_geom(self.root/"zero.pak",vb=[(0,0,0)],ib=[])
        report=pre.inspect_pak(path,inspect_geometry=True)
        self.assertEqual(report.geometry["buffer_tables"]["vertex_infos"]["zero_size"],1)
        self.assertFalse(report.failures)

    def test_zero_block2_size_does_not_invalidate_model_bin_buffers(self):
        # Block2 is irrelevant to per-model BIN vertex/index payloads.
        path=make_geom(self.root/"no_block2.pak",vb=[(16,0,0)],ib=[],
                       b2_size=0)
        report=pre.inspect_pak(path,inspect_geometry=True)
        self.assertFalse(report.failures)

if __name__=="__main__":
    unittest.main()
