"""The furniture parts of gen_voxel_buildings.interior_specs, on synthetic
drawings: a side dressed as its front's casing (plain_side); a table told
standing on its legs (leg_split) and built as a top on posts, a pedestal
and an apron (table_plan, table_parts), its top's side what its front shows
under the same edge (slab_side), and still its drawing, pixel for pixel, at
45 degrees (voxel_building.ortho_check); a top against a wall lying flat
(voxel_building.Relief `against`); a piece's pixels as its outline
delimits them (outline_trim); what is drawn no deeper than its front
(is_flat); a frame seen through from behind as from before (Relief
`see_through`); a dome, round and still its drawing at 45 degrees
(dome_fit, voxel_building.ProjDome); a glass table's feet a row tall
(leg_split `explicit`); an escalator built from the side, its slopes
its own and its drawing at 45 degrees (voxel_building.ProjRun); a counter
modelled whole on its U of a plan, its corners cut on the slant and the
yard between its arms open (voxel_building.PlanPrism); a ball, round and
its circle at 45 degrees (ball_fit); a jar turned, its belly and neck
as drawn (lathe_fit); a card lifted onto a stand (Card `base`); what of
a stand's shape is its top,
not the pots in a row on it (support_left `sibs`), nor the room round it
a pot's (auto_leave); what a ball hides of the arm it
stands on, that arm's own column (fill_hidden "column"); and a stool in
blocks, its cushion's corners cut on its base and foot, its drawing at 45
degrees and each hidden side a quad of its wide strip
(voxel_building_specs.center_stool)."""

import collections
import pathlib
import sys
import tempfile
import types

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import gen_voxel_buildings as g  # noqa: E402
import voxel_building as vb  # noqa: E402
from PIL import Image  # noqa: E402

FAILED = []
OUTLINE, WOOD, CLOTH = (65, 65, 82, 255), (148, 115, 90, 255), (246, 205, 238, 255)
SHADOW, CLEAR, GREY = (131, 131, 148, 255), (0, 0, 0, 0), (139, 139, 148, 255)
BLUE, RED, HEM = (98, 98, 197, 255), (197, 74, 65, 255), (213, 123, 172, 255)


def check(name, ok, detail=""):
    print("%s %-46s %s" % ("PASS" if ok else "FAIL", name, detail))
    if not ok:
        FAILED.append(name)


def hexes_of(img):
    px = img.load()
    return [["%02x%02x%02x" % px[x, y][:3] for x in range(img.width)] for y in range(img.height)]


def own_of(img):
    return {(x, y) for y in range(img.height) for x in range(img.width) if img.getpixel((x, y))[3]}


def exact(obj, art, parts, cells):
    model = vb.Model("t", art, parts, cells, 0)
    judged = types.SimpleNamespace(art=obj, mesh=model.mesh, name="t")
    return model, vb.ortho_check(judged, None, exact=[(0, 0, obj.width, obj.height)])


# ── plain_side: the casing its front shows at its ends ───────────────────

front = Image.new("RGBA", (16, 8), WOOD)
fpx = front.load()
for x in range(16):
    fpx[x, 3] = OUTLINE                         # the line between two drawers
    fpx[x, 7] = OUTLINE                         # its foot
mine = {(x, y) for y in range(8) for x in range(16)}
side = g.plain_side(mine, fpx, 8, 8)
rows = [side.getpixel((0, v)) for v in range(8)]
check("plain side: no drawer line inside it", all(c == WOOD for c in rows[:7]), rows)
check("plain side: its foot's outline, no rim it has not",
      rows[7] == OUTLINE and rows[0] == WOOD)
check("plain side: every column alike",
      all(side.getpixel((u, v)) == rows[v] for v in range(8) for u in range(side.width)))
plank = g.plain_side({(x, y) for y in (6, 7) for x in range(16)}, fpx, 2, 8)
check("plain side: a desk's thin edge keeps its outline under it",
      [plank.getpixel((0, v)) for v in (0, 1)] == [WOOD, OUTLINE])
