"""Relief must consume the buildings output in desktop and web builds."""
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from emerald3ds_builder import voxel

BUILDINGS = '''import argparse
from pathlib import Path
ap = argparse.ArgumentParser()
ap.add_argument("--output", required=True)
args = ap.parse_args()
Path(args.output).write_bytes(b"buildings")
'''
RELIEF = '''import argparse
from pathlib import Path
ap = argparse.ArgumentParser()
ap.add_argument("--output", required=True)
args = ap.parse_args()
data = Path("romfs/voxel/buildings.bin").read_bytes()
Path(args.output).write_bytes(b"relief:" + data)
'''


class GeneratorDependencyTests(unittest.TestCase):
    def test_relief_reads_completed_buildings_with_both_runners(self):
        for runner in voxel.RUNNERS:
            with self.subTest(runner=runner), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                scripts = root / "payload/scripts"
                scripts.mkdir(parents=True)
                (scripts / "gen_voxel_buildings.py").write_text(BUILDINGS)
                (scripts / "gen_voxel_relief.py").write_text(RELIEF)
                expected = {"voxel/buildings.bin": b"buildings",
                            "voxel/relief.bin": b"relief:buildings"}
                entries = [{"path": name, "size": len(data), "crc": zlib.crc32(data)}
                           for name, data in expected.items()]
                # One worker makes the old relief-first order fail deterministically.
                with patch.object(voxel.os, "cpu_count", return_value=1):
                    actual = voxel.run_generators(root / "tree", root / "payload",
                                                  entries, runner=runner)
                self.assertEqual(actual, expected)
