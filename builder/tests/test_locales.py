"""The locale manifests and tools: positions only, and the Japanese edit kinds."""
import gzip
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "locale_research"))


class ManifestTests(unittest.TestCase):
    def test_manifests_hold_positions_and_offsets_only(self):
        for name in ("spanish", "japanese"):
            with self.subTest(name):
                manifest = json.loads(gzip.decompress((ROOT / ("tools/locales/%s.json.gz" % name)).read_bytes()))
                self.assertEqual(len(manifest["rom_sha1"]), 40)
                for relative, file in manifest["files"].items():
                    self.assertEqual(len(file["sha256"]), 64)
                    previous = 0
                    for start, end, offset, size, kind in sorted(file["edits"]):
                        self.assertTrue(all(isinstance(n, int) for n in (start, end, offset, size)))
                        self.assertLessEqual(previous, start, relative)
                        self.assertLess(start, end)
                        self.assertTrue(0 <= offset < offset + size <= 16 << 20)
                        self.assertIsInstance(kind, str)
                        # bits5,3,8,8,8 are agbcc bitfield widths, and
                        # struct2B a two-dimensional table's count then its
                        # format: japanese_extras.py emits both shapes.
                        self.assertRegex(kind, r"^(c|cfixed|asm|u32|u16s|u16array|u8array|bard|u8rows22|u16rows10|bits[0-9,]+|struct[0-9BHbhIix]+)$")
                        previous = end
                for relative, offset, size in manifest["graphics"]:
                    self.assertFalse(Path(relative).is_absolute())
                    self.assertTrue(0 <= offset < offset + size <= 16 << 20)

    def test_japanese_manifest_is_for_bpej(self):
        from emerald3ds_builder import rom
        manifest = json.loads(gzip.decompress((ROOT / "tools/locales/japanese.json.gz").read_bytes()))
        self.assertEqual(manifest["rom_sha1"], rom.JAPANESE_SHA1)
        self.assertEqual(manifest["credits"]["pages"], 45)


class JapaneseKindsTests(unittest.TestCase):
    def test_replacements(self):
        import localize_japanese as L
        self.assertEqual(L.replacement("c", b"\x01\x02\xff"), "{0x01,0x02,0xFF}")
        self.assertEqual(L.replacement("asm", b"\x01\xff"), "\n\t.byte 0x01,0xFF\n")
        # a field filled to its width with spaces (0x00) and no terminator
        self.assertEqual(L.replacement("cfixed", b"\x10\x18\x00\x00\x00\x00"), "{0x10,0x18,0xFF}")
        self.assertEqual(L.replacement("u32", b"\x05\x00\x00\x00"), "5")
        self.assertEqual(L.replacement("u16s", bytes(range(12))), "0x0100,0x0302,0x0504,0x0706,0x0908,0x0B0A")
        self.assertEqual(L.replacement("structBBBBBBH", bytes([0, 2, 15, 27, 4, 15, 1, 0])), "{0,2,15,27,4,15,1}")
        self.assertEqual(L.replacement("structBBxx", bytes([1, 2, 0, 0, 3, 4, 0, 0])), "{1,2},{3,4}")
        with self.assertRaises(ValueError):
            L.replacement("bard", b"")


class RebaseTests(unittest.TestCase):
    def test_positions_follow_inserted_lines(self):
        import rebase_manifest
        old = 'a = _("ONE");\nb = _("TWO");\n'
        new = 'a = _("ONE");\n#if X\nc = 1;\n#endif\nb = _("TWO");\n'
        start = old.index('_("TWO")')
        moved = rebase_manifest.rebase_file(old, new, [[start, start + 8, 100, 4, "c"]])
        self.assertEqual(new[moved[0][0]:moved[0][1]], '_("TWO")')
        self.assertEqual(moved[0][2:], [100, 4, "c"])

    def test_a_changed_span_is_refused(self):
        import rebase_manifest
        old = 'b = _("TWO");\n'
        new = 'b = _("THREE");\n'
        with self.assertRaises(ValueError):
            rebase_manifest.rebase_file(old, new, [[4, 12, 0, 4, "c"]])


if __name__ == "__main__":
    unittest.main()
