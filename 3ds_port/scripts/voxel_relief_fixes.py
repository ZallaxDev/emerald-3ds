#!/usr/bin/env python3
"""Hand corrections to the voxel terrain, applied over what the generators read
off the drawing.

gen_voxel_relief.py reads every height off the art; where it reads one wrong,
the correction is written down here instead of in the generator, as numbers:
assets/voxel/relief_fixes.json. Nothing in it comes from the cartridge - it
holds heights in pixels, metatile ids and names - so it ships with the
generators and every build, the builder's included, gets the same terrain.

    {
      "version": 1,
      "pieces": {                       one shape, reused wherever it is enabled
        "face_s_07c": {
          "tileset": "gTileset_General",   the tileset the metatile is drawn from
          "metatile": 124,                  its id (>= 512: the secondary's)
          "grid": [25 x px],                the 5x5 lattice, row major, over the
                                            cell's level (0 = its top terrace)
          "note": "..."
        }
      },
      "layouts": {
        "LAYOUT_ROUTE106": {
          "base": 32,                       the whole map's level, px (optional)
          "drawn": true,                    the relief is all of the map's terrain:
                                            it casts as a drawn mountain does
          "belt": 48,                       px, a multiple of 16: the border stands
                                            this high, laid as the drawing lays it
                                            (VoxelRelief_Belt) - a cave carved out of
                                            the rock, its edges this high all round
          "edges": [{"edge": "W",           past the map's edges, voxel only: this
                     "first": 0, "last": 12,  metatile (the map's own id), or a 2x2
                     "metatile": 368,        block [nw, ne, sw, se] tiled as the
                     "top": null}],          border is, in place of the border block
                                            along those rows (W/E) or columns (N/S),
                                            null: all; its top at "top" px over the
                                            base, null: the map's edge
                                            (VoxelRelief_EdgeFill)
          "pieces":["face_s_07c",          enabled on this map: every cell of
                     {"id": "x", "rect": [x0, y0, x1, y1]}],   that metatile
          "cells": {
            "49,12": {
              "level": 32,                  the cell's level, px over the base
                                            (default: its top, read off the
                                            generated lattice to 16 px)
              "piece": "face_s_07c",        this piece here, enabled or not
              "nopiece": true,              no enabled piece here
              "grid": [25 x px],            the lattice outright, over the base
              "offset": 8,                  added to every point
              "remove": true,               not lifted at all
              "cut": {"wall": 124, "sides": 255, "ground": 1, "foot": 16,
                      "flags": 0, "variant": "keep" | "none"}
            }
          }
        }
      },
      "furniture": {"pc1f": {"ball_w": {"height": 12, "base": 10}}}
    }

In a cell, the order is: the generated lattice, then a piece (the cell's own
"piece", else the first enabled one for its metatile), then "grid", then
"offset". "remove" drops the cell and its cut record.

    python voxel_relief_fixes.py romfs/voxel/relief.bin    applies the fixes in place
"""

import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
ROOT = os.path.dirname(PORT)
FIXES = os.environ.get("VOXEL_RELIEF_FIXES",
                       os.path.join(PORT, "assets", "voxel", "relief_fixes.json"))

MAGIC = b"VXL4"
SIDE = 5
DRAWN = 0x8000
UNIT2 = 0x4000
LEVEL = 0x8000          # of the width: once a level map (no depth); kept, no longer set or read
WIDE = 0x4000           # of the width: the cells' heights are s16 pixels (a mountain past 16 levels)
BELT_SHIFT = 10         # bits 10-13 of the height: the belt's courses
BELT_MAX = 15
EASE_SHIFT = 10         # bits 10-13 of the width: the south seam's ease, 8 px a unit (seam_eases)
EASE_MAX = 15
HEIGHT_MASK = 0x03FF
NO_VARIANT = 0xFFFF
NONE16 = 0xFFFF
NO_FACE = 0xFFFE
# an edge fill (voxel_relief.c VoxelRelief_EdgeFill): u16 layout, u8 edge, u8
# flags (2 top given, bits 4-7 metatile 0-3 is water), s16 first, s16 last,
# s16 top, u16 metatiles[4] (a 2x2 block, as the game's border)
EDGE_FMT = "<HBBhhh4H"
EDGE_BYTES = 18
EDGES = {"W": 1, "E": 2, "N": 4, "S": 8}
EDGE_TOP, EDGE_WATER_SHIFT = 2, 4
# Layouts that are one map drawn twice (the game swaps them as the story
# goes): the second takes the fixes of the first, where it has none of its
# own - Route 111 without its Mirage Tower is Route 111, and its marks (its
# edge fills too: they stood at the edge's level on a save past the tower)
# were lost the day the tower fell. voxel_terraces reads its hand marks so.
TWINS = {"LAYOUT_ROUTE111_NO_MIRAGE_TOWER": "LAYOUT_ROUTE111"}