shelf = Image.new("RGBA", (16, 6), GREY)        # a bookcase: frame, books inside it
spx = shelf.load()
for y in range(6):
    spx[0, y] = spx[15, y] = OUTLINE
    for x in range(4, 12):
        spx[x, y] = BLUE if x % 2 else RED
side = g.plain_side(own_of(shelf), spx, 6, 6)
check("plain side: a bookcase's frame, not its books",
      all(side.getpixel((0, v)) == GREY for v in range(6)))


# ── a table on two legs, its shadow between them ─────────────────────────

def table(legs=True, cut_corner=False, wood_corners=False):
    """28 x 32: the cloth's top (rows 0-23), its hem (24-26), legs and the
    shadow under it (27-31) - or, legs False, the cloth down to the floor.
    With wood_corners the top's corners and the hem under them are wood."""
    img = Image.new("RGBA", (32, 40), CLEAR)
    px = img.load()
    for y in range(0, 32):
        for x in range(2, 30):
            corner = wood_corners and (x < 7 or x > 24)
            if y < 24:
                px[x, y] = WOOD if corner and (y < 5 or y > 18) else CLOTH
            elif y < 27:
                px[x, y] = OUTLINE if y == 26 else (WOOD if corner else HEM)
            elif not legs:
                px[x, y] = CLOTH
            elif x in (4, 5, 6, 25, 26, 27):
                px[x, y] = WOOD if x not in (4, 27) else OUTLINE
            else:
                px[x, y] = SHADOW
    if cut_corner:
        for y in range(29, 32):
            px[2, y] = px[29, y] = CLEAR
    return img


img = table()
hexes = hexes_of(img)
mine = own_of(img)
split = g.leg_split(mine, hexes, 32, 8, {"838394"})
check("legs: the hem found", split is not None and split[0] == 27,
      None if split is None else split[0])
check("legs: the shadow between them is the floor's",
      split is not None and split[1] == {(x, y) for (x, y) in mine
                                         if y >= 27 and img.getpixel((x, y)) == SHADOW})
check("no legs: the cloth to the floor", g.leg_split(
    {(x, y) for y in range(32) for x in range(2, 30)}, hexes_of(table(False)), 32, 8,
    {"838394"}) is None)
box = table(False, cut_corner=True)
check("no legs: a box with its corners cut",
      g.leg_split(own_of(box), hexes_of(box), 32, 8, {"838394"}) is None)
# a glass table: its top's front four rows, then feet a row tall at its
# corners with its shadow between - legs found only when they are given
glass = Image.new("RGBA", (32, 40), CLEAR)
gpx = glass.load()
for y in range(27, 32):
    for x in range(2, 30):
        gpx[x, y] = BLUE if y < 31 else (WOOD if x in (2, 3, 28, 29) else SHADOW)
split = g.leg_split(own_of(glass), hexes_of(glass), 32, 5, {"838394"}, explicit=4)
check("legs given: feet a row tall under a glass top",
      split is not None and split[0] == 31
      and split[1] == {(x, 31) for x in range(4, 28)}, None if split is None else split[0])
check("legs not given: a row is no legs",
      g.leg_split(own_of(glass), hexes_of(glass), 32, 5, {"838394"}) is None)


def built(img, hem, height, foot, back=None, against=False):
    """As interior_specs does: its own pixels (the floor under its top
    left to the floor), its sides under it at row 40, its parts."""
    obj = img.crop((0, 0, 32, 32))
    opx = obj.load()
    for (x, y) in g.leg_split(own_of(obj), hexes_of(obj), foot, height, {"838394"})[1]:
        opx[x, y] = CLEAR
    plan = g.table_plan(obj, hem, foot, height, back)
    art = Image.new("RGBA", (max(32, plan[1].width), 40 + plan[1].height), CLEAR)
    art.paste(obj, (0, 0))
    art.paste(plan[1], (0, 40))
    pc = {"name": "t", "height": height, "against": 32 if against else None}
    return obj, art, plan, g.table_parts(pc, obj, plan, hem, foot, back, None, 40)


