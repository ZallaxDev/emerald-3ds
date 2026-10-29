"""The inputs of the voxel generators, described for the builder.

The voxel generators (3ds_port/scripts/gen_voxel_*.py) read a handful of files
from the decomp tree: tileset art, palettes, metatiles, metatile attributes,
layout blockdata, layouts.json, the maps' map.json and two headers. The
builder rebuilds exactly those files from the player's ROM in a temporary
tree and runs the same generators there.

Binary inputs become recipe entries (ROM copies, LZ77 streams); the JSON and
header files become metadata: names, paths and the ROM offsets of the
structures (MapLayout, MapHeader, Tileset) whose fields the builder reads
back from the ROM. No field value of those structures is stored here.
"""

from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path

CONNECTION_DIRECTIONS = {1: "down", 2: "up", 3: "left", 4: "right", 5: "dive", 6: "emerge"}

# Tileset animation frames a model's texels follow (voxel_building_specs.py
# FOUNTAIN_ANIM: the jet of Rustboro's fountain; MAUVILLE_GYM_GATES: the
# gym's electric barriers), raw 4bpp in the ROM.
ANIM_FRAMES = ["data/tilesets/secondary/rustboro/anim/fountain/0.4bpp",
               "data/tilesets/secondary/rustboro/anim/fountain/1.4bpp",
               "data/tilesets/secondary/mauville_gym/anim/electric_gates/0.4bpp",
               "data/tilesets/secondary/mauville_gym/anim/electric_gates/1.4bpp"]


def gba_symbols(nm_tool: str, elf: Path) -> dict[str, int]:
    out = subprocess.check_output([nm_tool, "--defined-only", str(elf)], text=True, errors="ignore")
    result: dict[str, int] = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] not in result:
            addr = int(parts[0], 16)
            if 0x08000000 <= addr < 0x0A000000:
                result[parts[2]] = addr - 0x08000000
    return result


def binary_inputs(root: Path) -> list[str]:
    files: list[str] = []
    layouts = json.loads((root / "data/layouts/layouts.json").read_text(encoding="utf-8"))["layouts"]
    for entry in layouts:
        for key in ("blockdata_filepath", "border_filepath"):
            if key in entry:
                files.append(entry[key])
    text = (root / "src/data/tilesets/metatiles.h").read_text(encoding="utf-8")
    files += re.findall(r'INCBIN_U16\("([^"]+)"\)', text)
    for kind in ("primary", "secondary"):
        for folder in sorted((root / "data/tilesets" / kind).iterdir()):
            if not folder.is_dir():
                continue
            rel = folder.relative_to(root).as_posix()
            # Only what exists: the generators test for these files, so the
            # builder's tree must hold exactly the same set as the build's.
            if (folder / "tiles.4bpp").exists():
                files.append(rel + "/tiles.4bpp")
            for i in range(16):
                if (folder / "palettes" / ("%02d.gbapal" % i)).exists():
                    files.append(rel + "/palettes/%02d.gbapal" % i)
    files += [f for f in ANIM_FRAMES if (root / f).exists()]
    # The intro's leaves scene, for scripts/gen_intro_margins.py.
    scene = root / "graphics/intro/scene_1"
    files += [p.relative_to(root).as_posix() for p in
              [scene / "bg.4bpp"] + [scene / ("bg%d_map.bin" % i) for i in range(4)] if p.exists()]
    seen, ordered = set(), []
    for f in files:
        if f not in seen:
            seen.add(f)
            ordered.append(f)
    return ordered


def metadata(root: Path, syms: dict[str, int]) -> dict:
    meta: dict = {}
    text = (root / "src/data/tilesets/metatiles.h").read_text(encoding="utf-8")
    meta["metatiles_h"] = re.findall(r'const u16 (gMetatile\w+)\[\]\s*=\s*INCBIN_U16\("([^"]+)"\)', text)
    headers = (root / "src/data/tilesets/headers.h").read_text(encoding="utf-8")
    meta["headers_h"] = []
    for name, body in re.findall(r"const struct Tileset (gTileset_\w+)\s*=\s*\{(.*?)\};", headers, re.S):
        m = re.search(r"\.metatiles\s*=\s*(gMetatiles_\w+)", body)
        meta["headers_h"].append([name, m.group(1) if m else ""])
    meta["tilesets"] = {name: syms[name] for name, _ in meta["headers_h"] if name in syms}
    layouts = json.loads((root / "data/layouts/layouts.json").read_text(encoding="utf-8"))
    meta["layouts_table_label"] = layouts.get("layouts_table_label", "gMapLayouts")
    meta["layouts"] = []
    for entry in layouts["layouts"]:
        item = {"id": entry["id"]}
        if "name" in entry:
            item.update(name=entry["name"], blockdata_filepath=entry["blockdata_filepath"],
                        border_filepath=entry["border_filepath"], rom=syms.get(entry["name"]))
        meta["layouts"].append(item)
    groups = json.loads((root / "data/maps/map_groups.json").read_text(encoding="utf-8"))
    meta["maps"] = []
    for g, group in enumerate(groups["group_order"]):
        for n, folder in enumerate(groups[group]):
            mj = json.loads((root / "data/maps" / folder / "map.json").read_text(encoding="utf-8"))
            meta["maps"].append({"folder": folder, "id": mj["id"], "name": mj["name"],
                                 "group": g, "num": n, "rom": syms.get(mj["name"])})
    types = (root / "include/constants/map_types.h").read_text(encoding="utf-8")
    meta["map_types"] = {int(v): k for k, v in re.findall(r"#define\s+(MAP_TYPE_\w+)\s+(\d+)", types)}
    meta["connection_directions"] = CONNECTION_DIRECTIONS
    meta["metatile_behaviors"] = metatile_behaviors(root)
    return meta


def metatile_behaviors(root: Path) -> list[list]:
    """[name, value] for every entry of the metatile behaviour enum."""
    text = (root / "include/constants/metatile_behaviors.h").read_text(encoding="utf-8")
    body = text[text.index("{") + 1:text.index("}")]
    out, value = [], 0
    for item in body.split(","):
        item = re.sub(r"//.*", "", item).strip()
        if not item:
            continue
        m = re.match(r"(\w+)\s*(?:=\s*(\w+))?", item)
        if m.group(2):
            value = int(m.group(2), 0)
        out.append([m.group(1), value])
        value += 1
    return out
