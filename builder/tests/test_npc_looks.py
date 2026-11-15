"""New looks for the people and the player (emerald3ds_builder/npc_looks.py)
on synthetic sheets only: no Facets or Emerald Lens art and no network."""

import io
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "builder"))
from emerald3ds_builder import npc_looks  # noqa: E402


def sheet_png(width, height, colours):
    """An RGBA PNG: a column of `colours` cycling down a transparent sheet."""
    from PIL import Image

    image = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    for y in range(height):
        for x in range(4, width - 4):
            image.putpixel((x, y), colours[(x + y) % len(colours)] + (255,))
    out = io.BytesIO()
    image.save(out, "PNG")
    return out.getvalue()


class ConvertTests(unittest.TestCase):
    def test_person_sheet(self):
        data, _ = npc_looks.convert(sheet_png(32, 32 * 9, [(255, 0, 0), (0, 255, 0)]))
        self.assertEqual(data[:4], b"NPL1")
        self.assertEqual(tuple(data[4:7]), (9, 32, 32))
        self.assertEqual(len(data), 40 + 9 * 512)
        palette = struct.unpack("<16H", data[8:40])
        self.assertEqual(palette[0], 0)
        self.assertIn(0x001F, palette)   # red, BGR555

    def test_many_colours_cut_to_fifteen(self):
        colours = [(i * 12, 255 - i * 12, i * 5) for i in range(20)]
        data, _ = npc_looks.convert(sheet_png(32, 32 * 9, colours))
        indices = {b & 15 for b in data[40:]} | {b >> 4 for b in data[40:]}
        self.assertLessEqual(max(indices), 15)

    def test_narrow_player_sheet_centred(self):
        data, shown = npc_looks.convert(sheet_png(16, 32 * 18, [(0, 0, 255)]), None)
        self.assertEqual(data[4], 18)
        self.assertEqual(shown.size, (32, 32 * 18))
        self.assertEqual(shown.getpixel((0, 0))[3], 0)

    def test_back_picture(self):
        data, _ = npc_looks.convert(sheet_png(64, 64 * 4, [(1, 2, 3)]), None, 64)
        self.assertEqual(tuple(data[4:7]), (4, 64, 64))
        self.assertEqual(len(data), 40 + 4 * 2048)

    def test_wrong_size_refused(self):
        with self.assertRaises(ValueError):
            npc_looks.convert(sheet_png(32, 100, [(1, 2, 3)]))


class SectionTests(unittest.TestCase):
    def test_paths_cover_the_lists(self):
        paths = npc_looks.pack_paths()
        self.assertEqual(len(paths), len(npc_looks.FILES) + len(npc_looks.PLAYER_FILES) + 2)
        self.assertTrue(all(p.startswith(("npcs/", "players/")) for p in paths))

    def test_player_looks_in_the_option_order(self):
        looks = {look for look, _, _, _ in npc_looks.PLAYER_FILES}
        self.assertEqual(looks, set(npc_looks.PLAYER_LOOKS))


if __name__ == "__main__":
    unittest.main()
