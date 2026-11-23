#!/usr/bin/env python3
"""Synthetic tests of the strict capture comparator, never using ROM artwork."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "stereo_capture_compare.py"


def run(base, candidate, work, *extra):
    return subprocess.run([sys.executable, str(SCRIPT),
                           "--baseline", str(base),
                           "--candidate", str(candidate),
                           "--report", str(work / "report.json"),
                           "--diff-dir", str(work / "diffs"), *extra],
                          capture_output=True, text=True)


def fixtures(base, candidate):
    for scene in ("single", "double"):
        for eye in ("left", "right"):
            image = Image.new("RGBA", (400, 240), (10, 20, 30, 255))
            image.putpixel((20, 31), (99, 72, 3 if eye == "left" else 6, 255))
            image.putpixel((255, 199), (32, 80 if scene == "single" else 60, 120, 120))
            image.save(base / f"{scene}.{eye}.png")
            image.save(candidate / f"{scene}.{eye}.png")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        base, off = root / "baseline", root / "off"
        base.mkdir()
        off.mkdir()
        assert run(base, off, root).returncode == 2, "empty directories must not pass"
        fixtures(base, off)
        first = run(base, off, root, "--require-scenes", "single,double")
        assert first.returncode == 0, (first.stdout, first.stderr)
        report = json.loads((root / "report.json").read_text())
        assert report["passed"] and report["checked_eyes"] == 4
        assert report["different_pixels"] == 0
        assert len(report["scenes"]) == 2 and len(report["files"]) == 4

        changed = off / "single.left.png"
        with Image.open(changed) as image:
            updated = image.copy()
        updated.putpixel((42, 40), (12, 34, 56, 254))  # alpha-only change also counts
        updated.save(changed)
        second = run(base, off, root)
        assert second.returncode == 1, (second.stdout, second.stderr)
        report = json.loads((root / "report.json").read_text())
        assert not report["passed"] and report["different_pixels"] == 1, report
        assert (root / "diffs" / "single.left.png").exists(), "diff PNG not written"
        assert any(x["difference_bbox"] == [42, 40, 43, 41] for x in report["files"])

        changed.unlink()
        assert run(base, off, root).returncode == 2, "missing one eye must fail"
        fixtures(base, off)
        assert run(base, off, root, "--require-scenes", "single,double,trainer").returncode == 2

        Image.new("RGBA", (401, 240)).save(changed)
        assert run(base, off, root).returncode == 2, "incorrect resolution must fail"
        changed.unlink()
        fixtures(base, off)
        (off / "uncropped.png").write_bytes((off / "single.left.png").read_bytes())
        assert run(base, off, root).returncode == 2, "unpaired or invalid names must fail"
    print("PASS stereo capture comparison: exact eyes, alpha, missing pairs, resolution, coverage")


if __name__ == "__main__":
    main()
