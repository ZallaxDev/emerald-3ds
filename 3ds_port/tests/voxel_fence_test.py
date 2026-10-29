"""The fences built post by post (scripts/voxel_fence.py, the `fence` and
`stakes` specs of voxel_building_specs.py), against the cartridge's maps:

- every metatile any map draws a fence post's cap or a log's cut face with
  (on either layer) is one of a posts spec's, in every layout over its
  tilesets: no variant is left flat (but a tree's, its crown before a fence
  that runs behind it);
- the posts stand where the drawing puts them: along a row 8 high, down a
  column 10 (the run's end post shows it), every 8 rows; a log 7 high,
  2.5 across and 2 deep; the rails 2 thick between them;
- every block of every run reproduces its drawing exactly in the GBA's
  45-degree view, at one texel per pixel;
- the Battle Pyramid and the Battle Dome leave their fences to the `fence`
  spec: no slab of theirs, and their cells' fence pixels are not theirs."""

import json
import os
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import gen_voxel_buildings as g  # noqa: E402
import voxel_art  # noqa: E402
import voxel_building as vb  # noqa: E402
import voxel_building_specs as specs  # noqa: E402
import voxel_cells as vc  # noqa: E402
import voxel_fence  # noqa: E402

FAILED = []


def check(name, ok, detail=""):
    print("%s %-60s %s" % ("PASS" if ok else "FAIL", name, detail))
    if not ok:
        FAILED.append(name)


def spec(name):
    return next(s for s in specs.SPECS if s["name"] == name)


def tiles_for(comp, entry):
    if "primary" in comp and entry["primary_tileset"] != comp["primary"]:
        return set()
    if "secondary" in comp and entry["secondary_tileset"] != comp["secondary"]:
        return set()
    return set(comp["tiles"]) | set(comp.get("also", {}).get(entry["secondary_tileset"], ()))


def test_every_variant():
    """A post's cap (the fence's) or a cut face (the logs') drawn by a
    metatile no posts spec lists."""
    layouts = json.load(open(os.path.join(vb.ROOT, "data", "layouts", "layouts.json"),
                             encoding="utf-8"))["layouts"]
    families = [spec("fence")["components"], spec("stakes")["components"]]
    missed = []
    for entry in layouts:
        if "blockdata_filepath" not in entry:
            continue
        pair = voxel_art.Pair(entry["primary_tileset"], entry["secondary_tileset"])
        listed = set()
        patterns = []
        for comp in families:
            mine = tiles_for(comp, entry)
            listed |= mine
            if not mine and not ("primary" in comp and entry["primary_tileset"] == comp["primary"]):
                continue
            fam = comp["posts"]
            ref = dict(pair.layer_pixels(fam["ref"], 1))
            u, v, w, h = fam["find"]
            patterns.append([[ref.get((u + a, v + b)) for a in range(w)] for b in range(h)])
        if not patterns:
            continue
        blocks = voxel_art.read_u16(os.path.join(vb.ROOT, entry["blockdata_filepath"]))
        for m in sorted({b & 0x3FF for b in blocks} - listed):
            drawn = dict(pair.layer_pixels(m, 0))
            drawn.update(pair.layer_pixels(m, 1))
            green = sum(1 for r, gg, b in drawn.values() if gg > 64 and gg > r + 16 and gg > b + 16)
            if drawn and green / len(drawn) >= vc.FOLIAGE:
                continue    # a tree before a fence (Route 110's 040): the tree's cell
            for layer in (0, 1):
                px = pair.layer_pixels(m, layer)
                if any(all(px.get((u + a, v + b)) == pat[b][a] for b in range(len(pat))
                           for a in range(len(pat[0])))
                       for pat in patterns for v in range(17 - len(pat))
                       for u in range(17 - len(pat[0]))):
                    missed.append("%s %#x" % (entry["id"], m))
                    break
    check("every metatile drawing a post or a log is a posts spec's", not missed,
          ", ".join(missed[:8]))


def build(layout_id, rect_cells):
    """The posts and rails of the run block whose cells are rect_cells."""
    for sp in g.component_specs(spec("fence" if "MAUVILLE" not in layout_id else "stakes"), {}):
        x, y, _, _ = sp["rect"]
        if sp["layout"] == layout_id and {(x + i, y + j) for (i, j) in sp["owned"]} >= rect_cells:
            layout = vb.LayoutArt(layout_id)
            art, parts, ref = voxel_fence.build(layout, sp, sp["relief"]["posts"],
                                                g.kept_by_others(layout_id))
            return sp, parts
    return None, []