obj, art, plan, parts = built(table(wood_corners=True), 27, 8, 32)
runs = [r["kind"] for r in plan[0]]
check("table: two legs of five pixels are posts", runs == ["post", "post"], runs)
model, proof = exact(obj, art, parts, (2, 2))
tags = {t for (_, _, t) in model.mesh.tris}
check("table: back posts behind, hidden at 45 degrees",
      any("~depth" in t and "flank" in t for t in tags), sorted(tags)[:4])
middle = [min(p[1] for p in tri) for (tri, _, tag) in model.mesh.tris
          if tag.endswith("front") and min(p[0] for p in tri) < 24 and max(p[0] for p in tri) > 8]
check("table: open under its top between the legs",
      bool(middle) and min(middle) >= 5, min(middle) if middle else None)
check("table: its drawing again at 45 degrees", proof == (0, 0, 0), proof)
slab = plan[1].crop(plan[2]["slab"])
ends = [slab.getpixel((d, 0)) for d in (1, slab.width - 2)]
mid = slab.getpixel((slab.width // 2, 0))
check("table: its top's side, wood at its corners", ends == [WOOD, WOOD], ends)
check("table: its top's side, the hem in the middle", mid == HEM, mid)
check("table: its top's side, the front's outline under it",
      all(slab.getpixel((d, 2)) == OUTLINE for d in range(slab.width)))
flanks = [tri for (tri, _, tag) in model.mesh.tris if tag.endswith(".flank")]
check("table: no row of its side hanging under its top",
      all(min(p[1] for p in tri) >= 5 - 1e-6 for tri in flanks if max(p[1] for p in tri) > 5),
      sorted({min(p[1] for p in tri) for tri in flanks}))


# ── a desk: a top over a leg, an apron and drawers ───────────────────────

def desk():
    """32 x 24, against a wall at row 4: a top (rows 4-14, its front edge
    15-16), a leg at x 1-4 with the floor beside it, the dark under the
    top between it and the drawers (17-18), the drawers at x 20-30."""
    img = Image.new("RGBA", (32, 40), CLEAR)
    px = img.load()
    for y in range(4, 17):
        for x in range(32):
            px[x, y] = OUTLINE if y == 16 or x in (0, 31) else (WOOD if y == 15 else CLOTH)
    for y in range(17, 23):
        for x in range(32):
            if 1 <= x <= 4:
                px[x, y] = OUTLINE if x in (1, 4) else (GREY if y >= 20 else WOOD)
            elif 20 <= x <= 30:
                px[x, y] = OUTLINE if x in (20, 30) or y in (19, 22) else WOOD
            elif 5 <= x <= 19 and y < 19:
                px[x, y] = OUTLINE
            else:
                px[x, y] = SHADOW
    return img


d = desk()
split = g.leg_split(own_of(d), hexes_of(d), 23, 8, {"838394"})
check("desk: its top ends where the floor shows beside its leg",
      split is not None and split[0] == 17, None if split is None else split[0])
obj, art, plan, parts = built(d, 17, 8, 23, back=12, against=True)
runs = [(r["kind"], r["u0"], r["u1"]) for r in plan[0]]
check("desk: a post, the dark under its top, its drawers",
      [k for k, _, _ in runs] == ["post", "apron", "pedestal"], runs)
model, proof = exact(obj, art, parts, (2, 2))
check("desk: its drawing again at 45 degrees", proof == (0, 0, 0), proof)
post = next(p for p in parts if isinstance(p, vb.Relief) and p.side.rect[0] == 1)
check("desk: a post's side is its own front, its foot and all",
      post.side.rect == (1, 17, 5, 23), post.side.rect)


# ── a top against a wall lies flat ───────────────────────────────────────

top = Image.new("RGBA", (16, 24), CLEAR)
tpx = top.load()
for y in range(2, 20):
    for x in range(16):
        tpx[x, y] = WOOD
relief = vb.Relief("t", top, 8, vb.Tile(0, 0, 16, 8), foot=20, solid=True, back=14,
                   against=True)
model = vb.Model("t", top, [relief], (1, 2), 0)
tops = {round(p[1], 3) for (tri, _, tag) in model.mesh.tris if ".top" in tag for p in tri}
check("against a wall: no riser up it",
      not any("riser" in t for (_, _, t) in model.mesh.tris))
check("against a wall: its top flat, to the wall",
      tops == {8.0} and min(p[2] for (tri, _, tag) in model.mesh.tris if ".top" in tag
                            for p in tri) == 14, tops)


# ── outline_trim: what the outline closes in ─────────────────────────────

def grid(rows):
    """A drawing as text: # outline, b book, p pages (the desk's beige
    too), d the desk, g grey; I a mid-grey outline, w white, m grey."""
    colours = {"#": "414152", "b": "c54a41", "p": "bdbda4", "d": "fff67b", "g": "8b8b94",
               "y": "eee67b", "I": "62627b", "w": "ffffff", "m": "949494", ".": None}
    hx = [[colours[c] or "000000" for c in r] for r in rows]
    cand = {(x, y) for y, r in enumerate(rows) for x, c in enumerate(r) if c != "."}
    return hx, cand


book = ["dddddddddd",
        "d#pppppp#d",       # its back edge in the desk's beige, between its corners
        "d#bbbbbbpd",       # its page edge open on the right
        "d#bbbbbbpd",
        "d########d",
        "dddddddddd"]
hx, cand = grid(book)
desk_px = {(x, y) for (x, y) in cand if hx[y][x] in ("fff67b", "bdbda4")}
kept = g.outline_trim(cand, hx, under=desk_px)
check("trim: the desk round a book is the desk's",
      not any((x, y) in kept for (x, y) in cand if book[y][x] == "d"))
check("trim: its back edge between its corners stays",
      all((x, 1) in kept for x in range(2, 8)))
check("trim: its page edge open on one side stays", (8, 2) in kept and (8, 3) in kept)
pages = ["wwwwwwww",        # an open book on a white desk, its outline mid-grey
         "wIIIIIIw",
         "wIwwmwIw",
         "wIwwmwIw",
         "wIIIIIIw",
         "wwwwwwww"]
hx, cand = grid(pages)
kept = g.outline_trim(cand, hx, under={(x, y) for (x, y) in cand if pages[y][x] in "wm"})
check("trim: a book as white as its desk keeps its pages",
      all((x, y) in kept for x in range(2, 6) for y in (2, 3)))
check("trim: and gives the desk round it back",
      not any((x, y) in kept for (x, y) in cand if x in (0, 7) or y in (0, 5)))
crate = ["gggggg.",          # its top edge, grey as its shadow
         "#yyyy#.",
         "#yyyy#g",          # its shadow down its side
         "######g"]
hx, cand = grid(crate)
kept = g.outline_trim(cand, hx)
check("trim: a box's shadow beside it is the floor's", (6, 2) not in kept and (6, 3) not in kept)
check("trim: its top edge above its outline stays", all((x, 0) in kept for x in range(6)))


# ── support_left: what of a stand's shape is its top ────────────────────

# a stand of green slats with three pots in a row on it, each its orange
# down one side and its dark the rest: the pots in front of the back one
# are not the stand's top, nor their orange the stand's colour
SW, SH = 8, 40
pots = [[[x < SW and y0 <= y < y0 + 10 for x in range(SW)] for y in range(SH)]
        for y0 in (2, 15, 28)]
hx = [["9cd59c"] * SW for _ in range(SH)]
for pg in pots:
    for y in range(SH):
        for x in range(SW):
            if pg[y][x]:
                hx[y][x] = "e6a44a" if x < 3 else "414a6a"
stand = [[True] * SW for _ in range(SH)]
grids = pots + [stand]
alone = g.support_left(0, 3, grids, hx, SW, SH)
check("support: the pots in front are the stand's (no sibs)", len(alone) == 30, len(alone))
check("support: pots in a row on a stand keep their colours",
      not g.support_left(0, 3, grids, hx, SW, SH, sibs=[1, 2]))
# and a pot standing on it as a card: the room beside the stand has a
# counter drawn on its floor in the pot's orange, which is not round the pot
RW = 16
room_hx = [row + ["e6a44a"] * (RW - SW) for row in hx]
card_grids = [[row + [False] * (RW - SW) for row in pots[0]],
              [[x < SW for x in range(RW)] for _ in range(SH)]]
cards = [{"name": "plant", "card": True, "base": 8, "height": 10}, {"name": "stand", "height": 8}]
check("support: a card on the floor leaves the room's orange",
      "e6a44a" in g.auto_leave(cards, card_grids, None, room_hx, {}, RW, SH)[0])
check("support: a card on a stand keeps its pot's orange",
      not g.auto_leave(cards, card_grids, None, room_hx, {0: 1}, RW, SH)[0])


# ── what the drawing shows no depth of, and what is seen through ────────

board = Image.new("RGBA", (16, 16), CLEAR)
bpx = board.load()
for y in range(4, 12):
    for x in range(16):
        bpx[x, y] = OUTLINE if y == 4 else WOOD
check("flat: a board and the one row of its top edge", g.is_flat(board, 7))
check("flat: not a box drawn two rows deep", not g.is_flat(board, 6))
frame = Image.new("RGBA", (16, 16), CLEAR)
fpx = frame.load()
for y in range(4, 12):
    for x in range(16):
        if x < 3 or x > 12 or y < 7:
            fpx[x, y] = GREY                    # a bed's head: rails round a hole
relief = vb.Relief("t", frame, 7, vb.Tile(0, 0, 16, 7), foot=12, solid=True, back=10,
                   see_through=True)
model = vb.Model("t", frame, [relief], (1, 1), 0)
backs = [p for (tri, _, tag) in model.mesh.tris if ".back" in tag for p in tri]
check("see through: its back is its front again, hole and all",
      backs and all(abs(p[4] - (12 - p[1])) < 1e-6 and abs(p[3] - p[0]) < 1e-6 for p in backs))

# ── a dome: round, its drawing projected, exact at 45 degrees ───────────

dome = Image.new("RGBA", (32, 32), CLEAR)
dpx = dome.load()
for y in range(32):
    for x in range(32):
        if ((x + 0.5 - 16) / 8.5) ** 2 + ((y + 0.5 - 12) / 7.5) ** 2 <= 1.0:
            dpx[x, y] = RED if y < 16 else HEM
cx, cz, rx, rz, crown, rim = g.dome_fit(dome, 5, 10)
_, res = exact(dome, dome, [vb.ProjDome("t", cx, cz, rx, rz, crown, y0=rim, seg=16, rings=4)],
               (2, 2))
check("dome: on its support's top, its crown over its drawing", rim == 10 and crown >= 5)
check("dome: its drawing again at 45 degrees", res == (0, 0, 0), res)

# ── an escalator built from the side: its slopes, exact at 45 degrees ───
# a balustrade 7 tall climbing half a pixel a pixel west of x 16, at z 18
# to 20, and the steps behind it to z 30, climbing with it


def rail_top(x):
    return 7 + max(0.0, (16 - x) / 2.0)


esc = Image.new("RGBA", (32, 32), CLEAR)
epx = esc.load()
for y in range(32):
    for x in range(32):
        if 18 - rail_top(x + 0.5) < y + 0.5 < 30:
            epx[x, y] = RED if y < 20 else GREY
rail = [(0, 0), (32, 0), (32, 7), (16, 7), (0, 15)]
steps = [(0, 0), (32, 0), (32, 0.5), (16, 0.5), (0, 8.5)]
model, res = exact(esc, esc, [vb.ProjRun("rail", rail, 18, 20),
                              vb.ProjRun("steps", steps, 20, 30)], (2, 2))
check("escalator: its slope and balustrade, its drawing again", res == (0, 0, 0), res)
check("escalator: no face on the floor",
      not any(all(abs(p[1]) < 1e-9 for p in tri) for (tri, _, _) in model.mesh.tris))
ends = [tag for (_, _, tag) in model.mesh.tris if ".end" in tag]
check("escalator: its ends hidden at 45 degrees", ends and all("~behind" in t for t in ends))

# ── a counter modelled whole: a U on its plan, its front corners cut ─────
# arms back to z 6.25, the bar's back at z 16.25, the front at z 22.25, 4
# tall; quarter pixels off the pixels' centres, so that the drawing is
# the plan's 45-degree view without a tie on any edge


def counter_plan():
    return [(4.0, 22.25), (28.0, 22.25), (29.75, 20.5), (29.75, 6.25), (23.75, 6.25),
            (23.75, 16.25), (8.25, 16.25), (8.25, 6.25), (2.25, 6.25), (2.25, 20.5)]


def in_plan(plan, x, z):
    hit = False
    for i in range(len(plan)):
        (xa, za), (xb, zb) = plan[i], plan[(i + 1) % len(plan)]
        if (za > z) != (zb > z) and x < xa + (xb - xa) * (z - za) / (zb - za):
            hit = not hit
    return hit


bar = Image.new("RGBA", (32, 32), CLEAR)
bpx = bar.load()
for v in range(32):
    for u in range(32):
        # what of the counter the view through this pixel's centre meets
        if any(in_plan(counter_plan(), u + 0.5, v + 0.5 + i / 50.0) for i in range(201)):
            bpx[u, v] = RED if v < 18 else GREY
model, res = exact(bar, bar, [vb.PlanPrism("t", counter_plan(), 0, 4, (12, 16, 22))], (2, 2))
check("counter in plan: its drawing again at 45 degrees", res == (0, 0, 0), res)
tops = [tri for (tri, _, tag) in model.mesh.tris if all(abs(p[1] - 4) < 1e-9 for p in tri)]
check("counter in plan: the yard between its arms open",
      tops and not any(8.25 < sum(p[0] for p in t) / 3 < 23.75 and sum(p[2] for p in t) / 3 < 16.25
                       for t in tops))
square = vb.Model("t", bar, [vb.PlanPrism("t", [(2, 2), (30, 2), (30, 20), (2, 20)], 0, 4,
                                          (12, 16, 22))], (2, 2), 0)
lid = [tri for (tri, _, tag) in square.mesh.tris if all(abs(p[1] - 4) < 1e-9 for p in tri)]
check("counter in plan: a convex top fanned as it was",
      len(lid) == 2 and all(any(p[0] == 2 and p[2] == 2 for p in t) for t in lid))
slant = [tri for (tri, _, tag) in model.mesh.tris
         if tag.endswith("~proj") and len({round(p[0] + p[2], 6) for p in tri}) == 1]
check("counter in plan: a cut corner one slanting face", len(slant) == 2, len(slant))

# ── a ball: round, as its circle at 45 degrees, on its support ──────────

ball = Image.new("RGBA", (32, 32), CLEAR)
lpx = ball.load()
for y in range(32):
    for x in range(32):
        if (x + 0.5 - 16) ** 2 + (y + 0.5 - 12) ** 2 <= 36:
            lpx[x, y] = RED if y < 12 else (OUTLINE if y == 17 else WOOD)
cx, cz, rx, rz, profile = g.ball_fit(ball, 10)
model, res = exact(ball, ball, [vb.ProjLathe("t", cx, cz, profile, squash=rz / rx, seg=16)], (2, 2))
check("ball: its drawing again at 45 degrees", res == (0, 0, 0), res)
ys = [p[1] for (tri, _, _) in model.mesh.tris for p in tri]
check("ball: on its support's top", abs(min(ys) - 10) < 1e-9, min(ys))
check("ball: as deep as it is tall", abs((max(ys) - min(ys)) / 2 - rz) < 1e-9,
      (max(ys) - min(ys), rz))
check("ball: as wide as its circle, its 45-degree view as tall",
      6 <= rx < 6.6 and abs(rz * 2 ** 0.5 - (rx - 0.0)) < 0.7, (rx, rz))

# ── a jar: turned, its belly and neck as drawn (lathe_fit) ──────────────
# Mr. Briney's jars: an open mouth on a neck, a wide belly, its foot
# drawn narrower; 14 wide and 14 tall, standing on the floor

JAR = [".....#OOOO#.....",
       "....#O####O#....",
       "...#O######O#...",
       "...#O######O#...",
       "...#WO####OW#...",
       "..#OOWOOOOWWW#..",
       ".#OHHOWWWWWWWW#.",
       ".#OHHOOWWWWWWW#.",
       ".#OOOOOWWWWWWW#.",
       ".#WOOOWWWWWWWW#.",
       ".#RWWRRWWRRWWR#.",
       "..#RRRRRRRRRR#..",
       "..#RRRRRRRRRR#..",
       "...##RRRRRR##..."]
jar = Image.new("RGBA", (16, 16), CLEAR)
jpx = jar.load()
for y, row in enumerate(JAR):
    for x, c in enumerate(row):
        if c != ".":
            jpx[x, y + 1] = {"#": OUTLINE, "O": WOOD, "W": CLOTH, "H": HEM, "R": RED}[c]
cx, cz, profile = g.lathe_fit(jar, 7, 0)
model, res = exact(jar, jar, [vb.ProjLathe("t", cx, cz, profile, squash=0.7, seg=16)], (1, 1))
check("jar: its drawing again at 45 degrees", res == (0, 0, 0), res)
ys = [p[1] for (tri, _, _) in model.mesh.tris for p in tri]
check("jar: on the floor, as tall as told", min(ys) == 0 and abs(max(ys) - 7) < 1e-9,
      (min(ys), max(ys)))
radii = [r for (_, r) in profile]
check("jar: its belly wider than its foot and its mouth",
      max(radii) == max(radii[1:3]) and radii[1] > radii[-1] + 1, radii)
hull = g.ViewHull()
vb.ProjLathe("t", cx, cz, profile, squash=0.7, seg=16).emit(hull)
own = {(x, y) for y in range(16) for x in range(16) if jpx[x, y][3]}
over = sum(1 for y in range(16) for x in range(16)
           if (x, y) not in own and hull.covers(x + 0.5, y + 0.5))
check("jar: no wider than its drawing but a few pixels (no ball's shoulders)", over <= 8, over)

# ── a card lifted onto a stand (Card `base`): its foot as drawn ─────────

pot = Image.new("RGBA", (16, 16), CLEAR)
ppx = pot.load()
for y in range(4, 14):
    for x in range(4, 12):
        ppx[x, y] = OUTLINE if x in (4, 11) or y == 13 else (CLOTH if y < 8 else WOOD)
model, res = exact(pot, pot, [vb.Card("t", pot, 14, base=8)], (1, 1))
check("lifted card: its drawing again at 45 degrees", res == (0, 0, 0), res)
ys = [p[1] for (tri, _, _) in model.mesh.tris for p in tri]
zs = {p[2] for (tri, _, _) in model.mesh.tris for p in tri}
check("lifted card: standing on the stand's top, as far back", min(ys) == 7 and zs == {22.0},
      (min(ys), zs))

# ── what a ball hides of a counter's arm: the arm's own column ──────────
# the arm: its outline at x 0 and 15, its rim at 1 and 14, its top between;
# the ball (piece 0) over rows 4 to 9, from rim to rim

arm = Image.new("RGBA", (16, 14), CLOTH)
apx = arm.load()
for y in range(14):
    for x in (0, 15):
        apx[x, y] = OUTLINE
    for x in (1, 14):
        apx[x, y] = HEM
owner = [[0 if 4 <= y < 10 and 1 <= x < 15 else 1 for x in range(16)] for y in range(14)]
column = g.fill_hidden({"fill": "column", "shape": [(0, 0, 16, 14)]}, 1, owner, apx, {1})
check("column fill: under the ball its top, rim to rim",
      all(column[(x, y)] == (HEM if x in (1, 14) else CLOTH)
          for y in range(4, 10) for x in range(1, 15)))
row = g.fill_hidden({"fill": 1, "shape": [(0, 0, 16, 14)]}, 1, owner, apx, {1})
check("column fill: along its row it would be the arm's outline",
      row[(1, 5)] == OUTLINE and row[(14, 5)] == OUTLINE)

# ── a stool in blocks (voxel_building_specs.center_stool): its cushion's
# back corners cut, on its base, on its foot; the drawing what the view
# through each pixel's centre meets first, from the top down - a block's
# top or its front

import voxel_building_specs as specs  # noqa: E402

blocks = specs.center_stool(0, 0)
paint = {"cushion": (CLOTH, HEM), "base": (WOOD, GREY), "foot": (SHADOW, OUTLINE)}
stool = Image.new("RGBA", (16, 18), CLEAR)
spx = stool.load()
for v in range(18):
    for u in range(16):
        for i in range(601):
            y = 6 - i / 100.0
            hit = next((b for b in blocks
                        if b[2] <= y <= b[3] and in_plan(b[1], u + 0.5, v + 0.5 + y)), None)
            if hit:
                spx[u, v] = paint[hit[0]][0 if y > hit[3] - 0.005 else 1]
                break
model, res = exact(stool, stool, [vb.PlanPrism(name, plan, y0, y1, side)
                                  for (name, plan, y0, y1, side) in blocks], (1, 2))
check("stool in blocks: its drawing again at 45 degrees", res == (0, 0, 0), res)
backs = collections.Counter(tag.split(".")[0] for (_, _, tag) in model.mesh.tris
                            if tag.endswith(".side~behind"))
check("stool in blocks: a hidden side a quad, its strip wide",
      backs == {"cushion": 10, "base": 12, "foot": 6}, dict(backs))

with tempfile.TemporaryDirectory() as folder:
    diagnostic = pathlib.Path(folder) / "stool.png"
    judged = types.SimpleNamespace(art=stool, mesh=model.mesh)
    counts = vb.ortho_check(judged, diagnostic, diagnostic_limit=0)
    check("proof image: no file on a pass", counts == (0, 0, 0) and not diagnostic.exists())
    changed = stool.copy()
    changed.putpixel(min(own_of(stool)), (0, 0, 0, 255))
    counts = vb.ortho_check(judged, diagnostic, reference=changed, diagnostic_limit=1)
    check("proof image: allowed mismatch skips file",
          counts == (1, 0, 0) and not diagnostic.exists())
    counts = vb.ortho_check(judged, diagnostic, reference=changed, diagnostic_limit=0)
    full = pathlib.Path(folder) / "full.png"
    vb.ortho_check(judged, full, reference=changed)
    check("proof image: same file on a failure",
          counts == (1, 0, 0) and diagnostic.exists() and diagnostic.read_bytes() == full.read_bytes())


if FAILED:
    raise SystemExit("%d interior parts case(s) failed: %s" % (len(FAILED), ", ".join(FAILED)))
print("PASS interior parts: casings, tops on legs, flat against walls, cut by their outlines,"
      " seen through, round, built from the side, modelled whole")
