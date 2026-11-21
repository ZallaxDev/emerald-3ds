"""Developer caches must miss after content changes and interrupted writes."""

import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "3ds_port" / "scripts"))
import voxel_cache


class VoxelCacheTests(unittest.TestCase):
    def test_inputs_are_content_addressed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            port = root / "3ds_port"
            paths = [root / "data/maps/Example/map.json", root / "data/tilesets/tiles.4bpp",
                     port / "scripts/helper.py", root / "local-fixes.json"]
            for path in paths:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"version 1")

            def stamp():
                voxel_cache.fingerprint.cache_clear()
                return voxel_cache.fingerprint(root, port, paths[-1])

            for path in paths:
                before = stamp()
                path.write_bytes(b"version 2")
                self.assertNotEqual(stamp(), before)
                path.write_bytes(b"version 1")
                self.assertEqual(stamp(), before)

    def test_corruption_and_failed_publish(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cache.bin"
            voxel_cache.write(path, ("input A",), {"answer": 42})
            self.assertEqual(voxel_cache.read(path, ("input A",)), {"answer": 42})
            self.assertIsNone(voxel_cache.read(path, ("input B",)))
            with patch.object(Path, "replace", side_effect=OSError("simulated interrupted replace")):
                voxel_cache.write(path, ("input A",), {"answer": 99})
            self.assertEqual(voxel_cache.read(path, ("input A",)), {"answer": 42})
            self.assertEqual(list(Path(tmp).iterdir()), [path])
            path.write_bytes(path.read_bytes()[:-2])
            self.assertIsNone(voxel_cache.read(path, ("input A",)))
