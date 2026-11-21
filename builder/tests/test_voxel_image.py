"""Pixel access preserves the values and order needed by the voxel generators."""

import sys
import unittest
import warnings
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "3ds_port" / "scripts"))
from voxel_image import image_pixels


class ImagePixelsTests(unittest.TestCase):
    def test_preserves_repeats_alpha_and_row_order(self):
        pixels = ((2, 3, 4, 0), (2, 3, 4, 0), (8, 7, 6, 128), (12, 1, 0, 255))
        image = Image.new("RGBA", (2, 2))
        image.putdata(pixels)
        with warnings.catch_warnings():
            warnings.simplefilter("error", DeprecationWarning)
            self.assertEqual(image_pixels(image), pixels)

    def test_palette_indices_and_empty_image(self):
        image = Image.new("P", (3, 1))
        image.putdata([5, 1, 5])
        self.assertEqual(image_pixels(image), (5, 1, 5))
        self.assertEqual(image_pixels(Image.new("L", (0, 0))), ())

    def test_legacy_image_falls_back_to_getdata(self):
        class LegacyImage:
            def getdata(self):
                return iter((7, 3, 7))

        self.assertEqual(image_pixels(LegacyImage()), (7, 3, 7))
