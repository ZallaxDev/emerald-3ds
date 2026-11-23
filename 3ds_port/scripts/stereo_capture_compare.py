#!/usr/bin/env python3
"""Strict pixel comparison of already-cropped, synchronized 3DS eye captures.

Compare base integration against a feature-disabled build, not feature-ON
parallax against a flat image. Images must be actual native 400x240 top-eye
PNGs; no resizing, tolerance, interpolation or ignored regions are allowed.
Never treats successful comparison as evidence of hardware performance.
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

from PIL import Image

SIZE = (400, 240)
NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]*\.(left|right)\.png$")


class CaptureError(ValueError):
    """An invalid or incomplete paired capture set."""


def discover(folder):
    folder = Path(folder)
    if not folder.is_dir():
        raise CaptureError(f"not a directory: {folder}")
    paths = sorted(folder.glob("*.png"))
    if not paths:
        raise CaptureError(f"no PNG captures in {folder}")
    found = {}
    for path in paths:
        match = NAME.fullmatch(path.name)
        if match is None:
            raise CaptureError(f"bad capture filename: {path.name} (expected scene.left.png / scene.right.png)")
        scene = path.name.rsplit(".", 2)[0]
        found.setdefault(scene, set()).add(match.group(1))
    for scene, eyes in sorted(found.items()):
        if eyes != {"left", "right"}:
            raise CaptureError(f"scene {scene!r} needs both left and right eyes, found {sorted(eyes)}")
    return {p.name: p for p in paths}


def compare_pair(reference_path, candidate_path, diff_dir=None):
    with Image.open(reference_path) as original, Image.open(candidate_path) as updated:
        if original.size != SIZE or updated.size != SIZE:
            raise CaptureError(
                f"expected native 400x240 images: {reference_path.name}={original.size}, "
                f"{candidate_path.name}={updated.size}"
            )
        # Decode to a canonical byte representation, including transparency.
        a, b = original.convert("RGBA").tobytes(), updated.convert("RGBA").tobytes()

    changed = 0
    left, top, right, bottom = SIZE[0], SIZE[1], -1, -1
    mask = bytearray(len(a)) if diff_dir is not None else None
    for i in range(0, len(a), 4):
        if a[i:i + 4] != b[i:i + 4]:
            changed += 1
            x, y = (i // 4) % SIZE[0], (i // 4) // SIZE[0]
            left, top, right, bottom = min(left, x), min(top, y), max(right, x), max(bottom, y)
            if mask is not None:
                mask[i:i + 4] = bytes((255, 64, 64, 255))
    if changed and mask is not None:
        diff_dir.mkdir(parents=True, exist_ok=True)
        Image.frombytes("RGBA", SIZE, bytes(mask)).save(diff_dir / reference_path.name)
    return {
        "file": reference_path.name,
        "identical": changed == 0,
        "different_pixels": changed,
        "difference_bbox": [left, top, right + 1, bottom + 1] if changed else None,
        "reference_rgba_sha256": hashlib.sha256(a).hexdigest(),
        "candidate_rgba_sha256": hashlib.sha256(b).hexdigest(),
    }


def evaluate(baseline, candidate, required=(), diff_dir=None):
    reference = discover(baseline)
    updated = discover(candidate)
    if set(reference) != set(updated):
        missing = sorted(set(reference) - set(updated))
        extra = sorted(set(updated) - set(reference))
        raise CaptureError(f"capture sets differ: missing={missing}, extra={extra}")
    scenes = sorted({name.rsplit(".", 2)[0] for name in reference})
    absent = sorted(set(required) - set(scenes))
    if absent:
        raise CaptureError(f"required scenes not captured: {absent}")
    results = [compare_pair(reference[name], updated[name], diff_dir)
               for name in sorted(reference)]
    return {
        "test": "exact RGBA pixel parity of synchronized 400x240 eye captures",
        "baseline": str(Path(baseline)),
        "candidate": str(Path(candidate)),
        "scenes": scenes,
        "checked_eyes": len(results),
        "different_pixels": sum(r["different_pixels"] for r in results),
        "passed": all(r["identical"] for r in results),
        "files": results,
        "limitation": "This does not prove feature-ON appearance, eye depth, frame pacing, or physical 3DS compatibility.",
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path,
                        help="directory of original integration left/right eye PNG captures")
    parser.add_argument("--candidate", required=True, type=Path,
                        help="directory of otherwise identical feature-OFF captures")
    parser.add_argument("--report", required=True, type=Path,
                        help="JSON result with SHA-256 and exact mismatch counts")
    parser.add_argument("--diff-dir", type=Path, help="red pixel-difference masks on failure")
    parser.add_argument("--require-scenes", default="",
                        help="comma-separated required scene names (e.g. single,double,trainer)")
    args = parser.parse_args(argv)
    required = tuple(s.strip() for s in args.require_scenes.split(",") if s.strip())
    if any(not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*", s) for s in required):
        parser.error("--require-scenes accepts only capture scene names")
    try:
        result = evaluate(args.baseline, args.candidate, required, args.diff_dir)
    except (CaptureError, OSError, ValueError) as exc:
        print(f"INVALID CAPTURE SET: {exc}", file=sys.stderr)
        return 2
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    status = "PASS" if result["passed"] else "DIFFERENT"
    print(f"{status}: {result['checked_eyes']} eyes, {len(result['scenes'])} scenes, "
          f"{result['different_pixels']} differing RGBA pixels ({args.report})")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
