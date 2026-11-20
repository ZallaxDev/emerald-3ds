#!/usr/bin/env python3
"""Synthetic floor-mask microbenchmark, not a full-build timing estimate.

Run with Python 3.15.2 and Pillow 13.0.0; repeat with PYTHON_JIT=0 and
PYTHON_JIT=1 to compare interpreter modes. Both implementations run in
alternating order in each process, including their lookup preparation.
"""

import argparse
import json
from pathlib import Path
import random
import statistics
import sys
import time

from PIL import Image, __version__ as pillow_version

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "3ds_port/scripts"))
from voxel_floor import match_floor_pixels


def real_rooms(tree):
    """Prepare the same room images as interior_specs, outside timed work."""
    sys.path.insert(0, str(tree.resolve() / "3ds_port/scripts"))
    import gen_voxel_buildings as generator
    for spec in generator.specs.SPECS:
        if "interior" not in spec:
            continue
        room = spec["interior"]
        layout = generator.drawn_as(room["layout"], room.get("draw"))
        art = Image.new("RGBA", (layout.w * 16, layout.h * 16))
        for y in range(layout.h):
            for x in range(layout.w):
                art.paste(layout.cell_image(layout.metatile(x, y)), (x * 16, y * 16))
        yield art, [layout.cell_image(tile) for tile in room["ground"]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pairs", type=int, default=6)
    parser.add_argument("--tree", type=Path,
                        help="benchmark all authored rooms in a prepared tree instead of synthetic art")
    args = parser.parse_args()
    if args.pairs < 1:
        parser.error("--pairs must be positive")
    rng = random.Random(247)
    floors = [Image.new("RGBA", (16, 16), colour) for colour in
              [(0, 0, 0, 0), (24, 41, 82, 255), (98, 98, 123, 255), (255, 255, 255, 255)]]
    art = Image.new("RGBA", (640, 480))
    art.putdata([rng.choice([(24, 41, 82, 255), (65, 74, 106, 255),
                            (255, 255, 255, 255)]) for _ in range(640 * 480)])

    times = {"baseline": [0.0] * args.pairs, "candidate": [0.0] * args.pairs}
    rooms, pixels_tested = 0, 0
    workloads = real_rooms(args.tree) if args.tree else [(art, floors)]
    for art, floors in workloads:
        width, height = art.size

        def baseline():
            pixels, tiles = art.load(), [floor.load() for floor in floors]
            return [[any(tile[x % 16, y % 16] == pixels[x, y] for tile in tiles)
                     for x in range(width)] for y in range(height)]

        def candidate():
            return match_floor_pixels(art, floors)

        for pair in range(args.pairs):
            results = {}
            order = [("baseline", baseline), ("candidate", candidate)]
            for name, function in order if (pair + rooms) % 2 == 0 else reversed(order):
                start = time.perf_counter()
                results[name] = function()
                times[name][pair] += time.perf_counter() - start
            if results["baseline"] != results["candidate"]:
                raise AssertionError("floor masks differ in room %d" % rooms)
        rooms += 1
        pixels_tested += width * height
    print(json.dumps({"python": sys.version, "pillow": pillow_version,
                      "jit": sys._jit.is_enabled() if hasattr(sys, "_jit") else None,
                      "workload": "authored room masks" if args.tree else "synthetic 640x480 RGBA room, four floor tiles",
                      "rooms": rooms, "pixels": pixels_tested, "all_masks_equal": True,
                      "seconds": times,
                      "median_seconds": {key: statistics.median(value)
                                         for key, value in times.items()}}, indent=2))


if __name__ == "__main__":
    main()
