#!/usr/bin/env python3
"""Synthetic PC PAK + BIN model-reference validation. No mission data."""
from pathlib import Path
import importlib.util
import struct
import sys
import tempfile
import unittest
import zlib

SRC = Path(__file__).resolve().parents[1] / "tools" / "pak_pool_preflight.py"
spec = importlib.util.spec_from_file_location("native_bin_model_test", SRC)
assert spec and spec.loader
pre = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = pre
spec.loader.exec_module(pre)

KEY = 0x12345678
MODEL_KEY = 0xAA0100CC

def put(data: bytearray, at: int, value: int) -> None:
    struct.pack_into("<I", data, at, value)

def fixture(root: Path, *, vb_offset: int = 0, vb_ptr: int = 640,
            mesh_idx: int = 0, asset_key: int = KEY,
            bin_size: int = 70, packed: bool = False,
            bad_zlib: bool = False, extra_texture: bool = False
           ) -> tuple[Path, Path]:
    block1 = bytearray(1024)
    # ModelInfo at 0: one LOD0 slot, one BufferInfo, BIN asset key.
    put(block1, 0, MODEL_KEY)
    put(block1, 4, 1)
    put(block1, 12, 256)
    put(block1, 48, 720)
    for at in (56, 60, 64, 68):
        put(block1, at, 1)
    put(block1, 132, 1)
    put(block1, 208, asset_key)
    # BufferInfo at 256: one vertex stream + index buffer.
    put(block1, 256, vb_ptr)
    put(block1, 256 + 128, 16)
    put(block1, 256 + 260, 672)
    put(block1, 256 + 264, 3)
    put(block1, 640 + 4, 64)
    put(block1, 640 + 12, vb_offset)
    put(block1, 672 + 4, 6)
    put(block1, 672 + 8, 0x10)
    put(block1, 672 + 16, 64)
    put(block1, 720, mesh_idx)

    header = [0] * 118
    header[2], header[7], header[8] = 0x13, 472, len(block1)
    header[20], header[30], header[31] = 356, 56, 52
    header[44], header[45], header[55], header[56] = 1, 1, 1, 1
    header[69], header[70], header[80], header[81] = 0, 256, 640, 672
    pak = root / "synthetic.pak"
    pak.write_bytes(struct.pack("<118I", *header) + block1)

    bin_header = bytearray(172)
    put(bin_header, 0, 6)
    put(bin_header, 20, 2 if extra_texture else 1)
    put(bin_header, 24, 172)
    put(bin_header, 32, 1)  # vdata_num: model section boundary
    payload = bytes(bin_size)
    if packed:
        payload = zlib.compress(payload)
    if bad_zlib:
        payload = b"not-a-zlib-stream"
    payload_at = 172 + (40 if extra_texture else 20)
    handles = bytearray(40 if extra_texture else 20)
    struct.pack_into("<5I", handles, 0, KEY, payload_at, bin_size,
                     len(payload) if packed or bad_zlib else 0, 0xDEADBEEF)
    if extra_texture:
        struct.pack_into("<5I", handles, 20, KEY, payload_at, 1, 0, 0xABCD)
    bin_path = root / "synthetic.bin"
    bin_path.write_bytes(bin_header + handles + payload)
    return pak, bin_path

class ModelBinReferences(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def inspect(self, **kwargs):
        pak, bin_path = fixture(self.root, **kwargs)
        return pre.inspect_pak(pak, inspect_geometry=True,
                               inspect_links=True, bin_path=bin_path)

    def test_valid_native_refs_and_bin_spans(self):
        r = self.inspect()
        self.assertEqual(r.failures, [])
        self.assertEqual(r.model_links["mesh_slots"], 1)
        self.assertEqual(r.model_links["distinct_buffer_infos"], 1)
        self.assertEqual(r.model_links["checked_vb_refs"], 1)
        self.assertEqual(r.model_links["checked_ib_refs"], 1)
        self.assertEqual(r.model_links["checked_asset_spans"], 2)

    def test_wrong_legacy_block2_check_does_not_reject_valid_bin(self):
        # PAK Block2 == 0 while vertex and index payload live in BIN.
        r = self.inspect()
        self.assertFalse(r.failures)
        self.assertEqual(r.geometry["block2_uncompressed_bytes_unrelated_to_model_geometry"], 0)

    def test_bin_asset_span_overflow_is_rejected(self):
        r = self.inspect(vb_offset=0xFFFFFFF0)
        self.assertTrue(any("BIN asset payload OOB" in x for x in r.failures))

    def test_invalid_unaligned_vb_info_pointer_is_rejected(self):
        r = self.inspect(vb_ptr=641)
        self.assertTrue(any("invalid Block1 vertex pointer" in x for x in r.failures))

    def test_mesh_order_index_is_model_relative(self):
        r = self.inspect(mesh_idx=1)
        self.assertTrue(any("BufferInfo relative index 1 >= 1" in x for x in r.failures))

    def test_missing_model_bin_handle_is_rejected(self):
        r = self.inspect(asset_key=0xDEADBEEF)
        self.assertTrue(any("missing model BIN handle" in x for x in r.failures))

    def test_tex_section_handle_does_not_shadow_model(self):
        r = self.inspect(extra_texture=True)
        self.assertEqual(r.model_links["checked_asset_spans"], 2)
        self.assertFalse(r.failures)

    def test_compressed_bin_payload_can_be_verified(self):
        pak, bin_path = fixture(self.root, packed=True)
        r = pre.inspect_pak(pak, inspect_links=True, bin_path=bin_path,
                            verify_bin_streams=True)
        self.assertFalse(r.failures)
        self.assertEqual(r.model_links["verified_model_asset_streams"], 1)

    def test_invalid_bin_zlib_stream_is_reported(self):
        pak, bin_path = fixture(self.root, packed=True, bad_zlib=True)
        r = pre.inspect_pak(pak, inspect_links=True, bin_path=bin_path,
                            verify_bin_streams=True)
        self.assertTrue(any("zlib" in s or "compressed stream" in s
                            for s in r.failures))

    def test_bin_handle_table_truncation_is_detected(self):
        pak, bin_path = fixture(self.root)
        data = bytearray(bin_path.read_bytes())
        put(data, 20, 0xFFFFFFF0)
        bin_path.write_bytes(data)
        r = pre.inspect_pak(pak, inspect_links=True, bin_path=bin_path)
        self.assertTrue(any("handle table OOB" in s for s in r.failures))

    def test_model_links_without_bin_says_unverified(self):
        pak, _ = fixture(self.root)
        r = pre.inspect_pak(pak, inspect_links=True)
        self.assertFalse(r.failures)
        self.assertEqual(r.model_links["checked_asset_spans"], 0)
        self.assertTrue(any("UNVERIFIED" in s for s in r.warnings))

    def test_cli_with_matching_bin_pair(self):
        pak, bin_path = fixture(self.root)
        self.assertEqual(pre.main(["--base", str(pak), "--bin-base", str(bin_path),
                                   "--geometry", "--model-links", "--json"]), 0)

if __name__ == "__main__":
    unittest.main()
