#!/usr/bin/env python3
"""What every cell of a layout is, for the voxel generators.

The cartridge answers most of it itself: the behaviour byte says water,
ledges and doors; the blockdata says what is blocked and at which elevation;
the maps' own events say where the doors (warps) and the signs are, and the
map type says whether it is outdoors. The drawing is only asked what the
data does not state: whether a blocked cell is drawn as foliage, and whether a
walkable one is drawn as steps.

Roles, in the order they are decided:

    water     a water behaviour
    ledge     a jump behaviour
    stair     walkable and drawn as steps (Layout.treads)
    floor     any other walkable cell
    signpost  an outdoor post: blocked, open on three sides (a sign, a lamp)
    wall      part of a building: the blocked mass above a house door
    tree      blocked and drawn mostly in foliage
    fence     blocked, one cell thin, walkable on both sides
    cliff     blocked terrain touching walkable ground
    shelf     blocked terrain away from walkable ground
    prop      nothing of the above

    python voxel_cells.py LAYOUT_ROUTE104      prints one layout's roles
"""

import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voxel_art  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
NUM_PRIMARY = 512
OUTDOOR_MAP_TYPES = {"MAP_TYPE_ROUTE", "MAP_TYPE_TOWN", "MAP_TYPE_UNDERWATER",
                     "MAP_TYPE_CITY", "MAP_TYPE_OCEAN_ROUTE"}
NEIGHBOURS = [(0, -1), (0, 1), (-1, 0), (1, 0)]
# A house is at most this many cells either side of its door and this tall.
HOUSE_HALF_WIDTH = 5
HOUSE_HEIGHT = 7
FOLIAGE = 0.5
# Secondary-tileset metatiles that stand alone with no sign event and are
# never a post (free_post): drawn flat with their curb or modelled
# (gen_voxel_buildings). As signs they stood up as boards: Slateport's curb
# ends (0x2b9, 0x2c7) by the beach steps and the Contest Hall, its harbour
# bollard (0x32d), the market's goods (0x20c, 0x26b, 0x26c), a moored
# sailboat's cell (0x338).
NOT_POSTS = {
    "gTileset_Slateport": {0x2b9, 0x2c7, 0x32d, 0x20c, 0x26b, 0x26c, 0x338},
}
# The General tileset's mountain rock (voxel_terraces.MOUNTAIN's own range):
# a cliff's corner standing free, or a rock in the sea, is blocked and open
# on three sides like a post, but it is the mountain - never a post. As one
# it stood up as a board drawn with the rock and the sand round it (Route
# 115's beach: the head of the hill's flank at (14, 61)), and Mossdeep's,
# Route 124's and Route 127's rocks, Lilycove's and the Safari Zone's
# corners, with it.
MOUNTAIN_ROCK = range(0x064, 0x0c0)
# Steps: mean luminance change between neighbouring pixels along a row (at
# most) and down a column (at least). The General tileset's flight measures
# 2 and 28; ground, planks and roofs change along their rows too.
TREAD_ACROSS = 4.0
TREAD_DOWN = 15.0
# Rows a covering drawing may leave short: its own outline, top and bottom.
COVER_SLACK = 2


def behaviours():
    """Metatile behaviour values by name, counted off the enum in
    include/constants/metatile_behaviors.h."""
    text = open(os.path.join(ROOT, "include", "constants", "metatile_behaviors.h"),
                encoding="utf-8").read()
    body = text[text.index("{") + 1:text.index("}")]
    values, value = {}, 0
    for item in body.split(","):
        item = re.sub(r"//.*", "", item).strip()
        if not item:
            continue
        m = re.match(r"(\w+)\s*(?:=\s*(\w+))?", item)
        if m.group(2):
            value = int(m.group(2), 0)
        values[m.group(1)] = value
        value += 1
    return values


