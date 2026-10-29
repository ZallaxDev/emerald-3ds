"""The Battle Frontier's outside, modelled whole (voxel_building_specs.py):

- every flag on its walls, East and West (voxel_props finds them by their
  tiles), stands as a card whose cloth follows the game's animation: each
  of its live texels reads the cloth's tile and pixel the map draws there,
  at every copy, and at every frame of tileset_anims.c's flag it is that
  frame's colour;
- the East's houses are the West's (drawn alike in its own tiles) and stand
  at each of them; the halls with the Frontier's emblem, the arches over
  the walk to the gate and the flowerbeds are modelled;
- the Battle Pike's Seviper is its head and body, a height field with no
  column short of its drawn top (such a column was carried back north as a
  wall, the dark slab its tail stood up as), and its tail a blade apart;
- every one of them reproduces its drawing exactly in the GBA's 45-degree
  view, at one texel per pixel."""

import os
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import gen_voxel_buildings as g  # noqa: E402
import voxel_building as vb  # noqa: E402
import voxel_building_specs as specs  # noqa: E402
import voxel_props  # noqa: E402

FAILED = []
EAST, WEST = "LAYOUT_BATTLE_FRONTIER_OUTSIDE_EAST", "LAYOUT_BATTLE_FRONTIER_OUTSIDE_WEST"
NAMES = ("frontier_flag_east", "frontier_flag_west", "frontier_house", "frontier_house_glass",
         "frontier_house_long", "frontier_hall", "frontier_hall_porch", "frontier_hall_gate",
         "frontier_arch", "frontier_planter_east", "frontier_planter_west", "battle_pike")
FLAG_ANIM = {EAST: "data/tilesets/secondary/battle_frontier_outside_east/anim/flag/%d.4bpp",
             WEST: "data/tilesets/secondary/battle_frontier_outside_west/anim/flag/%d.4bpp"}
# tileset_anims.c: the flag's frames over the secondary tileset's tiles 218-223
FLAG_FIRST_TILE = vb.NUM_PRIMARY + 218


def check(name, ok, detail=""):
    print("%s %-64s %s" % ("PASS" if ok else "FAIL", name, detail))
    if not ok:
        FAILED.append(name)


def models():
    """The Frontier's models alone (the rest of the catalogue unbuilt)."""
    keep = specs.SPECS
    specs.SPECS = [s for s in keep if s["name"] in NAMES]
    reuse = g.reuse_everywhere
    g.reuse_everywhere = lambda: None
    try:
        return g.build_models()
    finally:
        specs.SPECS, g.reuse_everywhere = keep, reuse


def test_flags(built):
    found = voxel_props.everywhere(["frontier_flag_east", "frontier_flag_west"])
    count = {lid: len(v) for lid, v in found.items()}
    check("every flag found: 21 East, 35 West", count == {EAST: 21, WEST: 35}, str(count))
    flags = [m for m in built if m.name.startswith("frontier_flag")]
    placed = {lid: 0 for lid in count}
    for m in flags:
        for (lid, _, _) in m.spec["at"]:
            placed[lid] += 1
    check("  and each stands as a model", placed == count, str(placed))
    check("  each a card whose cloth is live (6 tiles of 64 texels)",
          flags and all(len(m.live) == 6 * 64 for m in flags)
          and all(all("~live" in tag for (_, _, tag) in m.mesh.tris) for m in flags))
    bad = []
    for m in flags:
        layout = m.layout
        frames = [vb.anim_frame(FLAG_ANIM[layout.id] % k, FLAG_FIRST_TILE) for k in range(4)]
        x0, y0 = m.spec["rect"][:2]
        for (lid, cx, cy) in m.spec["at"]:
            for (u, v, tile, pixel, colours, bg) in m.live:
                src = layout.upper_source(cx * 16 + u, cy * 16 + v)
                if src[:2] != (tile, pixel) or tuple(layout.palette(src[2])) != colours:
                    bad.append("%s at %d,%d texel %d,%d" % (m.name, cx, cy, u, v))
                    break
        moving = 0
        for (u, v, tile, pixel, colours, bg) in m.live:
            seen = {layout.tile_pixel(tile, pixel, f) for f in frames}
            moving += len(seen) > 1
        if not moving:
            bad.append("%s: no texel changes from frame to frame" % m.name)
    check("  every copy's live texels read its own cloth, which waves", not bad, "; ".join(bad[:4]))


def test_places(built):
    lj = vb._layouts_json()
    where = {}
    for m in built:
        if m.name.startswith(("frontier_house", "frontier_hall", "frontier_arch")):
            where[m.name] = sorted((p[4], p[1], p[2]) for p in g.find_placements(m, lj))
    east = sorted(xy for n in ("frontier_house", "frontier_house_glass", "frontier_house_long")
                  for (lid, *xy) in where.get(n, []) if lid == EAST)
    check("the East's five houses stand", east == [[3, 41], [4, 5], [13, 48], [21, 48], [64, 28]],
          str(east))
    check("the halls with the emblem stand: by the stairs, under the Pyramid, the gate",
          where.get("frontier_hall") == [(EAST, 7, 24)]
          and where.get("frontier_hall_porch") == [(EAST, 32, 8)]
          and where.get("frontier_hall_gate") == [(WEST, 23, 61)])
    check("the three arches over the walk",
          where.get("frontier_arch") == [(WEST, 24, 46), (WEST, 24, 51), (WEST, 24, 56)],
          str(where.get("frontier_arch")))
    beds = {}
    for m in built:
        if m.name.startswith("frontier_planter"):
            for (x, y) in m.spec["repeat_at"]:
                beds.setdefault(m.spec["layout"], set()).update(
                    (x + i, y + j) for (i, j) in m.spec["owned"])
    check("the flowerbeds: 6 West, 2 East, every cell of them",
          len(beds.get(WEST, ())) == 2 * 4 * 2 + 5 * 3 + 10 * 3
          and len(beds.get(EAST, ())) == 5 * 6 + 9 * 3, str({k: len(v) for k, v in beds.items()}))


def test_pike(built):
    pike = next(m for m in built if m.name == "battle_pike")
    kinds = [type(p).__name__ for p in pike.prisms]
    check("the Pike: a height field and its tail apart", kinds == ["HeightSolid", "Blade"], str(kinds))
    check("  no column of it short of its drawn top", pike.prisms[0].short == {},
          str(sorted(pike.prisms[0].short)[:6]))


def test_exact(built):
    bad = []
    for m in built:
        wrong, missing, extra = vb.ortho_check(m, exact=m.spec.get("exact"),
                                               reference=m.spec.get("drawing"))
        dens = vb.density_check(m)
        if wrong or missing or extra or dens:
            bad.append("%s %d/%d/%d/%d" % (m.name, wrong, missing, extra, len(dens)))
    check("every one exact at 45 degrees (%d models)" % len(built), len(built) >= 30 and not bad,
          ", ".join(bad[:6]))


def main():
    os.chdir(vb.ROOT)
    built = models()
    test_flags(built)
    test_places(built)
    test_pike(built)
    test_exact(built)
    print("%d failure(s)" % len(FAILED))
    sys.exit(1 if FAILED else 0)


if __name__ == "__main__":
    main()
