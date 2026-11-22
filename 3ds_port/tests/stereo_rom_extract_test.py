"""Independent hand-encoded decoder cases; run with python3 this-file."""
import importlib.util
from pathlib import Path
import unittest
import tempfile
spec=importlib.util.spec_from_file_location('extractor',Path(__file__).resolve().parents[1]/'scripts/extract_stereo_fronts.py')
m=importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
class DecoderTests(unittest.TestCase):
    def test_literal(self):
        self.assertEqual(m.unlz(bytes.fromhex('10030000 00 123456')),(bytes.fromhex('123456'),8))
    def test_overlapping_backreference(self):
        self.assertEqual(m.unlz(bytes.fromhex('10060000 40 41 2000'))[0],b'AAAAAA')
    def test_truncated(self):
        with self.assertRaises(ValueError): m.unlz(bytes.fromhex('10030000 00 12'))
    def test_invalid_distance(self):
        with self.assertRaises(ValueError): m.unlz(bytes.fromhex('10030000 80 0000'))
    def test_output_overflow(self):
        with self.assertRaises(ValueError): m.unlz(bytes.fromhex('10030000 40 41 2000'))
    def test_tiling_and_nibble_order(self):
        # Two horizontal tiles: first pixels alternate 1,2; second 3,4.
        data=bytes([0x21])*32+bytes([0x43])*32
        pixels,height=m.untile(data,16)
        self.assertEqual(height,8)
        self.assertEqual(pixels,bytes([1,2]*4+[3,4]*4)*8)
    def test_duplicate_palette_provenance(self):
        stream=bytes.fromhex('10030000 00 123456')
        with tempfile.TemporaryDirectory() as folder:
            asset=Path(folder)/'palette.lz';asset.write_bytes(stream)
            rom=b'prefix'+stream+b'gap'+stream
            with self.assertRaises(ValueError): m.locate(rom,asset)
            raw,meta=m.locate(rom,asset,allow_identical_duplicates=True)
            self.assertEqual(raw,bytes.fromhex('123456'))
            self.assertEqual(meta['identical_match_offsets'],[6,17])
            self.assertEqual(meta['offset'],6)
    def test_tile_dimensions(self):
        with self.assertRaises(ValueError): m.untile(b'\0',64)
if __name__=='__main__': unittest.main()