MB = behaviours()
# The cells the console draws as water (voxel_world.c BehaviorShape): the
# game's surfable behaviours, puddles, shallow water, hot springs and the water
# drawn under a bridge. Not the bridges themselves, deep sand or the warps that
# merely have WATER or DEEP in their names: read as water, a pier sank into
# the sea and every shore became a step.
WATER = {MB[k] for k in (
    "MB_POND_WATER", "MB_INTERIOR_DEEP_WATER", "MB_DEEP_WATER", "MB_WATERFALL",
    "MB_SOOTOPOLIS_DEEP_WATER", "MB_OCEAN_WATER", "MB_PUDDLE", "MB_SHALLOW_WATER",
    "MB_NO_SURFACING", "MB_SEAWEED", "MB_SEAWEED_NO_SURFACING", "MB_HOT_SPRINGS",
    "MB_REFLECTION_UNDER_BRIDGE", "MB_EASTWARD_CURRENT", "MB_WESTWARD_CURRENT",
    "MB_NORTHWARD_CURRENT", "MB_SOUTHWARD_CURRENT") if k in MB}
JUMPS = {v for k, v in MB.items() if k.startswith("MB_JUMP_")}
HOUSE_DOORS = {v for k, v in MB.items() if k in ("MB_ANIMATED_DOOR", "MB_PETALBURG_GYM_DOOR",
                                                   "MB_CLOSED_SOOTOPOLIS_DOOR")}


def _incbin(pattern):
    text = open(os.path.join(ROOT, "src", "data", "tilesets", "metatiles.h"),
                encoding="utf-8").read()
    return {"gTileset_" + n: os.path.join(ROOT, p) for n, p in re.findall(pattern, text)}


ATTRIBUTES = _incbin(r"gMetatileAttributes_(\w+)\[\]\s*=\s*INCBIN_U16\(\"([^\"]+)\"\)")


