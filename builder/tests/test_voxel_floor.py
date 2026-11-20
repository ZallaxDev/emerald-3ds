"""Floor classification must preserve every RGBA comparison and tile offset."""

import importlib.util
from pathlib import Path
import random
import unittest

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "voxel_floor", ROOT / "3ds_port/scripts/voxel_floor.py")
FLOOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FLOOR)


class FloorMaskTests(unittest.TestCase):
    def test_repeated_tiles_match_original_classification(self):
        rng = random.Random(247)
        palette = [(17, 31, 45, 0), (17, 31, 45, 255),
                   (90, 80, 70, 255), (1, 2, 3, 255)]
        floors = []
        for _ in range(4):
            floor = Image.new("RGBA", (16, 16))
            floor.putdata([rng.choice(palette) for _ in range(256)])
            floors.append(floor)
        # Duplicate tiles, absent floors, partial tiles and several repeats.
        for candidates in ([], floors[:1], floors + floors[:1]):
            with self.subTest(count=len(candidates)):
                art = Image.new("RGBA", (67, 35))
                art.putdata([rng.choice(palette) for _ in range(67 * 35)])
                pixels = art.load()
                tiles = [floor.load() for floor in candidates]
                expected = [[any(tile[x % 16, y % 16] == pixels[x, y]
                                 for tile in tiles) for x in range(67)]
                            for y in range(35)]
                before = art.tobytes(), [tile.tobytes() for tile in candidates]
                self.assertEqual(FLOOR.match_floor_pixels(art, candidates), expected)
                self.assertEqual(before, (art.tobytes(), [tile.tobytes() for tile in candidates]))

    def test_alpha_is_part_of_the_match(self):
        floor = Image.new("RGBA", (16, 16), (17, 31, 45, 0))
        art = Image.new("RGBA", (1, 2), (17, 31, 45, 255))
        art.putpixel((0, 1), (17, 31, 45, 0))
        self.assertEqual(FLOOR.match_floor_pixels(art, [floor]), [[False], [True]])


if __name__ == "__main__":
    unittest.main()