# --- relief.bin, read and written without loss ---------------------------------
# (a height pack() cannot store as it is - an odd one in a map it has to move
# to 2-pixel units, or one past them - is rounded or clamped, with a warning)

def parse(blob):
    """{"layouts": {index: {w, h, flags, base, belt, level, cells{(x, y): [25 px]}}},
    "variants": [(layout, metatile, rows[16])],
    "cuts": {(layout, x, y): [variant, foot, ground, wall, sides, flags]},
    "edges": [(layout, edge, flags, first, last, top, m0, m1, m2, m3)]}"""
    if blob[:4] != MAGIC:
        raise ValueError("not a relief.bin")
    count, side = struct.unpack_from("<HH", blob, 4)
    if side != SIDE:
        raise ValueError("lattice side %d" % side)
    layouts = {}
    for i in range(count):
        lid, n, w, hh, off, base = struct.unpack_from("<HHHHIh", blob, 8 + 14 * i)
        unit = 2 if hh & UNIT2 else 1
        wide = bool(w & WIDE)
        stride, fmt = (52, "<25h") if wide else (27, "<25b")
        cells = {}
        for k in range(n):
            x, y = blob[off + stride * k], blob[off + stride * k + 1]
            cells[(x, y)] = [v * (1 if wide else unit) for v in struct.unpack_from(fmt, blob, off + stride * k + 2)]
        layouts[lid] = {"w": w & HEIGHT_MASK, "h": hh & HEIGHT_MASK, "flags": hh & (DRAWN | UNIT2),
                        "base": base, "belt": (hh >> BELT_SHIFT) & BELT_MAX,
                        "ease": (w >> EASE_SHIFT) & EASE_MAX,
                        "level": bool(w & LEVEL), "cells": cells}
    variants, cuts = [], {}
    if blob[-4:] == b"CUTS":
        at = struct.unpack_from("<I", blob, len(blob) - 8)[0]
        nv, nc = struct.unpack_from("<HH", blob, at)
        at += 4
        for _ in range(nv):
            lay, m, *rows = struct.unpack_from("<HH16H", blob, at)
            variants.append((lay, m, rows))
            at += 36
        for _ in range(nc):
            lay, x, y, k, foot, ground, wall, sides, flags = struct.unpack_from("<HBBHhHHBB", blob, at)
            cuts[(lay, x, y)] = [k, foot, ground, wall, sides, flags]
            at += 14
    edges = []
    if blob[-4:] == b"CUTS" and blob[at:at + 4] == b"EDGE":
        n = struct.unpack_from("<H", blob, at + 4)[0]
        at += 8
        for _ in range(n):
            edges.append(tuple(struct.unpack_from(EDGE_FMT, blob, at)))
            at += EDGE_BYTES
    return {"layouts": layouts, "variants": variants, "cuts": cuts, "edges": edges}


_CUT_DIR = ((0, -1), (0, 1), (-1, 0), (1, 0), (-1, -1), (1, -1), (-1, 1), (1, 1))