def read_u16(path):
    raw = open(path, "rb").read()
    return list(struct.unpack("<%dH" % (len(raw) // 2), raw[:len(raw) // 2 * 2]))


class MapEvents:
    """Per layout: outdoors or not, warp cells and sign cells, from every map
    that uses the layout."""

    def __init__(self):
        self.outdoor, self.warps, self.signs = set(), {}, {}
        maps_dir = os.path.join(ROOT, "data", "maps")
        for name in sorted(os.listdir(maps_dir)):
            path = os.path.join(maps_dir, name, "map.json")
            if not os.path.exists(path):
                continue
            with open(path, encoding="utf-8") as f:
                m = json.load(f)
            layout = m.get("layout")
            if not layout:
                continue
            if m.get("map_type") in OUTDOOR_MAP_TYPES:
                self.outdoor.add(layout)
            for w in m.get("warp_events") or []:
                self.warps.setdefault(layout, set()).add((w["x"], w["y"]))
            for b in m.get("bg_events") or []:
                if b.get("type") == "sign":
                    self.signs.setdefault(layout, set()).add((b["x"], b["y"]))
        # A layout a script swaps in (Route 111 once the tower has fallen,
        # Route 131 with the Sky Pillar, Sootopolis in the legends' battle)
        # belongs to no map: it has its map's events, the map it is named
        # after and drawn the size of.
        for alt, base in alternate_layouts().items():
            if base in self.outdoor:
                self.outdoor.add(alt)
            if base in self.warps:
                self.warps[alt] = set(self.warps[base])
            if base in self.signs:
                self.signs[alt] = set(self.signs[base])


_PAIRS = {}
_ART_MEMO = {}    # (primary, secondary) -> Layout's foliage, treads, covers


def pair_for(primary, secondary):
    key = (primary, secondary)
    if key not in _PAIRS:
        _PAIRS[key] = voxel_art.Pair(primary, secondary)
    return _PAIRS[key]


class Layout:
    """One layout and the questions the voxel generators ask of it."""

    def __init__(self, entry, events):
        self.layout_id = entry["id"]
        self.w, self.h = entry["width"], entry["height"]
        self.blocks = read_u16(os.path.join(ROOT, entry["blockdata_filepath"]))
        self.primary = entry["primary_tileset"]
        self.secondary = entry["secondary_tileset"]
        self.attr = (read_u16(ATTRIBUTES[self.primary]) if self.primary in ATTRIBUTES else [],
                     read_u16(ATTRIBUTES[self.secondary]) if self.secondary in ATTRIBUTES else [])
        self.outdoor = self.layout_id in events.outdoor
        self.warps = sorted(events.warps.get(self.layout_id, ()))
        self.signs = events.signs.get(self.layout_id, set())
        self.events = events
        self._memo = {}
        self._houses = None
        self._lamps = None
        # what a metatile's drawing says is the same in every layout over the
        # same tilesets: shared by them
        art = _ART_MEMO.setdefault((self.primary, self.secondary), ({}, {}, {}))
        self._foliage, self._treads, self._covers = art

    # ── what the cartridge states ─────────────────────────────────────────

    def off_map(self, x, y):
        return x < 0 or y < 0 or x >= self.w or y >= self.h

    def metatile(self, x, y):
        return None if self.off_map(x, y) else self.blocks[y * self.w + x] & 0x3FF

    def blocked(self, x, y):
        return False if self.off_map(x, y) else ((self.blocks[y * self.w + x] >> 10) & 3) != 0

    def elevation(self, x, y):
        return 0 if self.off_map(x, y) else (self.blocks[y * self.w + x] >> 12) & 0xF

    def attributes(self, metatile):
        if metatile is None:
            return 0
        which = 0 if metatile < NUM_PRIMARY else 1
        table = self.attr[which]
        index = metatile - which * NUM_PRIMARY
        return table[index] if index < len(table) else 0

    def behaviour(self, x, y):
        return self.attributes(self.metatile(x, y)) & 0xFF

    def touches_walkable(self, x, y):
        return any(not self.off_map(x + dx, y + dy) and not self.blocked(x + dx, y + dy)
                   for dx, dy in NEIGHBOURS)

    # ── what the drawing is asked ─────────────────────────────────────────

    def foliage(self, metatile):
        """Share of a metatile's drawn pixels that are leaves."""
        if metatile not in self._foliage:
            pair = pair_for(self.primary, self.secondary)
            drawn = dict(pair.layer_pixels(metatile, 0))
            drawn.update(pair.layer_pixels(metatile, 1))
            green = sum(1 for r, g, b in drawn.values() if g > 64 and g > r + 16 and g > b + 16)
            self._foliage[metatile] = green / len(drawn) if drawn else 0.0
        return self._foliage[metatile]

    def covers(self, metatile):
        """Drawn over the player right down the cell: the upper layer paints
        at least half of all but COVER_SLACK of its rows. A boulder, a bush
        or a rock outcrop is; a lamp or a post, whose top at most is drawn
        over the player, is not."""
        if metatile not in self._covers:
            rows = [0] * 16
            for (_, y) in pair_for(self.primary, self.secondary).layer_pixels(metatile, 1):
                rows[y] += 1
            self._covers[metatile] = sum(1 for n in rows if n >= 8) >= 16 - COVER_SLACK
        return self._covers[metatile]

    def treads(self, metatile):
        """Drawn as a flight of steps: the cell filled with bands, each row
        one colour right across and the rows changing colour down it. Only
        a staircase is drawn so; the elevation bits cannot tell it, since a
        flight is as often at elevation 0 (any level) as at its landing's."""
        if metatile not in self._treads:
            pair = pair_for(self.primary, self.secondary)
            drawn = dict(pair.layer_pixels(metatile, 0))
            drawn.update(pair.layer_pixels(metatile, 1))
            ok = len(drawn) >= 240
            if ok:
                lum = [[0.3 * r + 0.59 * g + 0.11 * b for (r, g, b) in
                        (drawn.get((x, y), (0, 0, 0)) for x in range(16))] for y in range(16)]
                across = sum(abs(row[x] - row[x + 1]) for row in lum for x in range(15)) / 240.0
                down = sum(abs(lum[y][x] - lum[y + 1][x]) for y in range(15) for x in range(16)) / 240.0
                ok = across <= TREAD_ACROSS and down >= TREAD_DOWN
            self._treads[metatile] = ok
        return self._treads[metatile]

    # ── buildings: the mass above each house door ──────────────────────────

    def houses(self):
        if self._houses is None:
            cells = set()
            for dx, dy in self.warps:
                if self.behaviour(dx, dy) not in HOUSE_DOORS:
                    continue
                seed = (dx, dy - 1)
                if self.off_map(*seed) or not self.blocked(*seed):
                    continue
                stack, seen = [seed], {seed}
                while stack:
                    x, y = stack.pop()
                    cells.add((x, y))
                    for ox, oy in NEIGHBOURS:
                        nx, ny = x + ox, y + oy
                        if ((nx, ny) in seen or self.off_map(nx, ny) or not self.blocked(nx, ny)
                                or abs(nx - dx) > HOUSE_HALF_WIDTH or ny > dy or ny < dy - HOUSE_HEIGHT
                                or self.foliage(self.metatile(nx, ny)) >= FOLIAGE):
                            continue
                        seen.add((nx, ny))
                        stack.append((nx, ny))
            self._houses = cells
        return self._houses

    # ── the roles ─────────────────────────────────────────────────────────

    def open_post(self, x, y):
        """Blocked outdoors and walkable to the west, east and south."""
        return (self.outdoor and self.blocked(x, y)
                and not any(self.blocked(x + dx, y + dy) for dx, dy in ((1, 0), (-1, 0), (0, 1))))

    def is_signpost(self, x, y):
        """A post standing on its own outdoors: blocked, walkable to the west,
        east and south. A sign event is one; so is a post without an event
        (Rustboro's street lamps) when it does not cover the player (a rock or
        a bush drawn over whoever stands behind it is not a post), is not
        foliage or part of a house, and has open ground or a
        different drawing to its north (a fence's run repeats itself).
        A sign drawn as a post (drawn_as_post) is one wherever it stands,
        walkable to its south: Littleroot's house signs stand against the
        house's wall."""
        if not self.outdoor or not self.blocked(x, y):
            return False
        if (not self.blocked(x, y + 1) and not self.off_map(x, y + 1)
                and self.drawn_as_post(self.metatile(x, y))):
            return True
        if self.free_post(x, y):
            return True
        # A lamp drawn like the map's free-standing ones is one against a
        # house's wall too, walkable to its south and on its other side:
        # Rustboro's lamps beside the Center and the Mart.
        if (self.blocked(x, y + 1) or self.off_map(x, y + 1)
                or (self.metatile(x, y), self.metatile(x, y - 1)) not in self.lamps()):
            return False
        sides = [(x + dx, y) for dx in (-1, 1) if self.blocked(x + dx, y)]
        return len(sides) == 1 and sides[0] in self.houses()

    def drawn_as_post(self, metatile):
        """Drawn as a sign that stands open on three sides somewhere: with
        its metatile (post_metatiles) or with its sign (draws_post_sign)."""
        return (post_key(self, metatile) in post_metatiles(self.events)
                or self.draws_post_sign(metatile))

    def draws_post_sign(self, metatile):
        """Its upper layer is a post's, pixel for pixel: that sign, over
        whatever ground its own tileset draws under it. Dewford's, Fortree's
        and Lavaridge's gym signs are the General tileset's, against the
        gym's wall or beside a tree."""
        post_metatiles(self.events)
        upper = self.upper_drawing(metatile)
        return upper is not None and upper in self.events.post_drawings

    def upper_drawing(self, metatile):
        """A metatile's upper layer, pixel by pixel, or None when it draws
        nothing there."""
        upper = pair_for(self.primary, self.secondary).layer_pixels(metatile, 1)
        return frozenset(upper.items()) if upper else None

    def free_post(self, x, y):
        """A post open on three sides: a sign event, or a drawing that is
        neither a cover, foliage nor a house, under open ground or another
        drawing."""
        if not self.open_post(x, y):
            return False
        if (x, y) in self.signs:
            return True
        m = self.metatile(x, y)
        if m >= NUM_PRIMARY and m in NOT_POSTS.get(self.secondary, ()):
            return False
        if m < NUM_PRIMARY and self.primary == "gTileset_General" and m in MOUNTAIN_ROCK:
            return False
        if m >= NUM_PRIMARY and self.mountain_drawn(m):
            return False
        # a corner of a mountain's outline: a little of its rock, the
        # mountain drawn on beside it (Route 114's corners over its grass,
        # mostly grass themselves, stood as signs on the plateau's edge)
        if m >= NUM_PRIMARY and self.rock_pixels(m) and any(
                not self.off_map(x + dx, y + dy) and self.mountain_drawn(self.metatile(x + dx, y + dy))
                for dx, dy in ((1, 0), (-1, 0), (0, 1))):
            return False
        if self.covers(m):
            return False
        if self.foliage(m) >= FOLIAGE or (x, y) in self.houses():
            return False
        return not self.blocked(x, y - 1) or self.metatile(x, y - 1) != m

    def mountain_drawn(self, metatile):
        """A secondary tileset's tile drawn mostly in the mountain's own
        colours (gen_voxel_relief.ROCK_ALL): its rock, like MOUNTAIN_ROCK.
        Fortree's 242, the corner of Route 120's cliff between its stairs and
        the river, stood as a sign laid with the stairs' tile. A map whose
        rock is the General's recoloured (gen_voxel_relief.alias_of) is read
        in the General's colours: Route 114's beige corners stood as signs."""
        drawn = self._drawn(metatile)
        return bool(drawn) and 2 * self.rock_pixels(metatile) >= len(drawn)

    def _drawn(self, metatile):
        pair = pair_for(self.primary, self.secondary)
        drawn = pair.layer_pixels(metatile, 0)
        drawn.update(pair.layer_pixels(metatile, 1))
        return drawn

    def rock_pixels(self, metatile):
        """How many of a metatile's pixels are in the mountain's colours
        (gen_voxel_relief.ROCK_ALL; the General's, through alias_of)."""
        import gen_voxel_relief as gr
        alias = gr.alias_of(self.layout_id, gr.ALIAS_LAYOUTS | gr.ALIAS_TERRACES)
        colour = alias[2] if alias else {}

        def rock(rgb):
            rgb = tuple(rgb)[:3]
            return rgb in gr.ROCK_ALL or colour.get(rgb) in gr.ROCK_ALL
        return sum(1 for rgb in self._drawn(metatile).values() if rock(rgb))

    def lamps(self):
        """(post, head) metatiles of the posts that stand free with no sign
        event and a head drawn in the open cell north of them: lamps."""
        if self._lamps is None:
            self._lamps = {(self.metatile(x, y), self.metatile(x, y - 1))
                           for y in range(1, self.h) for x in range(self.w)
                           if (x, y) not in self.signs and not self.blocked(x, y - 1)
                           and pair_for(self.primary, self.secondary).layer_pixels(
                               self.metatile(x, y - 1), 1)
                           and self.free_post(x, y)}
        return self._lamps

    def is_ledge_junction(self, x, y):
        """A blocked cell where a ledge turns a corner: ledges on two sides
        at right angles."""
        if not self.blocked(x, y):
            return False
        horizontal = any(self.behaviour(x + dx, y) in JUMPS for dx in (-1, 1))
        vertical = any(self.behaviour(x, y + dy) in JUMPS for dy in (-1, 1))
        return horizontal and vertical

    def role_at(self, x, y):
        hit = self._memo.get((x, y))
        if hit is not None:
            return hit
        behaviour = self.behaviour(x, y)
        if behaviour in WATER:
            role = "water"
        elif behaviour in JUMPS:
            role = "ledge"
        elif not self.blocked(x, y):
            role = "stair" if self.treads(self.metatile(x, y)) else "floor"
        elif self.is_signpost(x, y):
            role = "signpost"
        elif (x, y) in self.houses():
            role = "wall"
        elif self.foliage(self.metatile(x, y)) >= FOLIAGE:
            role = "tree"
        elif ((not self.blocked(x, y - 1) and not self.blocked(x, y + 1)
               and not self.off_map(x, y - 1) and not self.off_map(x, y + 1))
              or (not self.blocked(x - 1, y) and not self.blocked(x + 1, y)
                  and not self.off_map(x - 1, y) and not self.off_map(x + 1, y))):
            role = "fence"
        elif self.touches_walkable(x, y):
            role = "cliff"
        else:
            role = "shelf"
        self._memo[(x, y)] = role
        return role


def post_key(layout, metatile):
    """A metatile as the tileset it is drawn from names it."""
    return (layout.primary if metatile < NUM_PRIMARY else layout.secondary, metatile)


def post_metatiles(events):
    """The metatiles signs are drawn with: every sign event's that stands
    open on three sides somewhere (a town's sign in the middle of a square).
    The same metatile against a wall is the same sign. Kept on `events`,
    with the upper layers they draw (`post_drawings`, Layout.draws_post_sign)."""
    if getattr(events, "posts", None) is None:
        events.posts = set()        # the layouts read below ask it too
        events.post_drawings = set()
        found, drawings = set(), set()
        with open(os.path.join(ROOT, "data", "layouts", "layouts.json"), encoding="utf-8") as f:
            entries = json.load(f)["layouts"]
        for entry in entries:
            if not entry.get("id") or entry["id"] not in events.signs:
                continue
            layout = Layout(entry, events)
            for (x, y) in layout.signs:
                if not layout.off_map(x, y) and layout.open_post(x, y):
                    found.add(post_key(layout, layout.metatile(x, y)))
                    upper = layout.upper_drawing(layout.metatile(x, y))
                    if upper is not None:
                        drawings.add(upper)
        events.posts, events.post_drawings = found, drawings
    return events.posts


def alternate_layouts():
    """{layout no map uses: the layout of the map it stands in for} - one
    named after a map's layout and drawn the same size."""
    maps_dir = os.path.join(ROOT, "data", "maps")
    used = set()
    for name in os.listdir(maps_dir):
        path = os.path.join(maps_dir, name, "map.json")
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                used.add(json.load(f).get("layout"))
    size = {e["id"]: (e["width"], e["height"]) for e in load_layouts() if e.get("id")}
    out = {}
    for lid in size:
        if lid in used:
            continue
        bases = [b for b in used if b in size and lid.startswith(b + "_") and size[b] == size[lid]]
        if bases:
            out[lid] = max(bases, key=len)
    return out


def load_layouts():
    with open(os.path.join(ROOT, "data", "layouts", "layouts.json"), encoding="utf-8") as f:
        return json.load(f)["layouts"]


def main():
    want = sys.argv[1] if len(sys.argv) > 1 else "LAYOUT_LITTLEROOT_TOWN"
    entry = next(e for e in load_layouts() if e.get("id") == want)
    layout = Layout(entry, MapEvents())
    letters = {"water": "~", "ledge": "_", "stair": "=", "floor": ".", "signpost": "S", "wall": "W",
               "tree": "T", "fence": "|", "cliff": "#", "shelf": "%", "prop": "o"}
    for y in range(layout.h):
        print("".join(letters[layout.role_at(x, y)] for x in range(layout.w)))


if __name__ == "__main__":
    main()
