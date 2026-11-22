#!/usr/bin/env python3
import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from collapse_stereo_planes import collapse_pack, collapse_pixels, fill_frame, fill_transparent, fill_pack

class CollapseTest(unittest.TestCase):
    def test_runtime_equivalent_and_transparency(self):
        src = bytes(p | (15 << 4) for p in range(10))
        self.assertEqual(collapse_pixels(src, 10, 4), bytes(0xf0 | p for p in [0,0,0,1,1,2,2,2,3,3]))
        self.assertEqual(collapse_pixels(src, 10, 10), src)
        with self.assertRaises(ValueError): collapse_pixels(b'\xfa', 10, 4)

    def test_variable_frames_and_index_unchanged(self):
        src = (b'EM3DSTM1' + struct.pack('<HBBHH', 2, 10, 2, 64, 64)
               + struct.pack('<HHI', 1, 0, 32) + struct.pack('<HHI', 2, 1, 4128)
               + b'\xf9' * 4096 + b'\x05' * 2048)
        out = collapse_pack(src)
        self.assertEqual(out[:10], src[:10])
        self.assertEqual(out[11:32], src[11:32])
        self.assertEqual(out[10], 4)
        # Collapsed (9 -> 3), then the transparent odd columns filled from
        # their opaque neighbours: no 15 is left.
        self.assertEqual(out[32:4128], b'\x33' * 4096)
        self.assertEqual(out[4128:], b'\x02' * 2048)
        self.assertEqual(collapse_pack(out), out)
        self.assertEqual(collapse_pack(out, smart=True, cleanup=True), out)
        with self.assertRaises(ValueError): collapse_pack(src[:-1])
        overlap = bytearray(src)
        struct.pack_into('<I', overlap, 28, 32)
        with self.assertRaises(ValueError): collapse_pack(overlap)

    def test_fill_takes_nearest_plane(self):
        pixels = [15] * 4096
        pixels[10 * 64 + 10] = 1
        pixels[10 * 64 + 50] = 3
        out = fill_frame(pixels)
        self.assertNotIn(15, out)
        self.assertEqual(out[10 * 64 + 10], 1)
        self.assertEqual(out[10 * 64 + 50], 3)
        self.assertEqual(out[0], 1)
        self.assertEqual(out[30 * 64 + 63], 3)
        self.assertEqual(out[40 * 64 + 15], 1)
        self.assertEqual(out[40 * 64 + 45], 3)
        self.assertEqual(fill_frame([15] * 4096), [0] * 4096)
        packed = bytes(pixels[i] | pixels[i + 1] << 4 for i in range(0, 4096, 2))
        filled = fill_transparent(packed * 2)
        self.assertEqual(len(filled), 4096)
        self.assertFalse(any(b & 15 == 15 or b >> 4 == 15 for b in filled))
        with self.assertRaises(ValueError): fill_transparent(b'\xff')

    def test_finished_pack_only_filled(self):
        src = (b'EM3DSTM1' + struct.pack('<HBBHH', 1, 4, 1, 64, 64)
               + struct.pack('<HHI', 1, 1, 24) + b'\xf2' * 2048)
        out = collapse_pack(src)
        self.assertEqual(out[:24], src[:24])
        self.assertEqual(out[24:], b'\x22' * 2048)


    def test_release_pack_validation_and_deterministic_copy(self):
        # Synthetic one-frame and two-frame entries with valid packed planes.
        header = b'EM3DSTM1' + struct.pack('<HBBHH', 2, 4, 2, 64, 64)
        index = struct.pack('<HHI', 1, 1, 32) + struct.pack('<HHI', 2, 0, 2080)
        source = header + index + b'\xf2' * 2048 + b'\x23' * 4096
        output = fill_pack(source)
        self.assertEqual(output[:32], source[:32])
        self.assertEqual(output[32:2080], b'\x22' * 2048)
        self.assertEqual(output[2080:], source[2080:])
        self.assertEqual(fill_pack(output), output)
        self.assertEqual(fill_pack(b'EM3DSTM1' + struct.pack('<HBBHH', 0, 4, 2, 64, 64)),
                         b'EM3DSTM1' + struct.pack('<HBBHH', 0, 4, 2, 64, 64))

        invalid = []
        x = bytearray(source); x[10] = 1; invalid.append(x)  # unsupported plane count
        x = bytearray(source); x[11] = 6; invalid.append(x)  # too many frames for runtime
        x = bytearray(source); x[12] = 32; invalid.append(x)  # wrong width
        x = bytearray(source); struct.pack_into('<H', x, 8, 521); invalid.append(x)  # overfull index
        x = bytearray(source); struct.pack_into('<H', x, 24, 1); invalid.append(x)  # duplicate ID
        x = bytearray(source); struct.pack_into('<I', x, 28, 32); invalid.append(x) # overlapping offset
        x = bytearray(source); struct.pack_into('<I', x, 20, 33); invalid.append(x) # gap
        x = bytearray(source); x[32] = 0x45; invalid.append(x)  # plane >= 4
        invalid.append(source[:-1])    # missing payload byte
        invalid.append(source + b'\x00') # trailing data
        invalid.append(source[:23])    # missing index
        for case in invalid:
            with self.subTest(length=len(case), beginning=bytes(case[:32])):
                with self.assertRaises(ValueError):
                    fill_pack(case)

if __name__ == '__main__': unittest.main()
