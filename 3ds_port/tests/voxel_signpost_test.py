"""Signposts: the sign event rule, eventless posts (street lamps) and the sign
silhouette, on synthetic cells and on real maps."""

import json
import pathlib
import sys
import types

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import voxel_cells  # noqa: E402
from gen_voxel_sign_masks import sign_record  # noqa: E402
from voxel_sign_mask import cutout_mask, ground_colours, metatile_mask  # noqa: E402


class FakeLayout:
    is_signpost = voxel_cells.Layout.is_signpost
    open_post = voxel_cells.Layout.open_post
    free_post = voxel_cells.Layout.free_post
    drawn_as_post = voxel_cells.Layout.drawn_as_post
    primary, secondary = "primary", "secondary"

    def __init__(self, blocked, signs, outdoor=True, drawn=None, covering=(), posts=(),
                 copies=()):
        self.events = types.SimpleNamespace(posts={("primary", m) for m in posts})
        self._copies = set(copies)
        self.w = len(blocked[0])
        self.h = len(blocked)
        self.outdoor = outdoor
        self.signs = signs
        self._blocked = blocked
        self._drawn = drawn or [[0] * self.w for _ in range(self.h)]
        self._covering = set(covering)

    def metatile(self, x, y):
        return None if self.off_map(x, y) else self._drawn[y][x]

    def covers(self, metatile):
        return metatile in self._covering

    def draws_post_sign(self, metatile):
        return metatile in self._copies

    def foliage(self, metatile):
        return 0.0

    def houses(self):
        return set()

    def lamps(self):
        return set()

    def off_map(self, x, y):
        return x < 0 or y < 0 or x >= self.w or y >= self.h

    def blocked(self, x, y):
        return False if self.off_map(x, y) else self._blocked[y][x]


def run():
    # An enclosed transparent pocket survives the outside flood.
    rows = cutout_mask({(x, y) for x in range(3) for y in range(3) if (x, y) != (1, 1)}, 3, 3)
    assert rows[1] & (1 << 1)
    # A diagonal gap does not connect the pocket to the border.
    assert cutout_mask({(1, 0), (0, 1), (2, 1), (1, 2)}, 3, 3) == [2, 7, 2]
    # Opening the pocket with an orthogonal gap makes it exterior.
    assert cutout_mask({(0, 1), (2, 1), (1, 2)}, 3, 3) == [0, 5, 2]
    # A board that fills its cell is all sign.
    assert cutout_mask({(x, y): 1 for x in range(16) for y in range(16)}) == [65535] * 16

    blocked = [[False] * 3 for _ in range(3)]
    blocked[1][1] = True
    blocked[0][1] = True  # a building behind the sign is allowed
    assert FakeLayout(blocked, {(1, 1)}).is_signpost(1, 1)
    # No event: a post if it does not cover the player and differs from
    # what stands north of it (a lamp); not a boulder, nor a fence's run.
    drawn = [[0, 2, 0], [0, 1, 0], [0, 0, 0]]
    assert FakeLayout(blocked, set(), drawn=drawn).is_signpost(1, 1)
    assert not FakeLayout(blocked, set(), drawn=drawn, covering={1}).is_signpost(1, 1)
    drawn[0][1] = 1
    assert not FakeLayout(blocked, set(), drawn=drawn).is_signpost(1, 1)
    assert not FakeLayout(blocked, {(1, 1)}, outdoor=False).is_signpost(1, 1)
    blocked[1][0] = True
    assert not FakeLayout(blocked, {(1, 1)}).is_signpost(1, 1)         # a side is closed
    # ...unless it is drawn as a sign stands elsewhere, open on three sides:
    # a house's sign against its wall
    drawn = [[0, 2, 0], [0, 3, 0], [0, 0, 0]]
    assert FakeLayout(blocked, {(1, 1)}, drawn=drawn, posts={3}).is_signpost(1, 1)
    # ...or as its sign is, in another tileset's metatile
    assert FakeLayout(blocked, {(1, 1)}, drawn=drawn, copies={3}).is_signpost(1, 1)
    blocked[2][1] = True
    assert not FakeLayout(blocked, {(1, 1)}, drawn=drawn, posts={3}).is_signpost(1, 1)
    assert not FakeLayout(blocked, {(1, 1)}, drawn=drawn, copies={3}).is_signpost(1, 1)

    entry = next(e for e in voxel_cells.load_layouts() if e.get("id") == "LAYOUT_LITTLEROOT_TOWN")
    layout = voxel_cells.Layout(entry, voxel_cells.MapEvents())
    pair = voxel_cells.pair_for(layout.primary, layout.secondary)
    signs = [(x, y) for y in range(layout.h) for x in range(layout.w)
             if layout.role_at(x, y) == "signpost"]
    assert signs, "Littleroot Town has signs"
    assert layout.signs <= set(signs), "every Littleroot sign, the houses' too"
    for x, y in signs:
        rows = metatile_mask(pair, layout, x, y)
        assert any(rows)
        ground = ground_colours(pair, layout, x, y)
        for (px, py), colour in pair.layer_pixels(layout.metatile(x, y), 0).items():
            if colour not in ground:
                assert rows[py] & (1 << px)
        for px, py in pair.layer_pixels(layout.metatile(x, y), 1):
            assert rows[py] & (1 << px)
    # Rustboro's street lamps have no sign event and are posts all the same.
    entry = next(e for e in voxel_cells.load_layouts() if e.get("id") == "LAYOUT_RUSTBORO_CITY")
    rustboro = voxel_cells.Layout(entry, voxel_cells.MapEvents())
    lamps = [(x, y) for y in range(rustboro.h) for x in range(rustboro.w)
             if rustboro.role_at(x, y) == "signpost" and (x, y) not in rustboro.signs]
    assert len(lamps) >= 12, lamps
    # and so are the two against the Center's and the Mart's walls
    assert {(19, 38), (19, 45)} <= set(lamps), lamps
    # Every gym's sign stands whole: its round top, drawn in the cell north
    # of it, stands on it - against the gym's wall or a tree too (Dewford,
    # Fortree, Lavaridge: their tilesets' copies of the General tileset's).
    events = voxel_cells.MapEvents()
    layouts = {e.get("id"): e for e in voxel_cells.load_layouts()}
    gyms = 0
    for path in sorted((ROOT.parent / "data" / "maps").glob("*/map.json")):
        with open(path, encoding="utf-8") as f:
            m = json.load(f)
        for b in m.get("bg_events") or []:
            if not b.get("script", "").endswith("_EventScript_GymSign"):
                continue
            layout = voxel_cells.Layout(layouts[m["layout"]], events)
            pair = voxel_cells.pair_for(layout.primary, layout.secondary)
            x, y = b["x"], b["y"]
            assert layout.role_at(x, y) == "signpost", (path.parent.name, layout.role_at(x, y))
            rows, head, _, _ = sign_record(layout, pair, x, y)
            assert rows[15] and not all(r == 0xFFFF for r in rows), path.parent.name
            assert any(head), path.parent.name
            gyms += 1
    assert gyms == 8, gyms
    print("PASS voxel signpost: sign events, lamps, open sides, silhouettes "
          "(%d real signs, %d Rustboro lamps, %d gym signs)" % (len(signs), len(lamps), gyms))


if __name__ == "__main__":
    run()
