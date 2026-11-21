"""Rebuild, from the player's ROM, the files the voxel generators read.

The generators were written against the decomp tree's layout (tileset art in
data/tilesets, blockdata in data/layouts, map.json per map, two C headers that
name every tileset's files). This module writes exactly that layout into a
temporary directory:

* binary inputs come from the recipe's `inputs` entries (ROM copies and LZ77
  streams, each checked against its CRC);
* layouts.json and every map.json are read back from the ROM's own MapLayout
  and MapHeader structures, whose offsets the recipe names;
* metatiles.h, headers.h and metatile_behaviors.h are reduced to the lines
  the generators parse.
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

from .errors import BuilderError
from .recipe import Recipe, RecipeError, build_entry, valid_path

ROM_BASE = 0x08000000
# BgEvent kinds 0-4 are signs (read from any side or from one); the others are
# hidden items and secret bases.
BG_EVENT_SIGN_LAST = 4


def _ptr(rom: bytes, off: int) -> int | None:
    value = struct.unpack_from("<I", rom, off)[0]
    if value == 0:
        return None
    if not ROM_BASE <= value < ROM_BASE + len(rom):
        raise BuilderError("The ROM does not have the expected structure (bad pointer at %#x)." % off)
    return value - ROM_BASE


def read_layout(rom: bytes, off: int, tilesets: dict[int, str]) -> dict:
    width, height = struct.unpack_from("<ii", rom, off)
    primary = _ptr(rom, off + 16)
    secondary = _ptr(rom, off + 20)
    return {"width": width, "height": height,
            "primary_tileset": tilesets.get(primary, "NULL"),
            "secondary_tileset": tilesets.get(secondary, "NULL")}


def read_map(rom: bytes, off: int, meta: dict) -> dict:
    events = _ptr(rom, off + 4)
    connections = _ptr(rom, off + 12)
    layout_id = struct.unpack_from("<H", rom, off + 0x12)[0]
    map_type = rom[off + 0x17]
    layouts = meta["layouts"]
    maps_by_group = {(m["group"], m["num"]): m["id"] for m in meta["maps"]}
    out = {
        "layout": layouts[layout_id - 1]["id"] if 0 < layout_id <= len(layouts) else "LAYOUT_NONE",
        "map_type": meta["map_types"].get(str(map_type), "MAP_TYPE_NONE"),
        "warp_events": [],
        "bg_events": [],
        "connections": [],
    }
    if events is not None:
        warp_count = rom[events + 1]
        warps = _ptr(rom, events + 8)
        for i in range(warp_count):
            x, y, elevation, warp_id, num, group = struct.unpack_from("<hhBBBB", rom, warps + i * 8)
            out["warp_events"].append({"x": x, "y": y, "elevation": elevation,
                                       "dest_map": maps_by_group.get((group, num), "MAP_NONE"),
                                       "dest_warp_id": warp_id})
        bg_count = rom[events + 3]
        bgs = _ptr(rom, events + 16)
        for i in range(bg_count):
            x, y, elevation, kind = struct.unpack_from("<HHBB", rom, bgs + i * 12)
            if kind <= BG_EVENT_SIGN_LAST:
                out["bg_events"].append({"type": "sign", "x": x, "y": y, "elevation": elevation})
    if connections is not None:
        count = struct.unpack_from("<i", rom, connections)[0]
        table = _ptr(rom, connections + 4)
        for i in range(count):
            direction, offset, group, num = struct.unpack_from("<B3xiBB2x", rom, table + i * 12)
            out["connections"].append({
                "map": maps_by_group.get((group, num), "MAP_NONE"),
                "offset": offset,
                "direction": meta["connection_directions"].get(str(direction), str(direction)),
            })
    return out


def build_tree(rom: bytes, recipe: Recipe, root: Path, progress=None) -> None:
    try:
        _build_tree(rom, recipe, root, progress)
    except (RecipeError, KeyError, TypeError, ValueError, IndexError, struct.error) as exc:
        raise BuilderError("The release's voxel input recipe is invalid.", str(exc),
                           code="recipe_unreadable") from exc


def _build_tree(rom: bytes, recipe: Recipe, root: Path, progress) -> None:
    meta = recipe.vtree
    if not meta or not recipe.inputs:
        raise BuilderError("This release's recipe does not describe the voxel inputs.")
    total = len(recipe.inputs)
    made = set()
    for i, entry in enumerate(recipe.inputs):
        data = build_entry(entry, rom, recipe.literals, recipe.bitmaps)
        dst = root / entry["path"]
        if dst.parent not in made:
            dst.parent.mkdir(parents=True, exist_ok=True)
            made.add(dst.parent)
        dst.write_bytes(data)
        if progress and i % 50 == 0:
            progress(i / total)

    tilesets = {off: name for name, off in meta["tilesets"].items()}
    layouts = []
    for item in meta["layouts"]:
        if "name" not in item:
            layouts.append({"id": item["id"]})
            continue
        entry = {"id": item["id"], "name": item["name"]}
        entry.update(read_layout(rom, item["rom"], tilesets))
        for key in ("border_filepath", "blockdata_filepath"):
            if not valid_path(item[key]):
                raise RecipeError("invalid %s in voxel layout" % key)
            entry[key] = item[key]
        layouts.append(entry)
    path = root / "data" / "layouts" / "layouts.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"layouts_table_label": meta["layouts_table_label"], "layouts": layouts},
                               indent=2), encoding="utf-8")

    for item in meta["maps"]:
        folder = item["folder"]
        if not valid_path(folder) or "/" in folder:
            raise RecipeError("invalid voxel map folder")
        record = {"id": item["id"], "name": item["name"]}
        record.update(read_map(rom, item["rom"], meta))
        path = root / "data" / "maps" / folder / "map.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(record, indent=2), encoding="utf-8")

    if any(not valid_path(path) for _, path in meta["metatiles_h"]):
        raise RecipeError("invalid tile path in voxel input recipe")
    inc = root / "src" / "data" / "tilesets"
    inc.mkdir(parents=True, exist_ok=True)
    (inc / "metatiles.h").write_text(
        "".join('const u16 %s[] = INCBIN_U16("%s");\n' % (n, p) for n, p in meta["metatiles_h"]),
        encoding="utf-8")
    (inc / "headers.h").write_text(
        "".join("const struct Tileset %s =\n{\n    .metatiles = %s,\n};\n\n" % (n, m)
                for n, m in meta["headers_h"]),
        encoding="utf-8")
    behaviors = meta.get("metatile_behaviors")
    if not behaviors:
        raise BuilderError("This release's recipe does not name the metatile behaviours.")
    const = root / "include" / "constants"
    const.mkdir(parents=True, exist_ok=True)
    (const / "metatile_behaviors.h").write_text(
        "enum {\n" + "".join("    %s = %d,\n" % (n, v) for n, v in behaviors) + "};\n",
        encoding="utf-8")
    if progress:
        progress(1.0)
