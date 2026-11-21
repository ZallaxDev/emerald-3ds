#!/usr/bin/env python3
"""Regression for Python 3.15 sampling profiler's actual stdout heading."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from profile_voxel import sampling_hotspots  # noqa: E402


class SamplingHotspotsTest(unittest.TestCase):
    def test_actual_python_315_stdout(self):
        sample = ("voxel buildings: complete\n"
                  "Captured 269,119 samples in 269.12 seconds\n"
                  "Sample rate: 1,000.00 samples/sec\n"
                  "Error rate: 10.81\n"
                  "Profile Stats:\n"
                  "       nsamples   sample%   tottime (s)    cumul%   cumtime (s)  filename:lineno(function)\n"
                  "      72/240014       0.0         0.072     100.0       240.014  gen_voxel_buildings.py:main\n")
        self.assertEqual(sampling_hotspots(sample), sample[sample.index("Profile Stats:"):])

    def test_optional_heading(self):
        sample = ("output\nProfile Stats (Mode: wall):\n"
                  " nsamples  sample%  tottime\nentry\n")
        self.assertEqual(sampling_hotspots(sample), sample[sample.index("Profile Stats"):])

    def test_no_stats_rejected(self):
        self.assertIsNone(sampling_hotspots("build succeeded\n"))
        self.assertIsNone(sampling_hotspots("Profile Stats:\nno samples found\n"))


if __name__ == "__main__":
    unittest.main()