def cut_clear(rows):
    """The texels a cut variant's texture leaves clear, as voxel_relief.c
    VoxelRelief_CutClear has them (16 rows of bits): its background, less
    the halves of background pixels its straight 45-degree sides take in
    (the engine cuts the tile along those sides instead)."""
    rock = [(i, j) for j in range(16) for i in range(16) if not (rows[j] >> i) & 1]
    clear = list(rows)
    if not rock or len(rock) == 256:
        return clear
    c = [max(a * i + b * j for i, j in rock) for a, b in _CUT_DIR]
    for j in range(16):
        for i in range(16):
            inside = all(a * i + b * j <= ck for (a, b), ck in zip(_CUT_DIR, c))
            if inside == bool((rows[j] >> i) & 1):
                return clear        # no polygon: its staircase, as drawn
    if not any(c[k] < a * (c[3] if a > 0 else -c[2]) + b * (c[1] if b > 0 else -c[0])
               for k, (a, b) in enumerate(_CUT_DIR) if k >= 4):
        return clear                # no 45-degree side
    def takes(i, j):
        return all(a * (i if a > 0 else i + 1) + b * (j if b > 0 else j + 1) < ck + (a > 0) + (b > 0)
                   for (a, b), ck in zip(_CUT_DIR, c))
    # a horizontal or vertical side a pixel long, where a 45-degree side ends
    # at the tile's edge, is no side (VoxelRelief_CutClear): the staircase's
    # last step, which left the tile's corner pixel clear
    for k, (a, b) in enumerate(_CUT_DIR[:4]):
        if c[k] >= (15 if a + b > 0 else 0):
            continue
        c[k] += 1
        gained = sum(1 for j in range(16) for i in range(16)
                     if (clear[j] >> i) & 1 and a * i + b * j == c[k] and takes(i, j))
        if gained != 1:
            c[k] -= 1
    for j in range(16):
        for i in range(16):
            if (clear[j] >> i) & 1 and takes(i, j):
                clear[j] &= ~(1 << i)
    return clear


_AROUND = ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (1, -1), (-1, 1), (1, 1))


def cut_fringe(rows, clear):
    """{(x, y): (sx, sy)}: the background pixels cut_clear keeps drawn, each
    with the rock pixel beside it whose colour it takes (as voxel_relief.c
    VoxelRelief_CutFringe: west, east, north, south, then the corners)."""
    out = {}
    for y in range(16):
        for x in range(16):
            if not (rows[y] >> x) & 1 or (clear[y] >> x) & 1:
                continue
            for dx, dy in _AROUND:
                nx, ny = x + dx, y + dy
                if 0 <= nx < 16 and 0 <= ny < 16 and not (rows[ny] >> nx) & 1:
                    out[(x, y)] = (nx, ny)
                    break
    return out


_LINKS = []


