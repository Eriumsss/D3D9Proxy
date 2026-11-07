#!/usr/bin/env python3
"""Executable synthetic PAK checks for the read-only static-map preflight."""
from __future__ import annotations

import importlib.util
import json
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "pak_pool_preflight.py"
spec = importlib.util.spec_from_file_location("pak_pool_preflight", SCRIPT)
assert spec is not None and spec.loader is not None
pre = importlib.util.module_from_spec(spec)
import sys
sys.modules[spec.name] = pre
spec.loader.exec_module(pre)


def make_pak(folder: Path, name: str, counts: dict[str, int],
             *, compressed: bool = True, truncate_table: bool = False,
             corrupt_zlib: bool = False, duplicate_models: bool = False,
             zero_model_key: bool = False, model_mode_mask: int = 0xFFFFFFFF) -> Path:
    hdr = [0] * 118
    hdr[2] = 0x13
    block = bytearray()
    for table, (count_key, offset_key, stride, _) in pre.TABLES.items():
        count = counts.get(table, 0)
        hdr[pre.IDX[count_key]] = count
        hdr[pre.IDX[offset_key]] = len(block)
        for i in range(count):
            row = bytearray(stride)
            key = 0 if (table == "models" and i == 0 and zero_model_key) else (
                0x11223344 if (table == "models" and duplicate_models) else (i+1))
            struct.pack_into("<II", row, 0, key,
                             model_mode_mask if table == "models" else 0xFFFFFFFF)
            block.extend(row)
    if not block:
        block.extend(bytes(16))
    if truncate_table:
        # Leave a valid zlib region but a table that extends beyond Block1.
        hdr[pre.IDX["model_info_num"]] = counts.get("models", 0) + 100
    body = zlib.compress(block) if compressed else bytes(block)
    if corrupt_zlib:
        body = b"busted-zlib" + body[11:]
    hdr[pre.IDX["block1_offset"]] = 472
    hdr[pre.IDX["block1_size"]] = len(block)
    hdr[pre.IDX["block1_size_comp"]] = len(body) if compressed else 0
    out = folder / name
    out.write_bytes(struct.pack("<118I", *hdr) + body)
    return out


class StaticPoolPakTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_valid_uncompressed(self) -> None:
        p = make_pak(self.root, "helm.pak", {"models": 300, "textures": 150},
                     compressed=False)
        before = p.read_bytes()
        r = pre.inspect_pak(p, deep=True)
        self.assertEqual(r.counts["models"], 300)
        self.assertFalse(r.failures)
        self.assertEqual(r.details["models"]["unique_keys"], 300)
        self.assertEqual(p.read_bytes(), before)

    def test_valid_compressed_and_json(self) -> None:
        p = make_pak(self.root, "helm.pak", {"models": 100, "effects": 3})
        r = pre.inspect_pak(p, deep=True)
        self.assertEqual(r.details["effects"]["unique_keys"], 3)
        self.assertEqual(pre.main(["--base", str(p), "--json", "--deep"]), 0)

    def test_model_factory_bails_before_registry_counts_rise(self) -> None:
        p = make_pak(self.root, "merged.pak", {"models": 1100})
        r = pre.inspect_pak(p)
        self.assertTrue(any("models: raw info count 1100 > retail factory guard 1024" in
                            error for error in r.failures))
        self.assertEqual(pre.main(["--base", str(p)]), 1)

    def test_texture_and_effect_guards(self) -> None:
        p = make_pak(self.root, "merged.pak", {"textures": 2049, "effects": 513})
        r = pre.inspect_pak(p)
        self.assertEqual(len(r.failures), 2)

    def test_expanded_caps_are_explicit_assumptions_only(self) -> None:
        p = make_pak(self.root,"expanded.pak",{"models":1700,"textures":3400})
        stock = pre.inspect_pak(p)
        assumed = pre.inspect_pak(p,assume_caps={"models":2048,"textures":4096})
        self.assertEqual(len(stock.failures),2)
        self.assertEqual(assumed.caps["models"],2048)
        self.assertEqual(assumed.caps["textures"],4096)
        self.assertFalse(assumed.failures)
        self.assertEqual(pre.main(["--base",str(p)]),1)
        self.assertEqual(pre.main(["--base",str(p),
                                   "--assume-model2048","--assume-texture4096"]),0)

    def test_effect_patch_1024_is_explicitly_assumed_only(self) -> None:
        p = make_pak(self.root, "fx_merged.pak",
                     {"models":1400, "textures":2700, "effects":900})
        default = pre.inspect_pak(p)
        self.assertEqual(len(default.failures), 3)
        self.assertEqual(pre.main(["--base", str(p)]), 1)
        result=pre.inspect_pak(p, assume_caps={
            "models":2048, "textures":4096, "effects":1024})
        self.assertFalse(result.failures)
        self.assertEqual(pre.main([
            "--base", str(p),
            "--assume-model2048",
            "--assume-texture4096",
            "--assume-effects1024"]), 0)

    def test_assumed_caps_do_not_relax_unrelated_effect_and_gfx(self) -> None:
        p = make_pak(self.root,"still_bad.pak",{
            "models":1300,"textures":2500,"effects":513,"gfx_blocks":65})
        assumed=pre.inspect_pak(p, assume_caps={"models":2048,"textures":4096})
        self.assertEqual(len(assumed.failures),2)
        self.assertTrue(any("effects:" in x for x in assumed.failures))
        self.assertTrue(any("gfx_blocks:" in x for x in assumed.failures))

    def test_pair_projection_is_only_risk_not_false_confirmed_failure(self) -> None:
        base = pre.inspect_pak(make_pak(self.root, "helm.pak", {"models": 600}))
        donor = pre.inspect_pak(make_pak(self.root, "isengard.pak", {"models": 550}))
        row = next(x for x in pre.projection(base, donor) if x["registry"]=="models")
        self.assertTrue(row["risk_if_all_appended"])
        self.assertEqual(row["naive_append_upper_bound"], 1150)
        self.assertEqual(pre.main(["--base", base.path, "--donor", donor.path]), 0)

    def test_invalid_magic(self) -> None:
        p = make_pak(self.root, "wrong.pak", {"models": 1})
        raw = bytearray(p.read_bytes())
        struct.pack_into("<I", raw, 8, 0x1234)
        p.write_bytes(raw)
        with self.assertRaises(pre.InvalidPak):
            pre.inspect_pak(p)

    def test_truncated_header(self) -> None:
        p = self.root / "too_short.pak"
        p.write_bytes(bytes(200))
        with self.assertRaises(pre.InvalidPak):
            pre.inspect_pak(p)

    def test_corrupt_block_bounds(self) -> None:
        p = make_pak(self.root, "short.pak", {"models": 1})
        raw = bytearray(p.read_bytes())
        struct.pack_into("<I", raw, 7*4, 0xFFFFFF00)
        p.write_bytes(raw)
        with self.assertRaises(pre.InvalidPak):
            pre.inspect_pak(p)

    def test_out_of_bounds_model_table(self) -> None:
        p = make_pak(self.root, "bad_table.pak", {"models": 2},
                     truncate_table=True)
        r = pre.inspect_pak(p, deep=True)
        self.assertTrue(any("table OOB" in err for err in r.failures))

    def test_corrupt_zlib(self) -> None:
        p = make_pak(self.root, "bad_zlib.pak", {"models": 1},
                     corrupt_zlib=True)
        with self.assertRaises(pre.InvalidPak):
            pre.inspect_pak(p, deep=True)

    def test_gamemode_eligibility_is_not_the_factory_capacity(self) -> None:
        p = make_pak(self.root, "masked.pak", {"models": 1100}, model_mode_mask=0x04)
        masked_in = pre.inspect_pak(p, deep=True, game_mask=0x04)
        masked_out = pre.inspect_pak(p, deep=True, game_mask=0x08)
        self.assertEqual(masked_in.details["models"]["eligible_for_mask"], 1100)
        self.assertEqual(masked_out.details["models"]["eligible_for_mask"], 0)
        # Raw count remains 1100: retail 0x400 factory cap still fires regardless
        # of the active gamemode mask. This is why a model can be invisible.
        self.assertTrue(masked_out.failures)
        self.assertEqual(pre.main(["--base", str(p), "--deep", "--mask", "0x04"]), 1)

    def test_duplicate_and_zero_key_warnings(self) -> None:
        p = make_pak(self.root, "dup.pak", {"models": 3},
                     duplicate_models=True)
        r = pre.inspect_pak(p, deep=True)
        self.assertEqual(r.details["models"]["duplicate_keys"], 2)
        self.assertTrue(any("duplicate CRC" in x for x in r.warnings))
        q = make_pak(self.root, "zero.pak", {"models": 1},zero_model_key=True)
        self.assertEqual(pre.inspect_pak(q, deep=True).details["models"]["zero_keys"],1)

    def test_unsigned_overflow_safe_span_check(self) -> None:
        p = make_pak(self.root,"wrap.pak", {"models": 1})
        raw = bytearray(p.read_bytes())
        struct.pack_into("<I", raw, pre.IDX["model_info_offset"]*4,0xFFFFFFF0)
        p.write_bytes(raw)
        r = pre.inspect_pak(p, deep=True)
        self.assertTrue(r.failures)


if __name__ == "__main__":
    unittest.main()