def test_heights():
    # Route 110's fence along row 19 (0x148, 0x149): posts 8 high, 2 deep,
    # rails at 4-6 between them
    sp, parts = build("LAYOUT_ROUTE110", {(18, 19), (19, 19)})
    x, y, _, _ = sp["rect"]
    posts = [p for p in parts if p.name == "post"]
    rails = [p for p in parts if p.name == "rail"]
    row = [p for p in posts if min(z for _, z in p.plan) == (19 - y) * 16 + 11]
    check("a post along a row: 8 high, 4 by 2", bool(row) and all(
        p.y1 == 8 and max(a for a, _ in p.plan) - min(a for a, _ in p.plan) == 4 for p in row),
        str([(p.plan[0], p.y1) for p in row[:2]]))
    check("  its rails 2 thick at 4-6", bool(rails) and all((r.y0, r.y1) == (4.0, 6.0) for r in rails))
    # Route 110's fence down column 9 to its end post (0x133 at 9,10)
    sp, parts = build("LAYOUT_ROUTE110", {(9, 8), (9, 9), (9, 10)})
    x, y, _, _ = sp["rect"]
    col = sorted((min(z for _, z in p.plan), p.y1) for p in parts if p.name == "post"
                 and min(a for a, _ in p.plan) == (9 - x) * 16 + 10)
    steps = {b[0] - a[0] for a, b in zip(col, col[1:])}
    check("posts down a column every 8 rows, 10 high", steps == {8} and all(h == 10 for _, h in col),
          str(col))
    check("  the end post's foot where its front ends (row 13)", col and col[-1][0] + 2 == (10 - y) * 16 + 13,
          str(col[-1:]))
    # Mauville's logs along row 0 (0x291): 7 high, 2.5 across, 2 deep, on row 15
    sp, parts = build("LAYOUT_MAUVILLE_CITY", {(28, 2), (29, 2)})
    logs = [p for p in parts if p.name == "log"]
    check("a log: 7 high, 2.5 by 2, its foot on row 15", bool(logs) and all(
        (p.y1, p.rx, p.rz, (p.cz + p.rz) % 16) == (7, 2.5, 2.0, 15) for p in logs),
        str([(p.cx, p.cz, p.y1) for p in logs[:2]]))


def test_exact():
    bad, n = [], 0
    layouts = {}
    for name in ("fence", "stakes"):
        for sp in g.component_specs(spec(name), {}):
            lid = sp["layout"]
            layout = layouts.setdefault(lid, vb.LayoutArt(lid))
            art, parts, ref = voxel_fence.build(layout, sp, sp["relief"]["posts"], g.kept_by_others(lid))
            x, y, w, h = sp["rect"]
            m = vb.Model(sp["name"], art, parts, (w, h), 1)
            wrong, missing, extra = vb.ortho_check(m, exact=[(0, 0, w * 16, h * 16)], reference=ref)
            dens = vb.density_check(m)
            n += 1
            if wrong or missing or extra or dens:
                bad.append("%s %d/%d/%d/%d" % (sp["name"], wrong, missing, extra, len(dens)))
    check("every block of every fence exact at 45 degrees (%d)" % n, n > 150 and not bad,
          ", ".join(bad[:6]))


def test_frontier():
    import inspect
    check("the Battle Pyramid stands no fence of its own",
          "fence" not in inspect.getsource(specs.battle_pyramid).split('"""')[-1])
    check("the Battle Dome stands no fence box of its own",
          all(x0 >= 16 and x1 <= 192 for (x0, x1, _, _, _) in specs.DOME_BOXES))
    kept = g.kept_by_others("LAYOUT_BATTLE_FRONTIER_OUTSIDE_EAST")
    check("  the pyramid keeps its wall in a shared cell, not the posts",
          (51 * 16 + 9, 10 * 16 + 4) in kept and (51 * 16 + 3, 10 * 16 + 4) not in kept)


def main():
    test_every_variant()
    test_heights()
    test_exact()
    test_frontier()
    print("%d failure(s)" % len(FAILED))
    sys.exit(1 if FAILED else 0)


if __name__ == "__main__":
    main()
