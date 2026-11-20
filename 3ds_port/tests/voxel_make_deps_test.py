"""Check incremental rebuilds and grouped output recovery using the real Make rules."""

import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]
REGIONS = "romfs/voxel/regions.bin"
SIGNS = "romfs/voxel/signposts.bin"
BUILDINGS = "romfs/voxel/buildings.bin"
LIGHTS = "romfs/voxel/windowlights.bin"
GOALS = (REGIONS, SIGNS, BUILDINGS, LIGHTS)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        port = Path(tmp) / "3ds_port"
        port.mkdir()
        (port / "Makefile").write_text(
            f"ROOT := ..\ninclude {PORT / 'full.mk'}\n", encoding="utf-8")
        source = (PORT / "full.mk").read_text(encoding="utf-8")
        for relative in set(re.findall(r"\b(?:scripts|assets|src/voxel)/[\w./-]+\.(?:py|json|toml|h)\b", source)):
            path = port / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        events = port.parent / "data/maps/Fixture/map.json"
        geometry = port.parent / "data/layouts/Fixture/map.bin"
        for path in (events, geometry, port.parent / "data/layouts/layouts.json",
                     port.parent / "src/data/tilesets/metatiles.h",
                     port.parent / "include/constants/metatile_behaviors.h"):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()

        # Tiny replacements isolate Make's dependency graph from asset generation.
        for script, output in (("gen_voxel_regions.py", REGIONS), ("gen_voxel_sign_masks.py", SIGNS)):
            (port / "scripts" / script).write_text(
                f"from pathlib import Path\nPath({output!r}).write_bytes(b'data')\n", encoding="utf-8")
        (port / "scripts/gen_voxel_buildings.py").write_text(
            "from pathlib import Path\nimport sys\n"
            "p = Path(sys.argv[-1])\n"
            "p.write_bytes(b'building')\n"
            "p.with_name('windowlights.bin').write_bytes(b'lights')\n"
            "with Path('runs').open('a') as f: f.write('build\\n')\n", encoding="utf-8")

        def build(*goals):
            subprocess.run(["make", "-j4", f"PYTHON={sys.executable}", *goals], cwd=port, check=True,
                           stdout=subprocess.PIPE, text=True)

        def runs():
            return len((port / "runs").read_text().splitlines())

        build(*GOALS)
        assert runs() == 1
        stamp = (port / REGIONS).stat().st_mtime_ns
        build(*GOALS)
        assert runs() == 1 and (port / REGIONS).stat().st_mtime_ns == stamp
        for number, source_path in enumerate((events, geometry), start=2):
            before = [(port / goal).stat().st_mtime_ns for goal in GOALS]
            time.sleep(0.02)
            source_path.write_bytes(b"edited")
            build(*GOALS)
            assert runs() == number
            assert all((port / goal).stat().st_mtime_ns > old for goal, old in zip(GOALS, before))
        (port / LIGHTS).unlink()
        build(BUILDINGS, LIGHTS)
        assert runs() == 4 and (port / LIGHTS).read_bytes() == b"lights"
        print("PASS voxel Make dependencies: events, layout geometry, and grouped outputs")


if __name__ == "__main__":
    main()
