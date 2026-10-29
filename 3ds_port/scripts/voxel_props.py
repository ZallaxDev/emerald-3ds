#!/usr/bin/env python3
"""Objects drawn as one block of upper-layer subtiles, wherever it lands.

The grey rock in the sea is four 8x8 tiles of the General tileset (141, 142
over 157, 158, palette 1) drawn on the upper layer over the water. The
cartridge has a dozen metatiles that carry it - centred on four cells, half
in one cell and half in the next, beside a sandbank, by a shore - but the
rock is always the same four tiles in the same square. So it is found by its
tiles, not by its metatiles: every place in every layout where the square is
drawn, at whatever 8-pixel offset, is one rock.

Both generators read this: gen_voxel_buildings.py stands the model there,
and gen_voxel_relief.py leaves the cells to the model (flat, at the water's
level) instead of reading the rock's colours as a mountain.

    python voxel_props.py LAYOUT_ROUTE105      lists the objects of a layout
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voxel_art  # noqa: E402

ROOT = voxel_art.ROOT

# name -> the tileset the tiles are in, the subtile grid row by row (tile
# ids, None where nothing of the object is drawn) and the palette. A grid
# entry may be optional, and drawn in a palette of its own: (tile, True,
# palette) - drawn in some copies, left out in others (the sea stack's
# corner over a shallow).
OBJECTS = {
    "sea_rock": {
        "tileset": "gTileset_General",
        "palette": 1,
        "grid": ((141, 142),
                 (157, 158)),
    },
    "sand_boulder": {
        # on the land: its cell is ground at the level round it
        "land": True,
        "tileset": "gTileset_General",
        "palette": 3,
        "grid": ((88, 89),
                 (104, 105)),
    },
    "sea_stack": {
        "tileset": "gTileset_General",
        "palette": 3,
        "grid": ((None, 65, 68, (69, True, 0)),
                 (80, 81, 84, 85),
                 (128, 129, 132, 133),
                 (144, 145, 148, 149)),
    },
    # The same boulder and stack in Fallarbor's beige (Routes 114 and 115):
    # the General's tiles drawn in the secondary tileset's palette 11, the
    # boulder on the mountain's top, the stack in the sea and, on the land,
    # as the mounds on Route 115's terraces
    "sand_boulder_fallarbor": {
        "land": True,
        "tileset": "gTileset_General",
        "secondary": "gTileset_Fallarbor",
        "palette": 11,
        "grid": ((88, 89),
                 (104, 105)),
    },
    "sea_stack_fallarbor": {
        "tileset": "gTileset_General",
        "secondary": "gTileset_Fallarbor",
        "palette": 11,
        "grid": ((None, 65, 68, (69, True, 11)),
                 (80, 81, 84, 85),
                 (128, 129, 132, 133),
                 (144, 145, 148, 149)),
    },
    # The Battle Frontier's flags along its walls: the cloth three tiles
    # across and two down (its tiles the ones the game animates, 218-223 of
    # the secondary tileset), the pole two tiles under its west end, each
    # side's tileset its own copy of them
    "frontier_flag_east": {"tileset": "gTileset_General",
                           "secondary": "gTileset_BattleFrontierOutsideEast",
                           "land": True, "palette": 11,
                           "grid": ((730, 731, 732), (733, 734, 735), (746, None, None),
                                    (762, None, None))},
    "frontier_flag_west": {"tileset": "gTileset_General",
                           "secondary": "gTileset_BattleFrontierOutsideWest",
                           "land": True, "palette": 11,
                           "grid": ((730, 731, 732), (733, 734, 735), (746, None, None),
                                    (762, None, None))},
    # Slateport market's goods, each a cell of the secondary tileset's
    # upper layer over whatever paving it stands on
    "basket_green": {"tileset": "gTileset_General", "secondary": "gTileset_Slateport",
                     "land": True, "palette": 7, "grid": ((518, 519), (534, 535))},
    "basket_orange": {"tileset": "gTileset_General", "secondary": "gTileset_Slateport",
                      "land": True, "palette": 7, "grid": ((516, 517), (532, 533))},
    "basket_red": {"tileset": "gTileset_General", "secondary": "gTileset_Slateport",
                   "land": True, "palette": 7, "grid": ((514, 515), (530, 531))},
    "urn": {"tileset": "gTileset_General", "secondary": "gTileset_Slateport",
            "land": True, "palette": 7, "grid": ((614, 615), (630, 631))},
    "fruit": {"tileset": "gTileset_General", "secondary": "gTileset_Slateport",
              "land": True, "palette": 7, "grid": ((736, 737), (738, 739))},
}


def _layouts():
    return [e for e in json.load(open(os.path.join(ROOT, "data", "layouts", "layouts.json"),
                                      encoding="utf-8"))["layouts"]
            if e.get("id") and "blockdata_filepath" in e]


_PAIRS = {}


def _pair(primary, secondary):
    if (primary, secondary) not in _PAIRS:
        _PAIRS[(primary, secondary)] = voxel_art.Pair(primary, secondary)
    return _PAIRS[(primary, secondary)]


def upper_subtiles(pair, blocks, w, h):
    """{(sx, sy): (tile, palette, flips)} of the upper layer, in subtiles."""
    out = {}
    for y in range(h):
        for x in range(w):
            entries = pair.entries(blocks[y * w + x] & 0x3FF)
            if entries is None:
                continue
            for q in range(4):
                e = entries[4 + q]
                if e & 0x3FF:
                    out[(x * 2 + (q & 1), y * 2 + (q >> 1))] = (e & 0x3FF, (e >> 12) & 0xF,
                                                                 (e >> 10) & 3)
    return out


_CONNECTIONS = None


def connections():
    """{layout: [(neighbour layout, dx, dy)]}: where each neighbour's origin
    lies in the layout's cells, from the maps' connections."""
    global _CONNECTIONS
    if _CONNECTIONS is not None:
        return _CONNECTIONS
    maps_dir = os.path.join(ROOT, "data", "maps")
    by_id = {}
    for name in os.listdir(maps_dir):
        path = os.path.join(maps_dir, name, "map.json")
        if os.path.exists(path):
            m = json.load(open(path, encoding="utf-8"))
            by_id[m.get("id")] = m
    sizes = {e["id"]: (e["width"], e["height"]) for e in _layouts()}
    out = {}
    for m in by_id.values():
        a = m.get("layout")
        for c in m.get("connections") or []:
            other = by_id.get(c.get("map"))
            if other is None or a not in sizes or other.get("layout") not in sizes:
                continue
            b, off, d = other["layout"], c.get("offset", 0), c.get("direction")
            (aw, ah), (bw, bh) = sizes[a], sizes[b]
            at = {"down": (off, ah), "up": (off, -bh), "right": (aw, off),
                  "left": (-bw, off)}.get(d)
            if at and b != a:
                out.setdefault(a, []).append((b, at[0], at[1]))
    _CONNECTIONS = out
    return out


_SUBS = {}


def _subtiles_of(entry):
    if entry["id"] not in _SUBS:
        path = os.path.join(ROOT, entry["blockdata_filepath"])
        if not os.path.exists(path):
            _SUBS[entry["id"]] = {}
        else:
            blocks = voxel_art.read_u16(path)
            _SUBS[entry["id"]] = upper_subtiles(
                _pair(entry["primary_tileset"], entry["secondary_tileset"]), blocks,
                entry["width"], entry["height"])
    return _SUBS[entry["id"]]


def beyond(layout_id, x, y):
    """(neighbour layout, x, y) of a cell outside a layout, or None."""
    for (b, dx, dy) in connections().get(layout_id, ()):
        bw, bh = _sizes()[b]
        if 0 <= x - dx < bw and 0 <= y - dy < bh:
            return (b, x - dx, y - dy)
    return None


_SIZE = {}


def _sizes():
    if not _SIZE:
        _SIZE.update({e["id"]: (e["width"], e["height"]) for e in _layouts()})
    return _SIZE


def find(layout_entry, names=None):
    """[(name, sx, sy, present)] in one layout: the object's top-left subtile
    and the grid entries drawn there (a frozenset of (i, j)). An object a
    seam cuts is found from the map that holds its top-left corner; the rest
    of it is looked for in the map across the seam."""
    found = []
    primary, secondary = layout_entry["primary_tileset"], layout_entry["secondary_tileset"]
    w, h = layout_entry["width"], layout_entry["height"]
    path = os.path.join(ROOT, layout_entry["blockdata_filepath"])
    if not os.path.exists(path):
        return found
    entries = None
    subs = None
    for name, obj in OBJECTS.items():
        if names and name not in names:
            continue
        if primary != obj["tileset"] or obj.get("secondary", secondary) != secondary:
            continue
        if subs is None:
            subs = _subtiles_of(layout_entry)
            entries = {e["id"]: e for e in _layouts()}

        def sub_at(sx, sy):
            if 0 <= sx < w * 2 and 0 <= sy < h * 2:
                return subs.get((sx, sy))
            there = beyond(layout_entry["id"], sx // 2, sy // 2)
            if there is None:
                return None
            other = entries[there[0]]
            if other["primary_tileset"] != primary:
                return None
            return _subtiles_of(other).get((there[1] * 2 + sx % 2, there[2] * 2 + sy % 2))
        grid = obj["grid"]
        anchor = next((i, j, t) for j, row in enumerate(grid) for i, t in enumerate(row)
                      if t is not None and not isinstance(t, tuple))
        ai, aj, at = anchor
        for (sx, sy), (tile, pal, flip) in subs.items():
            if tile != at or pal != obj["palette"] or flip:
                continue
            x0, y0 = sx - ai, sy - aj
            present, ok = set(), True
            for j, row in enumerate(grid):
                for i, t in enumerate(row):
                    if t is None:
                        continue
                    want, optional, pal = t if isinstance(t, tuple) else (t, False, obj["palette"])
                    got = sub_at(x0 + i, y0 + j)
                    if got == (want, pal, 0):
                        present.add((i, j))
                    elif not optional:
                        ok = False
                        break
                if not ok:
                    break
            if ok:
                found.append((name, x0, y0, frozenset(present)))
    return sorted(found)


def cells_of(name, sx, sy):
    """The cells an object found at subtile (sx, sy) is drawn over."""
    grid = OBJECTS[name]["grid"]
    gw, gh = len(grid[0]), len(grid)
    return {((sx + i) // 2, (sy + j) // 2) for j in range(gh) for i in range(gw)
            if grid[j][i] is not None}


def cells_in(layout_entry, names=None):
    """{(x, y): set of names} of every cell of a layout an object is drawn
    over: its own objects, and those of its neighbours that a seam cuts."""
    out = {}
    w, h = layout_entry["width"], layout_entry["height"]
    for (name, sx, sy, _) in find(layout_entry, names):
        for (x, y) in cells_of(name, sx, sy):
            if 0 <= x < w and 0 <= y < h:
                out.setdefault((x, y), set()).add(name)
    entries = {e["id"]: e for e in _layouts()}
    for (b, dx, dy) in connections().get(layout_entry["id"], ()):
        for (name, sx, sy, _) in find(entries[b], names):
            for (x, y) in cells_of(name, sx, sy):
                X, Y = x + dx, y + dy
                bw, bh = _sizes()[b]
                if not (0 <= x < bw and 0 <= y < bh) and 0 <= X < w and 0 <= Y < h:
                    out.setdefault((X, Y), set()).add(name)
    return out


def everywhere(names=None):
    """{layout id: [(name, sx, sy, present)]} over every layout."""
    out = {}
    for e in _layouts():
        got = find(e, names)
        if got:
            out[e["id"]] = got
    return out


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    entry = next(e for e in _layouts() if e["id"] == sys.argv[1])
    for name, sx, sy, present in find(entry):
        print("%-10s subtile %3d,%3d  cells %s  drawn %d/%d" % (
            name, sx, sy, sorted(cells_of(name, sx, sy)), len(present),
            sum(t is not None for row in OBJECTS[name]["grid"] for t in row)))


if __name__ == "__main__":
    main()