def ease_rows(ease, height):
    """The rows a south seam's ease (units of 8 px) is spread over: six a
    level, at most half the map (voxel_relief.c EaseRows)."""
    return min(height // 2, (6 * 8 * ease + 15) // 16)


def seam_eases(relief):
    """{layout index: ease, units of 8 px}. A point stands as far south as it
    is high in the world, its map's base and its relief over it (u, h, v + h;
    voxel_relief.h), so two maps meeting at one height meet in the same row
    and there is no shortfall to ease: this finds none. It once stood as far
    south as it was high over its own map's base, and a map whose south edge
    was ground under its base - Route 111's, two levels under the plateau it
    stands on - met the map south of it rows short of it; the ease spread
    those rows over the map's last rows (EaseRows). The commonest shortfall
    along the seams where the heights meet; none under 8 px."""
    if not _LINKS:
        import gen_voxel_relief
        _LINKS.extend(gen_voxel_relief.map_links())
    tally = {}
    for a, b, direction, offset in _LINKS:
        if direction != "down":
            continue
        ia, ib = layout_index(a), layout_index(b)
        ta = relief["layouts"].get(ia)
        if ta is None:
            continue
        tb = relief["layouts"].get(ib)
        ea, eb = layout_entry(a), layout_entry(b)
        for x in range(max(0, offset), min(ea["width"], offset + eb["width"])):
            ga = ta["cells"].get((x, ea["height"] - 1))
            gb = tb["cells"].get((x - offset, 0)) if tb else None
            ha, hb = (ga[22] if ga else 0), (gb[2] if gb else 0)
            if abs(ta["base"] + ha - ((tb["base"] if tb else 0) + hb)) > 1:
                continue        # a step: the skirt closes it
            # depths in the world: base and relief
            da, db = ta["base"] + ha, (tb["base"] if tb else 0) + hb
            c = tally.setdefault(ia, {})
            c[db - da] = c.get(db - da, 0) + 1
    out = {}
    for ia, c in tally.items():
        gap, n = max(c.items(), key=lambda kv: kv[1])
        if gap >= 8 and 2 * n >= sum(c.values()):
            out[ia] = min(EASE_MAX, gap // 8)
    return out


def pack(relief):
    eases = seam_eases(relief)
    tables = []
    for lid in sorted(relief["layouts"]):
        t = relief["layouts"][lid]
        t["ease"] = eases.get(lid, 0)
        if t["ease"]:
            # every cell of the eased rows carries its depth: the engine's
            # plain ground (no cell) has none
            rows = ease_rows(t["ease"], t["h"])
            for y in range(t["h"] - rows, t["h"]):
                for x in range(t["w"]):
                    t["cells"].setdefault((x, y), [0] * 25)
        if not t["cells"] and not t["base"] and not t.get("belt"):
            continue
        flags = t["flags"]
        values = [v for g in t["cells"].values() for v in g]
        if values and not flags & UNIT2 and (max(values) > 127 or min(values) < -128):
            flags |= UNIT2
        unit = 2 if flags & UNIT2 else 1
        # past a signed byte of 2-pixel units (16 levels over the base): every
        # height in pixels, two bytes each - only the maps that need it
        wide = bool(values) and (max(values) > 254 or min(values) < -256)
        if wide:
            flags &= ~UNIT2
            unit = 1
        tables.append((lid, t, flags, unit, wide))
    head = MAGIC + struct.pack("<HH", len(tables), SIDE)
    offset = len(head) + 14 * len(tables)
    idx, body = bytearray(), bytearray()
    for lid, t, flags, unit, wide in tables:
        cells = sorted(t["cells"].items(), key=lambda kv: (kv[0][1], kv[0][0]))
        idx += struct.pack("<HHHHIh", lid, len(cells),
                           t["w"] | (LEVEL if t.get("level") else 0) | (WIDE if wide else 0)
                           | (t.get("ease", 0) << EASE_SHIFT),
                           t["h"] | flags | (t.get("belt", 0) << BELT_SHIFT),
                           offset + len(body), int(t["base"]))
        lossy = 0
        for (x, y), g in cells:
            body += struct.pack("<BB", x, y)
            if wide:
                stored = [max(-32768, min(32767, int(round(v)))) for v in g]
                lossy += sum(1 for s, v in zip(stored, g) if s != v)
                body += struct.pack("<25h", *stored)
                continue
            stored = [max(-128, min(127, int(round(v / unit)))) for v in g]
            lossy += sum(1 for s, v in zip(stored, g) if s * unit != v)
            body += struct.pack("<25b", *stored)
        if lossy:
            # what the console reads differs from what was asked for: halved
            # to 2-pixel units (a height past 127 px) and rounded, or clamped
            print("relief: WARNING layout %d: %d lattice height(s) not stored as given "
                  "(%d-pixel units, -128..127 of them)" % (lid, lossy, unit), file=sys.stderr)
    blob = bytes(head + idx + body)
    table = struct.pack("<HH", len(relief["variants"]), len(relief["cuts"]))
    for lay, m, rows in relief["variants"]:
        table += struct.pack("<HH16H", lay, m, *rows)
    for (lay, x, y), c in sorted(relief["cuts"].items()):
        table += struct.pack("<HBBHhHHBB", lay, x, y, *c)
    edges = relief.get("edges") or []
    if edges:
        table += b"EDGE" + struct.pack("<HH", len(edges), 0)
        for e in edges:
            table += struct.pack(EDGE_FMT, *e)
    return blob + table + struct.pack("<I", len(blob)) + b"CUTS"


# --- the maps the fixes name ----------------------------------------------------

_LAYOUTS = None


def layouts():
    """layouts.json's entries, in gMapLayouts order (index = position + 1)."""
    global _LAYOUTS
    if _LAYOUTS is None:
        _LAYOUTS = json.load(open(os.path.join(ROOT, "data", "layouts", "layouts.json"),
                                  encoding="utf-8"))["layouts"]
    return _LAYOUTS


def layout_index(name):
    for i, e in enumerate(layouts()):
        if e.get("id") == name:
            return i + 1
    return None


def layout_entry(name):
    return next((e for e in layouts() if e.get("id") == name), None)


def blocks(entry):
    raw = open(os.path.join(ROOT, entry["blockdata_filepath"]), "rb").read()
    return struct.unpack("<%dH" % (len(raw) // 2), raw)


def level_of(grid):
    """A cell's level when the fixes give none: the top of its lattice, to the
    terrace (16 px)."""
    if not grid:
        return 0
    return int(round(max(grid) / 16.0)) * 16


def load_fixes(path=None):
    path = path or FIXES
    if not os.path.exists(path):
        return {"version": 1, "pieces": {}, "layouts": {}, "furniture": {}}
    fixes = json.load(open(path, encoding="utf-8"))
    for k in ("pieces", "layouts", "furniture"):
        fixes.setdefault(k, {})
    return fixes


def _enabled(spec, pieces):
    """[(piece, rect or None)] enabled on a map, known pieces only."""
    out = []
    for p in spec.get("pieces", []):
        pid, rect = (p, None) if isinstance(p, str) else (p.get("id"), p.get("rect"))
        if pid in pieces:
            out.append((pieces[pid], rect))
    return out


def is_water(entry, metatile):
    """A metatile of a layout's tilesets the console lays as water
    (voxel_cells.WATER, voxel_world.c BehaviorShape)."""
    import voxel_cells as vc
    second = metatile >= vc.NUM_PRIMARY
    tileset = entry["secondary_tileset" if second else "primary_tileset"]
    if tileset not in vc.ATTRIBUTES:
        return False
    table = vc.read_u16(vc.ATTRIBUTES[tileset])
    i = metatile - (vc.NUM_PRIMARY if second else 0)
    return i < len(table) and (table[i] & 0xFF) in vc.WATER


def edge_rows(name, spec, index, entry):
    """A map's "edges" (the editor's edge fills): [{"edge": "W"|"E"|"N"|"S",
    "first": cell along the edge or null, "last": ..., "metatile": its own id
    or a 2x2 block of them [nw, ne, sw, se] tiled out as the game's border
    block is, "top": pixels over the map's base where the fill's top stands,
    or null to follow the map's edge}] as relief.bin's rows."""
    rows = []
    for e in spec.get("edges", []):
        side = EDGES.get(str(e.get("edge", "")).upper())
        if side is None or e.get("metatile") is None:
            raise ValueError("%s: edge fill %r needs an edge (W E N S) and a metatile" % (name, e))
        first, last = e.get("first"), e.get("last")
        if first is None:
            first = last = -1
        elif last is None:
            last = first
        ms = e["metatile"] if isinstance(e["metatile"], list) else [e["metatile"]] * 4
        if len(ms) != 4 or any(m is None for m in ms):
            raise ValueError("%s: edge fill %r: one metatile or four" % (name, e))
        ms = [int(m) for m in ms]
        flags = EDGE_TOP if e.get("top") is not None else 0
        for q, m in enumerate(ms):
            if is_water(entry, m):
                flags |= 1 << (EDGE_WATER_SHIFT + q)
        rows.append((index, side, flags, int(min(first, last)), int(max(first, last)),
                     int(e.get("top") or 0), *ms))
    return rows


def apply(relief, fixes, report=None):
    """The fixes over a parsed relief, in place. `report`, a list, gets one
    line per map changed."""
    pieces = fixes.get("pieces", {})
    layouts = dict(fixes.get("layouts", {}))
    for twin, first in TWINS.items():
        if first in layouts:
            layouts[twin] = dict(layouts[first], **layouts.get(twin, {}))
    for name, spec in layouts.items():
        index = layout_index(name)
        entry = layout_entry(name)
        if index is None:
            if report is not None:
                report.append("%s: no such layout" % name)
            continue
        t = relief["layouts"].get(index)
        if t is None:
            t = relief["layouts"][index] = {"w": entry["width"], "h": entry["height"],
                                            "flags": 0, "base": 0, "cells": {}}
        if "base" in spec:
            t["base"] = int(spec["base"])
        if spec.get("drawn"):
            t["flags"] |= DRAWN
        if "edges" in spec:
            relief["edges"] = [e for e in relief.get("edges", []) if e[0] != index]                 + edge_rows(name, spec, index, entry)
        if "belt" in spec:
            courses = int(spec["belt"]) // 16
            if courses * 16 != int(spec["belt"]) or not 0 <= courses <= BELT_MAX:
                raise ValueError("%s: belt %r is not 0-%d levels of 16 px" % (name, spec["belt"], BELT_MAX))
            t["belt"] = courses
        enabled = _enabled(spec, pieces)
        cells_fix = spec.get("cells", {})
        changed = 0
        if enabled or cells_fix:
            mt = blocks(entry)
            w = entry["width"]
            primary, secondary = entry["primary_tileset"], entry["secondary_tileset"]

            def tileset_of(m):
                return primary if m < 512 else secondary

            if enabled:
                for y in range(entry["height"]):
                    for x in range(w):
                        key = "%d,%d" % (x, y)
                        if cells_fix.get(key, {}).get("nopiece") or "piece" in cells_fix.get(key, {}):
                            continue
                        m = mt[y * w + x] & 0x3FF
                        for pc, rect in enabled:
                            if pc.get("metatile") != m or pc.get("tileset", tileset_of(m)) != tileset_of(m):
                                continue
                            if rect and not (rect[0] <= x < rect[2] and rect[1] <= y < rect[3]):
                                continue
                            g = t["cells"].get((x, y))
                            lvl = cells_fix.get(key, {}).get("level", level_of(g))
                            t["cells"][(x, y)] = [lvl + v for v in pc["grid"]]
                            changed += 1
                            break
            for key, fix in cells_fix.items():
                x, y = (int(v) for v in key.split(","))
                if fix.get("remove"):
                    t["cells"].pop((x, y), None)
                    relief["cuts"].pop((index, x, y), None)
                    changed += 1
                    continue
                g = t["cells"].get((x, y))
                lvl = fix.get("level", level_of(g))
                if fix.get("piece") in pieces:
                    g = [lvl + v for v in pieces[fix["piece"]]["grid"]]
                elif "level" in fix and g is None and "grid" not in fix:
                    g = [lvl] * 25
                if "grid" in fix:
                    g = [int(v) for v in fix["grid"]]
                if "offset" in fix:
                    g = [v + int(fix["offset"]) for v in (g or [0] * 25)]
                if g is not None:
                    t["cells"][(x, y)] = g
                if "cut" in fix:
                    c = fix["cut"]
                    rec = relief["cuts"].get((index, x, y), [NO_VARIANT, 0, NONE16, NONE16, 0xFF, 0])
                    if c.get("variant") == "none":
                        rec[0] = NO_VARIANT
                    for i, k in ((1, "foot"), (4, "sides"), (5, "flags")):
                        if k in c:
                            rec[i] = int(c[k])
                    for i, k in ((2, "ground"), (3, "wall")):
                        if k in c:
                            v = c[k]
                            rec[i] = NONE16 if v is None else (NO_FACE if v == "noface" else int(v))
                    if c.get("delete"):
                        relief["cuts"].pop((index, x, y), None)
                    else:
                        relief["cuts"][(index, x, y)] = rec
                        if (x, y) not in t["cells"]:
                            t["cells"][(x, y)] = [lvl] * 25
                changed += 1
        if report is not None and (changed or "base" in spec or "belt" in spec):
            report.append("%s: %d cell(s)%s%s" % (name, changed,
                                                  ", base %+d px" % t["base"] if "base" in spec else "",
                                                  ", belt %d px" % (16 * t["belt"]) if "belt" in spec else ""))
    return relief


def apply_file(path, fixes_path=None, quiet=False):
    fixes = load_fixes(fixes_path)
    if not fixes.get("layouts"):
        return
    report = []
    with open(path, "rb") as f:
        relief = apply(parse(f.read()), fixes, report)
    with open(path, "wb") as f:
        f.write(pack(relief))
    if not quiet:
        for line in report:
            print("relief fix:", line)


def furniture_pieces(room, pieces, fixes_path=None):
    """A room's pieces (voxel_building_specs) with the heights the fixes give."""
    over = load_fixes(fixes_path).get("furniture", {}).get(room, {})
    if not over:
        return pieces
    out = []
    for pc in pieces:
        o = over.get(pc["name"])
        if o:
            pc = dict(pc)
            for k in ("height", "base"):
                if k in o:
                    pc[k] = o[k]
        out.append(pc)
    return out


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    apply_file(sys.argv[1])
