#!/usr/bin/env python3
"""Building models, one reading of the drawing per building type.

Each spec names where a reference copy of the building sits (layout and cell
rectangle), which metatiles are the ground around it, the parts that rebuild
it, and the art rectangles the model must reproduce pixel for pixel when it
is rendered in the GBA's projection. Row numbers are art rows of that
rectangle.

Littleroot's houses
-------------------
The two houses are the same building. The drawing, from the top:

    rows  1- 8  upper ridge cap       rows 17-24 lower ridge cap  (the "ears")
    rows  9-15  upper ridge teeth     rows 25-31 lower ridge teeth
    rows 16-35  upper roof, 5 courses rows 32-51 lower roof, 5 courses
    rows 36-37  upper fascia          rows 52-53 lower fascia
    rows 38-44  upper storey wall     rows 54-79 ground floor facade

The ears are the same ridge and the same courses as the upper roof, sixteen
rows lower: a full-width lower roof, and a narrower upper storey that comes up
through it with its own roof.

The drawing is shallower than the house. Taken literally (every band at the
depth the 45-degree projection gives it) the house would be two tiles deep,
while it stands on four rows of collision. So the model keeps the real
footprint, and the depth the drawing does not have is covered the way a roof
covers it - with more courses of the same tiles (Strip), never with stretched
ones. The facade, the pent roof in front of the upper storey, the upper wall
and the upper roof's first four courses stay exactly the drawing; only the
roofs grow, by the courses the depth needs.

Both roofs are hipped: seen from the side they are tiles too, laid along the
side eave, so no gable wall of plaster ever shows.
"""

import math
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voxel_fortree  # noqa: E402

from voxel_building import (Band, ProjCanopy, ProjDome, ProjHull, ProjLathe, ProjSection, BakedLathe, VoxelLathe, Card, Grown, PlanHip, PlanPrism, PlanPyramid, PlatePrism, SectionPrism, Cylinder, Frustum, HeightSolid, HipRoof, Mound, PlainWall, Prism, Proj,
                            Strip, Tile, Vault, Volume, Walls, clip, own_bands, _unit, strip_face,
                            LayoutArt, anim_frame,
                            SHADE_ART, SHADE_BACK, SHADE_EAST, SHADE_WEST)

GRASS = 0x001

PITCH = 22.0        # degrees, front and back slopes
ROOF_COLUMNS = (12, 68)   # courses sampled away from the drawn verges


def littleroot_house(plaster_x):
    """Both houses. They share every row and every edge; only the middle of
    the facade is rearranged, so each names the 8-px column of plain plaster
    (no window, no door, no post) its walls are dressed from."""
    px0, px1 = plaster_x, plaster_x + 8
    front_z, back_z = 80, 16    # the four collision rows, 5..8
    overhang = 2

    lower = HipRoof(
        "roof_lo", 0, 82, zf=front_z + overhang, zb=back_z - overhang, y0=28,
        fascia=Strip((52, 54), wrap=ROOF_COLUMNS),
        # the seven rows drawn in front of the upper storey, then courses
        # continuing in phase (row 45 sits one above a course's first row)
        slope=Strip((45, 52), repeat=(16, 20), start=16, wrap=ROOF_COLUMNS),
        teeth=(25, 32), cap=(17, 25), pitch=PITCH, run=7,
        ridge_u=(0, 80), end_tile=Tile(0, 25, 8, 32))

    # The upper storey stands on the pent roof where its seventh row ends.
    z_wall, y_base = lower.slope_point(7)
    zf_hi = z_wall + overhang
    y0_hi = zf_hi - 38          # the upper fascia is drawn on rows 36-37
    zb_hi = (lower.zf + lower.zb) - zf_hi
    upper = HipRoof(
        "roof_hi", 8, 72, zf=zf_hi, zb=zb_hi, y0=y0_hi,
        fascia=Strip((36, 38), wrap=ROOF_COLUMNS),
        slope=Strip((32, 36), repeat=(16, 32), wrap=ROOF_COLUMNS),
        teeth=(9, 16), cap=(1, 9), pitch=PITCH, run=6,
        ridge_u=(8, 72), end_tile=Tile(12, 9, 20, 16))

    storey_back = zb_hi + overhang
    post = Tile(10, 38, 14, 45, top=y0_hi)
    storey = Prism(
        "storey", 10, 70,
        [(z_wall, 28), (z_wall, y0_hi), (storey_back, y0_hi), (storey_back, 28)],
        edges={0: Proj(38, 45), 2: Tile(20, 38, 28, 45, top=y0_hi)}, skip=(1, 3),
        caps=[Band(-64, 200, Tile(20, 38, 28, 45, top=y0_hi), z_wall,
                   front=post, back=post, z1=storey_back)])

    wall_top = 28
    corner = Tile(2, 54, 8, 80, top=26)
    base = Prism(
        "ground_floor", 2, 80,
        [(front_z, 0), (front_z, wall_top), (back_z, wall_top), (back_z, 0)],
        edges={0: Proj(54, 80), 2: Tile(px0, 54, px1, 80, top=26)}, skip=(1, 3),
        caps=[Band(0, 26, Tile(px0, 54, px1, 80, top=26), front_z,
                   front=corner, back=corner, z1=back_z),
              Band(26, wall_top + 1, Tile(px0, 54, px1, 55, top=wall_top), front_z)])
    return [base, lower, storey, upper]


# What the model must reproduce exactly: the ground floor, the pent roof in
# front of the upper storey, the upper storey, and the upper roof's fascia and
# first four courses - everything the drawing shows at its own depth.
HOUSE_EXACT = [(2, 54, 80, 80), (12, 45, 68, 54), (10, 38, 70, 45), (12, 16, 68, 38)]

def littleroot_lab():
    """Professor Birch's lab, cells 3..9 x 12..16 of Littleroot.

    The drawing, from the top (112 x 80):

        rows  1- 9  flat top of the roof, light panels, period 8 across
        rows 10-14  its front edge
        rows 15-49  olive tile courses, period 4 down and 8 across
        rows 50-52  fascia
        rows 53-79  facade: posts, two windows, the door

    and on the roof, x 16-47 rows 2-31, a ventilator: a box whose front is
    rows 25-31 and top rows 15-24, carrying a round cowl drawn as a circle
    (rows 2-24). The box stands on the slope exactly where its front row 31
    meets the courses, so its front is the drawing at 45 degrees.
    """
    front_z, back_z = 80, 16    # collision rows 13..16
    overhang = 2
    lab_columns = (8, 104)      # courses away from both verges (12 x 8)
    roof = HipRoof(
        "roof", 0, 114, zf=front_z + overhang, zb=back_z - overhang, y0=29,
        fascia=Strip((50, 53), wrap=lab_columns),
        slope=Strip((46, 50), repeat=(34, 46), wrap=lab_columns),
        teeth=(10, 15), cap=(1, 10), pitch=18.0, run=6,
        ridge_u=(0, 112), end_tile=Tile(48, 10, 57, 15),
        ridge_wrap=(48, 104))    # the drawn ridge is behind the vent at 17-47

    # Ventilator: box front bottom on the slope where art row 32 lies.
    zbox, ybox = roof.slope_point(50 - 32)
    ytop = zbox - 25
    grey = Tile(20, 25, 40, 32, top=ytop)
    box = Prism(
        "vent_box", 16, 48,
        [(zbox, ybox - 6), (zbox, ytop), (zbox - 10, ytop), (zbox - 10, ybox - 6)],
        edges={0: Proj(25, 32), 2: grey}, skip=(3,),
        caps=[Band(-64, 200, grey, zbox)])
    cowl = Cylinder("vent_cowl", 32, zbox - 10, 12, 9, ytop - 4, ytop + 4,
                    back_tile=Tile(24, 16, 40, 25))

    wall_top = 29
    post = Tile(2, 53, 8, 80, top=27)
    plaster = Tile(8, 53, 16, 80, top=27)
    base = Prism(
        "ground_floor", 2, 112,
        [(front_z, 0), (front_z, wall_top), (back_z, wall_top), (back_z, 0)],
        edges={0: Proj(53, 80), 2: plaster}, skip=(1, 3),
        caps=[Band(0, 27, plaster, front_z, front=post, back=post, z1=back_z),
              Band(27, wall_top + 1, Tile(8, 53, 16, 54, top=wall_top), front_z)])
    return [base, roof, box, cowl]


LAB_EXACT = [(2, 53, 112, 80), (8, 32, 104, 53), (48, 16, 104, 32)]

# The Center's crown, read off its drawn front arch (rows 16-23): the height
# of the arch over each column, 1 px at x 16 and 47 and 8 px from 27 to 36.
# Its back arch is drawn exactly 16 rows higher in every column, so the vault
# is 16 px deep - the one depth the drawing states outright.
CENTER_ARCH = [(16, 0), (16.5, 1), (17.5, 2), (18.5, 3), (19.5, 4), (20.5, 4),
               (21.5, 5), (22.5, 6), (23.5, 6), (24.5, 7), (26.5, 7), (27.5, 8),
               (36.5, 8), (37.5, 7), (39.5, 7), (40.5, 6), (41.5, 6), (42.5, 5),
               (43.5, 4), (44.5, 4), (45.5, 3), (46.5, 2), (47.5, 1), (48, 0)]


def center_or_mart(rib_repeat, crown=False):
    """The Pokemon Center and the Poke Mart: one building, two paint jobs.

    The drawing (64 x 64), from the top:

        rows  2- 8  the Center's raised crown (the Mart has none)
        rows  9-23  the flat top, ribbed front to back
        rows 24-37  the roof band with its emblem, drawn round
        rows 38-63  walls: fascia, glass, the door, the sign, the plinth

    The plan is an octagon: the front runs x 8-56, and 8-px chamfers turn the
    corners. The drawing says so itself - the plinth runs diagonally from
    (8, 63) to (0, 56) and the eave from (8, 38) to (0, 30), which is what a
    45-degree chamfer looks like at 45 degrees. The emblem is drawn round, so
    the band faces the camera square on: it pitches at 45 degrees, rising 7.

    Real depth: three collision rows, Z 16-64. The top grows backwards by rows
    of its own ribs, which run front to back and so repeat without a seam.
    """
    front, back = 64, 16
    # the front chamfers a pixel out: the plinth's outline is drawn on the
    # diagonal's outer side, and on it stood on the ground
    plan = [(7, front), (57, front), (64, front - 7), (64, back + 8),
            (56, back), (8, back), (0, back + 8), (0, front - 7)]
    parts = [Frustum(
        "body", plan, wall_top=26,
        wall_side=Strip((38, 64), wrap=(8, 16)),
        band_rise=7,
        band_side=Strip((24, 38), wrap=(8, 16)),
        top=Strip((9, 24), repeat=rib_repeat, wrap=(8, 56)))]
    if crown:
        # on the flat top (y 33), from its front edge (z 57) back 16
        parts.append(Vault("crown", CENTER_ARCH, zf=57, zb=41, y0=33))
    return parts


# The walls, the chamfers, the band and the top as drawn; the corners above
# the chamfers are the band's sides, which the real depth moves.
CENTER_EXACT = [(8, 30, 56, 64), (0, 38, 64, 64), (12, 9, 52, 30)]
# The Center adds its crown, whole: both arches and the ribs between them.
CROWN_EXACT = CENTER_EXACT + [(16, 0, 48, 24, True)]

def oldale_house():
    """Oldale's two houses, cells 4..7 x 4..7 and 14..17 x 13..16.

    The drawing (64 x 64), from the top:

        rows  0- 9  the ridge's top, notched along its back edge
        rows 10-13  its front
        rows 14-33  five courses of tiles, period 4 down and 8 across
        rows 34-35  fascia
        rows 36-63  facade: posts, the window, the door

    One storey under a hipped roof, the same reading as Littleroot's lower
    roof. Real depth: the three collision rows, Z 16-64.
    """
    front_z, back_z = 64, 16
    overhang = 2
    columns = (8, 56)           # courses away from both verges (6 x 8)
    roof = HipRoof(
        "roof", 0, 64, zf=front_z + overhang, zb=back_z - overhang, y0=30,
        fascia=Strip((34, 36), wrap=columns),
        slope=Strip((30, 34), repeat=(14, 30), wrap=columns),
        teeth=(10, 14), cap=(0, 10), pitch=22.0, run=6,
        ridge_u=(0, 64), end_tile=Tile(8, 10, 18, 14))

    wall_top = 30
    post = Tile(2, 36, 9, 64, top=28)
    plaster = Tile(9, 36, 15, 64, top=28)
    base = Prism(
        "ground_floor", 2, 64,
        [(front_z, 0), (front_z, wall_top), (back_z, wall_top), (back_z, 0)],
        edges={0: Proj(36, 64), 2: plaster}, skip=(1, 3),
        caps=[Band(0, 28, plaster, front_z, front=post, back=post, z1=back_z),
              Band(28, wall_top + 1, Tile(9, 36, 15, 37, top=wall_top), front_z)])
    return [base, roof]


OLDALE_HOUSE_EXACT = [(2, 36, 64, 64), (8, 14, 56, 36)]


def briney_house():
    """Mr Briney's cottage on Route 104, cells 15..19 x 47..50 (80 x 64).

    The drawing, from the top:

        rows  0- 8  the ridge's top: thatch bundles, period 8 across
        rows  9-15  its front: a batten and a short course between dark lines
        rows 16-35  the thatch: period 8 down (rows 16-23 = 24-31) and across,
                    its eave course fringed at rows 32-35
        rows 36-38  fascia
        rows 39-47  the lattice under the eave (x 0-80)
        rows 48-63  the ground floor (x 1-80): posts, reed walls, the door

    One storey under a hipped roof, like Oldale's houses. Real depth: the
    three collision rows, Z 16-64. The verges (x 0-7 and 72-79) are drawn
    as edge bundles, so the courses are sampled from x 8-72.
    """
    front_z, back_z = 64, 16
    overhang = 2
    columns = (8, 72)
    roof = HipRoof(
        "roof", 0, 80, zf=front_z + overhang, zb=back_z - overhang, y0=27,
        fascia=Strip((36, 39), wrap=columns),
        slope=Strip((16, 36), repeat=(16, 24), wrap=columns),
        teeth=(9, 16), cap=(0, 9), pitch=22.0, run=6,
        ridge_u=(0, 80), end_tile=Tile(8, 9, 18, 16))

    floor_top, wall_top = 16, 27
    post = Tile(1, 48, 8, 64, top=floor_top)
    reed = Tile(8, 48, 18, 64, top=floor_top)
    lattice = Tile(8, 39, 16, 48, top=25)
    lattice_end = Tile(0, 39, 3, 48, top=25)
    ground_floor = Prism(
        "ground_floor", 1, 80,
        [(front_z, 0), (front_z, floor_top), (back_z, floor_top), (back_z, 0)],
        edges={0: Proj(48, 64), 2: reed}, skip=(1, 3),
        caps=[Band(0, floor_top, reed, front_z, front=post, back=post, z1=back_z)])
    upper = Prism(
        "lattice", 0, 80,
        [(front_z, floor_top), (front_z, wall_top), (back_z, wall_top), (back_z, floor_top)],
        edges={0: Proj(39, 48), 2: lattice}, skip=(1, 3),
        caps=[Band(floor_top, wall_top + 1, lattice, front_z, front=lattice_end,
                   back=lattice_end, z1=back_z)])
    return [ground_floor, upper, roof]


BRINEY_HOUSE_EXACT = [(1, 39, 80, 64), (0, 39, 1, 48), (8, 16, 72, 39)]


def flower_shop():
    """The Pretty Petal flower shop on Route 104, cells 3..8 x 15..18 (96 x 64).

    A flat roof of corrugated sheet, seen from above as the GBA sees every
    flat top. The drawing, from the top:

        rows  0- 3  the roof's back edge: an outline and three light rows
        rows  4-27  the sheet, every row the same (ribs period 4 across)
        rows 28-37  the roof slab's red front and the dark line under it
        rows 38-63  the facade (x 1-95): posts, the awning, windows, the door

    Real depth: the three collision rows, Z 16-64. The sheet is laid back
    to them with more of its own rows, the back edge closing it.
    """
    front, back = 64, 16
    wall = 64 - 38
    top = wall + (38 - 28)
    post = Tile(1, 38, 6, 64, top=wall)
    siding = Tile(72, 40, 80, 48, top=wall - 2)
    rim = Tile(9, 28, 17, 38, top=top)
    body = Prism(
        "ground_floor", 1, 95,
        [(front, 0), (front, wall), (back, wall), (back, 0)],
        edges={0: Proj(38, 64), 2: siding}, skip=(1, 3),
        caps=[Band(0, wall, siding, front, front=post, back=post, z1=back)])
    roof = Prism(
        "roof", 0, 96,
        [(front, wall), (front, top), (back, top), (back, wall)],
        edges={0: Proj(28, 38), 1: Strip((4, 28), repeat=(20, 28), tail=(0, 4)), 2: rim},
        skip=(3,), caps=[Band(wall, top + 1, rim, front)])
    return [body, roof]


FLOWER_SHOP_EXACT = [(0, 4, 96, 38), (1, 38, 95, 64)]

def kit_house(width):
    """The General tileset's red-roofed house, any width (in pixels).

    Built from a kit - roof caps, repeated middles, a facade of posts, windows
    and a door - so the same drawing comes 4 and 5 cells wide. The rows are
    the kit's, whatever the width (64 high):

        rows  0- 9  the ridge's top, period 8 across
        rows 10-15  its front
        rows 16-34  scale tiles: two 8-row courses and the eave's three rows
        rows 35-37  fascia
        rows 38-63  facade

    Real depth: three collision rows, Z 16-64. Courses are sampled between
    the drawn verges (x 0-2 and the last three columns), keeping the phase.
    """
    front_z, back_z = 64, 16
    overhang = 2
    columns = (8, width - 8)
    roof = HipRoof(
        "roof", 0, width, zf=front_z + overhang, zb=back_z - overhang, y0=28,
        fascia=Strip((35, 38), wrap=columns),
        slope=Strip((32, 35), repeat=(16, 32), wrap=columns),
        teeth=(10, 16), cap=(0, 10), pitch=22.0, run=6,
        ridge_u=(0, width), end_tile=Tile(8, 10, 18, 16))
    wall_top = 28
    post = Tile(2, 38, 8, 64, top=26)
    boards = Tile(9, 38, 15, 64, top=26)
    base = Prism(
        "ground_floor", 2, width - 2,
        [(front_z, 0), (front_z, wall_top), (back_z, wall_top), (back_z, 0)],
        edges={0: Proj(38, 64), 2: boards}, skip=(1, 3),
        caps=[Band(0, 26, boards, front_z, front=post, back=post, z1=back_z),
              Band(26, wall_top + 1, Tile(9, 38, 15, 39, top=wall_top), front_z)])
    return [base, roof]


def kit_house_exact(width):
    return [(2, 38, width - 2, 64), (8, 16, width - 8, 38)]


def kit_house_keep(width):
    """The kit house's own pixels: its roof, and of its facade's rows what its
    ground floor stands on (x 2 to width - 2). The lawn either side and its
    shadow on the ground are the map's, laid under it (patch_own): as its
    own, nothing drew them and the ground round it showed there (the strip
    of paving between Slateport's two houses)."""
    return [(0, 0, width, 38), (2, 38, width - 2, 64)]


def dewford_house(width):
    """Dewford's blue tiled houses and Hall, with their own facade art.

    Three collision rows give Z 16..64. The ridge cap occupies rows 0..8,
    its front 9..15, the roof courses 16..34, fascia 35..37 and facade
    38..63. Clean ridge columns exclude the trees painted into the Hall's
    northern edge; the map's trees remain separate objects.
    """
    front, back, wall = 64, 16, 28
    columns = (8, width - 8)
    roof = HipRoof(
        "roof", 0, width, zf=front + 2, zb=back - 2, y0=wall,
        fascia=Strip((35, 38), wrap=columns),
        slope=Strip((32, 35), repeat=(16, 32), wrap=columns),
        teeth=(9, 16), cap=(0, 9), pitch=PITCH, run=6,
        ridge_u=(0, width), end_tile=Tile(16, 9, 25, 16),
        ridge_wrap=(16, width - 8))
    plaster = Tile(9, 38, 15, 64, top=26)
    post = Tile(2, 38, 8, 64, top=26)
    body = Prism(
        "ground_floor", 2, width - 2,
        [(front, 0), (front, wall), (back, wall), (back, 0)],
        edges={0: Proj(38, 64), 2: plaster}, skip=(1, 3),
        caps=[Band(0, 26, plaster, front, front=post, back=post, z1=back),
              Band(26, wall + 1, Tile(9, 38, 15, 39, top=wall), front)])
    return [body, roof]


def gym():
    """The gym of Petalburg, Mauville, Mossdeep and Lavaridge (96 x 80).

        rows  1-40  a flat gravel roof, period 2 down and across
        rows 41-44  its edge: a light lip and the fascia
        rows 45-71  the facade either side of the porch
        rows 41-48  (x 40-71) the roof running on over the porch
        rows 49-53  the porch's own fascia, eight rows lower: it stands out 8
        rows 54-79  the porch: doors square on, and 8-px chamfers either side,
                    whose plinth runs diagonally from (40, 71) to (48, 79)

    Real depth: the four collision rows reach the top of the drawing, so the
    back wall stands at Z 2 and the gravel repeats back to it.
    """
    front, porch, back = 72, 80, 2
    wall_top, roof_top = 27, 31
    panel = Tile(72, 45, 88, 72, top=wall_top)      # a window bay, for sides
    corner = Tile(88, 45, 94, 72, top=wall_top)
    lip = Tile(8, 41, 16, 45, top=roof_top)
    body = Prism(
        "body", 2, 94,
        [(front, -1), (front, wall_top), (back, wall_top), (back, -1)],
        edges={0: Proj(45, 72), 2: panel}, skip=(1, 3),
        caps=[Band(-1, wall_top, panel, front, front=corner, back=corner, z1=back)])
    roof = Prism(
        "roof", 0, 96,
        [(front, wall_top), (front, roof_top), (back, roof_top), (back, wall_top)],
        edges={0: Proj(41, 45), 1: Strip((5, 41), repeat=(5, 7)), 2: lip},
        skip=(3,), caps=[Band(wall_top, roof_top + 1, lip, front)])
    porch_roof = Prism(
        "porch_roof", 40, 72,
        [(porch, wall_top), (porch, roof_top), (front, roof_top), (front, wall_top)],
        edges={0: Proj(49, 54)}, skip=(2, 3),
        caps=[Band(wall_top, roof_top + 1, lip, porch)])
    porch_walls = Walls("porch", [(40, front), (48, porch), (64, porch), (72, front)],
                        -1, wall_top)
    return [body, roof, porch_roof, porch_walls]


GYM_EXACT = [(2, 45, 40, 72), (72, 45, 94, 72), (3, 5, 93, 45), (40, 41, 72, 80)]

def flat_block(width, height, roof, cornice, facade_top, unit=None, brick=None, rim=None,
               x0=0, x1=None, pilaster=None):
    """A flat-roofed block: parapeted roof, cornice, facade (any size).

    `roof` = (fixed, repeat, tail): the roof's rows as the drawing lays them
    from its front rim back, the course that repeats to the real depth, and
    the back parapet that closes it. `cornice` = (top, bottom) rows of the
    parapet's front; the facade runs from `facade_top` to the bottom. `unit`
    = (x0, x1, top, face, foot): a box on the roof whose top is drawn from
    row `top`, whose front from `face` to its foot on the roof at `foot`.

    Real depth: the collision starts one row below the drawing's top, so the
    back wall stands at Z 16 and the front at the drawing's foot.

    The sides and the back are dressed with `brick` (u0, v0, u1, v1: a clean
    stretch of the facade, by default the 8 x 16 next to its west corner)
    between the corner posts, and the parapet's sides with `rim` (by default
    the cornice next to the corner): a facade whose first rows there are
    something else (an awning drawn slanting) names its own. `pilaster`
    (u0, v0, u1, v1): the corner post at the sides' ends, by default the
    facade's first 8 columns, 16 rows.

    `x0`, `x1`: the columns the block is drawn over, when its drawing does
    not fill the whole rectangle (the gatehouses start a column in, the
    ground's, and end a column early, their shadow's): a block over the
    clear columns had them as slits up its corners and along its roof.
    """
    x1 = width if x1 is None else x1
    front, back = height, 16
    wall = height - facade_top
    c0, c1 = cornice
    top = wall + (c1 - c0)
    fixed, repeat, tail = roof
    brick = Tile(*(brick or (8, facade_top, 16, facade_top + 16)), top=wall)
    pilaster = Tile(*(pilaster or (x0, facade_top, x0 + 8, facade_top + 16)), top=wall)
    rim = Tile(*(rim or (8, c0, 16, c1)), top=top)
    body = Prism(
        "body", x0, x1,
        [(front, -1), (front, wall), (back, wall), (back, -1)],
        edges={0: Proj(facade_top, height), 2: brick}, skip=(1, 3),
        caps=[Band(-1, wall, brick, front, front=pilaster, back=pilaster, z1=back)])
    parts = [body]

    def slab(name, a, b, offset=0.0):
        return Prism(
            name, a, b,
            [(front, wall), (front, top), (back, top), (back, wall)],
            edges={0: Proj(c0, c1),
                   1: Strip(fixed, repeat=repeat, tail=tail,
                            repeat_offset=offset if offset else None), 2: rim},
            skip=(3,), caps=[Band(wall, top + 1, rim, front)],
            west=(a == x0), east=(b == x1))

    if unit is None:
        parts.append(slab("roof", x0, x1))
    else:
        ux0, ux1, t0, t1, t2 = unit
        # under and behind the unit the roof is laid from plain roof columns
        parts += [slab("roof_w", x0, ux0), slab("roof_u", ux0, ux1, offset=8 - ux0),
                  slab("roof_e", ux1, x1)]
        zu = t2 + top
        side = Tile(ux0 + 8, t1, ux0 + 16, t2, top=top + (t2 - t1))
        parts.append(Prism(
            "unit", ux0, ux1,
            [(zu, top - 1), (zu, top + (t2 - t1)), (zu - (t1 - t0), top + (t2 - t1)),
             (zu - (t1 - t0), top - 1)],
            edges={0: Proj(t1, t2), 1: Proj(t0, t1), 2: side}, skip=(3,),
            caps=[Band(top - 1, top + (t2 - t1) + 1, side, zu)]))
    return parts


def stone_block(width, height, meta):
    """Rustboro's stone blocks (224..21f): the flat roof drawn rows 0-38, a
    course of 4 rows repeating, the back parapet rows 0-6; the parapet's front
    rows 39-47; the facade below, one row of windows per storey."""
    return flat_block(width, height, ((7, 39), (7, 11), (0, 7)), (39, 48), 48)


def olive_block(width, height, meta):
    """Rustboro's olive-roofed blocks (220..243): the roof drawn rows 0-39 in
    8-row courses, the cornice rows 40-47, the facade below. Most carry a
    ventilation unit on the right end of the roof (metatiles 222 223)."""
    unit = (width - 24, width - 4, 1, 16, 31) if meta.get("unit") else None
    return flat_block(width, height, ((8, 40), (8, 16), (0, 8)), (40, 48), 48, unit)


def safari_canopy(z, v):
    """The Safari Zone building's porch roof over its glass door (x 24-56):
    3 out from the wall at `z`, its top drawn the 3 rows above `v`, its
    front rows v..v+4 - 23 high on a facade 33 high."""
    y1 = z + 3 - v
    return Prism(
        "canopy", 24, 56, [(z, y1 - 4), (z + 3, y1 - 4), (z + 3, y1), (z, y1)],
        edges={1: Proj(v, v + 4), 2: Proj(v - 3, v)}, skip=(0, 3),
        caps=[Band(y1 - 4, y1 + 1, Tile(24, v, 28, v + 4, top=y1), z + 3)])


def safari_gate():
    """The Safari Zone's entrance on Route 121 (80 x 96, cells 35..39 x
    0..5), cut by the map's top edge:

        rows  0-56  the flat roof, its pattern 8 rows (from row 2), the
                    parapet's rims down its sides and along its front
                    (rows 53-56); no back parapet: the map ends first
        rows 56-63  the parapet's front
        rows 63-96  the facade: windows, the hedge drawn along its foot,
                    the glass door under a canopy (x 24-56)

    The canopy stands 3 out at the door's top: its top drawn rows 73-76,
    its front 76-80, so it is 23 high at Z 99. The sides take a plain
    stretch of the facade (x 4-8: the dotted plaster, the hedge on period),
    the corners its outline columns."""
    parts = flat_block(80, 96, ((2, 56), (2, 10), None), (56, 63), 63,
                       brick=(4, 63, 8, 96), pilaster=(0, 63, 4, 96))
    return parts + [safari_canopy(96, 76)]


# The same building seen from inside the Safari Zone: an L across the seam
# of Safari South and Southeast, a long wing (South x 30-39 and Southeast x
# 0) whose front is the gate's, 16 rows higher, and an east wing (Southeast
# x 1-5) running south off the map. Both are 40 high, as the gate:
#
#     rows  0- 7  the back parapet over the north wall (collision row 29)
#     rows  7-40  the roof, its pattern 8 rows; the east wing's on down
#     rows 40-47  the long wing's parapet front
#     rows 47-80  its facade (the door's canopy in South, rows 57-64)
#
# Real depth: the walls stand where the collision starts, Z 0, so the roof
# is 40 rows deeper than drawn: its courses repeat back to the parapet
# (`tail`), and the drawn rows 0-7 are not proved. The east wing ends at the
# map's bottom edge, its south face the drawing projected. No wall stands at
# the seam: each map's half is open there.
SAFARI_ROOF = ((7, 40), (7, 15), (0, 7))


def safari_wing(x0, x1, brick, rim, west=True, east=True):
    return flat_part("wing", x0, x1, 80, 0, SAFARI_ROOF, (40, 47), 47, brick, rim,
                     west=west, east=east)


def safari_south():
    """The long wing's 160 columns in Safari South: dressed with the plain
    stretch between its third and fourth windows (x 72-88)."""
    return safari_wing(0, 160, (72, 47, 88, 80), (72, 40, 88, 47), east=False) + \
        [safari_canopy(80, 60)]


def safari_southeast():
    """Southeast's half: the long wing's last 16 columns, and the east wing
    (x 16-96) from the back wall to the map's bottom edge (Z 176), its roof
    the drawing's rows 7-136, its front the rest projected."""
    brick = Tile(8, 47, 16, 80, top=33)
    fixed, repeat, tail = SAFARI_ROOF
    wing = Prism(
        "east_wing", 16, 96, [(176, 0), (176, 40), (0, 40), (0, 0)],
        edges={0: Proj(), 1: Strip((fixed[0], 136), repeat=repeat, tail=tail), 2: brick},
        skip=(3,), caps=[Band(0, 41, brick, 176)])
    return safari_wing(0, 16, (8, 47, 16, 80), (8, 40, 16, 47), west=False, east=False) + [wing]


def flat_block_exact(width, height, first_roof_row):
    return [(0, first_roof_row, width, height)]

def flat_part(name, x0, x1, front, back, roof, cornice, facade_top,
              brick, rim, west=True, east=True):
    """One flat-roofed volume over [x0, x1): facade from `facade_top` to its
    foot at `front`, the parapet's front `cornice` above it, and the roof laid
    back to `back` by `roof` = (fixed, repeat, tail). `brick` and `rim` are
    the art rectangles its sides are dressed with."""
    wall = front - facade_top
    c0, c1 = cornice
    top = wall + (c1 - c0)
    fixed, repeat, tail = roof
    brick_t = Tile(*brick, top=wall)
    rim_t = Tile(*rim, top=top)
    body = Prism(
        name, x0, x1,
        [(front, 0), (front, wall), (back, wall), (back, 0)],
        edges={0: Proj(facade_top, front), 2: brick_t}, skip=(1, 3),
        caps=[Band(0, wall, brick_t, front)], west=west, east=east)
    slab = Prism(
        name + "_roof", x0, x1,
        [(front, wall), (front, top), (back, top), (back, wall)],
        edges={0: Proj(c0, c1), 1: Strip(fixed, repeat=repeat, tail=tail), 2: rim_t},
        skip=(3,), caps=[Band(wall, top + 1, rim_t, front)], west=west, east=east)
    return [body, slab]


def devon():
    """Devon Corporation (160 x 144): two wings and a tower between them.

        wings  x 0-47, 112-159: roof rows 2-31 (a 2-row lattice), parapet
               front rows 32-39, three storeys rows 40-127
        tower  x 48-111: roof rows 0-39 (8-row courses) with the company's
               emblem across its front rim, a double cornice rows 40-55,
               the facade and the arched doors rows 56-143 - it stands one
               row further south than the wings

    Real depth: the collision covers the drawing's top row, so the back wall
    stands at Z 0.
    """
    wing_roof = ((8, 32), (8, 10), (2, 8))
    parts = []
    parts += flat_part("wing_w", 0, 48, 128, 0, wing_roof, (32, 40), 40,
                       brick=(8, 40, 48, 72), rim=(8, 32, 48, 40), east=False)
    parts += flat_part("wing_e", 112, 160, 128, 0, wing_roof, (32, 40), 40,
                       brick=(112, 40, 152, 72), rim=(112, 32, 152, 40), west=False)
    parts += flat_part("tower", 48, 112, 144, 0, ((9, 40), (9, 17), (0, 9)), (40, 56), 56,
                       brick=(48, 56, 56, 88), rim=(56, 40, 64, 56))
    return parts


DEVON_EXACT = [(0, 8, 160, 128), (48, 128, 112, 144)]


# Rustboro's fountain (48 x 48), as its drawing tells it:
#
#   the basin's outer wall, 5 px: on the front rows 41-45 (its top edge,
#       three rows of stone, the outline), the chamfers a band as wide;
#       the plan below is its foot, the outline drawn round it
#   the rim's top at that height, in to the pool's edge: 6-7 px wide at the
#       sides and the front, 3 at the back (rows 2-4)
#   the pool's water 2 px lower: the back's inner wall is rows 5-6, the
#       inner chamfers' the grey line beside the water
#   the dish in the middle, its top 5 px over the water (the dotted rim's
#       ellipse, rows 11-23, x 15-33), its edge 2 px (rows 23-24 in front),
#       on a stem the dish hides; the grey under it, down to row 27, is its
#       shadow on the water
#   the jet standing on the dish, rows 8-16 - the only thing that moves
FOUNTAIN_RECT = (27, 38)
FOUNTAIN_FOOT = [(11, 7), (37, 7), (47, 17), (47, 34), (35, 46), (13, 46), (1, 34), (1, 17)]
FOUNTAIN_POOL = [(15, 10), (33, 10), (40, 17), (40, 32), (32, 40), (16, 40), (8, 32), (8, 17)]
FOUNTAIN_RIM, FOUNTAIN_WATER, FOUNTAIN_DISH = 5, 3, 8
DISH_AT, DISH_R, DISH_EDGE, STEM_R = (24.0, 25.0), (9.0, 6.0), 2, (3.0, 2.2)
# The jet: columns 19-28 of rows 8-16, drawn with Rustboro's secondary tiles
# 448-451, which the game redraws every 8 frames from two frames
# (tileset_anims.c QueueAnimTiles_Rustboro_Fountain; BG tiles 960-963).
JET_COLUMNS, JET_ROWS = (19, 29), (8, 17)
# (full.mk builds them, tools/vtree_manifest.py hands them to the builder)
FOUNTAIN_ANIM = ["data/tilesets/secondary/rustboro/anim/fountain/%d.4bpp" % f for f in (0, 1)]
FOUNTAIN_ANIM_TILE = 512 + 448
JET_BAND = 48       # the texture's row the jet's first row is laid on


def fountain_jet():
    """The jet's pixels: {(x, y): (tile, pixel, palette)} where each comes
    from on the upper layer, and their colours in each animation frame."""
    lay = LayoutArt("LAYOUT_RUSTBORO_CITY")
    ox, oy = FOUNTAIN_RECT[0] * 16, FOUNTAIN_RECT[1] * 16
    src = {(x, y): lay.upper_source(ox + x, oy + y)
           for y in range(*JET_ROWS) for x in range(*JET_COLUMNS)}
    frames = []
    for path in FOUNTAIN_ANIM:
        tiles = anim_frame(path, FOUNTAIN_ANIM_TILE)
        colours = {}
        for at, (tile, pixel, pal) in src.items():
            index = lay.tile_pixel(tile, pixel, tiles)
            if index == 0:
                raise SystemExit("rustboro_fountain: the jet's pixel %d,%d is clear in %s" % (at + (path,)))
            colours[at] = lay.palette(pal)[index]
        frames.append(colours)
    return src, frames


def fountain(art):
    """The fountain in 3D (see FOUNTAIN_FOOT), and its texture: the drawing
    with the jet taken out of it - the pool and the dish as they lie behind
    it - and the jet alone on a band below, read by a card standing on the
    dish. The card's texels follow the game's animation (`live`): the
    console redraws them from the jet's tiles whenever the game does, so
    the jet plays as it does on the GBA, and only the jet."""
    _, frames = fountain_jet()
    apx = art.load()
    for (x, y), rgb in frames[0].items():
        if apx[x, y][:3] != rgb:
            raise SystemExit("rustboro_fountain: the jet's first frame is not the drawing at %d,%d" % (x, y))
    # what lies behind the jet: above the dish the pool's own lines (the
    # ripple under the back rim, open water, the dish's far edge); on the
    # dish its water, wherever the two frames differ or draw the jet's
    # white and blue (down its middle)
    jet = {f[(x, y)] for f in frames for x in (23, 24) for y in range(9, JET_ROWS[1])}
    dish_water = frames[0][(21, 11)]
    behind = {}
    for (x, y) in frames[0]:
        a, b = frames[0][(x, y)], frames[1][(x, y)]
        if y < 11:
            behind[(x, y)] = frames[0][(JET_COLUMNS[0], y)]
        else:
            behind[(x, y)] = a if a == b and a not in jet else dish_water
    rows = JET_ROWS[1] - JET_ROWS[0]
    tex = Image.new("RGBA", (art.size[0], JET_BAND + rows), (0, 0, 0, 0))
    tex.paste(art, (0, 0))
    tpx = tex.load()
    live = {}
    for (x, y), bg in behind.items():
        tpx[x, y] = bg + (255,)
        rgb = frames[0][(x, y)]
        at = (x, JET_BAND + y - JET_ROWS[0])
        tpx[at] = rgb + (255,) if rgb != bg else (0, 0, 0, 0)
        live[at] = (x, y, bg)
    parts = [
        PlanPrism("basin", FOUNTAIN_FOOT, 0, FOUNTAIN_RIM, (14, 34, 46), open_top=True),
        PlanRing("rim", FOUNTAIN_FOOT, FOUNTAIN_POOL, FOUNTAIN_RIM),
        PlanPrism("pool", FOUNTAIN_POOL, FOUNTAIN_WATER, FOUNTAIN_RIM, (16, 32, 7),
                  open_top=True, inward=True),
        PlanRing("water", FOUNTAIN_POOL, None, FOUNTAIN_WATER),
        Cylinder("dish", DISH_AT[0], DISH_AT[1], DISH_R[0], DISH_R[1], FOUNTAIN_DISH - DISH_EDGE,
                 FOUNTAIN_DISH, back_tile=Tile(21, 23, 27, 25), sides=24),
        Cylinder("stem", DISH_AT[0], DISH_AT[1], STEM_R[0], STEM_R[1], FOUNTAIN_WATER,
                 FOUNTAIN_DISH - DISH_EDGE, back_tile=Tile(18, 25, 30, 28), sides=12, proj=False),
        # under the dish, a little over the water: inside what the dish hides
        # at 45 degrees (its top's ellipse swept down its edge, moved to this
        # height), its shadow's grey
        Shade("dish_shadow", DISH_AT[0], DISH_AT[1] - (FOUNTAIN_DISH - FOUNTAIN_WATER) + 1.25,
              DISH_R[0] - 0.5, DISH_R[1] + 0.5, FOUNTAIN_WATER + 0.25, (24, 25)),
        Jet("jet", JET_COLUMNS, DISH_AT[1], FOUNTAIN_DISH, JET_BAND - JET_ROWS[0]),
    ]
    return parts, tex, live


def fountain_drawing(model):
    """What the fountain is judged against: its drawing, the jet in it as
    the first frame draws it (the band laid back over the pool)."""
    art = model.art.crop((0, 0, 48, 48))
    band = model.art.crop((0, JET_BAND, 48, JET_BAND + JET_ROWS[1] - JET_ROWS[0]))
    art.paste(band, (0, JET_ROWS[0]), band)
    return art


class Awning:
    """A cloth roof on posts that people walk under: a flat top, a valance
    hanging round it and a post at each front corner.

    Drawn: the top over rows [top_v, valance_v), the valance's front over
    [valance_v, post_v), the posts below it to their foot at `foot_v`, in the
    columns `posts`. As drawn it would stand lower than a person, so the
    whole roof is lifted by `lift` along the GBA's line of sight, (0, l, l),
    which leaves it where it is drawn; the posts reach the ground with more
    of their own shaft, their foot `lift` rows south of where it is drawn.
    The top goes back to `back` (its real depth) with rows of its pattern
    (`repeat`); the valance runs round all four sides.
    """

    def __init__(self, name, x0, x1, top_v, valance_v, post_v, foot_v, back, lift,
                 repeat, posts, shaft, foot):
        self.name, self.x0, self.x1 = name, x0, x1
        self.top_v, self.valance_v, self.post_v, self.foot_v = top_v, valance_v, post_v, foot_v
        self.back, self.lift, self.repeat = back, lift, repeat
        self.posts, self.shaft, self.foot = posts, shaft, foot

    def roof(self, mesh, zf, yt, zb):
        # its back edge: the outline and the light row under it
        top = Strip((self.top_v + 2, self.valance_v), repeat=self.repeat,
                    tail=(self.top_v, self.top_v + 2))
        x0, x1 = self.x0, self.x1
        strip_face(mesh, [(x0, yt, zf), (x1, yt, zf), (x1, yt, zb), (x0, yt, zb)],
                   (0.0, yt, zf), (1.0, 0.0, 0.0), (0.0, 0.0, -1.0), top, SHADE_ART,
                   self.name + ".top")

    def emit(self, mesh):
        n, x0, x1, l = self.name, self.x0, self.x1, self.lift
        zf = self.foot_v + l                     # the posts' foot, on the ground
        yt = zf - self.valance_v                 # the top's height
        yb = zf - self.post_v                    # the valance's lower edge
        zb = self.back
        up = (0.0, 1.0, 0.0)
        self.roof(mesh, zf, yt, zb)
        val = Strip((self.valance_v, self.post_v))
        # the flanks take the front's columns from its west end on
        flank = Strip((self.valance_v, self.post_v), offset=x0 + 1 - zb)
        sides = [
            ([(x0, yb, zf), (x1, yb, zf), (x1, yt, zf), (x0, yt, zf)],
             (0.0, yb, zf), (1.0, 0.0, 0.0), SHADE_ART, val),
            ([(x1, yb, zb), (x0, yb, zb), (x0, yt, zb), (x1, yt, zb)],
             (0.0, yb, zb), (1.0, 0.0, 0.0), SHADE_BACK, val),
            ([(x0, yb, zb), (x0, yb, zf), (x0, yt, zf), (x0, yt, zb)],
             (x0, yb, 0.0), (0.0, 0.0, 1.0), SHADE_WEST, flank),
            ([(x1, yb, zf), (x1, yb, zb), (x1, yt, zb), (x1, yt, zf)],
             (x1, yb, 0.0), (0.0, 0.0, 1.0), SHADE_EAST, flank),
        ]
        cx, cz = (x0 + x1) / 2.0, (zf + zb) / 2.0
        for quad, origin, sdir, shade, val in sides:
            strip_face(mesh, quad, origin, sdir, up, val, shade, n + ".valance")
            # its inside, seen between the posts: a quarter of a pixel in,
            # or the two faces fight for the same depth
            inner = [(x + 0.25 * ((cx > x) - (cx < x)) * (sdir[2] != 0), y,
                      z + 0.25 * ((cz > z) - (cz < z)) * (sdir[0] != 0))
                     for (x, y, z) in reversed(quad)]
            o = inner[-1]
            strip_face(mesh, inner, (origin[0] if sdir[0] else o[0], origin[1],
                                     origin[2] if sdir[2] else o[2]),
                       sdir, up, val, SHADE_BACK, n + ".valance_in")
        # the posts: thin legs, so each is one flat card facing south with
        # the drawn front post on it - the foot as drawn, then the shaft up
        # to the valance. As boxes, every side repeated the front and from
        # the game's camera each leg read as two. The back ones (the roof
        # hides them at 45 degrees) stand at the back edge.
        for (pa, pb) in self.posts:
            for z in (zf, zb + (pb - pa)):
                strip_face(mesh, [(pa, 0, z), (pb, 0, z), (pb, yb, z), (pa, yb, z)],
                           (0.0, 0.0, z), (1.0, 0.0, 0.0), up,
                           Strip(self.foot, repeat=self.shaft), SHADE_ART, n + ".post")


class Tent(Awning):
    """A market stall: an awning whose roof is hipped, its ridge along X.

    Every face of it is a flat colour in the drawing - the front slope, the
    ridge, the back slope (seen over the ridge: the roof is low) and the two
    hips, lit from the west - so the faces take the GBA projection where the
    drawing shows them and their own colour where the real depth carries
    them beyond it. `ridge` = (xa, xb, v0, v1): the ridge's ends and the rows
    of its flat top; `pitch` the front slope's. `fills` = (back, west, east):
    an art rectangle of each face's colour.
    """

    def __init__(self, name, x0, x1, top_v, valance_v, post_v, foot_v, back, lift,
                 posts, shaft, foot, ridge, pitch, fills):
        Awning.__init__(self, name, x0, x1, top_v, valance_v, post_v, foot_v, back, lift,
                        None, posts, shaft, foot)
        self.ridge, self.pitch, self.fills = ridge, pitch, fills

    def roof(self, mesh, zf, ye, zb):
        n, x0, x1 = self.name, self.x0, self.x1
        xa, xb, r0, r1 = self.ridge
        # the front slope rises r1 - valance_v rows of drawing: run + rise
        rows = self.valance_v - r1
        t = math.tan(math.radians(self.pitch))
        run = rows / (1.0 + t)
        zr, yr = zf - run, ye + run * t
        zq = zr - (r1 - r0)

        def proj(pts, lo, hi):
            return [(x, y, z, x, min(max(z - y, lo), hi)) for (x, y, z) in pts]

        def fill(pts, rect):
            u0, v0, u1, v1 = rect
            return [(x, y, z, min(max(x, u0), u1), min(max(z - y, v0), v1))
                    for (x, y, z) in pts]

        front = [(x0, ye, zf), (x1, ye, zf), (xb, yr, zr), (xa, yr, zr)]
        mesh.poly(proj(front, r1, self.valance_v), SHADE_ART, n + ".slope_s~proj")
        top = [(xa, yr, zr), (xb, yr, zr), (xb, yr, zq), (xa, yr, zq)]
        mesh.poly(proj(top, r0, r1), SHADE_ART, n + ".ridge~proj")
        back, west, east = self.fills
        mesh.poly(fill([(xa, yr, zq), (xb, yr, zq), (x1, ye, zb), (x0, ye, zb)], back),
                  SHADE_ART, n + ".slope_n~proj")
        for hip, rect in (([(x0, ye, zb), (x0, ye, zf), (xa, yr, zr), (xa, yr, zq)], west),
                          ([(x1, ye, zf), (x1, ye, zb), (xb, yr, zq), (xb, yr, zr)], east)):
            pts = [(x, y, z, x, z - y) for (x, y, z) in hip]
            # as drawn below the ridge's front, its own colour behind it
            near = clip(pts, 4, float(r1), True)
            far = clip(pts, 4, float(r1), False)
            if len(near) >= 3:
                mesh.poly([(x, y, z, u, min(v, self.valance_v)) for (x, y, z, u, v) in near],
                          SHADE_ART, n + ".hip~proj")
            if len(far) >= 3:
                mesh.poly(fill([q[:3] for q in far], rect), SHADE_ART, n + ".hip~proj")


def proj_rows(mesh, pts, rows, tag, shade=SHADE_ART):
    """A face in the GBA projection, as drawn between `rows`; where the
    real depth carries it beyond them, its edge row held (courses that run
    straight across those rows go on as they are)."""
    lo, hi = rows
    poly = [(x, y, z, x, z - y) for (x, y, z) in pts]
    for piece, hold in ((clip(poly, 4, lo, False), lo), (clip(poly, 4, hi, True), hi)):
        if len(piece) >= 3:
            mesh.poly([q[:4] + (hold,) for q in piece], shade, tag)
    mid = clip(clip(poly, 4, lo, True), 4, hi, False) if len(poly) >= 3 else []
    if len(mid) >= 3:
        mesh.poly(mid, shade, tag)


class Mansard:
    """A hipped roof with a flat deck: four slopes from the eave rectangle
    (x0..x1, zb..zf at `ye`) up to the deck (inset `side` across, `run` front
    and back, at `ye + rise`).

    The front slope and the hips take the GBA projection - the front as
    drawn, the hips' courses run front to back, so beyond the drawing they
    go on as their own columns (rows clamped to `hip_rows`). The deck is laid
    from its front edge back by `deck` (a Strip), the back slope by `back`.
    """

    def __init__(self, name, x0, x1, zf, zb, ye, side, run, rise, front_rows, hip_rows,
                 deck, back):
        self.name, self.x0, self.x1, self.zf, self.zb, self.ye = name, x0, x1, zf, zb, ye
        self.side, self.run, self.rise = side, run, rise
        self.front_rows, self.hip_rows, self.deck, self.back = front_rows, hip_rows, deck, back

    def emit(self, mesh):
        n, x0, x1, zf, zb, ye = self.name, self.x0, self.x1, self.zf, self.zb, self.ye
        xa, xb = x0 + self.side, x1 - self.side
        za, zq, yt = zf - self.run, zb + self.run, ye + self.rise
        proj_rows(mesh, [(x0, ye, zf), (x1, ye, zf), (xb, yt, za), (xa, yt, za)],
                  self.front_rows, n + ".slope_s~proj")
        proj_rows(mesh, [(x0, ye, zb), (x0, ye, zf), (xa, yt, za), (xa, yt, zq)],
                  self.hip_rows, n + ".hip_w~proj")
        proj_rows(mesh, [(x1, ye, zf), (x1, ye, zb), (xb, yt, zq), (xb, yt, za)],
                  self.hip_rows, n + ".hip_e~proj")
        strip_face(mesh, [(x1, ye, zb), (x0, ye, zb), (xa, yt, zq), (xb, yt, zq)],
                   (0.0, ye, zb), (1.0, 0.0, 0.0), _unit((0.0, self.rise, self.run)),
                   self.back, SHADE_BACK, n + ".slope_n")
        strip_face(mesh, [(xa, yt, za), (xb, yt, za), (xb, yt, zq), (xa, yt, zq)],
                   (0.0, yt, za), (1.0, 0.0, 0.0), (0.0, 0.0, -1.0), self.deck, SHADE_ART,
                   n + ".deck")


class Posts:
    """Square posts standing in front of a wall: every side shows the drawn
    front of the post (its columns, its rows), the top its first row."""

    def __init__(self, name, posts, z0, z1, y1, last=None):
        self.name, self.posts, self.z0, self.z1, self.y1 = name, posts, z0, z1, y1
        # the drawing's last row: a post standing out past the model's cells
        # has its foot drawn in the cell south of them, which is not the
        # model's - its last drawn row goes on down to the ground (a post is
        # the same all the way down)
        self.last = last

    def emit(self, mesh):
        z0, z1, y1, n = self.z0, self.z1, self.y1, self.name
        top = z1 - y1
        if self.last is not None:
            emit_poly = mesh.poly
            last = self.last - 0.5

            class _Held:
                def poly(_, pts, shade, tag):
                    emit_poly([p[:4] + (min(p[4], last),) for p in pts], shade, tag)
            mesh = _Held()
        for (a, b) in self.posts:
            mesh.poly([(a, -1, z1, a, z1 + 1), (b, -1, z1, b, z1 + 1), (b, y1, z1, b, top),
                       (a, y1, z1, a, top)], SHADE_ART, n + "~proj")
            mesh.poly([(b, -1, z0, b, z1 + 1), (a, -1, z0, a, z1 + 1), (a, y1, z0, a, top),
                       (b, y1, z0, b, top)], SHADE_BACK, n + "~proj")
            # the sides: the front's columns laid across the depth, the
            # corner's own column at the front (one column stretched back
            # along the depth only made stripes)
            mesh.poly([(a, -1, z0, b, z1 + 1), (a, -1, z1, a, z1 + 1), (a, y1, z1, a, top),
                       (a, y1, z0, b, top)], SHADE_WEST, n + "~proj")
            mesh.poly([(b, -1, z1, b, z1 + 1), (b, -1, z0, a, z1 + 1), (b, y1, z0, a, top),
                       (b, y1, z1, b, top)], SHADE_EAST, n + "~proj")
            mesh.poly([(a, y1, z1, a, top), (b, y1, z1, b, top), (b, y1, z0, b, top + 1),
                       (a, y1, z0, a, top + 1)], SHADE_ART, n + "~proj")


class Canopy:
    """A porch roof over a door: a flat slab standing straight out of the
    facade (at `zw`) to its front at `zf`, on two posts.

    Drawn: its top, `y1` high, from the wall out (its flutes run away from
    the wall, so they are drawn as columns), then its front down to the
    posts' tops at `y0`, the front's lower edge arched over the door
    (`arch`: (x, y) from post to post). The top's back corners are cut
    (`chamfer` = (dx, dz)) as the drawing rounds them. Every face the
    drawing shows is projected; the vault under the arch takes the
    doorway's dark `inside` texel, the two ends the front's edge columns
    laid across the depth. The posts are one flat card each, facing south,
    as drawn down to their foot (`last`: their last drawn row goes on to
    the ground). Under it the wall paints what the drawing has there (the
    flutes, the posts again), seen past the posts as a second porch: a
    skin of the wall's own column beside the porch (`skin`) covers it
    beside the door, and the doorway's dark over it down to the row of its
    sill (`sill`).
    """

    def __init__(self, name, x0, x1, zw, zf, y0, y1, arch, chamfer, posts, inside, last,
                 skin, sill):
        self.name, self.x0, self.x1, self.zw, self.zf = name, x0, x1, zw, zf
        self.sill = sill
        self.y0, self.y1, self.arch, self.chamfer = y0, y1, arch, chamfer
        self.posts, self.inside, self.last, self.skin = posts, inside, last, skin

    def emit(self, mesh):
        n, x0, x1, zw, zf, y0, y1 = (self.name, self.x0, self.x1, self.zw, self.zf,
                                     self.y0, self.y1)
        cx, cz = self.chamfer
        iu, iv = self.inside

        def proj(pts):
            return [(x, y, z, x, z - y) for (x, y, z) in pts]
        mesh.poly(proj([(x0 + cx, y1, zw), (x1 - cx, y1, zw), (x1, y1, zw + cz), (x1, y1, zf),
                        (x0, y1, zf), (x0, y1, zw + cz)][::-1]), SHADE_ART, n + ".top~proj")
        edge = [(x0, y0)] + list(self.arch) + [(x1, y0)]
        for (xa, ya), (xb, yb) in zip(edge, edge[1:]):
            if xb - xa > 1e-6:
                mesh.poly(proj([(xa, ya, zf), (xb, yb, zf), (xb, y1, zf), (xa, y1, zf)]),
                          SHADE_ART, n + ".front~proj")
            # under it: flat over the posts, the vault under the arch
            mesh.poly([(xa, ya, zw, iu, iv), (xb, yb, zw, iu, iv), (xb, yb, zf, iu, iv),
                       (xa, ya, zf, iu, iv)], SHADE_BACK, n + ".under~clamp")
        # the ends: the front's edge columns laid across the depth; their cut
        # corners as drawn
        zc = zw + cz
        for x, xc, inward, shade in ((x0, x0 + cx, 1, SHADE_WEST), (x1, x1 - cx, -1, SHADE_EAST)):
            mesh.poly([(x, y, z, x + inward * (zf - z) * 0.5, zf - y) for (x, y, z) in
                       ((x, y0, zf), (x, y0, zc), (x, y1, zc), (x, y1, zf))],
                      shade, n + ".end~proj")
            mesh.poly(proj([(x, y0, zc), (xc, y0, zw), (xc, y1, zw), (x, y1, zc)]),
                      shade, n + ".end~proj")
        (pa, pb), (qa, qb) = self.posts
        (wa, wb) = self.skin
        for xa, xb, col in ((pa, pb, wa), (qa, qb, wb)):
            mesh.poly([(x, y, zw + 0.25, col, zw - max(y, 0)) for (x, y) in
                       ((xa, -1), (xb, -1), (xb, y0), (xa, y0))], SHADE_ART, n + ".skin~proj")
        sill = zw + 0.25 - self.sill            # the doorway's dark down to its sill
        for (xa, ya), (xb, yb) in zip(self.arch, self.arch[1:]):
            if xb - xa > 1e-6:
                mesh.poly([(xa, sill, zw + 0.25, iu, iv), (xb, sill, zw + 0.25, iu, iv),
                           (xb, yb, zw + 0.25, iu, iv), (xa, ya, zw + 0.25, iu, iv)],
                          SHADE_ART, n + ".door~clamp")
        last = self.last - 0.5
        for (a, b) in self.posts:
            mesh.poly([(x, y, zf, x, min(zf - y, last)) for (x, y) in
                       ((a, -1), (b, -1), (b, y0), (a, y0))], SHADE_ART, n + ".post~proj")


def fan_club():
    """Slateport's Pokemon Fan Club (80 x 80, cells 2..6 x 22..26).

        rows  8-39  the roof's deck, courses every 4 rows (x 14-65), the hips
                    either side, their courses running front to back
        rows 40-53  the front slope, courses every 3 rows
        rows 54-79  the facade: chamfered corners 7 px, pilasters, windows,
                    the door between two posts that stand 7 px out (to 86)

    Real depth: four collision rows, Z 16-80. The deck grows backwards by
    its own courses, the hips by their own columns.
    """
    zf, zb, ye = 80, 16, 26
    roof = Mansard("roof", 0, 80, zf, zb, ye, side=14, run=7, rise=7,
                   front_rows=(40, 54), hip_rows=(16, 54),
                   deck=Strip((12, 40), repeat=(12, 16), tail=(8, 12)),
                   back=Strip((12, 16), repeat=(12, 16)))
    # its sides on the drawing's outermost columns (their outline): on 1
    # and 79 the outline columns 0 and 79 stood on the ground beside it
    walls = Walls("facade", [(0, 73), (7, 80), (73, 80), (80, 73)], -1, ye)
    pilaster = Tile(8, 55, 16, 79)
    sides = [PlainWall("side_w", (0, 73), (0, zb), -1, ye, pilaster),
             PlainWall("side_e", (80, zb), (80, 73), -1, ye, pilaster),
             PlainWall("back", (0, zb), (80, zb), -1, ye, pilaster)]
    # the door's porch: a flat roof at Y 24, just under the eave, from the
    # wall out to Z 87 (its fluted top rows 56-63, its front 63-73), the
    # front arched over the door up to row 69; the posts under its front
    # (in steps, as drawn: a slope across a step cut its outline's pixels)
    arch = [(33, 14), (33, 15), (35, 15), (35, 16), (37, 16), (37, 17), (39, 17), (39, 18),
            (41, 18), (41, 17), (43, 17), (43, 16), (45, 16), (45, 15), (47, 15), (47, 14)]
    porch = Canopy("porch", 28, 52, 80, 87, 14, 24, arch, chamfer=(3, 2),
                   posts=[(29, 33), (47, 51)], inside=(40.5, 74.5), last=87, skin=(27.5, 53.5),
                   sill=77)
    return [roof, walls, porch] + sides


# the door posts' tops, which the drawing does not show, stand over 73-77; the
# grass beside the chamfers is drawn in the facade's own blocks
FAN_CLUB_EXACT = [(14, 12, 66, 54), (0, 16, 14, 54), (66, 16, 80, 54), (0, 55, 80, 73),
                  (3, 73, 29, 77), (33, 73, 47, 77), (51, 73, 77, 77)]


def museum():
    """Slateport's Oceanic Museum (112 x 96, cells 25..31 x 7..12).

        rows  2- 3  the roof's back edge (the trees' feet above it are theirs)
        rows  4-47  the roof's deck: red and white stripes running front to
                    back, the east flank darker
        rows 48-57  its steeper red skirt, chamfered at either end
        rows 58-61  fascia
        rows 62-95  the facade: portholes, the waves, the door

    The same reading as the fan club's roof. Real depth: four collision
    rows, Z 32-96; the stripes run on back as they are.
    """
    zf, zb, ye = 96, 32, 38
    roof = Mansard("roof", 0, 112, zf, zb, ye, side=8, run=5, rise=5,
                   front_rows=(48, 58), hip_rows=(4, 58),
                   deck=Strip((4, 48), repeat=(20, 28), tail=(2, 4)),
                   back=Strip((20, 28), repeat=(20, 28)))
    walls = Walls("facade", [(0, zf), (112, zf)], -1, ye)
    side = Tile(8, 62, 24, 96)
    sides = [PlainWall("side_w", (0, zf), (0, zb), -1, ye, side),
             PlainWall("side_e", (112, zb), (112, zf), -1, ye, side),
             PlainWall("back", (0, zb), (112, zb), -1, ye, side)]
    return [roof, walls] + sides


MUSEUM_EXACT = [(9, 4, 103, 58), (0, 58, 112, 96)]


class FlatSlab:
    """A flat roof slab on columns: its top at `y1`, its front edge (y0..y1)
    as drawn and its other three edges dressed with `edge`.

    The top is the drawing's `front` rows from the front edge back; then, to
    the real depth, `fill` = (rows, wrap): rows of it repeated, sampled
    between the columns `wrap` (clear of the lamps), with the rim's own
    columns (`rim` pixels each side) going on beside them; then the back
    rim, `tail` rows."""

    def __init__(self, name, x0, x1, zf, zb, y0, y1, front, fill, rim, tail, edge):
        self.name, self.x0, self.x1, self.zf, self.zb = name, x0, x1, zf, zb
        self.y0, self.y1, self.edge = y0, y1, edge
        self.front, self.fill, self.rim, self.tail = front, fill, rim, tail

    def emit(self, mesh):
        n, x0, x1, zf, zb, y0, y1 = self.name, self.x0, self.x1, self.zf, self.zb, self.y0, self.y1
        a, b = self.front
        za = zf - (b - a)                       # the drawn top ends here
        zt = zb + (self.tail[1] - self.tail[0])  # the back rim starts here
        north = (0.0, 0.0, -1.0)

        def lay(xa, xb, z0, z1, strip, tag):
            strip_face(mesh, [(xa, y1, z0), (xb, y1, z0), (xb, y1, z1), (xa, y1, z1)],
                       (0.0, y1, z0), (1.0, 0.0, 0.0), north, strip, SHADE_ART, n + tag)

        lay(x0, x1, zf, za, Strip(self.front), ".top")
        rows, wrap = self.fill
        r = self.rim
        lay(x0, x0 + r, za, zt, Strip(rows, repeat=rows), ".top")
        lay(x0 + r, x1 - r, za, zt, Strip(rows, repeat=rows, wrap=wrap), ".top")
        lay(x1 - r, x1, za, zt, Strip(rows, repeat=rows), ".top")
        lay(x0, x1, zt, zb, Strip(self.tail), ".top")
        mesh.poly([(x, y, z, x, z - y) for (x, y, z) in
                   [(x0, y0, zf), (x1, y0, zf), (x1, y1, zf), (x0, y1, zf)]], SHADE_ART,
                  n + ".front")
        for (a, b) in (((x0, zf), (x0, zb)), ((x1, zb), (x1, zf)), ((x0, zb), (x1, zb))):
            PlainWall(n + ".edge", a, b, y0, y1, self.edge).emit(mesh)


def contest_hall():
    """Slateport's Contest Hall (96 x 96, cells 28..33 x 22..27).

        rows  9-47  the flat roof: its rim and the deck with its lamps
        rows 48-55  the slab's front edge, the columns' caps in it
        rows 56-79  the facade, set back: chamfered 8 px at either end,
                    windows, the door
        rows 56-87  four columns standing out to Z 88 (x 8, 24, 64, 80)
        rows 76-87  the steps down from the door

    Real depth: four collision rows, Z 16-80, the columns in front. The deck
    grows backwards by its dashed rows, the seawall drawn behind is not its.
    """
    zf, zb = 88, 16
    roof = FlatSlab("roof", 0, 96, zf, zb, 32, 40, front=(16, 48), fill=((24, 32), (8, 40)),
                    rim=8, tail=(8, 16), edge=Tile(40, 48, 56, 56))
    # the chamfers a pixel out: their outline, drawn on the diagonal's outer
    # side, stood on the lawn
    walls = Walls("facade", [(0, 73), (7, 80), (89, 80), (96, 73)], -1, 32)
    side = Tile(16, 56, 24, 80)
    sides = [PlainWall("side_w", (0, 73), (0, zb), -1, 32, side),
             PlainWall("side_e", (96, zb), (96, 73), -1, 32, side),
             PlainWall("back", (0, zb), (96, zb), -1, 32, side)]
    columns = Posts("columns", [(8, 16), (24, 32), (64, 72), (80, 88)], 80, zf, 32)
    steps = Relief_top([(32, 80), (64, 80), (64, 88), (32, 88)], 0.5)
    return [roof, walls, columns, steps] + sides


# the deck's rounded corners are the rim's, which the real depth moves
CONTEST_EXACT = [(8, 16, 88, 48), (0, 48, 96, 88)]


class Porch:
    """A box standing out of a building's front: its facade drawn upright
    from `y1` down to the ground at z = `zf`, its top a strip back to `zb`,
    its two sides dressed with the facade's own edge columns (the pillars)."""

    def __init__(self, name, x0, x1, zf, zb, y1, side_w):
        self.name, self.x0, self.x1, self.zf, self.zb, self.y1 = name, x0, x1, zf, zb, y1
        self.side_w = side_w

    def emit(self, mesh):
        x0, x1, zf, zb, y1 = self.x0, self.x1, self.zf, self.zb, self.y1
        n = self.name
        mesh.poly([(x0, 0, zf, x0, zf), (x1, 0, zf, x1, zf), (x1, y1, zf, x1, zf - y1),
                   (x0, y1, zf, x0, zf - y1)], SHADE_ART, n + ".front")
        mesh.poly([(x0, y1, zf, x0, zf - y1), (x1, y1, zf, x1, zf - y1),
                   (x1, y1, zb, x1, zb - y1), (x0, y1, zb, x0, zb - y1)], SHADE_ART, n + ".top")
        # the sides: the pillar's own columns, from the facade back
        d = zf - zb
        k = min(1.0, float(self.side_w) / d)
        mesh.poly([(x0, 0, zb, x0 + d * k, zf), (x0, 0, zf, x0, zf), (x0, y1, zf, x0, zf - y1),
                   (x0, y1, zb, x0 + d * k, zf - y1)], SHADE_WEST, n + ".west~proj")
        mesh.poly([(x1, 0, zf, x1, zf), (x1, 0, zb, x1 - d * k, zf), (x1, y1, zb, x1 - d * k, zf - y1),
                   (x1, y1, zf, x1, zf - y1)], SHADE_EAST, n + ".east~proj")


def on_ray(vol, u, v, sink, tmax=96.0):
    """The point of the drawing's (u, v) where its 45-degree ray enters
    `vol`, `sink` pixels further down the ray: a tube's axis laid on it.
    A ray that passes over the volume (a cloth arm drawn past a dome's
    outline) gives the point of it nearest the volume's top: the arm
    lies on the dome there, standing out of its outline."""
    ins, t = vol.inside, tmax
    while t >= 0 and not ins(u, t, v + t):
        t -= 0.25
    if t >= 0:
        f = t - sink
        return (u, f, v + f)

    def top(z):
        y = tmax
        while y >= 0 and not ins(u, y, z):
            y -= 0.25
        return y
    best = min((t - y, t) for t, y in ((k * 0.5, top(v + k * 0.5)) for k in range(int(tmax * 2)))
               if y >= 0)
    f = best[1]
    return (u, f, v + f)


# The battle tent, read off its drawing (art pixels, the 5x5 cells' own):
#   a dome of blue and white panels, its grey skirt on the ground (v 64-76);
#   on its top, the highest thing, a low crown: a frustum, blue side (v 20-27)
#   and white lid in a dark rim (v 12-21);
#   four orange cloth arms in relief on the dome, from the crown down: two
#   down its front to the door's lintel (u 26-38 and 42-54, v 27-48); two
#   back over its top (where the 45-degree view sees past the crown: the
#   "M" of its outline, v 2-12) and down its flanks to the ground at its
#   sides (u 0-12 and 68-80);
#   the door, a box standing out of it (Porch).
TENT_BODY = dict(cx=40, cz=48, rx=33, rz=31, h=30, p=3.5)
TENT_CROWN = (40, 48, 17, 8, 14, 4.5, 24, 32)
# the arms' axes: (u, v, radius, y) - drawn at (u, v); laid on the dome
# (y None) or standing y high where they leave it, at its sides
TENT_OUTER = [(40, 9, 3.5, None), (32, 5, 4, None), (24, 7, 4.5, None), (16, 14, 5.5, None),
              (9, 24, 6.5, 20), (6, 36, 6.5, 14), (6, 50, 6.5, 9), (6, 66, 6, 3)]
TENT_INNER = [(32, 28, 5, None), (32, 36, 6, None), (31, 46, 5.5, None)]


_TENT_SHAPE = []


def battle_tent_elsewhere(art):
    """A battle tent drawn in a town's own shades (Verdanturf's outline),
    the town's drawing round it (its cliff in the top rows) cleared by the
    Slateport tent's outline: the same model, its own pixels."""
    if not _TENT_SHAPE:
        import voxel_building as vb
        la = vb.LayoutArt("LAYOUT_SLATEPORT_CITY")
        ref = la.building_art(8, 8, 5, 5, la.ground_tiles([GRASS]),
                              ground_px=la.ground_pixels([GRASS]))
        rpx = ref.load()
        _TENT_SHAPE.append({(u, v) for v in range(80) for u in range(80) if rpx[u, v][3] >= 128})
    art = art.copy()
    px = art.load()
    for v in range(80):
        for u in range(80):
            if (u, v) not in _TENT_SHAPE[0]:
                px[u, v] = (0, 0, 0, 0)
    return battle_tent(art)


def battle_tent(art):
    """The battle tent: its volume cut to its drawing (HeightSolid), and its door."""
    tex = Mound.with_ring(art, ())
    body = Volume().dome(**TENT_BODY)
    vol = Volume().dome(**TENT_BODY)
    vol.drum(*TENT_CROWN)
    for mirror in (False, True):
        f = (lambda u: 80 - u) if mirror else (lambda u: u)
        for arm in (TENT_OUTER, TENT_INNER):
            vol.tube([(on_ray(body, f(u), v, 0.4 * r) if y is None else (f(u), y, v + y)) + (r,)
                      for (u, v, r, y) in arm])
    door = lambda u, v: 19 <= u < 60 and 48 <= v < 80
    return [HeightSolid("tent", tex, art.size[1], vol, step=2, mask=door, mirror_z=48, lean=2.0),
            Porch("door", 19, 60, 80, 73, 25, 6)], tex


# Slateport's sailboat (art of the whole one at (34, 44), 3x3 cells): a
# hull on a long oval plan, its deck 8 high (the deck's oval v 22-32, the
# planked side v 32-40), a mast at u 28-31 up to v 3, and two sails in the
# plane of the boat's keel: the striped jib before the mast, the white
# mainsail behind it.
# the harbour bollard's profile, (y, radius) from its foot
BOLLARD = [(0, 6.5), (2, 6), (5, 6), (6, 5.5), (7, 7), (8.5, 7), (9, 4), (11, 0)]
BOAT_Z = 35
# the sails (x, y) in the keel's plane, and the mast (x0, x1, y0, y1)
BOAT_SAILS = [[(27, 33), (9, 12), (27, 12)], [(32, 32), (44, 11), (32, 11)]]
BOAT_MAST = (27.5, 31.5, 8, 33)
# the hull's plan and sheer (ProjHull)
BOAT_HULL = dict(cx=24.5, rx=24.5, rz=5.5, p=3.0, sheer=6, keel=0)


def _grown(poly, d):
    """A polygon's corners pushed `d` out from its middle."""
    cx = sum(p[0] for p in poly) / len(poly)
    cy = sum(p[1] for p in poly) / len(poly)
    out = []
    for (x, y) in poly:
        dx, dy = x - cx, y - cy
        k = math.hypot(dx, dy) or 1.0
        out.append((x + dx / k * d, y + dy / k * d))
    return out


def _inside(poly, x, y):
    inside = False
    for i in range(len(poly)):
        (xa, ya), (xb, yb) = poly[i], poly[i - 1]
        if (ya > y) != (yb > y) and x < xa + (y - ya) * (xb - xa) / (yb - ya):
            inside = not inside
    return inside


class SailCards:
    """A boat's sails and mast as flat cards in its keel's plane, the
    drawing projected on them: at 45 degrees exactly the drawing, its
    transparency cutting their outline. In the hull's height field they
    were plates two pixels thick, cut column by column, and from the game's
    camera their slanted edges stood in spikes. Each card is its outline
    grown a pixel and a half, so that the outline's own pixels are in it."""

    def __init__(self, name, zs, dx=0.0):
        self.name, self.zs, self.dx = name, zs, dx
        x0, x1, y0, y1 = BOAT_MAST
        self.polys = [_grown([(x + dx, y) for (x, y) in poly], 1.5) for poly in BOAT_SAILS]
        self.polys.append([(x0 + dx, y0), (x1 + dx, y0), (x1 + dx, y1), (x0 + dx, y1)])

    def owns(self, u, v):
        """Whether the 45-degree ray of pixel (u, v) meets a card."""
        return any(_inside(poly, u + 0.5, z - v - 0.5) for z in self.zs for poly in self.polys)

    def emit(self, mesh):
        for z in self.zs:
            for poly in self.polys:
                mesh.poly([(x, y, z, x, z - y) for (x, y) in poly], 1.0, self.name + "~proj")


def sailboat(art, boats=(0,), keel=0.0):
    """One sailboat, or several drawn one behind another (`boats`: how many
    rows lower each is drawn than the whole one's own art). `keel` moves
    the boat north: where a pier's edge hides its hull, the drawing ends
    at that edge and the boat lies behind it. Each hull a smooth decked
    ProjHull on a long, blunt-ended oval plan (BOAT_HULL: the drawing takes
    its ends down to the water square), its deck 8 high rising 6 to the
    ends; the sails and mast flat cards in its keel's plane (a height field
    had stood the hull in steps, the sails in spikes)."""
    zs = [BOAT_Z + dv + keel for dv in boats]
    h = BOAT_HULL
    # its foot a pixel under the water: on the console the vertices are
    # rounded, and a hull ending on the drawing's last row lost it in places
    return [ProjHull("boat", h["cx"], z, h["rx"], h["rz"], -2, 8, p=h["p"], sheer=h["sheer"],
                     keel=h["keel"], seg=24) for z in zs] + [SailCards("sail", zs)]


def rowboat(art):
    """The rowboat by the shipyard, up on stands: an open hull, its gunwale
    15 high (the rim's oval, v 0-19), its floor 8, a thwart across it at
    u 36-40. Smooth surfaces under its drawing (ProjHull)."""
    return [ProjHull("rowboat", 24, 22.5, 22.8, 8.5, 0, 15, p=2.3, sheer=2,
                     inner=(23.5, 22.5, 19.5, 6.5, 8),
                     # the thwart; the stand under the stern, drawn past the
                     # hull's curve down to row 30
                     boxes=[(36, 40, 8, 14, 15, 30), (33, 45, 0, 4, 25, 31)])]


# The stone lantern in Slateport's square (art 32x64, its foot at v 63): a
# turned thing on a plan 0.7 as deep as wide (the GBA draws its rings as flat
# ellipses; round, the drawing would leave it 30 high, its keyhole lying on
# a pedestal's top; at 0.5 it read too thin) - a bell for a base with the
# keyhole, a neck, a drum with its windows, a capped top. The profile is
# fitted to the drawing's outline row by row; the cubes are carved to it.
LANTERN = [(0, 12.5), (2, 14), (5, 16), (7, 16), (8, 14), (10, 14), (11, 12), (13, 10), (21, 10),
           (22, 12.5), (24, 14), (26, 16), (30, 16), (32, 14), (33, 12), (41, 12), (42, 9.5),
           (44, 8), (45, 6.5), (45.5, 0)]
LANTERN_Z = 55.3
# its plan 0.7 as deep as wide (the user's choice over 0.5, 2026-10-10)
LANTERN_SQUASH = 0.7
# stood half a cell north of where it is drawn, in the middle of its lawn
# (the drawing's cap then starts on the drawing's first row); the user's
# choice, 2026-10-10
LANTERN_BACK = 8


def lantern_drawing(model):
    """The lantern's drawing as it stands: moved LANTERN_BACK rows up."""
    from PIL import Image
    art = model.art
    out = Image.new("RGBA", art.size, (0, 0, 0, 0))
    out.paste(art.crop((0, LANTERN_BACK, art.size[0], 64)), (0, 0))
    return out


def lantern(art):
    """A low-poly lathe (BakedLathe, 32 sides, ~1200 triangles) with its
    texture baked off the true-cube model (VoxelLathe): the cubes' look -
    turned all round, its cap squarer (drawn with a straight back), no face
    stretched - at the cost of a prop. Its faces turned to the GBA's view
    read the drawing itself; only its outline is a polygon, not a stair."""
    px = art.load()
    W, H = art.size

    def drawn(u, v):
        return 0 <= u < W and 0 <= v < H and px[u, v][3] >= 128
    cubes = VoxelLathe("lantern", art, drawn, 16.0, LANTERN_Z, LANTERN, LANTERN_SQUASH,
                       power=lambda y: 4.0 if y >= 33.0 else 2.0)
    baked = BakedLathe("lantern", cubes, sides=32, shift_z=-LANTERN_BACK)
    return [baked], baked.texture()


def small_round(name, volume, depth=16.0, step=1, cell=2, tol=1, hang=(), side=None,
                open_top=False):
    """A thing off its volume alone (a bush, a bollard, a bin; `step` 4 and
    a wide `cell` for one of flat faces, a station, a tall block)."""
    def parts(art):
        tex = Mound.with_ring(art, ())
        return [HeightSolid(name, tex, art.size[1], volume, depth=(0.0, depth), step=step,
                            mirror_z=depth / 2, cell=cell, tol=tol, hang=hang, side=side,
                            open_top=open_top)], tex
    return parts


def silhouette(vol, u, v, depth):
    """Whether the 45-degree ray through pixel (u, v) meets the volume."""
    return any(vol.inside(u + 0.5, z - v - 0.5, z) for z in range(int(v), int(v) + depth))


def space_center(art):
    """Mossdeep's Space Center (art 144x224), split at the roof's back edge
    (v 96). The building: a block 83 high on z 179-224, its back corners
    cut 8 (flat faces, the drawing projected); on its roof the observatory's
    dome, the dish and the small dish (low domes, 16 deep and high for 19
    wide), a solid lifted onto the roof claiming the pixels it covers.
    Behind it, on the ground at z 96: the two lattice towers (flat boxes, 96
    high, their caps cut by the map's top edge), the rocket (round, r 8,
    fins r 12, the nose at 73) and the small tower; the towers' arms, beams
    in the air, lifted along the ray on their own."""
    tex = Mound.with_ring(art, ())
    H = art.size[1]
    apx = art.load()

    def px_on(u, v):
        return apx[u, v][3] >= 128
    L = 83.0
    domes = (Volume().dome(36, 202.5 - L, 20, 16.5, 16.5)
             .dome(104, 201 - L, 19, 14.5, 14.5, p=3.0)
             .dome(134, 218 - L, 7, 5.5, 5.5))
    roofish = {}

    def on_roof(u, v):
        if (u, v) not in roofish:
            roofish[(u, v)] = v >= 96 and silhouette(domes, u, v, 160)
        return roofish[(u, v)]
    TOWERS = ((15, 33), (95, 113))

    def own(u, v):
        if v >= 96:
            return "domes" if on_roof(u, v) else "block"
        if (32 <= u < 48 or 80 <= u < 96) and v < 50:
            return "arm_hi" if v < 30 else "arm_lo"
        if any(a <= u < b for a, b in TOWERS):
            return "tower"
        return "pad"
    names = ("domes", "pad", "arm_hi", "arm_lo")
    tex, band = own_bands(tex, H, {n: (lambda n: lambda u, v: own(u, v) == n)(n) for n in names})
    parts = [Grown(PlanPrism("space_block", [(8, 179), (136, 179), (144, 187), (144, 224), (0, 224),
                                             (0, 187)], 0, 83, (2, 8, 224)), 2 * H)]
    for (a, b) in TOWERS:
        parts.append(Grown(PlanPrism("space_tower", [(a, 82), (b, 82), (b, 96), (a, 96)], 0, 96,
                                     (a + 2, a + 6, 96), top=Tile(a + 2, 2, a + 6, 6)), 2 * H))
    pad = (Volume().lathe(64, 84, [(0, 13), (12, 10), (16, 8.5), (58, 8.5), (66, 5), (74, 1), (75, 0)])
           .box(50, 78, 0, 12, 80, 97).box(32, 49, 0, 33, 84, 96))
    arm = Volume().box(31, 49, 0, 5, 0, 60).box(79, 97, 0, 5, 0, 60)
    for name, vol, mz, st, cell, lift in (("domes", domes, 201.0 - L, 1, 2, L),
                                          ("pad", pad, 89.0, 1, 2, 0.0),
                                          ("arm_hi", arm, 11.0, 1, 4, 77.0),
                                          ("arm_lo", arm, 43.0, 1, 4, 46.0)):
        other = (lambda n: lambda u, v: px_on(u, v) and own(u, v) != n)(name)
        parts.append(HeightSolid("space_" + name, tex, H, vol, depth=(0.0, float(H)), step=st,
                                 mirror_z=mz, cell=cell, tol=1, band=band[name], lift=lift,
                                 mask=None if name == "domes" else other,
                                 behind=other if name == "domes" else None))
    return parts, tex


HUT_THATCH = {(180, 189, 123), (222, 230, 164), (90, 98, 41), (156, 164, 98), (131, 139, 74)}


def pacifidlog_hut(art):
    """Pacifidlog's round hut (art 48x64): a wall 22 high (rows 41-63) on a
    rounded plan (a superellipse, the GBA drawing its sides straight down),
    and its thatch, a cone with its finial whose eave overhangs the wall: a
    solid of its own lifted 20 along the ray, with an underside. Round
    things drawn as the GBA draws them: the plan half as deep as wide."""
    tex = Mound.with_ring(art, ())
    px = art.load()
    H = art.size[1]

    def own(u, v):
        if px[u, v][3] < 128:
            return None
        return "thatch" if v < 37 or px[u, v][:3] in HUT_THATCH else "wall"
    tex, band = own_bands(tex, H, {n: (lambda n: lambda u, v: own(u, v) == n)(n)
                                   for n in ("thatch", "wall")})
    L = 21.0

    def other(n):
        return lambda u, v: own(u, v) not in (None, n)
    wall = Volume().slab(24, 52.5, 21, 11, 0, 20.5, p=8.0)
    thatch = (Volume().lathe(24, 51 - L, [(0, 25.5), (3, 25.5), (4, 24), (16, 2.5), (17.5, 0)],
                             squash=0.54)
              .lathe(24, 51 - L, [(13, 3.5), (19, 3.5), (21, 7), (28, 7), (31, 4), (33, 0)]))
    parts = []
    for name, vol, lift in (("wall", wall, 0.0), ("thatch", thatch, L)):
        parts.append(HeightSolid("hut_" + name, tex, H, vol, depth=(0.0, float(H)), step=1,
                                 mirror_z=51 - lift, cell=2, band=band[name], lift=lift,
                                 side=(4, 10) if name == "wall" else None,
                                 # the eave stands in front of the wall
                                 mask=other(name) if name == "wall" else None,
                                 behind=other(name) if name == "thatch" else None))
    return parts, tex


SOOTOPOLIS_ROCK = {(205, 205, 189), (222, 222, 213), (246, 246, 238), (148, 148, 123),
                   (172, 172, 156), (131, 131, 139)}


def sootopolis_parts(volume, depth, step=1, cell=2, side=(2, 8)):
    """A Sootopolis house off `volume`: the white rock drawn round it (the
    terrace behind, between its tiles) is the map's, laid back as patches."""
    def parts(art):
        px = art.load()
        for v in range(art.size[1]):
            for u in range(art.size[0]):
                if px[u, v][3] >= 128 and px[u, v][:3] in SOOTOPOLIS_ROCK:
                    px[u, v] = (0, 0, 0, 0)
        return small_round("house", volume, depth=depth, step=step, cell=cell, side=side)(art)
    return parts


LEAGUE_FACADE = {(238, 156, 90), (246, 197, 123), (164, 90, 49), (205, 115, 41)}


def pokemon_league(art):
    """Ever Grande's Pokemon League (art 176x128), its facade cut by the
    map's top edge: a block on a plan with its front corners cut (the
    drawing's slanting wings, x 0-16 and 160-176, their feet rising 16 to
    z 80), its front z 96 and its roof above the map (open_top); four
    columns against the doors; a half-round dais 3.5 high before them
    reaching z 124; the curved canopy over them, 13 thick at y 35-48, lifted along
    the ray on its own; and the four round bushes either side."""
    tex = Mound.with_ring(art, ())
    px = art.load()
    H = art.size[1]
    # the grass flecks below the wings' slanting feet are the ground's
    for v in range(H):
        for u in range(art.size[0]):
            if (u < 16 and v > 80 + u) or (u >= 160 and v > 80 + (175 - u)):
                if v < 96:
                    px[u, v] = (0, 0, 0, 0)
    tex = Mound.with_ring(art, ())

    def own(u, v):
        if px[u, v][3] < 128:
            return None
        if v >= 96 and (u < 48 or u >= 128):
            return "bush"
        if 48 <= u < 128 and 47 <= v < 77 and px[u, v][:3] not in LEAGUE_FACADE:
            return "canopy"
        if v >= 94 and 40 <= u < 136:
            return "dais"
        return "block"
    names = ("block", "canopy", "dais", "bush")
    tex, band = own_bands(tex, H, {n: (lambda n: lambda u, v: own(u, v) == n)(n) for n in names})

    def other(n):
        return lambda u, v: own(u, v) not in (None, n)
    L = 35.0
    COLS = ((51, 61), (63, 74), (102, 113), (115, 125))
    columns = Volume()
    for cx in (56, 68, 108, 120):
        columns.lathe(cx, 96, [(0, 5.5), (L + 1, 5.5)], squash=0.55)
    canopy = Volume().slab(88, 96 - L, 41, 16, 0, 13.5, p=2.0)
    dais = Volume().slab(88, 96, 46, 28.5, 0, 3.5, p=2.0)
    dais.hollow(dais.box, 0, 176, -1, 10, 0, 97.5)     # its top drawn from row 94
    bush = Volume()
    for cx in (24, 40, 136, 152):
        for vb_ in (111, 127):
            bush.dome(cx, vb_ - 6, 8, 6.5, 8.5)
    # the block: flat faces, the drawing projected; its roof, above the
    # map's top edge, laid with the brickwork between the windows
    parts = [Grown(PlanPrism("league_block", [(16, 24), (160, 24), (176, 40), (176, 80), (160, 96),
                                              (16, 96), (0, 80), (0, 40)], 0, 160, (24, 40, 96),
                             top=Tile(100, 20, 116, 30)), 2 * H)]

    def column(u, v):
        return 76 <= v < 94 and any(a <= u < b for a, b in COLS) and own(u, v) == "block"
    # the columns: their own pixels; the facade round them is behind them,
    # the canopy and the dais hide their ends
    parts.append(HeightSolid("league_columns", tex, H, columns, depth=(0.0, float(H)), step=1,
                             mirror_z=96.0, cell=2, band=band["block"],
                             mask=lambda u, v: own(u, v) in ("canopy", "dais"),
                             behind=lambda u, v: own(u, v) in ("block", "bush") and not column(u, v)))
    for name, vol, lift, mz, st, cell in (
            ("canopy", canopy, L, 61.0, 1, 2),
            ("dais", dais, 0.0, 96.0, 2, 4),
            ("bush", bush, 0.0, 110.0, 1, 2)):
        parts.append(HeightSolid("league_" + name, tex, H, vol, depth=(0.0, float(H)), step=st,
                                 mirror_z=mz, cell=cell, band=band[name], lift=lift,
                                 mask=other(name) if name != "canopy" else None,
                                 behind=other(name) if name == "canopy" else None))
    return parts, tex


DOME_BOXES = ((24, 184, 24, 170, 216), (40, 168, 48, 160, 200),
              (80, 128, 30, 196, 224))      # x0, x1, height, z0, z1
# The fence down each side of the dome and the pyramid is the General's
# (the `fence` spec, post by post): what the building keeps of the cells it
# shares with it is its own wall, from the cells' middle in (`keep`).
DOME_KEEP = [(0, 0, 208, 64), (16, 64, 192, 80), (8, 80, 200, 208), (16, 208, 192, 224)]
PYRAMID_KEEP = [(0, 0, 240, 32), (16, 32, 224, 48), (8, 48, 232, 160), (16, 160, 224, 176)]


def battle_dome(art):
    """The Battle Dome (art 208x224, from map row 4): the white shell, a
    dome 92 high on an ellipse 98 by 70 (its drawn top row 30 is where the
    45-degree ray grazes it) with five ribs (r 8.5) lying over it from front
    to back, their tops row 12 - a solid claiming the pixels the boxes do
    not; and of flat faces, the drawing projected: the two-tier glass base
    (24 then 48 high), the porch. The fences either side are the `fence`
    spec's."""
    tex = Mound.with_ring(art, ())
    H = art.size[1]
    apx = art.load()
    cx, cz, rx, rz, h = 104.0, 146.0, 98.0, 70.0, 92.0
    vol = Volume().dome(cx, cz, rx, rz, h, p=2.6)
    for x in (72.0, 88.0, 104.0, 120.0, 136.0):
        pts = []
        for z in range(int(cz - 60), int(cz + 61), 15):
            q = 1.0 - ((z - cz) / rz) ** 2 - ((x - cx) / rx) ** 2
            pts.append((x, h * math.sqrt(max(0.0, q)) + 2.0, float(z)))
        vol.tube(pts, 8.5, n=16)
    # inside the base: the shell's foot stands within it, not before it
    vol.hollow(vol.box, 20, 188, -1, 24, 168, 230)
    vol.hollow(vol.box, 36, 172, -1, 48, 158, 230)

    def boxed(u, v):
        return any(x0 <= u < x1 and z0 - hh <= v < z1 for (x0, x1, hh, z0, z1) in DOME_BOXES)
    parts = [Grown(PlanPrism("dome_box", [(x0, z0), (x1, z0), (x1, z1), (x0, z1)], 0, hh,
                             (x0 + 4, x0 + 12, z1)), 2 * H)
             for (x0, x1, hh, z0, z1) in DOME_BOXES]
    parts.append(HeightSolid("dome_shell", tex, H, vol, depth=(0.0, float(H)), step=2, mirror_z=cz, side=(40, 48, 150),
                             cell=4, mask=lambda u, v: apx[u, v][3] >= 128 and boxed(u, v)))
    return parts, tex


def octagon(x0, x1, z0, z1, cut):
    """A plan polygon (x, z): the rectangle with its corners cut `cut`."""
    return [(x0 + cut, z0), (x1 - cut, z0), (x1, z0 + cut), (x1, z1 - cut), (x1 - cut, z1),
            (x0 + cut, z1), (x0, z1 - cut), (x0, z0 + cut)]


def flat(make):
    """A spec's flat-faced parts (PlanPrism and kin) on the drawing with its
    grown band: their hidden faces read it, so no clear texel is a hole."""
    def parts(art):
        tex = Mound.with_ring(art, ())
        return [Grown(p, 2 * art.size[1]) for p in make()], tex
    return parts


# The Battle Frontier's halls (art 112x80, the walls' foot row 80): a block
# 38 high on z 54-80, x 0-112, its corners rounded (its flat orange roof rows
# 17-41, the red band and the walls its front); on it, in the middle, the
# vault with the Frontier's emblem: x 40-72 over the roof's whole depth, its
# arched back edge the drawing's rows 9-16 (7 high in the middle), its top
# and its front the emblem. The gate has the walk through its middle under a
# lintel (x 40-72, from 24 up: the red band's middle, rows 42-56); the
# porch's two posts and its roof are drawn on the front.
HALL_FRONT, HALL_HIGH = 80, 38
# the vault's front (x, y), from the roof up: the back edge's rows read off
# the drawing (row = roof's back z - y)
HALL_VAULT = [[(40, 38), (72, 38), (71, 39), (70, 41), (69, 42), (67, 44), (66, 45), (46, 45), (44, 43),
               (42, 41)]]
# the porch's stands on a step 2 high, x 38-74
HALL_VAULT_PORCH = HALL_VAULT + [[(38, 38), (74, 38), (74, 40), (38, 40)]]
# what of its cells is the hall: the vault over what lies behind it, the rest
# from the roof's back edge down
HALL_KEEP = [(38, 9, 74, 16), (0, 16, 112, 80)]


def frontier_hall(art, gate=False, vault=HALL_VAULT):
    """Its parts, and its texture: the drawing alone - every face it does not
    show is dressed with a stretch of its own front (the block's wall west
    of the windows, the vault's west edge down to the band), opaque all the
    way up, so the page holds no grown band for it."""
    side = (14, 20, HALL_FRONT)
    back = HALL_FRONT - 26
    crown = [PlatePrism("vault", poly, back, HALL_FRONT, (40, 46, HALL_FRONT)) for poly in vault]
    if not gate:
        return [PlanPrism("hall", rounded_plan(56, (back + HALL_FRONT) / 2, 56, 13, p=12.0, n=48),
                          0, HALL_HIGH, side)] + crown, art
    wing = lambda x0, x1: [(x0, back), (x1, back), (x1, HALL_FRONT), (x0, HALL_FRONT)]
    # the wings: the whole block's plan, square where the walk cuts it
    whole = rounded_plan(56, (back + HALL_FRONT) / 2, 56, 13, p=12.0, n=48)

    def cut(clamp):
        pts = [clamp(x, z) for (x, z) in whole]
        return [p for k, p in enumerate(pts) if p != pts[k - 1]]
    return [PlanPrism("hall", cut(lambda x, z: (min(x, 40.0), z)), 0, HALL_HIGH, side),
            PlanPrism("hall", cut(lambda x, z: (max(x, 72.0), z)), 0, HALL_HIGH, side),
            PlanPrism("lintel", wing(40, 72), 24, HALL_HIGH, side)] + crown, art


# The Battle Frontier's arches over the walk to the gate (art 80x80, their
# feet on row 80): two pillars (x 0-16, 64-80) 44 high on z 68-80, and the
# arch between them, 12 deep, its top and front read off the drawing column
# by column - the drawing's first row the arch's back edge, its band's last
# the front's foot (ARCH_DEPTH rows of top, the rest its front)
ARCH_FRONT, ARCH_DEPTH, ARCH_PILLAR = 80, 12, 44


def frontier_arch(art):
    px = art.load()
    W = art.size[0]
    side = (2, 6, ARCH_FRONT)
    back = ARCH_FRONT - ARCH_DEPTH
    parts = [PlanPrism("pillar", [(x0, back), (x0 + 16, back), (x0 + 16, ARCH_FRONT), (x0, ARCH_FRONT)],
                       0, ARCH_PILLAR, side) for x0 in (0, W - 16)]
    # each edge of a segment from the two columns beside it: the higher top,
    # the lower end - the band's own last row, or under it the few pixels of
    # its underside's shadow (drawn apart: the front stands over the gap)
    band = {}
    for u in range(8, W - 6, 2):
        rows = [v for x in (u - 1, u) for v in range(ARCH_FRONT) if px[x, v][3] >= 128]
        end = max(v for v in rows if v < min(rows) + 40)
        band[u] = (back - min(rows), max(0, ARCH_FRONT - end - 1))
    for u in range(8, W - 8, 2):
        (ta, ba), (tb, bb) = band[u], band[u + 2]
        parts.append(PlatePrism("arch", [(u, ba), (u + 2, bb), (u + 2, tb), (u, ta)], back,
                                ARCH_FRONT, side))
    return parts


def rounded_plan(cx, cz, rx, rz, p=4.0, n=32):
    """A superellipse plan polygon (x, z), its corners rounded."""
    pts = []
    for k in range(n):
        t = 2 * math.pi * k / n
        c, s_ = math.cos(t), math.sin(t)
        pts.append((cx + rx * math.copysign(abs(c) ** (2 / p), c),
                    cz + rz * math.copysign(abs(s_) ** (2 / p), s_)))
    return pts


def sand_specks(make):
    """Drops the Battle Frontier sand's loose specks from a drawing (the
    ground's own flecks, painted on its tiles beside the building)."""
    def parts(art):
        px = art.load()
        for v in range(art.size[1]):
            for u in range(art.size[0]):
                if px[u, v][3] >= 128 and px[u, v][:3] in ((238, 230, 164), (213, 180, 106)):
                    px[u, v] = (0, 0, 0, 0)
        return make(art)
    return parts


def battle_pyramid():
    """The Battle Pyramid (art 240x176, from map 51,5): thirteen tiers 8
    high, each 8 in on the sides and 4 in front and back (the base x 8-232,
    z 57-160), the portico before the entrance and its tower. The fences
    either side are the `fence` spec's."""
    parts = [PlanPrism("tier", [(8 + 8 * i, 56 + 4 * i), (232 - 8 * i, 56 + 4 * i), (232 - 8 * i, 160 - 4 * i),
                                (8 + 8 * i, 160 - 4 * i)], 8 * i, 8 * i + 8, (60, 76, 160 - 4 * i))
             for i in range(13)]
    parts.append(PlanPrism("steps", [(100, 160), (140, 160), (140, 176), (100, 176)], 0, 4, (104, 112, 176)))
    parts.append(PlanPrism("portico", [(40, 140), (200, 140), (200, 160), (40, 160)], 0, 20, (60, 76, 160)))
    parts.append(PlanPrism("gate", [(104, 120), (136, 120), (136, 160), (104, 160)], 0, 40, (60, 76, 160)))
    return parts


def battle_factory():
    """The Battle Factory (art 240x144): octagonal towers side by side, flat
    topped, their corners cut - the middle one (x 88-152) 101 high, its top
    rows 2-42; the outer pairs 80 high, their tops rows 16-56 - and a fence
    line down the east end."""
    side = (100, 108, 144)
    parts = [PlanPrism("mid", octagon(87, 153, 101, 144, 8), 0, 101, side),
             PlanPrism("fence", [(0, 60), (8, 60), (8, 144), (0, 144)], 0, 14, (0, 8, 144))]
    for (a, b, cut) in ((8, 64, 8), (176, 232, 8), (60, 92, 6), (148, 180, 6)):
        parts.append(PlanPrism("tower", octagon(a, b, 96, 136, cut), 0, 80, side))
    parts.append(PlanPrism("fence", [(232, 60), (240, 60), (240, 144), (232, 144)], 0, 14, (232, 240, 144)))
    return parts


# The Battle Pike's Seviper (art 80x208): its tail the drawing's rows above
# 58 and its root's east edge down to row 74 (pike_tail), a blade standing
# up out of its back at z PIKE_TAIL_Z
PIKE_TAIL_ROWS = 74
PIKE_TAIL_Z = 132


def pike_tail(u, v):
    return v < 59 or (u >= 60 and v < PIKE_TAIL_ROWS)


def battle_pike_volume():
    """The Seviper's head and body, lying on the ground, round domes fitted
    to the drawing's outline (where a dome stood higher than the outline,
    the 45-degree cut left its sides as faces edge-on to the view): the
    body 80 high on z 94-206, x 14-66, the yellow plates down its back; the
    head wider and lower before it, 56 high on z 144-208, x 6-74, its front
    the open mouth (the door) and its top the eyes; the yellow fins along
    its sides, low humps against it."""
    vol = Volume()
    vol.dome(40, 150, 32, 60, 86, p=3.0)
    vol.dome(40, 176, 34, 32, 56, p=3.0)
    for (x, z, h) in ((8, 104, 30), (72, 104, 30), (9, 128, 26), (71, 128, 26), (9, 152, 26),
                      (71, 152, 26)):
        vol.dome(x, z, 9, 12, h)
    return vol


class Blade:
    """A flat thing standing up across the plan (the Seviper's tail): a card
    at depth `z` facing the view, over columns u0-u1 from `y0` up to `y1`, its
    drawing projected and read `voff` rows down, where its own pixels lie
    alone (the transparency there cuts its outline). Two faces, back to
    back: from behind it shows the same drawing, not nothing."""

    def __init__(self, name, u0, u1, z, y0, y1, voff):
        self.name, self.u0, self.u1, self.z, self.voff = name, u0, u1, z, voff
        self.y0, self.y1 = y0, y1

    def emit(self, mesh):
        z, o = self.z, self.voff
        quad = [(self.u0, self.y0), (self.u1, self.y0), (self.u1, self.y1), (self.u0, self.y1)]
        front = [(x, y, z, x, z - y + o) for (x, y) in quad]
        mesh.poly(front, SHADE_ART, self.name + ".card")
        mesh.poly(front[::-1], SHADE_ART, self.name + ".card")


def battle_pike(art):
    """The Battle Pike: the head and body a height field over its plan
    (battle_pike_volume), the drawing cut out of it; the tail a Blade at
    PIKE_TAIL_Z, its pixels laid alone below the texture's bands."""
    H, W = art.size[1], art.size[0]
    tex = Mound.with_ring(art, ())
    tex, band = own_bands(tex, H, {"body": lambda u, v: not pike_tail(u, v)})
    out = Image.new("RGBA", (W, tex.size[1] + PIKE_TAIL_ROWS), (0, 0, 0, 0))
    out.paste(tex, (0, 0))
    apx, opx = art.load(), out.load()
    mine = [(u, v) for v in range(PIKE_TAIL_ROWS) for u in range(W)
            if apx[u, v][3] >= 128 and pike_tail(u, v)]
    for (u, v) in mine:
        opx[u, tex.size[1] + v] = apx[u, v]
    cols = [u for (u, v) in mine]
    top = min(v for (u, v) in mine)
    body = HeightSolid("pike", out, H, battle_pike_volume(), depth=(0.0, 208.0), step=2,
                       mirror_z=150.0, cell=2, tol=1, mask=pike_tail, band=band["body"],
                       side=(30, 36, 112))
    blade = Blade("pike_tail", min(cols), max(cols) + 1, PIKE_TAIL_Z, PIKE_TAIL_Z - PIKE_TAIL_ROWS,
                  PIKE_TAIL_Z - top, tex.size[1])
    return [body, blade], out


def clear_rock(art):
    """The white rock drawn round a Sootopolis building is the map's (laid
    back as patches): clear in its drawing."""
    px = art.load()
    for v in range(art.size[1]):
        for u in range(art.size[0]):
            if px[u, v][3] >= 128 and px[u, v][:3] in SOOTOPOLIS_ROCK:
                px[u, v] = (0, 0, 0, 0)
    return art


# The spire's eight-sided pyramid of dark stone: its base the octagon x 4-44,
# z 39-73 (drawn rows 17-51) on the white base's top (y 22), its apex row 8.
SPIRE_PLAN = [(13, 39), (35, 39), (44, 50), (44, 65), (37, 73), (11, 73), (4, 65), (4, 50)]
SPIRE_APEX = (24, 48, 56)


def spire_parts(gate_side=None):
    """A spire house's parts, on its drawing 48x80 (the apex's row on top).

    The white base is an octagon 22 high (front rows 56-78, flat top rows
    18-56: z 40-79, the corners cut 8). The gated one's front, drawn a row
    lower (z 80), is two dark pillars (x 8-16, 32-40) either side of the
    gate, and the gate stands 4 back between them (z 76) under the lintel
    (y 17-22, the gate's top row 59), on a threshold 1 high. The sides take
    the plain front either side of the window (columns 9-16 on row 79): the
    gated one has no such stretch: `gate_side` builds it, its sides reading
    that tile (a band of the windowed one's front appended to its texture)."""
    if gate_side is None:
        base = [PlanPrism("base", octagon(0, 48, 40, 79, 8), 0, 22, (9, 16, 79))]
    else:
        side = gate_side
        whole = [(8, 40), (40, 40), (48, 48), (48, 71), (40, 79), (40, 80), (8, 80), (8, 79),
                 (0, 71), (0, 48)]
        notched = whole[:6] + [(32, 80), (32, 76), (16, 76), (16, 80)] + whole[6:]
        base = [PlanPrism("base", notched, 0, 17, side, open_top=True),
                PlanPrism("lintel", whole, 17, 22, side),
                PlanPrism("step", [(16, 76), (32, 76), (32, 80), (16, 80)], 0, 1, side)]
    return base + [PlanPyramid("spire", SPIRE_PLAN, 22, SPIRE_APEX)]


def sootopolis_spire(art):
    """Sootopolis's spire house (art 48x80): see spire_parts. All of flat
    faces, the drawing projected onto them."""
    H = clear_rock(art).size[1]
    return [Grown(p, 2 * H) for p in spire_parts()], Mound.with_ring(art, ())


# the windowed spire house, whose plain front dresses the gated one's sides
SPIRE_WINDOW = ("LAYOUT_SOOTOPOLIS_CITY", 22, 1)
SOOTOPOLIS_GROUND = [0x2D9, 0x2E1, 0x2E2, 0x2D3, 0x2DA]


def sootopolis_spire_gate(art):
    """The gated spire house: its texture, the drawing's three bands and,
    under them, the windowed house's plain front (columns 9-16, rows 57-79),
    which its sides read."""
    H = clear_rock(art).size[1]
    lid, x, y = SPIRE_WINDOW
    layout = LayoutArt(lid)
    window = clear_rock(layout.building_art(x, y, 3, 5, layout.ground_tiles(SOOTOPOLIS_GROUND),
                                            ground_px=layout.ground_pixels(SOOTOPOLIS_GROUND)))
    tex = Mound.with_ring(art, ())
    out = Image.new("RGBA", (tex.size[0], tex.size[1] + 24), (0, 0, 0, 0))
    out.paste(tex, (0, 0))
    out.paste(window.crop((9, 57, 16, 80)), (0, 3 * H))
    # a side reads its rows 2H down (Grown): the band's rows H-H+23 here
    return [Grown(p, 2 * H) for p in spire_parts((0, 7, H + 23))], out


# Sootopolis's white walls (sootopolis_wall): along a row (240-242), its
# corners turning south (246, 247, 25C, 26E, 26F), down a column (254, 255,
# 25E, 25F, 256) and turning east (25D). Not their ends drawn down the face
# beside a flight of stairs (262, 263): a face is upright, and a wall exact
# at 45 degrees on it could not stand on the ground at its foot - they stay
# painted on the face, and the wall above ends at the terrace's edge.
SOOTOPOLIS_WALL = {0x240, 0x241, 0x242, 0x246, 0x247, 0x25C, 0x26E, 0x26F, 0x254, 0x255, 0x25E,
                   0x25F, 0x256, 0x25D}


# The Cave of Origin's doorway (art 80x64 from map cell 29,15): a frame of the
# walls' white stone round the cave's mouth. The cliff it is set in is the
# relief's, upright at z 32 from the plaza (y 0) to its top (y 32): its two
# rows of cells stay the relief's (unowned), and the frame stands out of it.
CAVE_ORIGIN = ("LAYOUT_SOOTOPOLIS_CITY", 29, 15, 5, 4)
CAVE_ORIGIN_GROUND = [0x071]
# the crater's red rock, the plaza round the doorway
CRATER_ROCK = {(189, 148, 139), (222, 180, 164), (156, 115, 115), (238, 213, 205)}


def cave_origin_art():
    """The doorway's whole drawing, its cliff's rows too (an owned spec's art
    holds only its own cells), less the rock round the frame and the cliff
    inside it (x 16-64, rows 9-31), the relief's to draw."""
    lid, x, y, w, h = CAVE_ORIGIN
    layout = LayoutArt(lid)
    ground = CAVE_ORIGIN_GROUND + SOOTOPOLIS_GROUND
    art = clear_rock(layout.building_art(x, y, w, h, layout.ground_tiles(ground),
                                         ground_px=layout.ground_pixels(ground)))
    px = art.load()
    for v in range(art.size[1]):
        for u in range(art.size[0]):
            # the plaza, and the cliff with the cave's mouth inside the frame
            if (px[u, v][3] >= 128 and px[u, v][:3] in CRATER_ROCK) or (16 <= u < 64 and 9 <= v < 32):
                px[u, v] = (0, 0, 0, 0)
    return art


def cave_origin(art):
    """The frame, of flat faces with the drawing projected on them: an open
    frame out of the cliff's top over the plaza, on two legs.

    - the lintel along the cliff's top (x 9-71), 7 high and 2 out of it
      (its top drawn rows 0-2, its front 2-9);
    - from its ends two beams run out over the plaza (x 9-16, 64-71), their
      tops 25 up: drawn down to row 37 in the shade of a top, as the
      lintel's, not of a front;
    - the beam across their ends (x 9-71, z 57-62, 18-25 up: its top drawn
      rows 32-37, its front 37-44) - drawn as a sill before the cave, it is
      the frame's front: under it the plaza runs on to the cliff;
    - the two legs under its corners (x 9-16, 64-71, z 57-62), down to the
      plaza at row 62, drawn in the shade of a front.

    The cave's mouth and the rock round it are the cliff's own drawing."""
    art = cave_origin_art()
    H = art.size[1]
    side = (11, 15, 62)    # a beam's front: its stone between its outlines
    top, beam = 25, 18
    parts = [PlanPrism("lintel", [(9, 32), (71, 32), (71, 34), (9, 34)], top, 32, side),
             PlanPrism("front", [(9, 57), (71, 57), (71, 62), (9, 62)], beam, top, side)]
    for (x0, x1) in ((9, 16), (64, 71)):
        parts.append(PlanPrism("beam", [(x0, 34), (x1, 34), (x1, 57), (x0, 57)], beam, top, side))
        parts.append(PlanPrism("leg", [(x0, 57), (x1, 57), (x1, 62), (x0, 62)], 0, beam, side))
    return [Grown(p, 2 * H) for p in parts], Mound.with_ring(art, ())


def flower_box(art):
    """Verdanturf's flower box (art 16x32, cells 2B4 over 2BC): a box 8 high
    on z 8-32, the flowers its top (drawn rows 0-24 inside the outline), its
    planks the front (rows 24-32)."""
    H = art.size[1]
    parts = [PlanPrism("box", [(0, 8), (16, 8), (16, 32), (0, 32)], 0, 8, (2, 12, 32))]
    return [Grown(p, 2 * H) for p in parts], Mound.with_ring(art, ())


def sootopolis_posts(art):
    """The stone posts before the Cave of Origin (art 16x32, two cells of
    metatile 280), two to a cell: each 8 wide, a box 8 high on z 8-16 of
    its cell - its top (the cream cap with its white edge) drawn rows 0-8,
    its front, from the dark lip, rows 8-16, the last its dark foot."""
    px = art.load()
    for v in range(art.size[1]):
        for u in range(art.size[0]):
            if px[u, v][3] >= 128 and px[u, v][:3] in CRATER_ROCK:
                px[u, v] = (0, 0, 0, 0)
    H = art.size[1]
    parts = [PlanPrism("post", [(x, z + 8), (x + 8, z + 8), (x + 8, z + 16), (x, z + 16)], 0, 8,
                       (x + 1, x + 7, z + 16))
             for z in (0, 16) for x in (0, 8)]
    return [Grown(p, 2 * H) for p in parts], Mound.with_ring(art, ())


def sootopolis_flat_house(art, gate=False):
    """Sootopolis's flat-roofed white house (art 48x64): a block 33 high on
    z 33-64 (its front rows 31-63; the roof's rim, its lip at rows 29-30),
    and on the roof a raised slab 4 high (x 5-43, z 42-62: its top with the
    skylight drawn rows 5-25, its front rows 25-29).

    The gated one's front is set back to z 60 (the block 29 high, z 29-60)
    and its porch stands out of it in two steps: the outer frame x 9-39 out
    to z 62, 22 high (its top drawn rows 38-40), and the bay with the gate
    x 16-32 out to z 64, flush with the roof (its top rows 31-35); a plinth
    1 high runs along the foot, z 60-64, as drawn."""
    H = clear_rock(art).size[1]
    if not gate:
        parts = [PlanPrism("house", [(0, 33), (48, 33), (48, 64), (0, 64)], 0, 33, (8, 15, 64)),
                 PlanPrism("roof", [(5, 42), (43, 42), (43, 62), (5, 62)], 33, 37, (8, 15, 64))]
    else:
        # its sides: the plain front inside its corner posts, columns 3-5
        parts = [PlanPrism("house", [(0, 29), (48, 29), (48, 60), (0, 60)], 0, 29, (3, 6, 60)),
                 PlanPrism("roof", [(5, 38), (43, 38), (43, 58), (5, 58)], 29, 33, (3, 6, 60)),
                 PlanPrism("frame", [(9, 60), (39, 60), (39, 62), (9, 62)], 0, 22, (12, 15, 62)),
                 PlanPrism("bay", [(16, 60), (32, 60), (32, 64), (16, 64)], 0, 29, (3, 6, 64)),
                 PlanPrism("plinth", [(0, 60), (48, 60), (48, 64), (0, 64)], 0, 1, (3, 6, 64))]
    return [Grown(p, 2 * H) for p in parts], Mound.with_ring(art, ())


def sootopolis_gate_house(art):
    return sootopolis_flat_house(art, gate=True)


# Route 109's beach parasol (art 32x48): a round canopy (v 3-31) on a pole
# (v 32-39). The pole's foot is its last row: it stands at z 38.5. The
# canopy's rim, drawn as a ring 32 wide round v 19, is 19.25 high over it,
# its hub (v 13, where the panels meet) 6 higher. Its shadow drawn on the
# sand (v 27-47, these colours) is the ground's (`ground_rgb`): the sand
# lies plain under it and the console casts the canopy's own shadow.
PARASOL_SHADOW = {(205, 156, 82), (213, 180, 106), (222, 205, 131)}


def parasol(art):
    """Two parts of smooth faces, each with the drawing projected from a
    band of its own pixels (own_bands): the canopy a shallow cone, round
    (ProjCanopy: its rim lifted 3.5 at the front, where the drawing ends
    higher than a round rim would - exact at 45 degrees) and closed
    underneath; the pole a thin turned post up into it. (Solids of height
    fields had stood the canopy in fins and its drawn shadow in a mound.)"""
    tex = Mound.with_ring(art, ())
    px = art.load()
    W, H = art.size

    def own(u, v):
        if px[u, v][3] < 128:
            return None
        return "canopy" if v <= 31 else "pole"
    tex, band = own_bands(tex, H, {n: (lambda n: lambda u, v: own(u, v) == n)(n)
                                   for n in ("canopy", "pole")})
    # each band cut to the drawing where the part's edge is drawn: the
    # canopy's to its own pixels (its polygon stands a pixel past the drawn
    # stair here and there), the pole's round foot (its last row) to its own
    tpx = tex.load()
    for v in range(H):
        for u in range(W):
            o = own(u, v)
            if o != "canopy":
                tpx[u, v + band["canopy"] * H] = (0, 0, 0, 0)
            if v == 39 and o != "pole":
                tpx[u, v + band["pole"] * H] = (0, 0, 0, 0)
    return [ProjCanopy("canopy", 16, 38.5, 16.2, 19.25, 25.25, front=3.5, y_under=20.5,
                       offset=band["canopy"] * H, seg=24),
            ProjLathe("pole", 16, 38.5, [(0, 2), (21, 2)], seg=8, offset=band["pole"] * H)], tex


def sun_lounger(art):
    """Route 109's sun lounger (art 16x48, drawn v 9-37, x 1-14): a frame
    on four legs, open underneath. The seat (the footrest and the middle
    cushion, v 20-33) flat 5 high, 2 thick, its front (v 34-35, the rail
    between the legs) at z 39; the back (v 9-19) a cushion raised 40
    degrees from the hinge (z 24.5), 7.5 long, 1.2 thick; the front legs
    (v 35-37: x 1-5 and 10-14) stand at z 38, the back ones are struts
    under the back, inside its drawn outline, in the frame's colour (the
    drawing never shows them). Every face takes the drawing projected
    (ProjSection): exact at 45 degrees, and from the side a lounger, not a
    block (a height field had stood it on the sand whole)."""
    H = art.size[1]
    tex = Mound.with_ring(art, ())
    a = math.radians(40)
    t, L = 1.2, 7.5
    hz, top = 24.5, 5.0
    ez, ey = hz - L * math.cos(a), top + L * math.sin(a)
    nz, ny = math.sin(a), math.cos(a)
    seat = top - 2
    # where the back's underside meets the seat's
    k = (top - t * ny - seat) / math.sin(a)
    frame = [(39, seat), (39, top), (hz, top), (ez, ey), (ez - t * nz, ey - t * ny),
             (hz - t * nz + k * math.cos(a), seat)]

    def under_back(z):
        """The height of the back's underside over z."""
        return top - t * ny + (hz - t * nz - z) * math.tan(a)
    parts = [ProjSection("frame", frame, 1, 14, grown=2 * H)]
    for (x0, x1) in ((1, 5), (10, 14)):
        parts.append(ProjSection("leg", [(36.5, 0), (38, 0), (38, seat), (36.5, seat)], x0, x1,
                                 grown=2 * H))
    for (x0, x1) in ((2, 3.5), (11.5, 13)):
        parts.append(ProjSection("strut", [(20.5, 0), (21.5, 0), (21.5, under_back(21.5)),
                                           (20.5, under_back(20.5))], x0, x1, uv=(1.5, 20.5)))
    return parts, tex


def beach_mat(art):
    """Route 109's air mattress (art 16x48, drawn v 9-36, x 0-16), flat on
    the sand: its tubes 2 thick (the front, v 35-36, at z 37), the pillow
    across its head a tube 2.5 round on the same floor, 4.5 high (v 9-15).
    Every face takes the drawing projected (ProjSection); the pillow's back,
    which the view grazes, reads the grown band (`graze`): the drawing
    rounds the pillow's top corners, and they were holes from the side."""
    H = art.size[1]
    r, y0 = 2.5, 2.0
    zc = 9 + y0 + r * math.sqrt(2)      # the pillow's top at 45 degrees is row 9
    arc = [(zc + r * math.cos(math.pi * k / 8), y0 + r * math.sin(math.pi * k / 8)) for k in range(9)]
    sec = [(37, 0), (37, y0)] + arc + [(zc - r, 0)]
    return [ProjSection("mat", sec, 0, 16, grown=2 * H, graze=0.6)], Mound.with_ring(art, ())


def abandoned_ship(art):
    """Route 108's wreck, stranded on its shoal (art 96x64): a hull 8 high
    on a long plan (its deck v 16-36 = z 24-44; the side, then the green
    of its bottom, v 36-44), a cabin 15 high (roof v 10-29), a slatted
    house forward, a landing stage floating before it (v 47-56) and the
    hull's dark water at its foot (v 44-47)."""
    tex = Mound.with_ring(art, ())
    vol = Volume().slab(44, 34, 43, 10, -1, 8, p=4.0)
    vol.box(22, 61, 0, 15, 25, 44)
    vol.box(61, 75, 0, 11, 23, 44)
    vol.box(29, 51, 0, 2, 47, 56)
    vol.slab(45, 45.2, 43, 3.2, 0, 0.3, p=8.0)
    return [HeightSolid("wreck", tex, art.size[1], vol, depth=(0.0, 64.0), step=1,
                        mirror_z=34)], tex


def two_storey_house(width, front_z, lower, upper, upper_x, storey, pent, facade, side,
                     band):
    """Littleroot's build in other towns' tiles: a hipped lower roof over
    the whole width and a narrower upper storey coming up through it with
    its own hipped roof. `lower`/`upper` = dict(cap, teeth, slope, course,
    fascia, wrap, fascia_wrap, ridge_wrap) of art rows; `upper_x` the upper
    roof's span, `storey` the upper wall's rows, `pent` the lower roof's
    rows drawn before the upper wall, `facade` the ground floor's rows,
    `side`/`band` the columns the ground floor's and the upper wall's sides
    are dressed with. Real depth: z 16 to `front_z`."""
    back_z, overhang = 16, 2
    lo, hi = lower, upper
    roof_lo = HipRoof(
        "roof_lo", 0, width, zf=front_z + overhang, zb=back_z - overhang,
        y0=front_z + overhang - lo["fascia"][1],
        fascia=Strip(lo["fascia"], wrap=lo["fascia_wrap"]),
        slope=Strip(lo["slope"], repeat=lo["course"], wrap=lo["wrap"]),
        teeth=lo["teeth"], cap=lo["cap"], pitch=PITCH, run=8,
        ridge_u=(0, 16), end_tile=Tile(lo["wrap"][0], lo["teeth"][0], lo["wrap"][0] + 8,
                                        lo["teeth"][1]),
        ridge_wrap=lo["ridge_wrap"])
    z_wall, y_base = roof_lo.slope_point(pent)
    zf_hi = z_wall + overhang
    y0_hi = zf_hi - hi["fascia"][1]
    zb_hi = (roof_lo.zf + roof_lo.zb) - zf_hi
    roof_hi = HipRoof(
        "roof_hi", upper_x[0], upper_x[1], zf=zf_hi, zb=zb_hi, y0=y0_hi,
        fascia=Strip(hi["fascia"], wrap=hi["fascia_wrap"]),
        slope=Strip(hi["slope"], repeat=hi["course"], wrap=hi["wrap"]),
        teeth=hi["teeth"], cap=hi["cap"], pitch=PITCH, run=6,
        ridge_u=upper_x, end_tile=Tile(hi["wrap"][0], hi["teeth"][0], hi["wrap"][0] + 8,
                                        hi["teeth"][1]),
        ridge_wrap=hi["ridge_wrap"])
    storey_back = zb_hi + overhang
    wall_hi = Tile(band[0], storey[0], band[1], storey[1], top=y0_hi)
    upper_storey = Prism(
        "storey", upper_x[0] + 1, upper_x[1] - 1,
        [(z_wall, y_base), (z_wall, y0_hi), (storey_back, y0_hi), (storey_back, y_base)],
        edges={0: Proj(*storey), 2: wall_hi}, skip=(1, 3),
        caps=[Band(-64, 200, wall_hi, z_wall, front=wall_hi, back=wall_hi, z1=storey_back)])
    wall_top = roof_lo.y0
    wall = Tile(side[0], facade[0], side[1], facade[1], top=wall_top - 2)
    base = Prism(
        "ground_floor", 2, width - 2,
        [(front_z, 0), (front_z, wall_top), (back_z, wall_top), (back_z, 0)],
        edges={0: Proj(*facade), 2: wall}, skip=(1, 3),
        caps=[Band(0, wall_top - 2, wall, front_z, front=wall, back=wall, z1=back_z),
              Band(wall_top - 2, wall_top + 1,
                   Tile(side[0], facade[0], side[1], facade[0] + 1, top=wall_top), front_z)])
    return [base, roof_lo, upper_storey, roof_hi]


class GableRoof(HipRoof):
    """A HipRoof with no hips: its front and back slopes run to its ends,
    and the ends are gables - the fascia along their foot, the wall `gable`
    (a Tile) over it up to the slopes, the ridge's end tiles over that. The
    ridge's back (its teeth's north face) is laid too: a gable shows it."""

    def __init__(self, *args, gable=None, front=None, pieces=None, **kw):
        kw["run"] = 0
        HipRoof.__init__(self, *args, **kw)
        self.gable = gable
        # the front fascia's own strip, its columns as drawn (the fascia
        # round the other sides wraps its clean columns)
        self.front = front
        # [(x0, x1, strip)]: the slopes laid in pieces across, each with its
        # own strip (the drawn columns where they show, a clean course
        # wrapped where the drawing has something else)
        self.pieces = pieces

    def emit(self, mesh):
        from voxel_building import Mesh, PlanPrism, tile_pieces
        tmp = Mesh()
        HipRoof.emit(self, tmp)
        drop = (".hip_w", ".hip_e") + ((".fascia_s",) if self.front is not None else ()) \
            + ((".slope_s", ".slope_n") if self.pieces is not None else ())
        for (tri, shade, tag) in tmp.tris:
            if not tag.endswith(drop):
                mesh.tri(tri[0], tri[1], tri[2], shade, tag)
        x0, x1, zf, zb, y0, ye = self.x0, self.x1, self.zf, self.zb, self.y0, self.ye
        zr, zq, yr = self.z_rf, self.z_rb, self.yr
        if self.front is not None:
            strip_face(mesh, [(x0, y0, zf), (x1, y0, zf), (x1, ye, zf), (x0, ye, zf)],
                       (0, y0, zf), (1, 0, 0), (0.0, 1.0, 0.0), self.front, SHADE_ART,
                       self.name + ".fascia_s")
        for (xa, xb, sl) in self.pieces or ():
            t_s = _unit((0, self.rise, -(zf - zr)))
            t_n = _unit((0, self.rise, zf - zr))
            strip_face(mesh, [(xa, ye, zf), (xb, ye, zf), (xb, yr, zr), (xa, yr, zr)],
                       (0, ye, zf), (1, 0, 0), t_s, sl, SHADE_ART, self.name + ".slope_s")
            strip_face(mesh, [(xb, ye, zb), (xa, ye, zb), (xa, yr, zq), (xb, yr, zq)],
                       (0, ye, zb), (1, 0, 0), t_n, sl, SHADE_BACK, self.name + ".slope_n")
        # the ridge's back, its teeth turned north
        for (tri, shade, tag) in tmp.tris:
            if tag.endswith(".teeth"):
                back = [(x, y, zq, u, v) for (x, y, z, u, v) in tri]
                PlanPrism._tri(mesh, back, (0.0, 0.0, -1.0), SHADE_BACK, self.name + ".teeth_n")
        # the gables: a pentagon from the eave's top up to the ridge's foot
        sec = [(zb, ye), (zf, ye), (zr, yr), (zq, yr)]
        for x, nx, shade in ((x0, -1.0, SHADE_WEST), (x1, 1.0, SHADE_EAST)):
            for k in range(1, len(sec) - 1):
                tri = [sec[0], sec[k], sec[k + 1]]
                for piece in tile_pieces([(zf - z, yr - y) for (z, y) in tri], self.gable, None, None):
                    pts = [(x, yr - t, zf - s_, u, v) for s_, t, u, v in piece]
                    for j in range(1, len(pts) - 1):
                        PlanPrism._tri(mesh, (pts[0], pts[j], pts[j + 1]), (nx, 0.0, 0.0), shade,
                                       self.name + ".gable")


def trick_house():
    """Route 110's Trick House (80 x 96), a two-storey house in pink metal:

        rows  0- 8  upper cap             rows 16-24  lower cap (the wings)
        rows  8-14  upper ridge's front   rows 24-30  lower ridge's front
        rows 14-48  upper slope           rows 30-64  lower slope
        rows 48-53  upper eave            rows 64-72  lower eave, the full width
        rows 53-64  upper storey          rows 72-96  stone ground floor

    Real depth: five collision rows, z 16-96, the walls standing on all of
    them. A full-width lower roof, gabled (its wings' slopes run to their
    ends as drawn, no hips), and the upper storey coming up through it a
    tile in from its eaves, front and back, with its own gabled roof. The
    slopes lay their drawn rows from the eave and then more courses of
    them; the lower roof is flatter (LOW_PITCH), so its ridge stays inside
    the upper storey. The upper storey's front is its drawn band of windows
    on a wall standing where the lower roof's slope meets it, the lower
    slope in front of it a clean course of the wings' flutes. Exact at 45
    degrees: the facade, the lower eave and the wings' drawn slope. (It was
    built exact to its top: roofs at 45 degrees to the drawn caps, and
    behind them a fall at 45 degrees down to the back wall - seen from the
    side, a pyramid; then the upper storey stood on the lower eaves, front
    and back.) The gables and the walls are the facade's stone."""
    front_z, back_z, overhang, inset = 96, 16, 2, 16
    stone = Tile(3, 72, 8, 96)
    upper_stone = Tile(32, 56, 40, 63)
    zf_lo, zb_lo = front_z + overhang, back_z - overhang
    slope_lo = Strip((30, 64), repeat=(32, 40))
    flutes = Strip((30, 64), repeat=(32, 40), wrap=(2, 10))
    roof_lo = GableRoof(
        "roof_lo", 0, 80, zf=zf_lo, zb=zb_lo, y0=zf_lo - 72,
        fascia=Strip((64, 72), wrap=(2, 10)), front=Strip((64, 72)), slope=slope_lo,
        pieces=[(0, 16, slope_lo), (16, 64, flutes), (64, 80, slope_lo)],
        teeth=(24, 30), cap=(16, 24), pitch=TRICK_LOW_PITCH, ridge_u=(0, 16),
        end_tile=Tile(2, 24, 10, 30), ridge_wrap=(2, 10), gable=stone)
    # the upper storey: a tile in from the lower eaves, standing where the
    # lower slope reaches it, its drawn wall (11 rows) and two more under them
    z_front, z_back = zf_lo - inset, zb_lo + inset
    y_base = roof_lo.ye + inset * math.tan(math.radians(TRICK_LOW_PITCH))
    y_top = y_base + 13
    assert y_top > roof_lo.yt, "the lower ridge must stay inside the upper storey"
    roof_hi = GableRoof(
        "roof_hi", 16, 64, zf=z_front + overhang, zb=z_back - overhang, y0=y_top,
        fascia=Strip((48, 53), wrap=(18, 58)), front=Strip((48, 53)),
        slope=Strip((14, 48), repeat=(16, 24)),
        teeth=(8, 14), cap=(0, 8), pitch=PITCH, ridge_u=(16, 64),
        end_tile=Tile(18, 8, 26, 14), ridge_wrap=(18, 58), gable=upper_stone)
    wall_hi = Tile(*upper_stone.rect, top=y_top)
    storey = Prism(
        "storey", 18, 62,
        [(z_front, y_base), (z_front, y_top), (z_back, y_top), (z_back, y_base)],
        edges={0: Tile(18, 53, 62, 64, s0=18), 2: wall_hi}, skip=(1, 3),
        caps=[Band(-64, 200, wall_hi, z_front, front=wall_hi, back=wall_hi, z1=z_back)])
    wall_top = roof_lo.y0
    wall = Tile(*stone.rect, top=wall_top)
    base = Prism(
        "ground_floor", 2, 79.9,
        [(front_z, 0), (front_z, wall_top), (back_z, wall_top), (back_z, 0)],
        edges={0: Proj(70, 96), 2: wall}, skip=(1, 3),
        caps=[Band(0, wall_top, wall, front_z, front=wall, back=wall, z1=back_z)])
    return [base, roof_lo, storey, roof_hi]


# the Trick House's lower roof: flatter than PITCH, so that its ridge, over
# the middle of the house, stays under the top of the upper storey
TRICK_LOW_PITCH = 15.0
TRICK_EXACT = [(2, 72, 78, 96), (0, 64, 80, 72), (0, 30, 16, 64), (64, 30, 80, 64)]


def lilycove_motel():
    """Lilycove's Cove Lily Motel (96 x 80): the same build thatched, its
    upper storey (x 17-80) standing behind four rows of the lower roof:

        rows  0- 9  upper ridge cap           rows 16-25 lower ridge cap (wings)
        rows  9-16  upper ridge front         rows 25-31 lower ridge front
        rows 16-35  upper thatch              rows 31-50 lower thatch
        rows 35-40  upper eave                rows 50-56 lower eave
        rows 40-46  upper storey's windows    rows 56-80 ground floor
    """
    return two_storey_house(
        96, 80,
        dict(cap=(16, 25), teeth=(25, 31), slope=(31, 50), course=(32, 40), fascia=(50, 56),
             wrap=(18, 58), fascia_wrap=(18, 58), ridge_wrap=(2, 10)),
        dict(cap=(0, 9), teeth=(9, 16), slope=(16, 35), course=(16, 24), fascia=(35, 40),
             wrap=(18, 58), fascia_wrap=(18, 58), ridge_wrap=(22, 62)),
        (17, 80), (40, 46), 4, (56, 80), (3, 6), (30, 38))


def pink_house(width, x0=1):
    """Mauville's pink-roofed house, any width: the kit house's build in
    the Trick House's corrugated metal (its flutes at x 2-5 + 8k). Rows:

        rows  0- 8  ridge cap (period 8 from x 9)
        rows  8-14  ridge front
        rows 14-32  corrugated slope
        rows 32-38  eave
        rows 38-64  facade, yellow posts at its ends

    Real depth: three collision rows, z 16-64, like the kit house.

    `x0`: the facade's first column (Rydel's shop is drawn a column further
    in: its walls from column 1 left that column clear up the corner)."""
    front_z, back_z = 64, 16
    overhang = 2
    roof = HipRoof(
        "roof", 0, width, zf=front_z + overhang, zb=back_z - overhang, y0=28,
        fascia=Strip((32, 38), wrap=(10, 18)),
        slope=Strip((14, 32), repeat=(16, 24), wrap=(2, 10)),
        teeth=(8, 14), cap=(0, 8), pitch=22.0, run=6,
        ridge_u=(0, width), end_tile=Tile(10, 8, 18, 14), ridge_wrap=(9, 17))
    wall_top = 28
    post = Tile(x0 + 1, 38, x0 + 6, 64, top=26)
    base = Prism(
        "ground_floor", x0, width - 1,
        [(front_z, 0), (front_z, wall_top), (back_z, wall_top), (back_z, 0)],
        edges={0: Proj(38, 64), 2: post}, skip=(1, 3),
        caps=[Band(0, 26, post, front_z, front=post, back=post, z1=back_z),
              Band(26, wall_top + 1, Tile(x0 + 1, 38, x0 + 6, 39, top=wall_top), front_z)])
    return [base, roof]


def pink_house_exact(width):
    return [(2, 38, width - 2, 62), (10, 16, width - 10, 38)]


def game_corner(art):
    """Mauville's Game Corner (112 x 80): a flat-roofed block, its roof
    glazed in 8-row panels (rows 3-26, the back parapet rows 0-3), the
    cornice rows 26-32, the facade 32-64; and its portal (x 40-72) standing
    8 forward, drawn down to row 71 - its top a 45-degree slope back to the
    cornice, edge-on to the view, since the drawing never shows it."""
    tex = Mound.with_ring(art, ())
    # The roof's back corners are drawn cut off (a diagonal outline from
    # (8, 0) to (0, 8) and its mirror), but the roof is deeper than drawn:
    # those rows land at its back corners and halfway along its sides, and
    # their clear pixels were holes in it. The verge's hatching (period 4)
    # runs on to the back instead, under the back parapet's outline.
    px = tex.load()
    W = art.size[0]
    for x in list(range(0, 8)) + list(range(W - 8, W)):
        for v in range(8):
            px[x, v] = px[x, v + 8]
    for x in range(W):
        if px[x, 0][3] < 128:
            px[x, 0] = px[8, 0]
    # the sides and the back: the pilaster beside the west window (brown, a
    # pink stripe), uniform down its height - the 16 rows under the cornice
    # hold the awning's slanting end, and repeated along a side they made a
    # saw of blue teeth; the cornice's bulbs away from the corner
    parts = flat_block(112, 64, ((3, 26), (8, 16), (0, 3)), (26, 32), 32,
                       brick=(8, 40, 16, 56), rim=(16, 26, 24, 32))
    portal = lambda u, v: 40 <= u < 72 and 32 <= v < 72
    # its flanks, 8 deep: the west post's own 8 columns (outline, brown,
    # the column of bulbs), not its outline folded back dark
    parts.append(HeightSolid("portal", tex, art.size[1],
                             Volume().profile([(64, 0), (72, 0), (72, 40), (64, 32)], 40, 72),
                             depth=(0.0, 80.0), step=2, mirror_z=68,
                             mask=lambda u, v: not portal(u, v), side=(40, 48, 72)))
    return parts, tex


def pitched_house(width, cap, teeth, slope, course, fascia, foot, wrap, ridge_wrap, wall, inset=2,
                  overhang=2, back_z=16, x0=0, run=6):
    """A town's hip-roofed house drawn like the kit house, any width: the
    roof's rows - `cap` and `teeth` of the ridge, the `slope` from its top
    row to the eave with `course` repeating, the `fascia` - then the facade
    from the fascia's foot down to `foot`, the ground's row. `wrap` and
    `ridge_wrap` are the clean columns (a whole number of tiles) the slope
    and the ridge are sampled from; `wall` the facade's columns its sides
    are dressed with. Real depth: three collision rows, z 16 to `foot`.
    `inset`: the walls' ends in from the roof's (a hall whose eaves reach
    over its deck). `x0`: where it starts (a porch before a hall).
    """
    front_z = foot
    y0 = front_z + overhang - fascia[1]
    roof = HipRoof(
        "roof", x0, x0 + width, zf=front_z + overhang, zb=back_z - overhang, y0=y0,
        fascia=Strip(fascia, wrap=wrap),
        slope=Strip(slope, repeat=course, wrap=wrap),
        teeth=teeth, cap=cap, pitch=22.0, run=run,
        ridge_u=(x0, x0 + width), end_tile=Tile(wrap[0], teeth[0], wrap[0] + 8, teeth[1]),
        ridge_wrap=ridge_wrap)
    wall_top = y0
    boards = Tile(wall[0], fascia[1], wall[1], foot, top=wall_top - 4)
    base = Prism(
        "ground_floor", x0 + inset, x0 + (width - inset + 1 if inset == 2 else width - inset),
        [(front_z, 0), (front_z, wall_top), (back_z, wall_top), (back_z, 0)],
        edges={0: Proj(fascia[1], foot), 2: boards}, skip=(1, 3),
        caps=[Band(0, wall_top - 4, boards, front_z, front=boards, back=boards, z1=back_z),
              Band(wall_top - 4, wall_top + 1,
                   Tile(wall[0], fascia[1], wall[1], fascia[1] + 1, top=wall_top), front_z)])
    return [base, roof]


def verdanturf_house(width):
    """Verdanturf's green-tiled house: ridge cap rows 0-7, its beige beam
    7-14, tiled slope 14-37 in courses of 8 (seams at rows 19, 27), eave
    37-40, wooden facade 40-62. Tiles 8 wide from x 2 (the left verge
    highlighted: sampled from x 10)."""
    return pitched_house(width, (0, 7), (7, 14), (14, 37), (19, 27), (37, 40), 62,
                         (10, 18), (9, 17), (3, 7))


def cozmo_house(art):
    """Fallarbor's house with an antenna on its roof (64 x 80): the house a
    row lower, and the antenna (x 32-56, rows 2-31) a card standing on the
    front slope where the slope is drawn at its foot."""
    parts = pitched_house(64, (16, 25), (25, 29), (29, 51), (35, 43), (51, 55), 78,
                          (10, 18), (9, 17), (3, 7))
    roof = parts[1]
    mast = art.copy()
    px = mast.load()
    for v in range(mast.size[1]):
        for u in range(mast.size[0]):
            if not (32 <= u < 57 and 2 <= v < 32):
                px[u, v] = (0, 0, 0, 0)
    # it stands on the front slope where the slope is drawn at its foot's
    # row: z - y = 32 on the slope (its last row, 31, stands just above it)
    t = math.tan(math.radians(roof.pitch))
    z = (32 + roof.ye + roof.zf * t) / (1 + t)
    return parts + [Card("antenna", mast, z)], art


def mossdeep_house_parts(width):
    """Mossdeep's red-roofed house built as Oldale's: a hipped roof (its
    white ridge beam rows 0-5, the two tiers of red slope with their posts
    rows 8-33 in courses of 14, the dark fascia 33-37) over the facade down
    to row 62; the slope sampled from 16 columns (a post and its tiles), the
    sides dressed with the vine-grown stone of the facade's ends."""
    return pitched_house(width, (0, 5), (5, 8), (8, 33), (19, 33), (33, 37), 62,
                         (12, 28), (12, 28), (2, 8))


def lilycove_house(width):
    """Lilycove's blue-tiled house: ridge cap rows 0-8, its front 8-12, tiles
    12-35 in courses of 8, eave 35-40, facade 40-63."""
    return pitched_house(width, (0, 8), (8, 12), (12, 35), (14, 22), (35, 40), 63,
                         (14, 22), (10, 18), (5, 8))


def lilycove_house_exact(width):
    return [(4, 40, width - 4, 62), (14, 12, width - 14, 40)]


def fallarbor_house(width):
    """Fallarbor's orange-roofed house: ridge cap rows 0-9, its front 9-13,
    ribbed slope 13-35 in courses of 8, eave 35-39, green facade 39-62."""
    return pitched_house(width, (0, 9), (9, 13), (13, 35), (19, 27), (35, 39), 62,
                         (10, 18), (9, 17), (3, 7))


def verdanturf_house_exact(width):
    return [(3, 40, width - 2, 62), (10, 14, width - 10, 40)]


def fallarbor_house_exact(width):
    return [(3, 39, width - 2, 62), (10, 13, width - 10, 39)]


# the pitch of the shipyard's back half (shipyard): at 20 degrees, from the
# game's camera at its default 40, the band of slats beyond the ridge stands
# to the corrugated half as in the drawing (about 0.3); at 25 it read as a
# thin line, the camera's view of the roof's back being lower than 40
SHIPYARD_BACK_PITCH = 20.0


def shipyard():
    """Stern's shipyard in Slateport (128 x 112, cells 24..31 x 32..38).

        rows  1-15  the roof's back half beyond the ridge, its white slats
                    (low enough for the view to see it), the hatches' lids
        row  16     the ridge: the short line dividing the two halves
        rows 16-63  the front half, corrugated, period 4 across; two hatches
                    across the ridge (x 16-31, 96-111, rows 8-31); gold verges
                    x 0-4 down both: a gabled roof, not a hipped one
        rows 64-71  fascia: teeth, gold, purple
        rows 72-111 the facade: pink corner pilasters, boards, the door

    Real depth: six collision rows, Z 16-112. The front half at 20 degrees
    spans its 48 drawn rows from the eave (Y 48) to the ridge. The back half
    falls at SHIPYARD_BACK_PITCH over the rest of the depth: drawn in its 15
    rows, it would fall at 37, and the game's camera (34-46 degrees) would
    hardly see it; so its slats run on back (more of the same rows), its
    outline and trim at its real eave. The gable ends and the back wall take the
    facade's boards. (It was a hipped roof with the slats as a tall ridge
    standing up like a sheet.)
    """
    front, back, wall = 112, 16, 40
    ye = 48                                     # the fascia's top at the eave
    run = 48 / (1 + math.tan(math.radians(20.0)))
    zr, yr = front - run, ye + run * math.tan(math.radians(20.0))
    yb = yr - (zr - back) * math.tan(math.radians(SHIPYARD_BACK_PITCH))
    post = Tile(0, 72, 5, 112, top=wall)
    boards = Tile(8, 72, 30, 112, top=wall)
    gable = Tile(8, 72, 30, 112)
    body = Prism(
        "ground_floor", 0, 128,
        [(back, 0), (front, 0), (front, ye), (zr, yr), (back, yb)],
        edges={1: Proj(), 4: boards}, skip=(0, 2, 3),
        caps=[Band(0, wall, boards, front, front=post, back=post, z1=back),
              Band(wall, yr + 1, gable, front, z1=back)])
    return [body, ShipyardRoof(0, 128, front, back, ye, zr, yr, yb)]


class ShipyardRoof:
    """The shipyard's two roof halves, the drawing projected on both. The
    back half is seen foreshortened, less than one texel per pixel down its
    fall line: so is it drawn ("~proj")."""

    def __init__(self, x0, x1, zf, zb, ye, zr, yr, yb):
        self.x0, self.x1, self.zf, self.zb = x0, x1, zf, zb
        self.ye, self.zr, self.yr, self.yb = ye, zr, yr, yb

    def emit(self, mesh):
        x0, x1 = self.x0, self.x1

        def proj(pts):
            return [(x, y, z, x, z - y) for (x, y, z) in pts]
        mesh.poly(proj([(x0, self.ye, self.zf), (x1, self.ye, self.zf), (x1, self.yr, self.zr),
                        (x0, self.yr, self.zr)]), SHADE_ART, "roof.slope_s")
        back = proj([(x0, self.yr, self.zr), (x1, self.yr, self.zr), (x1, self.yb, self.zb),
                     (x0, self.yb, self.zb)])
        # as drawn from the ridge to the slats' first row; then that row
        # held back to the last three rows, which are its trim and outline
        slats, end = 4.0, self.zb - self.yb
        near = clip(back, 4, slats, True)
        rest = clip(back, 4, slats, False)
        mid = clip(rest, 4, end + 3, True) if len(rest) >= 3 else []
        edge = clip(rest, 4, end + 3, False) if len(rest) >= 3 else []
        if len(near) >= 3:
            mesh.poly(near, SHADE_ART, "roof.slope_n~proj")
        if len(mid) >= 3:
            mesh.poly([q[:4] + (slats + 1.5,) for q in mid], SHADE_ART, "roof.slope_n~clamp")
        if len(edge) >= 3:
            mesh.poly([q[:4] + (q[4] - end + 1,) for q in edge], SHADE_ART, "roof.slope_n~proj")


# its back half's top rows lie further back than drawn (ShipyardRoof)
SHIPYARD_EXACT = [(0, 4, 128, 112)]


def stall(x1, ridge_e, fills):
    """Slateport market's stall, its roof from x 8 to `x1`, lifted 12 like
    the awnings so that people walk under it."""
    return Tent("stall", 8, x1, top_v=1, valance_v=25, post_v=29, foot_v=47, back=12, lift=12,
                posts=[(8, 14), (x1 - 6, x1)], shaft=(34, 44), foot=(44, 47),
                ridge=(22.5, ridge_e, 9, 11), pitch=30.0, fills=fills)


def stall_keep(x1):
    """The stall's own pixels: its roof and its two front posts."""
    return [(8, 1, x1, 29), (8, 29, 14, 47), (x1 - 6, 29, x1, 47)]


class Relief_top:
    """A flat polygon at height `h`, projected: the basin's rim and water."""

    def __init__(self, poly, h):
        self.poly, self.h = poly, h

    def emit(self, mesh):
        h = self.h
        mesh.poly([(x, h, z, x, z - h) for (x, z) in reversed(self.poly)], 1.0, "fountain.top")
        # the sides and back the drawing never shows: the front wall's stone
        n = len(self.poly)
        for i in range(2, n - 1):
            (xa, za), (xb, zb) = self.poly[i], self.poly[(i + 1) % n]
            quad = [(xa, -1, za), (xb, -1, zb), (xb, h, zb), (xa, h, za)]
            # projected: from the front it is the drawing, gaps included
            mesh.poly([q + (q[0], q[2] - q[1]) for q in quad], 0.7, "fountain.side~proj")


class PlanRing:
    """A flat ring at height `y` between two plan polygons of as many
    corners, corner i of one facing corner i of the other (a basin's rim),
    or with `inner` None the whole of `outer` (its water). Projected: the
    drawing at 45 degrees."""

    def __init__(self, name, outer, inner, y):
        self.name, self.outer, self.inner, self.y = name, outer, inner, y

    def emit(self, mesh):
        y = self.y

        def pt(p):
            return (p[0], y, p[1], p[0], p[1] - y)
        o, i = self.outer, self.inner
        if i is None:
            mesh.poly([pt(p) for p in o], SHADE_ART, self.name + "~proj")
            return
        for k in range(len(o)):
            k1 = (k + 1) % len(o)
            mesh.poly([pt(o[k]), pt(o[k1]), pt(i[k1]), pt(i[k])], SHADE_ART, self.name + "~proj")


class Shade:
    """A flat ellipse at height `y`, about (cx, cz), of one colour - the
    texel `texel` of the drawing: what a dish's shadow is under the dish.
    The water there, projected, reads the dish's own pixels (the dish hides
    them at 45 degrees); seen from the side, under the dish's edge, they
    showed as its dots twice. Hidden at 45 degrees itself, so it is not the
    drawing's to judge (~behind), and one texel stretched (~clamp)."""

    def __init__(self, name, cx, cz, rx, rz, y, texel, sides=24):
        self.name, self.cx, self.cz, self.rx, self.rz = name, cx, cz, rx, rz
        self.y, self.texel, self.sides = y, texel, sides

    def emit(self, mesh):
        u, v = self.texel[0] + 0.5, self.texel[1] + 0.5
        ring = [(self.cx + self.rx * math.cos(2 * math.pi * k / self.sides), self.y,
                 self.cz + self.rz * math.sin(2 * math.pi * k / self.sides), u, v)
                for k in range(self.sides)]
        mesh.poly(ring, SHADE_ART, self.name + "~behind~clamp")


class Jet:
    """The fountain's jet: a card standing across the dish at depth `z`,
    from the dish's top `y0` up, over the drawing's columns `columns` and
    rows `rows` - each point reading the drawing's pixel it covers at 45
    degrees, moved down `band` rows to where the jet is laid alone. Its
    texels are `live` (gen_voxel_buildings.py): the console redraws them
    from the game's tiles."""

    def __init__(self, name, columns, z, y0, band, rows=JET_ROWS):
        self.name, self.columns, self.z, self.y0, self.band = name, columns, z, y0, band
        self.y1 = z - rows[0]
        if abs((z - rows[1]) - y0) > 1e-6:
            raise SystemExit("%s: the card's foot is not the dish's top" % name)

    def emit(self, mesh):
        (x0, x1), z = self.columns, self.z
        quad = [(x0, self.y0, z), (x1, self.y0, z), (x1, self.y1, z), (x0, self.y1, z)]
        mesh.poly([(x, y, z_, x, z_ - y + self.band) for (x, y, z_) in quad], SHADE_ART,
                  self.name + "~live")


# ── Interiors ─────────────────────────────────────────────────────────────
#
# A room is cut into pieces, front first; see gen_voxel_buildings.interior_specs.
# Shapes are in pixels of the room's drawing; `height` is the rows of a piece's
# front, standing at its drawn foot; `base` lifts it onto another piece.

WALL_COLOURS = ("f6b473", "de7b31", "ffe6b4")   # the Center's orange wall


def piece(name, shape, height, base=0, fill=None, leave=(), side=None, solid=None,
          back=None, card=False, top=None, foot=None, walls=(), cells=(), against=None,
          claim=(), through=False, round=None, legs=None, run=(), plan=None, skip=(),
          when=None, at=(), tiles=None, anim=None, ribbon=None, found=False, flat=False,
          variants=False, posts=None, squash=None, turn=None):
    """`back`: the Z its top runs back to, where the cartridge's collision
    says it ends (or the wall it stands against); `against`: that wall's Z,
    where a top drawn deeper than the room in front of it stops, flat, the
    wall hiding the rows drawn behind it; `top`: the stretch of the room's
    drawing that depth is laid with, when its own drawn top is too thin to
    repeat; `card`: leaves, not a box; `foot`: the row a wall with a doorway
    stands on; `walls`: the side walls it carries, ((x, z), (x, z)[, height])
    - facing left of a->b - dressed with `side`; `through`: the room is seen
    through it - its `leave` colours inside its outline too are holes (a
    tap's wire, a bed's open head frame), not the room painted on it;
    `round`: "dome", a round lid as a smooth cap under its drawing, `height`
    its crown over `base` (gen_voxel_buildings.dome_fit); "ball", round all
    over, its circle's size (gen_voxel_buildings.ball_fit); "lathe", turned
    (a jar), `height` tall, its belly and neck fitted to its drawing
    (gen_voxel_buildings.lathe_fit); `plan`: modelled
    whole, an upright prism on that polygon of (x, z) in the room's pixels
    (vb.PlanPrism), its corners cut as the drawing cuts them - a counter;
    or blocks [(part, polygon, y0, y1, (x0, x1, foot)), ...], each its own
    prism with that stretch of its front for its sides - a terminal's head
    on its pedestal, a table's top on its feet (its legs its own blocks:
    no leg_split);
    `fill`: "column", what the pieces before it hide of its top is its own
    up or down its column, not along its row; `legs`: the rows
    of its front that are its top, the rest its legs, where the drawing's
    own are too short to be found (gen_voxel_buildings.leg_split); `run`:
    built from the side, [(part, [(x, y), ...], z0, z1)] - each section, x
    across and y up in the room's pixels, running back from z0 to z1 with
    the drawing projected on it (vb.ProjRun): an escalator's ramp and its
    balustrades. A `foot` below its drawing: a machine standing behind a
    counter that hides its front's foot, not on it. `skip`: cells (x, y)
    of its rectangle it neither lays nor stands on, left to the map as the
    game draws it - a switch a script presses beside a side wall. `when`:
    (x, y, metatile) - it stands only while that cell of the room holds that
    metatile (a beam a script switches, a switch it presses); `at`: cells
    its rectangle's corner stands at again, the same piece (one post
    drawn whole, standing for every post); `tiles`: (lo, hi), only the
    pixels drawn from those tiles of the upper layer are its own (the beam,
    not the posts it runs between); `anim`: ([the frames' .4bpp], their
    first tile in BG VRAM) - its texels follow the game's animation of
    those tiles (`tiles` names them); `ribbon`: ((za, ya), (zb, yb)) - a
    strip running back from one post to the next (vb.Ribbon), not a card;
    `found`: found in other rooms by its own pixels, however few (a
    statue's ball: gen_voxel_buildings.LOOSE_PIXELS); `flat`: a block whose
    drawing is its plan, laid on its top as drawn (not projected), its sides
    its `walls` - the dark beside a room's entrance, its rim at its north
    edge (its room's `redrawn` waives the 45-degree proof there);
    `variants`: with `at`, each place it stands textured as the room draws
    it there, round its shape and a pixel out - one model to each way it is
    drawn (a column with another before it, drawn lighter where it shows;
    one a neighbour's outline joins); `posts`: (width, step) - with "lathe",
    a row of little turned things that wide, each column of them one
    behind another that far down the drawing, each fitted on its own;
    `squash`: with "lathe", its plan that deep for its width (0.7 if not
    given; 1.0 round); `turn`: with "lathe", (sides, bands) - its polygon's
    sides and the rings up it, (16, 4) if not given, fewer for a plain
    drum (the console's chunks hold so many triangles)."""
    return {"name": name, "shape": shape, "height": height, "base": base,
            "fill": fill, "leave": leave, "side": side, "solid": solid,
            "back": back, "card": card, "top": top, "foot": foot, "walls": walls,
            "cells": cells, "against": against, "claim": claim, "through": through,
            "round": round, "legs": legs, "run": run, "plan": plan, "skip": skip,
            "when": when, "at": at, "tiles": tiles, "anim": anim, "ribbon": ribbon,
            "found": found, "flat": flat, "variants": variants, "posts": posts,
            "squash": squash, "turn": turn}


def own_shell(pieces):
    """Marks a room's shell - its walls, its doorways' recesses, a maze's
    blocks - as its own (`alone`): the generator stands a known piece
    wherever its cells are drawn again, and a side wall, which is drawn
    nowhere, was found wherever another room had the same floor down a
    column - walls standing loose in the middle of Rustboro's flats."""
    for pc in pieces:
        if pc["name"].split("_")[0] in ("wall", "side", "stairwell", "edge", "block",
                                         "partition", "divider"):
            pc["alone"] = True
    return pieces


# the Center's floor and its shadows, and the counter's cream and trim: what
# a stool, the table or a Poke Ball on the counter leaves round it
CENTER_FLOOR = ("cdc58b", "eedea4", "ffffc5")
CENTER_COUNTER = ("ffffc5", "e6e6b4", "de9c62", "ffe6b4", "de7b31")


def facet(name, a, b, height, walls=(), side=None, cells=()):
    """A chamfered corner: a wall along its foot, from a = (x, top, foot) to
    b in the drawing's pixels, carrying the side walls that run on from it
    (edge on to the GBA camera, drawn nowhere), `height` tall."""
    (xa, ta, fa), (xb, tb, fb) = a, b
    return {"name": name, "shape": [[(xa, ta), (xb, tb), (xb, fb), (xa, fa)]],
            "height": height, "facet": (a, b), "walls": walls, "side": side,
            "cells": cells}


# a stretch of the Center's back wall, full height and one panel wide, as the
# room looks with the furniture taken out: for the faces the drawing never shows
CENTER_WALL_SIDE = (64, 0, 80, 32)


def center_walls(front):
    """The Center's walls, 32 px: the back one, standing at row 32 - in two,
    west and east of x 128, so each half counts against the chunk it stands in
    - and its corners, chamfered at 45 degrees, from which the side walls run
    on to where the floor ends at `front`. The drawing has no side walls: they
    are edge on to its camera; from the console's they close the room."""
    return [
        facet("corner_w", (0, 15, 47), (16, -1, 31), 32, walls=[((0, front), (0, 47))],
              side=CENTER_WALL_SIDE, cells=[(0, front // 16 - 1)]),
        facet("corner_e", (208, -1, 31), (224, 15, 47), 32, walls=[((224, 47), (224, front))],
              side=CENTER_WALL_SIDE, cells=[(13, front // 16 - 1)]),
        piece("wall_e", [(128, 0, 208, 32)], 32, fill=16, side=CENTER_WALL_SIDE),
        piece("wall", [(16, 0, 128, 32)], 32, fill=16, side=CENTER_WALL_SIDE),
    ]


def center_escalator(flat, way):
    """The Center's escalator from the side (piece `run`): a ramp between
    two glass balustrades two pixels thick and 7 tall, to z 96 and 120, running
    west into the side wall. East of x `flat` it is the landing, level with
    the floor; west of it the balustrades and the steps between them climb
    (`way` 1: up to the floor above) half a pixel a pixel, or sink (-1:
    down to the one below) to the floor at x 0. The console's floor has no
    hole for them: sinking, the steps and the glass below the floor are
    the drawing's, laid on it - the GBA's 45 degrees exactly, and from the
    console's camera the stair the drawing shows going down. Rising, the
    balustrades are a band that climbs, the steps a plate: the drawing
    shows the floor under them where they leave it (its glass ends a row
    lower every two columns, from row 113 at x 0), and stood down to the
    floor there they took it on their faces - a pale band under the glass,
    cut in steps."""
    top = 7 + way * flat / 2.0
    rail = [(0, 0), (32, 0), (32, 7), (flat, 7), (0, top)]
    if way > 0:
        # the glass's foot, a straight line through its drawn steps' middles
        # (row 112.5 + x / 2 at the front): every pixel of glass inside it,
        # every one of floor out - its edge the line, not a stair the
        # texture's transparency cuts
        rail = [(0, flat / 2.0 - 0.5), (flat - 1, 0), (32, 0), (32, 7), (flat, 7), (0, top)]
        steps = [(0, flat / 2.0), (flat, 0), (32, 0), (32, 0.5), (flat, 0.5),
                 (0, flat / 2.0 + 0.5)]
        front = 118
    else:
        # down to the floor at x 0; the steps' plate runs on under the south
        # balustrade, where the glass below the floor is drawn
        rail = [(0, 0), (32, 0), (32, 7), (flat, 7)]
        steps, front = [(0, 0), (32, 0), (32, 0.5), (0, 0.5)], 120
    return [("rail_n", rail, 94, 96), ("rail_s", rail, 118, 120), ("steps", steps, 96, front)]


def center_counter_plan(x0, x1, arm, z_back, z_bar, z_front, cut=3, nick=1.2):
    """The Center's counter in plan: a bar across the front from x0 to x1,
    an arm `arm` wide back to `z_back` at each end, the yard between them
    (the nurse's) behind the bar's back at `z_bar`. Its front corners are
    cut on the slant `cut` pixels - the drawing's bands step up a row a
    column round them, its foot a pixel a column - and its arms' back
    corners by a nick, where the drawing leaves the wall."""
    a, b = x0 + arm, x1 - arm
    return [(x0 + cut, z_front), (x1 - cut, z_front), (x1, z_front - cut),
            (x1, z_back + nick), (x1 - nick, z_back), (b + nick, z_back), (b, z_back + nick),
            (b, z_bar), (a, z_bar), (a, z_back + nick), (a - nick, z_back),
            (x0 + nick, z_back), (x0, z_back + nick), (x0, z_front - cut)]


def center_stool(x0, y0):
    """A stool of the Centers, drawn in the 16 x 16 at (x0, y0) (all six
    alike): its cushion, 14 wide, its back corners cut on the slant as the
    drawing rounds them (its top's first two rows two pixels and one in),
    from y 4 to 6; under it the base, a pixel in each side, to y 1; and its
    foot, the dark line at its bottom, four in. Each takes its sides from
    its own rows of the front, the run of middle columns alike in them (a
    strip as wide as that, so that a side is a quad or two): the cushion's
    oranges, the base's band, dark line and grey, the foot's dark."""
    z = y0 + 16
    cushion = [(x0 + 3.25, y0 + 8), (x0 + 12.75, y0 + 8), (x0 + 15, y0 + 10.25), (x0 + 15, z),
               (x0 + 1, z), (x0 + 1, y0 + 10.25)]
    base = [(x0 + 2, y0 + 9), (x0 + 14, y0 + 9), (x0 + 14, z), (x0 + 2, z)]
    foot = [(x0 + 4, y0 + 11), (x0 + 12, y0 + 11), (x0 + 12, z), (x0 + 4, z)]
    return [("cushion", cushion, 4, 6, (x0 + 3, x0 + 13, y0 + 12)),
            ("base", base, 1, 4, (x0 + 5, x0 + 11, y0 + 15)),
            ("foot", foot, 0, 1, (x0 + 4, x0 + 12, y0 + 16))]


def center_table():
    """The Center's glass table: its top a slab from y 1 to 5, its back
    corners round - each row of the drawing's top ending half way between
    the last pixel of glass and the first of floor, so that no pixel's
    centre is on its edge - its front corners cut a pixel; on four feet a
    pixel tall, 2 x 2, the front two where they are drawn under its edge,
    the back two hidden under the glass. The slab's sides are the glass's
    edge (its front's pale and teal rows and the dark line under them, the
    five columns alike in all four), the feet's their dark."""
    left = [(183.5, 101), (183, 101.5), (181, 102.5), (180, 103.5), (179, 104.5),
            (179, 105.5), (178, 106.5), (178, 125.5), (179, 126)]
    # the drawing is symmetric about x 192
    slab = left + [(384 - x, z) for (x, z) in reversed(left)]

    def foot(x, z):
        return [(x, z), (x + 2, z), (x + 2, z + 2), (x, z + 2)]
    return ([("top", slab, 1, 5, (185, 190, 125))]
            + [("foot_%d" % n, foot(x, z), 0, 1, (180, 182, 126))
               for n, (x, z) in enumerate([(180, 124), (202, 124), (180, 104), (202, 104)])])


POKEMON_CENTER_1F = [
    # the Poke Balls on the counter's two arms: round, each as its drawn
    # circle - its shadow under it and the arm round it the counter's
    piece("ball_w", [("ellipse", 73, 46, 6, 6)], 12, base=10, leave=CENTER_COUNTER,
          round="ball"),
    piece("ball_e", [("ellipse", 151, 46, 6, 6)], 12, base=10, leave=CENTER_COUNTER,
          round="ball"),
    # the counter: a bar across the front, an arm back to the wall each end,
    # inside their outlines (the floor beside them is the floor's, the PC's
    # edge the PC's, the row over the bar's back edge the nurse's floor and
    # the machines' feet); the wall round its arms' rounded backs and the
    # floor round its foot's corners left. As deep as its drawn top: run
    # back to its cells' edge it would lay its plate again behind it, where
    # the nurse's floor is seen between the machines. One piece on its
    # plan, its front corners one slanting face each, its top under the
    # balls its arms' own; its sides its front's bands, a stretch with no
    # slot in it (its ends' grey bevel laid along its arms is a slab of
    # shade). The wall in its west arm's back corner is the wall's, though
    # the dark behind the arm closes the outline round it
    piece("counter", [(65, 24, 79, 64), (79, 25, 80, 64), (144, 24, 159, 64), (65, 45, 159, 64)],
          10, fill="column", leave=("de7b31", "cdc58b", "eedea4"), side=(108, 54, 116, 64),
          plan=center_counter_plan(65, 159, 15, 34, 55, 64)),
    # behind it, on the floor where the nurse stands: the healing machine
    # lies flat - its tray is its top, its front the band over the counter,
    # which hides its foot
    piece("healer", [(80, 14, 112, 45)], 14, leave=WALL_COLOURS, foot=54, against=32,
          cells=[(5, 3)]),
    # and the telephone beside it, against the wall: a box, its top the lid
    # with the cross (rows 25 to 32), its front the grey under it; and its
    # dial, a block in front of the box's west end, its keys over its top
    # and front. Inside their outlines: cut wider, the wall and the floor
    # round them stood up as a cream slab beside the box
    piece("phone", [(133, 25, 144, 41), (128, 31, 133, 42)], 8,
          leave=WALL_COLOURS + CENTER_FLOOR,
          plan=[("box", [(133, 33), (141.5, 33), (144, 35.5), (144, 41), (133, 41)], 0, 8,
                 (135, 140, 41)),
                ("dial", [(128, 36), (133, 36), (133, 42), (128, 42)], 0, 5, (129, 130, 42))]),
    # the PC by the map, inside its outline: its top the screen's housing
    # (rows 8 to 15), against the wall; cut two pixels wider, it stood the
    # counter's outline and the baseboard's line beside it up its sides
    piece("pc", [(160, 8, 176, 40)], 24, leave=WALL_COLOURS, back=32),
    piece("bookcase", [(32, 14, 65, 42)], 18, leave=WALL_COLOURS, back=32),
    # a card: the wall, the corner's shade and the floor round its pot left
    piece("plant", [(14, 10, 34, 40)], 26, card=True, through=True,
          leave=WALL_COLOURS + CENTER_FLOOR + ("f6eebd", "bd5220", "949494")),
    piece("stool_1", [(16, 47, 32, 64)], 6, leave=CENTER_FLOOR,
          plan=center_stool(16, 47)),
    piece("stool_2", [(32, 47, 48, 64)], 6, leave=CENTER_FLOOR,
          plan=center_stool(32, 47)),
    piece("stool_3", [(160, 95, 176, 111)], 6, leave=CENTER_FLOOR,
          plan=center_stool(160, 95)),
    piece("stool_4", [(160, 111, 176, 127)], 6, leave=CENTER_FLOOR,
          plan=center_stool(160, 111)),
    piece("stool_5", [(176, 127, 192, 144)], 6, leave=CENTER_FLOOR,
          plan=center_stool(176, 127)),
    piece("stool_6", [(192, 127, 208, 144)], 6, leave=CENTER_FLOOR,
          plan=center_stool(192, 127)),
    # a glass top on four feet a row tall, under its edge (center_table)
    piece("table", [(176, 95, 208, 128)], 5, leave=CENTER_FLOOR, plan=center_table()),
    # up to the floor above, into the west wall
    piece("escalator", [(0, 78, 34, 123)], 8, leave=CENTER_FLOOR + ("949494",),
          run=center_escalator(16, 1)),
    # the walls: the back one, its chamfered corners, the two side walls
    # whose tops are the lines down the room's edges, the front corners
] + center_walls(144)



def center_terminal(x0):
    """A link terminal of the Center's 2F standing at x0 (its drawing 16
    wide, its foot on row 80): its head, a box as deep as its drawn top (rows
    56 to 59: four pixels), from the outline under its screen (row 70, y 9)
    up to y 20, its sides the grey of its casing's frame; under it the
    pedestal, two pixels narrower each side as drawn, its sides its front's
    grey with the dark line under the head and its foot's."""
    head = [(x0, 76), (x0 + 16, 76), (x0 + 16, 80), (x0, 80)]
    stand = [(x0 + 2, 77), (x0 + 14, 77), (x0 + 14, 80), (x0 + 2, 80)]
    return [("head", head, 9, 20, (x0 + 1, x0 + 2, 71)),
            ("stand", stand, 0, 9, (x0 + 7, x0 + 8, 80))]


POKEMON_CENTER_2F = [
    # the three link terminals, in front of the counter, and the glass
    # tubes that stand behind them - each tube's foot its own, not drawn on
    # the terminal's top. The first is the ground floor's PC, pixel for
    # pixel inside its outline, and stands here as it is
    # (gen_voxel_buildings.reuse_pieces; the tube behind it in view over
    # it); the other two are a head on a pedestal, each a block
    # (center_terminal)
    piece("terminal_2", [(128, 56, 144, 81)], 20, plan=center_terminal(128)),
    piece("terminal_3", [(192, 56, 208, 81)], 20, plan=center_terminal(192)),
    # the gates between the counter's stretches: low, set back to its back
    piece("gate_1", [(80, 46, 96, 58)], 6, back=48),
    piece("gate_2", [(144, 46, 160, 58)], 6, back=48),
    piece("counter", [(0, 40, 48, 63), (64, 40, 80, 63), (96, 40, 128, 63),
                      (160, 40, 192, 63), (208, 40, 218, 63)], 9, fill=1, back=48),
    # glass tubes: as deep as they are wide, the rest of their drawing
    # height; their sides the white of their glass, whole - the rounded
    # corners of their feet are not legs
    piece("tube_1", [(48, 6, 64, 42)], 24, leave=WALL_COLOURS, side=(51, 16, 61, 36),
          solid=True),
    piece("tube_2", [(128, 6, 144, 56)], 33, leave=WALL_COLOURS, side=(131, 23, 141, 52),
          solid=True),
    piece("tube_3", [(192, 6, 208, 56)], 33, leave=WALL_COLOURS, side=(195, 23, 205, 52),
          solid=True),
    # cards, the floor round their pots left
    piece("plant_1", [(80, 128, 96, 160)], 24, card=True, leave=CENTER_FLOOR + ("f6eebd",)),
    piece("plant_2", [(96, 128, 112, 160)], 24, card=True, leave=CENTER_FLOOR + ("f6eebd",)),
    piece("plant_3", [(144, 128, 160, 160)], 24, card=True, leave=CENTER_FLOOR + ("f6eebd",)),
    piece("plant_4", [(160, 128, 176, 160)], 24, card=True, leave=CENTER_FLOOR + ("f6eebd",)),
    # the five stools along the front are the ground floor's, tile for tile,
    # and stand here as they are (gen_voxel_buildings.reuse_pieces)
    # down to the floor below: its steps drawn sinking into the floor
    piece("escalator", [(0, 86, 34, 122)], 8, leave=CENTER_FLOOR + ("949494",),
          run=center_escalator(14, -1)),
] + center_walls(160)


MART_WALL = ("ffffff", "8bd5de", "d5ded5", "b4b4a4", "83b4b4")
MART_WALL_SIDE = (32, 0, 48, 32)

MART = [
    # the till on the glass counter's arm; the counter, an L from the wall
    piece("till", [(32, 24, 48, 39)], 8, base=15, leave=MART_WALL),
    # a glass case, whose drawn top is two rows: its depth is laid with the
    # glass of the arm that runs back to the wall, which shows its top whole
    piece("counter", [(32, 24, 48, 80), (0, 58, 48, 80)], 15, fill=1, back=64,
          top=(34, 56, 46, 70)),
    # the shelves along the back wall, the plant, the three standing shelves
    piece("shelves_back", [(96, 14, 160, 42)], 20, leave=MART_WALL, back=32, side="plain"),
    piece("plant", [(160, 30, 172, 64)], 26, leave=MART_WALL, card=True),
    # the display stands: low, their front the band at their foot, their
    # three compartments of goods their top, as long as the three cells
    # their collision blocks
    piece("shelf_1", [(96, 62, 112, 111)], 14, back=64),
    piece("shelf_2", [(112, 62, 128, 111)], 14, back=64),
    piece("shelf_3", [(160, 54, 172, 111)], 14, back=64),
    # the walls as the Center's: 7-px corners, the side walls run on from them
    facet("corner_w", (0, 6, 40), (7, -1, 33), 34, walls=[((0, 128), (0, 40))],
          side=MART_WALL_SIDE, cells=[(0, 7)]),
    facet("corner_e", (169, -1, 33), (176, 6, 40), 34, walls=[((176, 40), (176, 128))],
          side=MART_WALL_SIDE, cells=[(10, 7)]),
    piece("wall", [(7, 0, 169, 32)], 32, fill=16, side=MART_WALL_SIDE),
]


def _replace(pieces, names, new):
    """A room like another but for some pieces: `new` stands where the first
    of `names` did, and the rest of them are gone."""
    out, done = [], False
    for pc in pieces:
        if pc["name"] in names:
            if not done:
                out += new
                done = True
            continue
        out.append(pc)
    return out


# Lavaridge's Center: a door to the hot spring in the back wall, a plant
# either side of it. Its counter, Poke Balls, machines, stools, table and
# escalator are the other Centers', tile for tile, and stand here as they are
# (gen_voxel_buildings.reuse_pieces), and so is the back wall east of the
# door; the plants and the rest of the walls are its own.
LAVARIDGE_CENTER_1F = [
    piece("plant_w", [(14, 10, 34, 40)], 26, leave=WALL_COLOURS),
    piece("plant_e", [(46, 10, 66, 40)], 26, leave=WALL_COLOURS),
] + [pc for pc in center_walls(144) if pc["name"] != "wall_e"]


# The front corners the GBA leaves black: the outside of a room whose front
# wall it never draws. In the round they are floor, not a hole in it.
CENTER_1F_OPEN = [[(0, 126), (18, 144), (0, 144)], [(224, 126), (206, 144), (224, 144)]]
CENTER_2F_OPEN = [[(0, 142), (18, 160), (0, 160)], [(224, 142), (206, 160), (224, 160)]]
MART_OPEN = [[(0, 118), (10, 128), (0, 128)], [(176, 118), (166, 128), (176, 128)]]

# ── Littleroot's two houses ───────────────────────────────────────────────
#
# Brendan's and May's, each a ground floor and a bedroom, the one house the
# other way round. The ground floor's back wall steps forward a cell where the
# stairs go up, and the stairs are a doorway in it: the flight is drawn inside
# the doorway as the GBA sees it from above, so it lies on the floor of a
# recess one cell deep, with the doorway's sides and back round it. Upstairs
# the same doorway leads down, drawn on the floor behind the wall's face.
#
# Furniture against the back wall stands on the wall's own row of collision,
# so what it is drawn with in front of the wall is all the depth it has; a
# desk drawn with more top than that stands its back rows up the wall.

HOUSE_WALL = ("d5d5b4", "b4b4a4", "ffffff")          # plaster, baseboard, trim
HOUSE_FLOOR = ("b4a44a", "947329", "ac8b39", "cdc55a")   # the planks
BLUE_RUG = ("6a94c5", "8bbdf6", "838394")               # and the chairs' shadows
PINK_RUG = ("d57bac", "838394")


def house_chair(name, back, seat, leave):
    """A kitchen chair seen from the side: its back, a narrow slab, and its
    seat, as deep as one another."""
    return [piece(name + "_back", [back], 9, leave=leave, solid=True),
            piece(name + "_seat", [seat], 5, leave=leave, solid=True)]


def stairwell(name, x0, x1, front, back, opening, side, crest):
    """The sides and back of a doorway's recess, x0..x1 from the wall's face
    at `front` back to `back`; `opening` is the doorway's height. `crest` is
    the row the wall's top is drawn on: nothing of the recess may rise above
    it at 45 degrees, so the sides step down where the doorway's height
    would."""
    low = min(opening, back - crest)
    mid = crest + opening
    if mid >= front or mid <= back:
        walls = [((x0, front), (x0, back), low), ((x1, back), (x1, front), low)]
    else:
        walls = [((x0, front), (x0, mid), opening), ((x0, mid), (x0, back), low),
                 ((x1, back), (x1, mid), low), ((x1, mid), (x1, front), opening)]
    walls.append(((x0, back), (x1, back), low))
    return piece(name, [], 32, side=side, walls=walls)


def house_tv():
    """Littleroot's television (piece `plan`): the set, a box as deep as its
    drawn top - the bezel's grey four rows, and behind them the tube's
    housing, narrower row by row (rows 61 to 63) - from y 8 to 20, its sides
    its casing's dark; under it the stand, on the same plan, its sides its
    frame's dark. Laid back over the cell it blocks with one row of that
    top repeated, the set was a black box with a grey band at its front."""
    # its edges between the drawn pixels, through their rows' middles: every
    # centre of the housing's pixels inside, every one of the floor's out
    plan = [(67.5, 81), (76.5, 81), (78, 82.5), (80, 83.5), (80, 88), (64, 88), (64, 83.5),
            (66, 82.5)]
    return [("set", plan, 8, 20, (64, 66, 80)), ("stand", plan, 0, 8, (71, 73, 88))]


def brendan_1f():
    rug = BLUE_RUG
    return (house_chair("chair_nw", (33, 96, 37, 112), (37, 100, 47, 112), rug)
            + house_chair("chair_sw", (33, 112, 37, 128), (37, 116, 47, 128), rug)
            + house_chair("chair_ne", (91, 96, 95, 112), (81, 100, 91, 112), rug)
            + house_chair("chair_se", (91, 112, 95, 128), (81, 116, 91, 128), rug) + [
        piece("table", [(50, 96, 78, 128)], 8, leave=rug, solid=True),
        # the television on its stand (house_tv), and the low white cupboard
        # beside it
        piece("tv", [(64, 61, 80, 88)], 20, leave=HOUSE_FLOOR, plan=house_tv()),
        piece("cabinet", [(32, 65, 63, 88)], 9, back=72),
        # the kitchen along the back wall: fridge, sink, china cabinet. The
        # fridge and the cabinet are as deep as drawn, their tops their
        # drawn rows: laid back to the wall with those rows repeated, a top
        # carried its back outline across its middle (the fridge's dark
        # line, the cabinet's yellow plain twice its size)
        piece("fridge", [(0, 18, 16, 48)], 24, leave=HOUSE_FLOOR, back=42),
        # a wire of a tap: the baseboard between its spokes is seen through it
        piece("tap", [(22, 24, 29, 29)], 5, base=8, leave=HOUSE_WALL[:2], through=True),
        piece("sink", [(16, 29, 48, 41), (17, 41, 47, 48)], 8, back=32, top=(18, 38, 46, 39)),
        # on two legs: the floor under it is the floor
        piece("dresser", [(49, 19, 72, 48)], 23, back=42, leave=HOUSE_FLOOR),
        stairwell("stairwell", 128, 144, 48, 29, 19, (128, 29, 144, 34), 16),
        # the back wall, a cell forward east of the step; the doorway is cut
        # out of it and it stands on its foot. Its west edge, the outline
        # down the step's corner, is its face's full height: drawn only from
        # row 32 down, the face had a slit a pixel wide above, and from
        # below 45 degrees the void behind the wall showed through it
        piece("wall_e", [(112, 16, 113, 48), (113, 16, 176, 29), (113, 29, 128, 48),
                         (144, 29, 176, 48)], 32, fill=16, foot=48,
              side=(156, 16, 172, 48), walls=[((112, 32), (112, 48))]),
        piece("wall", [(0, 0, 113, 32), (113, 0, 117, 16)], 32, fill=16, foot=32,
              side=(72, 0, 80, 32)),
        piece("side_w", [], 32, side=(72, 0, 80, 32), walls=[((0, 144), (0, 32))]),
        piece("side_e", [], 32, side=(156, 16, 172, 48), walls=[((176, 48), (176, 144))]),
    ])


def may_1f():
    rug = PINK_RUG
    # the chairs' seats are Brendan's, pixel for pixel on another rug, and
    # stand here as they are; their backs are drawn over the rug
    return [pc for pc in (house_chair("chair_nw", (81, 96, 85, 112), (85, 100, 95, 112), rug)
                          + house_chair("chair_sw", (81, 112, 85, 128), (85, 116, 95, 128), rug)
                          + house_chair("chair_ne", (139, 96, 143, 112), (129, 100, 139, 112), rug)
                          + house_chair("chair_se", (139, 112, 143, 128), (129, 116, 139, 128), rug))
            if pc["name"].endswith("_back")] + [
        piece("table", [(98, 96, 126, 128)], 8, leave=rug, solid=True),
        # the television and its cabinet, the fridge, the tap and the sink
        # are Brendan's, pixel for pixel, and stand here as they are
        # (gen_voxel_buildings.reuse_pieces)
        piece("dresser", [(105, 19, 128, 48)], 23, back=42, leave=HOUSE_FLOOR),
        stairwell("stairwell", 32, 48, 48, 29, 19, (32, 29, 48, 34), 16),
        piece("wall_w", [(63, 16, 64, 48), (0, 16, 63, 29), (0, 29, 32, 48),
                         (48, 29, 63, 48)], 32, fill=16, foot=48,
              side=(4, 16, 20, 48), walls=[((64, 48), (64, 32))]),
        piece("wall", [(63, 0, 176, 32), (59, 0, 63, 16)], 32, fill=16, foot=32,
              side=(96, 0, 104, 32)),
        piece("side_w", [], 32, side=(4, 16, 20, 48), walls=[((0, 144), (0, 48))]),
        piece("side_e", [], 32, side=(96, 0, 104, 32), walls=[((176, 32), (176, 144))]),
    ]


def brendan_2f():
    return [
        # the desk: the computer and an open book on it, the chair before it
        piece("chair_back", [(1, 33, 5, 48)], 9, leave=HOUSE_FLOOR, solid=True),
        piece("chair_seat", [(5, 36, 14, 48)], 5, leave=HOUSE_FLOOR, solid=True),
        piece("pc", [(1, 9, 16, 30)], 16, base=9, leave=HOUSE_WALL, back=32),
        # lying open: its pages its top, their edge its front, a pixel thick
        piece("book", [(18, 24, 30, 31)], 1, base=9),
        piece("desk", [(0, 20, 32, 40)], 9, leave=HOUSE_FLOOR, against=32),
        # the games console and its pad; the television is the ground
        # floor's, tile for tile (gen_voxel_buildings.reuse_pieces)
        piece("console", [(51, 24, 64, 40)], 9, leave=HOUSE_WALL + HOUSE_FLOOR, back=32),
        # the bed: the pillow lying on the mattress, and behind it the head,
        # an open frame the floor is seen through
        piece("pillow", [(18, 66, 30, 71)], 3, base=7, leave=HOUSE_FLOOR),
        piece("bed_head", [(12, 62, 36, 70)], 7, base=7, leave=HOUSE_FLOOR, through=True),
        piece("bed", [(12, 70, 36, 93)], 7, leave=HOUSE_FLOOR, solid=True, claim=[(12, 70, 36, 72)]),
        stairwell("stairwell", 112, 128, 32, 13, 19, (113, 14, 127, 26), 0),
        piece("wall", [(0, 0, 112, 32), (112, 0, 128, 13), (128, 0, 144, 32)], 32, fill=16,
              foot=32, side=(96, 0, 104, 32)),
        piece("side_w", [], 32, side=(96, 0, 104, 32), walls=[((0, 128), (0, 32))]),
        piece("side_e", [], 32, side=(96, 0, 104, 32), walls=[((144, 32), (144, 128))]),
    ]


def may_2f():
    return [
        piece("chair_back", [(139, 33, 143, 48)], 9, leave=HOUSE_FLOOR, solid=True),
        piece("chair_seat", [(130, 36, 139, 48)], 5, leave=HOUSE_FLOOR, solid=True),
        piece("pc", [(128, 9, 143, 30)], 16, base=9, leave=HOUSE_WALL, back=32),
        piece("book", [(114, 24, 126, 31)], 1, base=9),
        piece("desk", [(112, 20, 144, 40)], 9, leave=HOUSE_FLOOR, against=32),
        piece("console", [(83, 24, 98, 40)], 9, leave=HOUSE_WALL + HOUSE_FLOOR, back=32),
        # the television is Brendan's, tile for tile
        piece("pillow", [(114, 66, 126, 71)], 3, base=7, leave=HOUSE_FLOOR),
        piece("bed_head", [(108, 62, 132, 70)], 7, base=7, leave=HOUSE_FLOOR, through=True),
        piece("bed", [(108, 70, 132, 93)], 7, leave=HOUSE_FLOOR, solid=True,
              claim=[(108, 70, 132, 72)]),
        stairwell("stairwell", 16, 32, 32, 13, 19, (17, 14, 31, 26), 0),
        piece("wall", [(0, 0, 16, 32), (16, 0, 32, 13), (32, 0, 144, 32)], 32, fill=16,
              foot=32, side=(40, 0, 48, 32)),
        piece("side_w", [], 32, side=(40, 0, 48, 32), walls=[((0, 128), (0, 32))]),
        piece("side_e", [], 32, side=(40, 0, 48, 32), walls=[((144, 32), (144, 128))]),
    ]


# ── The two houses most of Hoenn lives in ─────────────────────────────────
#
# LAYOUT_HOUSE1 and LAYOUT_HOUSE2 (Oldale's two houses, and nineteen more
# maps'): a glass case and a chest of drawers or a kitchen along the back
# wall, a table and its chairs in the room, potted plants. Built like
# Littleroot's: furniture against the back wall stands on the wall's row of
# collision; what the second house repeats of the first tile for tile (the
# glass case, the plant in the corner) is the first's model
# (gen_voxel_buildings.reuse_pieces).

GENERIC_WALL = ("d5d5b4", "b4b4a4", "ffffff", "629c8b")   # plaster, trim, baseboard
GENERIC_FLOOR = ("ded552", "bdb431", "8b8b8b", "9c9410")  # the planks, their shade
GENERIC_RUG = ("ffcd8b", "f6f6a4")
GENERIC_TABLE = ("fff683", "bdac52")


def potted_plant(x, y):
    """The potted plant's outline, its crown's top-left at (x, y): the
    lines between the planks are drawn in the pot's own outline colour, so
    the shape follows the crown, the stem and the pot instead of a box."""
    return [(x, y, x + 16, y + 13), (x + 2, y + 13, x + 14, y + 14),
            (x + 3, y + 14, x + 13, y + 15), (x + 5, y + 15, x + 11, y + 16),
            (x + 6, y + 16, x + 10, y + 18), (x + 4, y + 18, x + 12, y + 19),
            (x + 3, y + 19, x + 13, y + 20), (x + 2, y + 20, x + 14, y + 30),
            (x + 4, y + 30, x + 12, y + 31)]


def house1():
    rug = GENERIC_RUG
    return (house_chair("chair_n", (74, 64, 78, 80), (65, 68, 74, 80), rug)
            + house_chair("chair_s", (74, 80, 78, 96), (65, 84, 74, 96), rug) + [
        # the teapot on the table
        piece("teapot", [(49, 72, 59, 80)], 5, base=11, leave=GENERIC_TABLE),
        # what the teapot hides of the table top is the table's own pixels
        piece("table", [(34, 64, 62, 97)], 11, leave=rug, solid=True, fill=1, foot=97),
        piece("plant_se", potted_plant(144, 113), 31, leave=GENERIC_FLOOR, card=True),
        piece("plant_1", potted_plant(128, 17), 31, leave=GENERIC_WALL + GENERIC_FLOOR, card=True),
        piece("plant_2", potted_plant(144, 17), 31, leave=GENERIC_WALL + GENERIC_FLOOR, card=True),
        # the glass case and the chest of drawers against the back wall
        piece("case", [(0, 12, 32, 40)], 20, leave=GENERIC_WALL + GENERIC_FLOOR, back=32),
        # on two feet a pixel in from its ends, the floor's shade between; the
        # line under them is the planks', drawn in its outline's colour
        piece("drawers", [(33, 11, 57, 39)], 21, leave=GENERIC_WALL + GENERIC_FLOOR, back=32,
              side="plain"),
        piece("wall", [(0, 0, 160, 32)], 32, fill=16, foot=32, side=(64, 0, 80, 32)),
        piece("side_w", [], 32, side=(64, 0, 80, 32), walls=[((0, 144), (0, 32))]),
        piece("side_e", [], 32, side=(64, 0, 80, 32), walls=[((160, 32), (160, 144))]),
    ])


def house2():
    fl = GENERIC_FLOOR
    # the east chairs' seats and the plant are the first house's, pixel for
    # pixel on another floor, and stand here as they are
    return (house_chair("chair_nw", (66, 64, 70, 80), (70, 68, 79, 80), fl)
            + house_chair("chair_sw", (66, 80, 70, 96), (70, 84, 79, 96), fl)
            + [piece("chair_ne_back", [(122, 64, 126, 80)], 9, leave=fl, solid=True),
               piece("chair_se_back", [(122, 80, 126, 96)], 9, leave=fl, solid=True)] + [
        # on its legs: the line under them is the planks', darkened
        piece("table", [(82, 64, 110, 95)], 9, leave=fl, solid=True),
        # the television on its glass-doored stand: Littleroot's set, whose
        # tile this is, on a stand a row shorter. Its doors' teal is the
        # baseboard's, and its own; its sides the set's casing over the
        # stand's middle post
        piece("tv", [(32, 13, 48, 39)], 19, leave=GENERIC_WALL[:3] + fl, back=32,
              top=(36, 14, 46, 15), side=[(32, 20, 34, 31), (39, 31, 41, 39)]),
        # the kitchen: a cupboard, the sink and hob, the fridge
        piece("tap", [(133, 16, 142, 25)], 8, base=8, leave=GENERIC_WALL[:2]),
        piece("cupboard", [(112, 17, 128, 40)], 16, leave=GENERIC_WALL + fl, back=32),
        piece("sink", [(128, 21, 160, 40)], 8, leave=GENERIC_WALL + fl, against=32),
        piece("fridge", [(160, 10, 176, 40)], 24, leave=GENERIC_WALL + fl, back=32),
        piece("wall", [(0, 0, 176, 32)], 32, fill=16, foot=32, side=(48, 0, 64, 32)),
        piece("side_w", [], 32, side=(48, 0, 64, 32), walls=[((0, 128), (0, 32))]),
        piece("side_e", [], 32, side=(48, 0, 64, 32), walls=[((176, 32), (176, 128))]),
    ])


def house_with_bed():
    """LAYOUT_HOUSE_WITH_BED (Petalburg's second house, and two more maps'):
    the two houses' room with a bed in it, two chests and a bookcase along
    the back wall, a table and two chairs."""
    fl = GENERIC_FLOOR
    # the chairs and the chest of drawers are the second house's, pixel for
    # pixel, and stand here as they are (gen_voxel_buildings.reuse_pieces)
    return [
        # on its legs; its cloth's stripes run front to back, so its sides
        # are the cloth's edge, not its front's stripes again
        piece("table", [(128, 64, 158, 95)], 9, leave=fl, solid=True, side="plain"),
        # the bed: the pillow lying on the mattress, and behind it the head,
        # an open iron frame the floor is seen through
        piece("pillow", [(10, 53, 22, 58)], 3, base=7, leave=fl),
        piece("bed_head", [(4, 48, 28, 56)], 7, base=7, leave=fl, through=True, side="plain"),
        piece("bed", [(4, 56, 28, 80)], 7, leave=fl, solid=True),
        # along the back wall: the green chest, the bookcase
        piece("chest", [(16, 15, 32, 40)], 17, leave=GENERIC_WALL + fl, back=32),
        # its frame's teal is the baseboard's
        piece("bookcase", [(128, 10, 160, 39)], 21, leave=GENERIC_WALL[:3] + fl, back=32),
        piece("wall", [(0, 0, 160, 32)], 32, fill=16, foot=32, side=(64, 0, 80, 32)),
        piece("side_w", [], 32, side=(64, 0, 80, 32), walls=[((0, 128), (0, 32))]),
        piece("side_e", [], 32, side=(64, 0, 80, 32), walls=[((160, 32), (160, 128))]),
    ]


# Its sliding doors (field_specials.c PetalburgGymSetDoorMetatiles): (x, y)
# of each one's top cell, the room's back wall's first row; its bottom cell
# under it. PetalburgGymSlideOpenRoomDoors opens them through five frames
# (SlidingDoor_Frame0-4, and the bottom's a row of the tileset on), and
# PetalburgGymUnlockRoomDoors sets the last at once.
PETALBURG_GYM_DOORS = [(1, 104), (7, 104), (1, 78), (7, 78), (1, 91), (7, 91), (7, 39),
                       (1, 52), (7, 52), (1, 65), (7, 13), (1, 26)]
PETALBURG_GYM_DOOR_FRAMES = 5
PETALBURG_GYM_DOOR = 0x218          # SlidingDoor_Frame0; its bottom 8 on
PETALBURG_GYM_SIDE = (16, 208, 48, 240)     # a stretch of the second room's panelling


def petalburg_door(x, y, frame):
    """The door whose top cell is (x, y), a stretch of its wall, as it stands
    while the game has it at `frame`."""
    return piece("wall_door_%d_%d_%d" % (x, y, frame),
                 [(x * 16, y * 16, x * 16 + 16, y * 16 + 32)], 32,
                 foot=y * 16 + 32, side=PETALBURG_GYM_SIDE,
                 when=(x, y, PETALBURG_GYM_DOOR + frame))


def petalburg_door_draw(frame):
    """The room with every door at `frame`, the doors' pieces read off it."""
    return {c: m for (x, y) in PETALBURG_GYM_DOORS
            for c, m in (((x, y), PETALBURG_GYM_DOOR + frame),
                         ((x, y + 1), PETALBURG_GYM_DOOR + 8 + frame))}


def petalburg_gym():
    """Petalburg's gym: nine rooms one under another in the black between
    them, each nine cells wide and eight deep under a back wall two cells
    tall, its plate drawn on the wall's face and its sliding doors in it:
    each door a stretch of the wall that stands while the game has it at its
    frame (petalburg_door, `when`), as it slides open; the wall round them
    stands whole. The drawing has no side walls: each room gets its pair, to
    close it from the console's camera."""
    side = PETALBURG_GYM_SIDE
    out = []
    for room in range(9):
        y = room * 13 * 16
        doors = sorted(dx for (dx, dy) in PETALBURG_GYM_DOORS if dy * 16 == y)
        rects, x = [], 0
        for d in doors:
            if d * 16 > x:
                rects.append((x, y, d * 16, y + 32))
            x = d * 16 + 16
        rects.append((x, y, 144, y + 32))
        out += [petalburg_door(d, y // 16, 0) for d in doors]
        out.append(piece("wall_%d" % room, rects, 32, fill=16, foot=y + 32, side=side))
        # each a plane along the room's edge: the cells beside it are the
        # map's, the entrance's yellow floor as the rooms' boards
        bottom = (room * 13 + 8) * 16
        beside = lambda cx: [(cx, cy) for cy in range(y // 16, bottom // 16)]   # noqa: E731
        out += [piece("side_w%d" % room, [], 32, side=side, walls=[((0, bottom), (0, y + 32))],
                      skip=beside(0)),
                piece("side_e%d" % room, [], 32, side=side, walls=[((144, y + 32), (144, bottom))],
                      skip=beside(8))]
    return out


def petalburg_gym_doors(frame):
    return [petalburg_door(x, y, frame) for (x, y) in PETALBURG_GYM_DOORS]


# ── The other houses of the general indoor tileset ────────────────────────
#
# Rooms no one has modelled piece by piece yet: a back wall two cells tall,
# blocked all across, over a rectangle of floor. Their walls stand - the back
# one, and where the floor runs to the room's edges the two sides the drawing
# has no pixel of - and their furniture stays drawn on the floor until the
# room gets pieces of its own. Each: its layout, its size in cells, its floor,
# the wall's cell the sides are dressed with (bare panelling: the pair of
# metatiles the wall repeats most), and whether it has side walls.

PLAIN_ROOMS = [
    ("LAYOUT_HOUSE3", 10, 8, 0x229, 1, True),
    ("LAYOUT_DEWFORD_TOWN_HALL", 17, 9, 0x223, 3, True),
    ("LAYOUT_HOUSE4", 10, 9, 0x229, 3, True),
    ("LAYOUT_LILYCOVE_CITY_HOUSE2", 8, 8, 0x229, 3, True),
    ("LAYOUT_VERDANTURF_TOWN_WANDAS_HOUSE", 17, 8, 0x223, 5, True),
    ("LAYOUT_PACIFIDLOG_TOWN_HOUSE1", 10, 9, 0x3A3, 4, True),
    ("LAYOUT_PACIFIDLOG_TOWN_HOUSE2", 10, 9, 0x3A3, 5, True),
    ("LAYOUT_RUSTBORO_CITY_HOUSE", 12, 9, 0x32C, 3, True),
    ("LAYOUT_RUSTBORO_CITY_HOUSE1", 13, 8, 0x32C, 4, True),
    ("LAYOUT_RUSTBORO_CITY_CUTTERS_HOUSE", 11, 9, 0x32C, 4, True),
    ("LAYOUT_FORTREE_CITY_HOUSE1", 8, 6, 0x3D9, 2, True),
    ("LAYOUT_FORTREE_CITY_HOUSE2", 8, 6, 0x3D9, 1, True),
    ("LAYOUT_ROUTE116_TUNNELERS_REST_HOUSE", 10, 9, 0x229, 3, True),
    ("LAYOUT_ROUTE110_TRICK_HOUSE_ENTRANCE", 12, 8, 0x229, 1, True),
    ("LAYOUT_FORTREE_CITY_DECORATION_SHOP", 8, 6, 0x3D9, 1, False),
    ("LAYOUT_SOOTOPOLIS_CITY_LOTAD_AND_SEEDOT_HOUSE", 8, 7, 0x3FE, 2, True),
    ("LAYOUT_SOOTOPOLIS_CITY_HOUSE1", 8, 7, 0x3FE, 1, True),
    ("LAYOUT_SOOTOPOLIS_CITY_HOUSE2", 8, 7, 0x3FE, 4, True),
    ("LAYOUT_SOOTOPOLIS_CITY_HOUSE3", 8, 7, 0x3FE, 3, True),
    ("LAYOUT_MOSSDEEP_CITY_STEVENS_HOUSE", 11, 8, 0x223, 3, True),
]


def plain_room(width, height, plain_x, sides):
    w, h = width * 16, height * 16
    side = (plain_x * 16, 0, plain_x * 16 + 16, 32)
    pieces = [piece("wall", [(0, 0, w, 32)], 32, fill=16, foot=32, side=side)]
    if sides:
        pieces += [piece("side_w", [], 32, side=side, walls=[((0, h), (0, 32))]),
                   piece("side_e", [], 32, side=side, walls=[((w, 32), (w, h))])]
    return pieces


# ── Mr. Briney's house ────────────────────────────────────────────────────
#
# A room of tatami by the dock on Route 104: a chest of drawers, a small one
# and a glass case along the back wall, three jars in the corner, a low table.

BRINEY_FLOOR = ("c5ffac", "a4cd6a", "83ac4a", "7b7b83", "6a8b31", "414a6a")
BRINEY_WALL = ("d5b483", "ffffff", "d5c54a", "947329", "ac8b39", "629c8b")


def briney_room():
    # the furniture's outline is the tatami's dark border line, and its wood
    # the wall's pillars' and baseboard's: each piece leaves only the colours
    # of the room that are round it, the plaster and the tatami
    fl = BRINEY_FLOOR
    mat = tuple(c for c in fl if c != "414a6a")
    plaster = ("d5b483", "d5c54a")
    side = (48, 0, 64, 32)
    return [
        # low, on four short legs: its top's edge band and the dark under it
        # its front
        piece("table", [(114, 67, 142, 95)], 7, leave=mat, solid=True),
        # turned, their mouths open on top: belly, shoulder and neck as drawn
        piece("jar_s", [(1, 65, 15, 79)], 7, leave=mat, round="lathe"),
        piece("jar_w", [(1, 49, 15, 63)], 7, leave=mat, round="lathe"),
        piece("jar_e", [(17, 49, 31, 63)], 7, leave=mat, round="lathe"),
        piece("drawers", [(17, 10, 40, 40)], 22, leave=plaster + ("947329",) + mat, back=32),
        piece("chest", [(128, 16, 144, 45)], 16, leave=plaster + mat, back=32, side="plain"),
        piece("case", [(144, 12, 176, 45)], 20, leave=plaster + mat, back=32),
        piece("wall", [(0, 0, 192, 32)], 32, fill=16, foot=32, side=side),
        piece("side_w", [], 32, side=side, walls=[((0, 144), (0, 32))]),
        piece("side_e", [], 32, side=side, walls=[((192, 32), (192, 144))]),
    ]


# ── Rooms with a flight of stairs in the back wall ────────────────────────
#
# Rustboro's flats (and whatever else is built like them): the plain room's
# walls, with a doorway a cell wide in the back wall for every flight. Like
# Littleroot's: the flight is drawn inside the doorway as the GBA sees it from
# above, so it lies on the floor of a recess one cell deep, with the doorway's
# sides and back round it - a way out of the room, not a picture of stairs
# flat on the floor. Each: its layout, its size in cells, its floor, the
# wall's cell the sides are dressed with, and the doorways' columns - and, in
# Rustboro's second block of flats, where the wall that parts the room in two
# runs down from the back wall (its left edge), with the low wall the same
# three floors have along the front of the east half.

FLAT_FLOOR = ("d5d5b4", "f6f6a4", "b4b4a4", "8b8b8b", "ded552")

STAIR_ROOMS = [
    ("LAYOUT_RUSTBORO_CITY_FLAT1_1F", 14, 8, 0x32C, 5, (2,)),
    ("LAYOUT_RUSTBORO_CITY_FLAT1_2F", 14, 8, 0x32C, 5, (2,)),
    ("LAYOUT_RUSTBORO_CITY_FLAT2_1F", 14, 9, 0x32C, 1, (3,), 80),
    ("LAYOUT_RUSTBORO_CITY_FLAT2_2F", 14, 9, 0x32C, 9, (1, 3), 80),
    ("LAYOUT_RUSTBORO_CITY_FLAT2_3F", 14, 9, 0x32C, 3, (1,), 64),
    ("LAYOUT_LILYCOVE_CITY_COVE_LILY_MOTEL_1F", 12, 9, 0x229, 4, (2,)),
    ("LAYOUT_LILYCOVE_CITY_COVE_LILY_MOTEL_2F", 12, 9, 0x229, 4, (2,)),
    ("LAYOUT_ROUTE114_FOSSIL_MANIACS_HOUSE", 10, 8, 0x229, 2, (4,)),
    ("LAYOUT_ROUTE110_TRICK_HOUSE_END", 12, 8, 0x229, 6, (2, 10)),
    # the Devon Corporation's upper floors
    ("LAYOUT_RUSTBORO_CITY_DEVON_CORP_2F", 19, 9, 0x380, 4, (2, 14)),
    ("LAYOUT_RUSTBORO_CITY_DEVON_CORP_3F", 19, 9, 0x380, 4, (2,)),
]


def stair_room(width, height, plain_x, doors, partition=None):
    w, h = width * 16, height * 16
    side = (plain_x * 16, 0, plain_x * 16 + 16, 32)
    wall, pieces, x = [], [], 0
    if partition is not None:
        # seen from above: the white of their tops, and a face at the end
        pieces += [
            piece("divider", [(128, 90, 224, 127)], 27, solid=True, leave=FLAT_FLOOR),
            piece("partition", [(partition, 0, partition + 16, 127)], 29, solid=True),
        ]
    for door in sorted(doors):
        d = door * 16
        wall += [(x, 0, d, 32), (d, 0, d + 16, 13)]
        # the doorway: 19 pixels tall under the 13 of wall over it, its
        # sides the dark of the flight's own well
        pieces.append(stairwell("stairwell_%d" % door, d, d + 16, 32, 13, 19,
                                (d + 1, 14, d + 15, 26), 0))
        x = d + 16
    wall.append((x, 0, w, 32))
    return pieces + [
        piece("wall", wall, 32, fill=16, foot=32, side=side),
        piece("side_w", [], 32, side=side, walls=[((0, h), (0, 32))]),
        piece("side_e", [], 32, side=side, walls=[((w, 32), (w, h))]),
    ]


# ── Rustboro's Pokemon school ─────────────────────────────────────────────
#
# One classroom. Its back wall is drawn from 8 rows down the first cell (the
# black over it is nothing) to its foot 36 rows down, the blackboard and the
# windows on its face; the pillars' feet and the teacher's step, drawn a few
# rows further, stay on the floor.

def school_room():
    side = (176, 8, 192, 36)
    return [
        piece("wall", [(0, 8, 192, 36)], 28, fill=16, foot=36, side=side),
        piece("side_w", [], 28, side=side, walls=[((0, 176), (0, 36))]),
        piece("side_e", [], 28, side=side, walls=[((192, 36), (192, 176))]),
    ]


# ── The Pretty Petal flower shop ──────────────────────────────────────────
#
# Route 104's shop: planters of flowers and racks of potted plants about the
# floor, two tables of pots and a shelf along the back wall. The counter, the
# stools and the vases stay drawn on the floor. A potted plant on a stand or
# a table is a card lifted onto it, as the plants on the floor are cards.

SHOP_FLOOR = ("eef6ff", "d5d5e6", "b4acb4")
SHOP_WALL = ("bdbd94", "e6e68b", "ffffbd", "629c62", "9cd59c", "4a7b41")


def flower_shop_room():
    fl = SHOP_FLOOR
    side = (48, 0, 64, 32)

    def planter(name, x, y0, y1):
        return piece(name, [(x, y0, x + 16, y1)], 10, leave=fl, solid=True)

    def rack(name, x):
        # a low stand of green slats on four legs, the floor seen between
        # them, three potted plants in a row along it, each pot's rim
        # twelve rows before the one in front:
        # each plant its crown, the leaf tips either side of the pot behind
        # it, its rim and its pot. Where a crown is drawn over the pot behind
        # it, that pot's; under each pot's foot, the stand's top
        def plant(part, r, crown, foot):
            return piece("%s_%s" % (name, part),
                         crown + [(x + 2, r - 3, x + 14, r), (x + 3, r, x + 13, r + 3),
                                  (x + 4, r + 3, x + 12, r + 8)] + foot,
                         16, base=8, leave=fl, card=True)

        def tips(r):
            return [(x + 3, r - 7, x + 4, r - 3), (x + 12, r - 7, x + 13, r - 3)]
        return [
            plant("plant_s", 111, tips(111), [(x + 6, 119, x + 10, 120)]),
            plant("plant_c", 99, tips(99), []),
            plant("plant_n", 87, [(x + 2, 80, x + 14, 84)], []),
            piece(name, [(x, 87, x + 16, 128)], 8, leave=fl, side="plain", through=True),
        ]

    def back_table(name, x):
        # a low table on four legs against the wall, three cacti in pots
        # along its back and two plants before them, each its own card on
        # it: white flowers in a grey pot, red in a yellow one. What a plant
        # is drawn over of a pot behind it is the plant's
        def card(part, shape, leave=fl):
            return piece("%s_%s" % (name, part), shape, 12, base=7, leave=leave, card=True)

        white = [(x + 6, 27, x + 7, 28), (x + 11, 27, x + 12, 28), (x + 5, 28, x + 13, 29),
                 (x + 3, 29, x + 15, 30), (x + 2, 30, x + 16, 33), (x + 3, 33, x + 15, 35),
                 (x + 4, 35, x + 14, 37), (x + 5, 37, x + 13, 38), (x + 6, 38, x + 12, 39),
                 (x + 7, 39, x + 11, 40)]
        red = [(x + 20, 27, x + 21, 28), (x + 25, 27, x + 26, 28), (x + 19, 28, x + 27, 29),
               (x + 17, 29, x + 29, 30), (x + 16, 30, x + 30, 33), (x + 17, 33, x + 29, 35),
               (x + 18, 35, x + 28, 37), (x + 19, 37, x + 27, 38), (x + 20, 38, x + 26, 39),
               (x + 21, 39, x + 25, 40)]
        return [card("white", white), card("red", red)] + [
            card("cactus_%d" % (k + 1), [(x + 3 + 9 * k, 19, x + 11 + 9 * k, 29)],
                 fl + ("e6e68b",)) for k in range(3)] + [
            piece(name, [(x, 25, x + 32, 48)], 7, leave=fl, back=32, side="plain"),
        ]

    return [
        planter("planter_sw", 0, 112, 144),
        planter("planter_w", 80, 96, 128), planter("planter_c", 128, 96, 128),
        planter("planter_e1", 192, 80, 128), planter("planter_e2", 208, 80, 128),
    ] + rack("rack_1", 96) + rack("rack_2", 112) + rack("rack_e", 224) + [
        planter("planter_n1", 176, 48, 64), planter("planter_n2", 192, 48, 64),
        piece("shelf", [(112, 24, 152, 56)], 18, leave=SHOP_WALL + fl, back=42),
    ] + back_table("table_w", 160) + back_table("table_e", 192) + [
        piece("wall", [(0, 0, 240, 32)], 32, fill=16, foot=32, side=side),
        piece("side_w", [], 32, side=side, walls=[((0, 144), (0, 32))]),
        piece("side_e", [], 32, side=side, walls=[((240, 32), (240, 144))]),
    ]


# ── Dewford's gym ─────────────────────────────────────────────────────────
#
# A maze cut in rock two cells tall: every blocked cell is rock, its top or
# the two rows of its face, so each column's run of blocked cells (x, y0, y1,
# y1 inclusive, read off the collision) is a block drawn from the run's first
# row (and the lip of its top 8 rows north of it) to its foot, a piece to
# each rectangle of them (run_rects). The rock
# down the west and east edges, the leader's alcove and the statues are not
# among them.

DEWFORD_GYM_RUNS = [
    (1, 6, 9), (1, 12, 14), (1, 18, 27), (2, 7, 8), (2, 19, 20), (2, 23, 27), (3, 7, 9),
    (3, 11, 15), (3, 17, 21), (3, 24, 27), (4, 7, 9), (4, 11, 15), (4, 17, 21), (4, 26, 27),
    (5, 7, 13), (5, 17, 21), (6, 6, 13), (6, 15, 21), (7, 0, 4), (7, 6, 7), (7, 9, 12),
    (7, 15, 21), (7, 26, 27), (8, 0, 4), (8, 6, 7), (8, 9, 12), (8, 15, 17), (8, 23, 27),
    (9, 0, 4), (9, 6, 7), (9, 15, 20), (9, 23, 27), (10, 0, 2), (10, 6, 7), (10, 9, 13),
    (10, 16, 20), (10, 23, 27), (11, 0, 3), (11, 6, 7), (11, 9, 13), (11, 23, 27),
    (12, 0, 4), (12, 16, 18), (12, 26, 27), (13, 0, 5), (13, 9, 18), (13, 20, 21),
    (13, 23, 27), (14, 0, 5), (14, 7, 11), (14, 13, 17), (14, 20, 21), (14, 23, 27),
    (15, 0, 2), (15, 7, 11), (15, 13, 17), (15, 20, 21), (15, 23, 27), (16, 0, 8),
    (16, 19, 27),
]
# the floor's shadow: its shaded metatiles carry the lips of the blocks'
# tops too, the blocks' own
DEWFORD_GYM_FLOOR = ["737352"]


def dewford_gym():
    # A block's front is its face alone, 24 px from its foot: the white lip
    # above it is its top's edge, as down its sides and along its back, and
    # the rock's brown lies behind it. Its sides (solid) are that face too:
    # projected off its drawing, they were its outline's navy drawn out
    # across them.
    side = (128, 424, 144, 448)     # a stretch of the front wall's face
    # The rock the room is cut in, down its west and east edges, is a wall's
    # face each side and not a block: a block's art is its drawing three times
    # over (its hidden top and sides), and those two columns' would put the
    # layout's page past the console's 512x512.
    return [
        # the leader's alcove: the bookcases either side of the dais, their
        # sides a plain patch of their olive, against its back wall, whole
        # behind them with its emblem
        piece("shelf_w", [(16, 32, 32, 64)], 24, against=48, side=(19, 41, 29, 47)),
        piece("shelf_e", [(96, 32, 112, 64)], 24, against=48, side=(99, 41, 109, 47)),
        piece("block_alcove", [(16, 0, 112, 48)], 24, side=side),
    ] + [
        # each drawn from 8 rows into the cell north of it, as Rustboro's
        # blocks: the white lip of its top lies there, on the floor in
        # front of the block north. A run a single cell tall stands 4 px
        # lower than its cell, or it would be all front and no top
        piece("block_%d_%d" % (x0 // 16, y0 // 16), [(x0, y0 - 8 if y0 else 0, x1, y1)],
              min(24, y1 - y0 - 4), side=side, solid=True)
        for (x0, y0, x1, y1) in run_rects(DEWFORD_GYM_RUNS)
    ] + [
        # the rock down the west and east edges, in three stretches each
        # (one the room's length would take a page of its own), each
        # stretch's top run back over the one north of it, where that one's
        # front stands, hidden - laid with its own first rows of rock, not
        # the lip its top ends in
        piece("block_edge_%d_%d" % (x, y0), [(x * 16, y0 * 16, x * 16 + 16, y1 * 16)], 24,
              side=side, solid=True, back=y0 * 16 if y0 else None,
              top=(x * 16, y0 * 16, x * 16 + 16, y0 * 16 + 24) if y0 else None)
        for x in (0, 17) for (y0, y1) in ((0, 10), (10, 19), (19, 28))
    ]


# ── Professor Birch's lab ─────────────────────────────────────────────────
#
# Desks and a bookcase along the back wall, standing on its row of collision
# (so no deeper than the drawing puts them in front of it); in the room, the
# boxes, book stacks, bookcases and desks run back as far as the cells they
# block, and the machine is read column by column off its round drawing.

LAB_SHADOW = ("b4b4a4", "949494")      # the floor's shade, and its grid in it
LAB_FLOOR = ("d5d5b4",)                 # the floor itself, where it shows beside a stack


def lab_machine():
    """The machine under the lab's dome in plan: an octagon whose edges
    pass between the drawing's pixels - every centre of one of its pixels
    inside, every one of the floor's out - so that at 45 degrees no face
    behind shows at a corner. The back corners follow the drawn top's rows
    (one wider the first row, then one a row, then the drawing's kink
    where two rows end alike), the front ones the drawn foot, a slant half
    a pixel out of its pixels' corners; symmetric about x 176."""
    left = [(160, 120.5), (160, 113.5), (161, 112.5), (161, 111.5), (165, 107.5), (168, 106)]
    right = [(352 - x, z) for (x, z) in reversed(left)]
    return left + right + [(184.5, 128), (167.5, 128)]


def lab():
    sh = LAB_SHADOW
    return [
        piece("plant_s", [(48, 189, 64, 208)], 18, card=True),
        # the desk down the west side: its computer at the far end, the book
        # stacks against the wall, the chair
        # seen from behind: its back stands at the back of its seat
        piece("chair_sw_back", [(34, 160, 47, 167)], 7, base=4, leave=sh, solid=True),
        piece("chair_sw_seat", [(34, 167, 47, 176)], 4, leave=sh, solid=True),
        piece("books_sw1", [(1, 160, 16, 176)], 9, leave=sh + LAB_FLOOR, back=160),
        piece("books_sw2", [(1, 176, 16, 192)], 9, leave=sh + LAB_FLOOR, back=176),
        # the computer at its north end, inside its own outline: the desk's
        # rails run on beside it, its top back to row 148 under it
        piece("pc_sw", [(18, 145, 30, 160)], 8, base=8, solid=True),
        piece("desk_sw", [(16, 160, 32, 192), (16, 148, 18, 160), (30, 148, 32, 160)], 8,
              leave=sh, back=156),
        # and its twin down the east side
        piece("plant_se", [(192, 141, 208, 161)], 18, card=True),
        piece("boxes_se", [(192, 164, 208, 192)], 20, leave=sh, back=176),
        piece("chair_se_back", [(161, 160, 165, 176)], 9, leave=sh, solid=True),
        piece("chair_se_seat", [(165, 164, 176, 176)], 5, leave=sh, solid=True),
        piece("pc_se", [(178, 145, 190, 160)], 8, base=8, solid=True),
        piece("desk_se", [(176, 160, 192, 192), (176, 148, 178, 160), (190, 148, 192, 160)], 8,
              leave=sh, back=156),
        # the machine, its dome, the two canisters beside it: a round glass
        # dome on a machine whose sides are its casing, its shadow the floor's
        piece("dome", [("ellipse", 176, 103.5, 8.5, 7.5)], 5, base=10, round="dome"),
        # an octagon, its corners cut on the slant as drawn (lab_machine):
        # a box, its sides square to the corners' slants, stood a dark slab
        # beside it
        piece("machine", [(160, 95, 192, 128)], 10, leave=sh, side=(172, 118, 180, 128),
              plan=lab_machine()),
        piece("canisters", [(192, 109, 208, 128)], 16, leave=sh, back=112),
        # the two bookcases in the room, a box on each
        piece("box_w1", [(8, 82, 23, 96)], 6, base=21, solid=True),
        piece("box_w2", [(40, 82, 55, 96)], 6, base=21, solid=True),
        piece("bookcase_w1", [(0, 85, 32, 128)], 21, back=96),
        piece("bookcase_w2", [(32, 85, 64, 128)], 21, back=96),
        # boxes and book stacks in the north-east corner
        piece("boxes_ne1", [(144, 56, 160, 80)], 8, leave=sh),
        piece("boxes_ne2", [(160, 52, 176, 80)], 18, leave=sh, back=64),
        piece("books_ne1", [(177, 32, 193, 48)], 8, leave=sh, back=32),
        piece("books_ne2", [(193, 32, 208, 48)], 8, leave=sh, back=32),
        # its shape round the floor its top leaves beside it, under the next stack
        piece("books_ne3", [(193, 48, 205, 53), (193, 53, 208, 64)], 8, leave=sh + LAB_FLOOR,
              back=48),
        piece("chair_n_back", [(75, 48, 79, 64)], 9, leave=sh, solid=True),
        piece("chair_n_seat", [(64, 52, 75, 64)], 5, leave=sh, solid=True),
        piece("boxes_w", [(0, 41, 16, 64)], 7, leave=sh),
        # along the back wall
        piece("plant_nw", [(32, 29, 48, 48)], 18, card=True),
        # the computer: a tower and a monitor whose white tops are drawn
        # above their fronts (the drive bays, the screen from its bezel down)
        piece("computer", [(49, 10, 72, 30)], 14, base=8, back=32),
        piece("desk_pc", [(48, 20, 80, 39)], 8, leave=sh, against=32),
        # books lying on the desks: a cover on top and the pages' edges
        # below it - a big red one, a stack of two blue ones, one askew.
        # Each inside its own outline: the line down the desks' join and
        # the desk's beige below a book's corner are the desks'. Drawn
        # over the desk's rows the wall hides, so is a book's back
        piece("book_red", [(117, 19, 127, 30)], 3, base=8, against=32),
        piece("book_open", [(131, 19, 147, 31)], 3, base=8, against=32),
        piece("binder", [(147, 17, 156, 30)], 5, base=8, against=32),
        piece("desk_a", [(96, 20, 128, 39)], 8, leave=sh, against=32),
        piece("desk_b", [(128, 20, 160, 39)], 8, leave=sh, against=32),
        piece("bookcase_nw", [(0, 8, 32, 40)], 21, against=32),
        piece("wall", [(0, 0, 208, 32)], 32, fill=16, foot=32, side=(80, 0, 96, 32)),
        piece("side_w", [], 32, side=(80, 0, 96, 32), walls=[((0, 208), (0, 32))]),
        piece("side_e", [], 32, side=(80, 0, 96, 32), walls=[((208, 32), (208, 208))]),
    ]


# ── Rustboro's gym ────────────────────────────────────────────────────────
#
# A maze of low stone walls on a tiled floor: each block stands on the cells
# it blocks, its top drawn from 8 rows into the cell north of them and its
# front the last 10 rows of its own. The floor's shadows are floor. At the
# back, the gym's crenellated wall, one cell deeper at either end.

GYM_WALL_SIDE = (32, 7, 48, 32)
GYM_FLOOR = ("bdbdac",)             # the floor's shade round a statue's foot     # a stretch of the back wall without the emblem


def gym_block(name, rects):
    """A maze block: `rects` in cells (x0, y0, x1, y1), each drawn from 8 rows
    north of its cells to its foot."""
    return piece(name, [(x0 * 16, y0 * 16 - 8, x1 * 16, y1 * 16) for (x0, y0, x1, y1) in rects],
                 10, side=(56, 118, 72, 128))


def gym_statue(x):
    """A gym statue's outline, its cell's left edge at x: its pedestal's
    bottom corners are rounded off, the floor showing there."""
    return [(x, 272, x + 16, 302), (x + 1, 302, x + 15, 303), (x + 2, 303, x + 14, 304)]


def rustboro_gym():
    return [
        # the statues by the door: a ball on a pedestal. Every gym has them,
        # on its own floor: they stand in each as they are (reuse_pieces),
        # so they must not take the floor showing at their feet
        # (the ball found by its own few pixels, standing on the pedestal)
        piece("statue_w_ball", [("ellipse", 40, 279, 7, 7)], 8, base=16, found=True),
        piece("statue_w", gym_statue(32), 16, back=288),
        piece("statue_e_ball", [("ellipse", 136, 279, 7, 7)], 8, base=16, found=True),
        piece("statue_e", gym_statue(128), 16, back=288),
        # the maze, front first
        gym_block("block_sw", [(2, 13, 4, 15), (2, 15, 5, 16)]),
        gym_block("block_s", [(6, 15, 8, 16)]),
        gym_block("block_w", [(4, 9, 5, 11), (2, 11, 5, 12)]),
        gym_block("block_c", [(6, 9, 8, 11)]),
        # the comb along the north and east, in three: a piece's art is its
        # whole rectangle, and one of 11 x 10 cells for this outline would
        # take a texture page of half a megabyte. Cut between columns, so no
        # column's run is split
        gym_block("block_e_arm", [(6, 12, 9, 13), (7, 13, 9, 14)]),
        gym_block("block_e", [(9, 6, 11, 16)]),
        gym_block("block_n", [(3, 6, 9, 8)]),
        gym_block("block_west", [(0, 6, 1, 16)]),
        # the back wall and its two deeper ends
        piece("wall_w", [(0, 0, 16, 48)], 25, fill=16, foot=48, side=GYM_WALL_SIDE),
        piece("wall_e", [(160, 0, 176, 48)], 25, fill=16, foot=48, side=GYM_WALL_SIDE),
        piece("wall", [(16, 0, 160, 32)], 25, fill=16, foot=32, side=GYM_WALL_SIDE),
        # the side walls in two each: one the room's length would need a
        # texture page too tall to share VRAM with the city's
        piece("side_w", [], 25, side=GYM_WALL_SIDE, walls=[((0, 176), (0, 48))]),
        piece("side_w2", [], 25, side=GYM_WALL_SIDE, walls=[((0, 320), (0, 176))]),
        piece("side_e", [], 25, side=GYM_WALL_SIDE, walls=[((176, 48), (176, 176))]),
        piece("side_e2", [], 25, side=GYM_WALL_SIDE, walls=[((176, 176), (176, 320))]),
    ]


# ── The contest stage ─────────────────────────────────────────────────────
#
# The stage a contest is held on (LAYOUT_CONTEST_HALL and its five coloured
# twins; Lilycove's contest hall draws three of them again): one floor, every
# cell of it walked on at the same elevation, so nothing on it stands up but
# what blocks the way - the low cushions round the green, the trophy's case
# and the back wall. The stairs down its front and the emblem in the middle
# are walked on, and stay drawn on the floor.

STAGE_FLOOR = ("94acb4", "738383", "838b83", "bddec5")   # the dotted floor, its channel, the green

# the cushions round the green, each with its dark outline: the back row,
# the two sides, the front row either side of the stairs
STAGE_CUSHIONS = (
    [(x, 48, x + 14, 55) for x in range(49, 178, 16)]
    # (a side one's shape stops at its front's last row - a shape takes its
    # bottom row, and the row under its front is the channel's grey between
    # its side lines)
    + [(x, y, x + 5, y + 13) for y in range(57, 122, 16) for x in (49, 185)]
    + [(x, 136, x + 14, 143) for x in (49, 65, 81, 145, 161, 177)])


def contest_stage():
    cushions = sorted(STAGE_CUSHIONS, key=lambda r: (-r[3], r[0]))
    return [piece("cushion_%d" % i, [r], 4, leave=STAGE_FLOOR) for i, r in enumerate(cushions)] + [
        # the trophy in its glass case, by the green's north-east corner
        piece("case", [(144, 73, 160, 88)], 8, leave=STAGE_FLOOR),
        piece("wall", [(0, 0, 240, 16)], 16, fill=16, foot=16, side=(0, 0, 16, 16)),
    ]


# ── Lilycove's contest lobby ──────────────────────────────────────────────
#
# The reception's counter, a U whose arms run back to the wall, the terminal
# beside it, the four Berry Blenders, and the back wall with its two
# doorways into the hall. The sofas down the side walls stand on cells the
# player walks on, and stay drawn on the floor.

LOBBY_FLOOR = ("f6b49c", "d5ac7b", "cd9c7b", "b48b5a", "c55231")   # the checker, the wall's shade and foot
LOBBY_WALL_SIDE = (16, 3, 32, 32)       # a stretch of curtain with nothing on it
LOBBY_CURTAIN = ("cd295a", "de6a52")    # the curtain, seen past the counter's rounded back corners


def contest_lobby():
    blenders = [piece("blender_%d_%d" % (bx, by), [(bx, by, bx + 16, by + 15)], 8, leave=LOBBY_FLOOR)
                for (bx, by) in ((368, 144), (432, 144), (368, 80), (432, 80))]
    return blenders + [
        # the counter, a U in plan: a bar across the front as deep as its
        # drawn top (z 54 to 63), an arm back towards the wall at each end
        # (from z 35, its back corners nicked as the Center's are, where the
        # drawing leaves the wall), the
        # receptionists' floor between them; 10 px tall, its
        # front the green lip and the white panels. The bow on the east arm's
        # front corner is its own paint; the row under its front, its shadow.
        # In three: the lobby's script opens and closes the flaps beside the
        # arms (cells 12,2-3 and 17,2-3, setmetatile), so they stay drawn - a
        # piece's rectangle would lay them in its model, shut
        piece("counter_w", [(176, 25, 192, 62)], 10, leave=LOBBY_FLOOR + LOBBY_CURTAIN,
              plan=[(176, 36.2), (177.2, 35), (190.8, 35), (192, 36.2), (192, 63), (176, 63)]),
        piece("counter", [(208, 44, 272, 62)], 10, leave=LOBBY_FLOOR + LOBBY_CURTAIN,
              plan=[(208, 54), (272, 54), (272, 63), (208, 63)]),
        piece("counter_e", [(288, 25, 304, 62)], 10, leave=LOBBY_FLOOR + LOBBY_CURTAIN,
              plan=[(288, 36.2), (289.2, 35), (302.8, 35), (304, 36.2), (304, 63), (288, 63)]),
        piece("terminal", [(304, 8, 320, 39)], 24, back=32),
        # the back wall, its two doorways painted on its face as Lavaridge's
        # Center's door to the spring is
        piece("wall", [(0, 3, 496, 32)], 29, fill=16, foot=32, side=LOBBY_WALL_SIDE),
        piece("side_w", [], 29, side=LOBBY_WALL_SIDE, walls=[((0, 192), (0, 32))]),
        piece("side_e", [], 29, side=LOBBY_WALL_SIDE, walls=[((496, 32), (496, 192))]),
    ]


# ── Lilycove's contest hall ───────────────────────────────────────────────
#
# The hall the three stages stand in (their pieces are the contest stage's,
# found again). Its back wall steps forward in three tiers either side - the
# judges' portraits in the middle, a plaque on each wing -, each 60 px from
# the cap drawn over it to its foot; side walls close each step and the
# hall. The potted palms are cards; the chairs stand on walked cells and
# stay drawn on the floor.

HALL_FLOOR = ("f6b49c", "d5ac7b")
HALL_WALL_SIDE = (176, 4, 192, 64)      # a stretch of the middle's curtain with nothing on it
HALL_PALMS = ((8, 9), (42, 9), (2, 11), (5, 11), (45, 11), (48, 11),
              (3, 32), (7, 32), (11, 32), (39, 32), (43, 32), (47, 32))   # their pots' cells


def lilycove_contest_room():
    side = HALL_WALL_SIDE
    palms = sorted(HALL_PALMS, key=lambda c: (-c[1], c[0]))
    return [piece("palm_%d_%d" % (cx, cy), [(cx * 16, cy * 16 - 16, cx * 16 + 16, cy * 16 + 14)],
                  31, leave=HALL_FLOOR, card=True) for (cx, cy) in palms] + [
        piece("wall_ow", [(0, 68, 80, 128)], 60, fill=16, foot=128, side=side),
        piece("wall_oe", [(736, 68, 816, 128)], 60, fill=16, foot=128, side=side),
        piece("wall_ww", [(80, 36, 160, 96)], 60, fill=16, foot=96, side=side),
        piece("wall_we", [(656, 36, 736, 96)], 60, fill=16, foot=96, side=side),
        # the middle in two halves, as the Center's back wall: one 496 px
        # long left no room beside it on the hall's texture page
        piece("wall_e", [(408, 4, 656, 64)], 60, fill=16, foot=64, side=side),
        piece("wall", [(160, 4, 408, 64)], 60, fill=16, foot=64, side=side),
        # the steps' sides, then the hall's
        piece("side_step_ow", [], 60, side=side, walls=[((80, 128), (80, 96))]),
        piece("side_step_ww", [], 60, side=side, walls=[((160, 96), (160, 64))]),
        piece("side_step_we", [], 60, side=side, walls=[((656, 64), (656, 96))]),
        piece("side_step_oe", [], 60, side=side, walls=[((736, 96), (736, 128))]),
        piece("side_w", [], 60, side=side, walls=[((0, 528), (0, 128))]),
        piece("side_e", [], 60, side=side, walls=[((816, 128), (816, 528))]),
    ]


# ── Lilycove's Department Store ───────────────────────────────────────────
#
# 1F: the reception's counter, a U whose arms run back to the wall with a
# computer on each, and two planters by the west wall. Its potted palms are
# the Center's (reused); the stools stand on walked cells and stay drawn on
# the floor. The elevator's door and the escalator's are painted on the back
# wall, as Lavaridge's Center's door is.

DEPT_1F_FLOOR = ("cd8320", "ac5a20", "e69c39", "f6c539")     # the orange floor
DEPT_WALL = ("d5ded5", "b4b4a4", "ffa47b", "cd947b")         # plaster, trim, the peach band
DEPT_WALL_SIDE = (64, 0, 80, 32)        # a stretch of plain wall, its trim and band


def dept_store_1f():
    return [
        # the planters: their box drawn in the floor's own oranges, so no
        # colour is left - their outline is their shape
        piece("planter_s", [(0, 105, 31, 127)], 9),
        piece("planter_n", [(0, 73, 31, 95)], 9),
        # the computers on the counter's arms
        piece("pc_w", [(98, 37, 112, 52)], 8, base=10, solid=True, leave=("cdffd5",)),
        piece("pc_e", [(178, 25, 191, 39)], 8, base=10, solid=True),
        # the counter, a U in plan as the contest lobby's: the bar across the
        # front as deep as its drawn top, the arms back towards the wall (their
        # back corners square: drawn in the counter's own grey)
        piece("counter", [(96, 24, 112, 63), (176, 24, 192, 63), (96, 44, 192, 63)], 10,
              leave=DEPT_1F_FLOOR + DEPT_WALL,
              plan=[(96, 34), (112, 34), (112, 54), (176, 54), (176, 34), (192, 34), (192, 64),
                    (96, 64)]),
        piece("wall", [(0, 0, 288, 32)], 32, fill=16, foot=32, side=DEPT_WALL_SIDE),
        piece("side_w", [], 32, side=DEPT_WALL_SIDE, walls=[((0, 128), (0, 32))]),
        piece("side_e", [], 32, side=DEPT_WALL_SIDE, walls=[((288, 32), (288, 128))]),
    ]


# 2F: a glass counter, its bar along the back and an arm towards the room at
# each end, the clerk's floor between them; its monitor is 1F's (reused), as
# are the Mart's shelves and the Center's palms. The floor is drawn in the
# counter's own mint and white, which its outline keeps apart.

DEPT_GREEN_FLOOR = ("a4eebd", "ffffff", "cdffd5", "b4b4a4", "9cd57b", "d5ded5", "b4de8b")


def dept_store_2f():
    return [
        piece("counter", [(96, 76, 112, 127), (176, 76, 192, 127), (112, 77, 176, 95)], 10,
              leave=DEPT_GREEN_FLOOR,
              # (the glass bar's top drawn a row below the arms': its back at z 87)
              plan=[(96, 86), (112, 86), (112, 87), (176, 87), (176, 86), (192, 86), (192, 128),
                    (176, 128), (176, 96), (112, 96), (112, 128), (96, 128)]),
        piece("wall", [(0, 0, 288, 32)], 32, fill=16, foot=32, side=DEPT_WALL_SIDE),
        piece("side_w", [], 32, side=DEPT_WALL_SIDE, walls=[((0, 128), (0, 32))]),
        piece("side_e", [], 32, side=DEPT_WALL_SIDE, walls=[((288, 32), (288, 128))]),
    ]


# 3F: the glass counter as a U, 1F's in plan, its glass bar's top a row
# below the arms' (its back at z 55); both computers are 1F's (reused).


def dept_store_3f():
    return [
        piece("counter", [(96, 24, 112, 63), (176, 24, 192, 63), (112, 45, 176, 63)], 10,
              leave=DEPT_GREEN_FLOOR,
              plan=[(96, 34), (112, 34), (112, 55), (176, 55), (176, 34), (192, 34), (192, 64),
                    (96, 64)]),
        piece("wall", [(0, 0, 288, 32)], 32, fill=16, foot=32, side=DEPT_WALL_SIDE),
        piece("side_w", [], 32, side=DEPT_WALL_SIDE, walls=[((0, 128), (0, 32))]),
        piece("side_e", [], 32, side=DEPT_WALL_SIDE, walls=[((288, 32), (288, 128))]),
    ]


# 4F: the glass counter as 2F's, a cell narrower; three pairs of low display
# shelves, two-tiered, each as long as the two cells its collision blocks,
# as the Mart's are (5F stands them again).

DEPT_4F_SHELVES = (16, 32, 208, 224, 240, 256)


def dept_store_4f():
    return [piece("shelf_%d" % x, [(x, 80, x + 16, 111)], 17, back=80, leave=DEPT_GREEN_FLOOR)
            for x in DEPT_4F_SHELVES] + [
        piece("counter", [(96, 76, 112, 127), (160, 76, 176, 127), (112, 77, 160, 95)], 10,
              leave=DEPT_GREEN_FLOOR,
              plan=[(96, 86), (112, 86), (112, 87), (160, 87), (160, 86), (176, 86), (176, 128),
                    (160, 128), (160, 96), (112, 96), (112, 128), (96, 128)]),
        piece("wall", [(0, 0, 288, 32)], 32, fill=16, foot=32, side=DEPT_WALL_SIDE),
        piece("side_w", [], 32, side=DEPT_WALL_SIDE, walls=[((0, 128), (0, 32))]),
        piece("side_e", [], 32, side=DEPT_WALL_SIDE, walls=[((288, 32), (288, 128))]),
    ]


# 5F: the glass counter as 3F's U, a cell wider; by the east wall a glass
# counter as 2F's, one arm and its bar running into the wall. Its low shelves
# are 4F's, its tall one the Mart's, its computers 1F's (all reused).

DEPT_5F_FLOOR = ("ffffcd", "dedea4", "ffc5ac", "cdffcd", "acdeb4", "e6a483", "bdbd83")


def dept_store_5f():
    return [
        piece("counter_e", [(224, 76, 240, 127), (240, 77, 288, 95)], 10, leave=DEPT_5F_FLOOR,
              plan=[(224, 86), (240, 86), (240, 87), (288, 87), (288, 96), (240, 96), (240, 128),
                    (224, 128)]),
        piece("counter", [(80, 24, 96, 63), (176, 24, 192, 63), (96, 45, 176, 63)], 10,
              leave=DEPT_5F_FLOOR,
              plan=[(80, 34), (96, 34), (96, 55), (176, 55), (176, 34), (192, 34), (192, 64),
                    (80, 64)]),
        piece("wall", [(0, 0, 288, 32)], 32, fill=16, foot=32, side=DEPT_WALL_SIDE),
        piece("side_w", [], 32, side=DEPT_WALL_SIDE, walls=[((0, 128), (0, 32))]),
        piece("side_e", [], 32, side=DEPT_WALL_SIDE, walls=[((288, 32), (288, 128))]),
    ]


# The elevator: its back wall, drawn from 8 rows down its first cell (the
# black over it is nothing) to its foot 48 rows down, the window and the
# golden handrail on its face; the side walls dressed with its pillar.

DEPT_ELEVATOR_SIDE = (0, 8, 8, 48)      # the west pillar and its blue base


def dept_store_elevator():
    return [
        piece("wall", [(0, 8, 80, 48)], 40, fill=16, foot=48, side=DEPT_ELEVATOR_SIDE),
        piece("side_w", [], 40, side=DEPT_ELEVATOR_SIDE, walls=[((0, 96), (0, 48))]),
        piece("side_e", [], 40, side=DEPT_ELEVATOR_SIDE, walls=[((80, 48), (80, 96))]),
    ]


# The rooftop: a railing all round its edge, the floor seen between its bars
# (`through`); the vending machines against its back, the elevator's
# housing with its door, two planters, two benches and two chairs. The
# roof's ledge outside the railing, and the store's front below it, stay
# drawn on the floor.

ROOF_FLOOR = ("b4b4a4", "d5ded5", "b4de8b", "9cd57b", "8b8b8b")
ROOF_LEDGE = ("cdeef6", "dee6ee", "bdcde6", "83838b", "62627b", "ffffff", "9cb4de")
ROOF_BARS = (48, 8, 64, 19)             # a stretch of the back railing: its top rail and bars


def dept_store_rooftop():
    rail = ROOF_FLOOR + ROOF_LEDGE
    return [
        piece("rail_s", [(16, 104, 272, 119)], 11, through=True, leave=rail),
        piece("bench_w", [(80, 83, 112, 95)], 6, leave=ROOF_FLOOR),
        piece("bench_e", [(144, 83, 176, 95)], 6, leave=ROOF_FLOOR),
        piece("chair_s", [(256, 84, 272, 95)], 8, leave=ROOF_FLOOR),
        piece("chair_n", [(256, 68, 272, 79)], 8, leave=ROOF_FLOOR),
        piece("planter_s", [(16, 73, 48, 95)], 11, leave=ROOF_FLOOR),
        piece("planter_n", [(16, 41, 48, 63)], 11, leave=ROOF_FLOOR),
        # the railings down the sides are drawn as their top line: their long
        # sides carry the back railing's bars (ROOF_BARS), as Rustboro's
        # railings' flanks carry the row's
        piece("rail_e", [(270, 40, 280, 119)], 10, through=True, leave=rail, side=ROOF_BARS),
        piece("rail_w", [(8, 8, 17, 119)], 10, through=True, leave=rail, side=ROOF_BARS),
        # the housing: its roof drawn from row 17 over its 35-px front, so it
        # stands back to z 52 only - the back railing is drawn over it, seen
        # behind it; laid back to its cells' z 16 it covered the railing and
        # carried its bars on its roof
        piece("housing", [(192, 17, 240, 63)], 35, back=52, leave=ROOF_FLOOR),
        piece("vending", [(144, 0, 176, 31)], 24, leave=ROOF_FLOOR + ROOF_LEDGE),
        # round the housing's corner: the back railing turns south, then east
        piece("rail_ne_e", [(244, 40, 276, 51)], 10, through=True, leave=rail),
        piece("rail_ne_s", [(240, 11, 248, 39)], 10, through=True, leave=rail, side=ROOF_BARS),
        piece("rail_n", [(16, 8, 244, 19)], 10, through=True, leave=rail),
    ]



# Lilycove's museum, 1F: the back wall hung with paintings, a wall down from
# it, and a partition across the west half with its own end down the room;
# the reception counter a ring round the clerk's floor, the case with its
# stone tablet and two statues. Counter, case and statues are drawn in the
# floor's own creams, so no colour is left: their outline is their shape.

MUSEUM_1F_FLOOR = ("e6e68b", "ffffcd", "cdb462", "decd73", "7b6229")
# a stretch of the partition's face with no painting on it, skirting and all
MUSEUM_WALL_SIDE = (64, 80, 80, 112)
# the counter's back corners, rounded over 6 px: its outline row by row
MUSEUM_RING_NW = [(16, 170), (17, 170), (17, 168), (18, 168), (18, 167), (19, 167), (19, 166),
                  (20, 166), (20, 165), (22, 165), (22, 164)]
MUSEUM_RING_NE = [(106, 164), (106, 165), (108, 165), (108, 166), (109, 166), (109, 167),
                  (110, 167), (110, 168), (111, 168), (111, 170), (112, 170)]


def museum_1f():
    return [
        # a ball on a low pedestal, as the gyms' statues
        piece("statue_r_ball", [("ellipse", 296, 181.5, 6, 5.5)], 8, base=4, round="ball"),
        piece("statue_r", [(288, 176, 304, 192)], 4, leave=MUSEUM_1F_FLOOR, fill="column"),
        # a figure turned round its axis on a pedestal
        piece("statue_h", [(240, 176, 256, 195)], 19, base=6, round="lathe",
              leave=MUSEUM_1F_FLOOR),
        piece("statue_h_base", [(240, 195, 256, 208)], 6),
        # the stone tablet, a slab lying on the case
        piece("tablet", [(256, 113, 304, 136)], 7, base=6, leave=("ffffcd",)),
        piece("case", [(240, 110, 320, 160)], 6),
        # the ring: two halves, each a C in plan, the clerk's floor between
        piece("counter_w", [(16, 158, 64, 176), (16, 176, 32, 224), (32, 206, 64, 224)], 6,
              plan=MUSEUM_RING_NW + [(64, 164), (64, 176), (32, 176), (32, 212), (64, 212),
                                     (64, 224), (16, 224)]),
        piece("counter_e", [(64, 158, 112, 176), (96, 176, 112, 224), (64, 206, 96, 224)], 6,
              plan=[(64, 164)] + MUSEUM_RING_NE + [(112, 224), (64, 224), (64, 212), (96, 212),
                                                   (96, 176), (64, 176)]),
        piece("partition_s", [(144, 72, 160, 144)], 32, fill=16, foot=144, side=MUSEUM_WALL_SIDE,
              walls=[((144, 112), (144, 144)), ((160, 144), (160, 104))]),
        piece("partition", [(0, 72, 144, 112)], 32, fill=16, foot=112, side=MUSEUM_WALL_SIDE),
        piece("wall_c", [(208, 0, 224, 80)], 32, fill=16, foot=80, side=MUSEUM_WALL_SIDE,
              walls=[((208, 32), (208, 80)), ((224, 80), (224, 32))]),
        piece("wall", [(0, 0, 208, 32), (224, 0, 336, 32)], 32, fill=16, foot=32,
              side=MUSEUM_WALL_SIDE),
        piece("side_w", [], 32, side=MUSEUM_WALL_SIDE, walls=[((0, 224), (0, 32))]),
        piece("side_e", [], 32, side=MUSEUM_WALL_SIDE, walls=[((336, 32), (336, 224))]),
    ]



# 2F: the back wall with the stairs up painted on it, its two ends a cell
# forward, and a statue each side of the stairs - the west one 1F's, found
# again (reuse_pieces); the east one faces the other way. The five stands
# stay drawn: the game hangs the contest paintings in their frames
# (setmetatile, LilycoveCity_LilycoveMuseum_2F/scripts.inc), and a model
# would keep the empty frames of the drawing.

MUSEUM_2F_FLOOR = ("e6e6e6", "9c9c9c", "d5d5d5", "c5c5c5", "b4b4b4", "7b7b7b")
MUSEUM_2F_WALL_SIDE = (96, 0, 112, 32)


def museum_2f():
    return [
        piece("statue_e", [(240, 16, 256, 35)], 19, base=6, round="lathe", leave=MUSEUM_2F_FLOOR),
        piece("statue_e_base", [(240, 35, 256, 48)], 6),
        piece("wall_w", [(0, 0, 64, 48)], 32, fill=16, foot=48, side=MUSEUM_2F_WALL_SIDE,
              walls=[((64, 48), (64, 32))]),
        piece("wall_e", [(288, 0, 352, 48)], 32, fill=16, foot=48, side=MUSEUM_2F_WALL_SIDE,
              walls=[((288, 32), (288, 48))]),
        piece("wall", [(64, 0, 288, 32)], 32, fill=16, foot=32, side=MUSEUM_2F_WALL_SIDE),
        piece("side_w", [], 32, side=MUSEUM_2F_WALL_SIDE, walls=[((0, 208), (0, 48))]),
        piece("side_e", [], 32, side=MUSEUM_2F_WALL_SIDE, walls=[((352, 48), (352, 208))]),
    ]



# Lilycove's Pokemon Trainer Fan Club: the houses' furniture stands in it as
# it is (reuse_pieces) - the bookcase, the chests, the television, the tables
# and chairs, the cupboard, the teapot; what is its own is its shell: the
# back wall and the two low partitions across its middle. The sofas lie on
# cells the player walks on and stay drawn: the west one hides the foot of
# the partition behind it.

FAN_CLUB_WALL_SIDE = (48, 0, 64, 32)


def trainer_fan_club():
    return [
        # the west table: House1's (found east of it, with its teapot), bare
        piece("table_w", [(66, 144, 94, 177)], 11, leave=GENERIC_RUG, solid=True, fill=1,
              foot=177),
        piece("partition_w", [(0, 107, 47, 137)], 26, fill=16, foot=144, side=FAN_CLUB_WALL_SIDE,
              walls=[((47, 144), (47, 118))]),
        piece("partition_e", [(145, 107, 192, 144)], 26, fill=16, foot=144,
              side=FAN_CLUB_WALL_SIDE, walls=[((145, 118), (145, 144))]),
        piece("wall", [(0, 0, 192, 32)], 32, fill=16, foot=32, side=FAN_CLUB_WALL_SIDE),
        piece("side_w", [], 32, side=FAN_CLUB_WALL_SIDE, walls=[((0, 224), (0, 32))]),
        piece("side_e", [], 32, side=FAN_CLUB_WALL_SIDE, walls=[((192, 32), (192, 224))]),
    ]



# Lanette's house on Route 114: the lab's tileset, and the lab's bookcases,
# desks, computers, books and boxes stand in it as they are (reuse_pieces);
# what is its own is its shell, two yellow boxes and the desks of two of
# the lab's computers.

LANETTE_WALL_SIDE = (64, 0, 80, 32)


def lanettes_house():
    return [
        # two boxes of the lab's yellow
        piece("box_e", [(112, 82, 127, 96)], 6, leave=LAB_SHADOW + LAB_FLOOR),
        piece("box_sw", [(16, 114, 31, 128)], 6, leave=LAB_SHADOW + LAB_FLOOR),
        # the two computers east of the middle are the lab's; their desks
        # are a single row, the lab's rails beside each computer
        piece("desks_e", [(96, 64, 128, 78), (96, 52, 98, 64), (110, 52, 114, 64),
                          (126, 52, 128, 64)], 8, leave=LAB_SHADOW, back=60),
        piece("wall", [(0, 0, 176, 32)], 32, fill=16, foot=32, side=LANETTE_WALL_SIDE),
        piece("side_w", [], 32, side=LANETTE_WALL_SIDE, walls=[((0, 128), (0, 32))]),
        piece("side_e", [], 32, side=LANETTE_WALL_SIDE, walls=[((176, 32), (176, 128))]),
    ]



# The Pokemon Day Care on Route 117: the counter, an L from the wall round
# the keeper's floor, its east end cut on the slant; two crates against the
# wall, the computer as the Centers' (back to the wall, its foot a little
# into the room); a table on the grass, two plants, and the fence along the
# south edge with its post up the west side, the room seen between its
# rails. The stools stand on cells the player walks on and stay drawn.

DAY_CARE_FLOOR = ("6283e6", "7b9cff", "4a6ad5", "4162a4", "838383", "8b8b8b")
DAY_CARE_GRASS = ("a4c562", "bde673", "6aa439")
DAY_CARE_PAINT = ("bdd5ee", "9cacde", "dee6ee", "ffffff")
DAY_CARE_WALL = DAY_CARE_PAINT + ("e6c56a", "d5a44a", "a48341")
DAY_CARE_WALL_SIDE = (132, 0, 148, 32)


def day_care():
    return [
        piece("plant_n", [(0, 82, 16, 111)], 28, leave=DAY_CARE_FLOOR, card=True),
        piece("plant_s", [(0, 114, 16, 143)], 28, leave=DAY_CARE_FLOOR, card=True),
        piece("table", [(160, 82, 192, 111)], 5, leave=DAY_CARE_GRASS, solid=True,
              side=(164, 84, 188, 100)),
        piece("fence", [(96, 128, 192, 144)], 15, through=True,
              leave=DAY_CARE_FLOOR + DAY_CARE_GRASS),
        piece("gatepost", [(96, 112, 105, 128)], 15, leave=DAY_CARE_FLOOR),
        piece("pc", [(160, 8, 176, 40)], 24, leave=DAY_CARE_WALL, back=32),
        # the crates' rims are the wall's wood: only its paint is left
        piece("crate_w", [(80, 23, 96, 48)], 7, leave=DAY_CARE_PAINT, against=32),
        piece("crate_e", [(96, 22, 112, 48)], 7, leave=DAY_CARE_PAINT, against=32),
        piece("counter", [(0, 44, 80, 63), (64, 24, 80, 44)], 6, leave=DAY_CARE_FLOOR,
              against=32,
              plan=[(0, 50), (64, 50), (64, 32), (80, 32), (80, 56), (73, 63), (0, 63)]),
        piece("wall", [(0, 0, 192, 32)], 32, fill=16, foot=32, side=DAY_CARE_WALL_SIDE),
        piece("side_w", [], 32, side=DAY_CARE_WALL_SIDE, walls=[((0, 144), (0, 32))]),
        piece("side_e", [], 32, side=DAY_CARE_WALL_SIDE, walls=[((192, 32), (192, 144))]),
    ]



# The Seashore House on Route 109: its ten low tables in three rows, the two
# drinks fridges and the machine with its dome against the slatted back
# wall, the swim rings painted on it. The bin beside the machine stands on a
# cell the player walks on and stays drawn.

SEASHORE_FLOOR = ("c5cd94", "e6e6b4", "f6f6cd", "7bc5b4", "52ac94", "b4deff", "b4b48b")
SEASHORE_WALL = ("e6c562", "ffe67b", "bd9c4a", "83838b", "7bc5b4")
SEASHORE_WALL_SIDE = (48, 0, 58, 32)


def seashore_tables():
    """The tables, two cells or three wide, a cell deep, in rows 4, 6 and 8."""
    out = []
    for row, y in (("n", 64), ("m", 96), ("s", 128)):
        spans = [("w", 0, 32), ("e", 208, 240)]
        if row != "s":
            spans[1:1] = [("cw", 48, 96), ("ce", 128, 176)]
        for col, x0, x1 in spans:
            out.append(piece("table_%s%s" % (row, col), [(x0, y, x1, y + 17)], 8,
                             leave=SEASHORE_FLOOR, solid=True))
    return out


def seashore_house():
    return seashore_tables() + [
        piece("machine", [(112, 16, 128, 44)], 24, back=32, leave=SEASHORE_WALL + SEASHORE_FLOOR),
        piece("fridge_w", [(16, 14, 48, 46)], 21, back=32, leave=SEASHORE_WALL + SEASHORE_FLOOR),
        piece("fridge_e", [(192, 14, 224, 46)], 21, back=32, leave=SEASHORE_WALL + SEASHORE_FLOOR),
        piece("wall", [(0, 0, 240, 32)], 32, fill=16, foot=32, side=SEASHORE_WALL_SIDE),
        piece("side_w", [], 32, side=SEASHORE_WALL_SIDE, walls=[((0, 160), (0, 32))]),
        piece("side_e", [], 32, side=SEASHORE_WALL_SIDE, walls=[((240, 32), (240, 160))]),
    ]



# Devon Corporation's lobby, 1F: its front wall in three stretches, and
# behind it two recesses, each with its own back wall and side walls (their
# tops the orange strips down the drawing): the reception's, with its desk
# across the front and a glass case in each back corner, and the stairs',
# the stairs up painted on its back wall. Every wall 28 px, its top 4 px.

DEVON_FLOOR = ("cd837b", "deaca4", "bd6252")
DEVON_WALL = ("835a31", "e69c62", "6a7b41", "839473", "9c9c9c", "acb494")
DEVON_WALL_SIDE = (0, 52, 12, 80)


def devon_corp_1f():
    side = DEVON_WALL_SIDE
    return [
        piece("case_w", [(48, 22, 64, 48)], 26, round="lathe", leave=DEVON_FLOOR + DEVON_WALL),
        piece("case_e", [(128, 22, 144, 48)], 26, round="lathe", leave=DEVON_FLOOR + DEVON_WALL),
        piece("desk", [(48, 62, 128, 80)], 8, leave=DEVON_FLOOR, solid=True),
        # the recesses' side walls, down from their back walls to the front
        piece("wall_rww", [(44, 0, 48, 80)], 28, fill=16, foot=80, side=side,
              walls=[((48, 80), (48, 32))]),
        piece("wall_rwe", [(144, 0, 148, 80)], 28, fill=16, foot=80, side=side,
              walls=[((144, 32), (144, 80))]),
        piece("wall_rew", [(204, 0, 208, 80)], 28, fill=16, foot=80, side=side,
              walls=[((208, 80), (208, 32))]),
        piece("wall_ree", [(256, 0, 260, 80)], 28, fill=16, foot=80, side=side,
              walls=[((256, 32), (256, 80))]),
        # the recesses' back walls
        piece("wall_rw", [(48, 0, 144, 32)], 28, fill=16, foot=32, side=side),
        piece("wall_re", [(208, 0, 256, 32)], 28, fill=16, foot=32, side=side),
        # the front wall
        piece("wall_w", [(0, 48, 44, 80)], 28, fill=16, foot=80, side=side),
        piece("wall_c", [(148, 48, 204, 80)], 28, fill=16, foot=80, side=side),
        piece("wall_e", [(260, 48, 304, 80)], 28, fill=16, foot=80, side=side),
        piece("side_w", [], 28, side=side, walls=[((0, 144), (0, 80))]),
        piece("side_e", [], 28, side=side, walls=[((304, 80), (304, 144))]),
    ]



# The Battle Tents (Slateport, Verdanturf, Fallarbor). The lobby, the three
# tents': the tent's curtain falling down each side, two C-shaped counters
# (the ticket machine painted on the west one's front) and a machine by the
# east one, the door painted on the back wall. The corridor and the battle
# rooms: their back walls and their doors; the battlefield, the pool and
# the bushes of Verdanturf's are drawn on the floor (the trainers walk in on
# a script, nowhere near the walls).

TENT_FLOOR = ("b4acf6", "cdd5ff", "9c83ee")
TENT_WALL_SIDE = (64, 0, 80, 32)
# the west counter's inner corner, rounded down from its arm to its bar: its
# outline row by row
TENT_COUNTER_INNER = [(48, 78), (49, 78), (49, 80), (50, 80), (50, 81), (51, 81), (51, 82),
                      (54, 82), (54, 83)]


def tent_lobby():
    side = TENT_WALL_SIDE
    return [
        piece("machine", [(176, 56, 192, 88)], 24, leave=TENT_FLOOR),
        piece("counter_w", [(32, 29, 64, 48), (32, 48, 48, 96), (48, 76, 96, 96)], 6,
              leave=TENT_FLOOR,
              plan=[(32, 35), (64, 35), (64, 48), (48, 48)] + TENT_COUNTER_INNER
              + [(95, 83), (95, 84), (96, 84), (96, 96), (32, 96)]),
        piece("counter_e", [(144, 29, 176, 48), (160, 48, 176, 96), (112, 76, 160, 96)], 6,
              leave=TENT_FLOOR,
              plan=[(144, 35), (176, 35), (176, 96), (112, 96), (112, 84), (113, 84), (113, 83)]
              + [(208 - x, z) for x, z in reversed(TENT_COUNTER_INNER)] + [(160, 48), (144, 48)]),
        # the curtain down each side, from the back wall
        piece("wall_cw", [(0, 0, 16, 80)], 32, fill=16, foot=80, side=side, leave=TENT_FLOOR,
              walls=[((16, 80), (16, 32))]),
        piece("wall_ce", [(192, 0, 208, 80)], 32, fill=16, foot=80, side=side, leave=TENT_FLOOR,
              walls=[((192, 32), (192, 80))]),
        piece("wall", [(16, 0, 192, 32)], 32, fill=16, foot=32, side=side),
        piece("side_w", [], 32, side=side, walls=[((0, 160), (0, 80))]),
        piece("side_e", [], 32, side=side, walls=[((208, 80), (208, 160))]),
    ]


def tent_room(w, h):
    side = TENT_WALL_SIDE
    return [
        piece("wall", [(0, 0, w, 32)], 32, fill=16, foot=32, side=side),
        piece("side_w", [], 32, side=side, walls=[((0, h), (0, 32))]),
        piece("side_e", [], 32, side=side, walls=[((w, 32), (w, h))]),
    ]



# Slateport's Pokemon Fan Club: the bookcases, two wooden pillars and two
# plants along the back wall (drawn 8 px down, the void above it), a plant
# in each south corner, and the glass table between the sofas - the sofas
# and armchairs stand on cells the player walks on and stay drawn.

SLATEPORT_FAN_FLOOR = ("ded58b", "f6eeb4", "c5bd7b")
SLATEPORT_FAN_RUG = ("ee836a", "cd6a6a", "ffcd94")
SLATEPORT_FAN_WALL = ("e6d583", "f6eecd", "9c9c8b", "a49441", "d5a46a", "000000")
SLATEPORT_FAN_SIDE = (64, 8, 80, 40)


def slateport_fan_club():
    side = SLATEPORT_FAN_SIDE
    wall_floor = SLATEPORT_FAN_WALL + SLATEPORT_FAN_FLOOR
    return [
        piece("plant_sw", [(0, 150, 16, 176)], 26, card=True, leave=SLATEPORT_FAN_FLOOR),
        piece("plant_se", [(208, 150, 224, 176)], 26, card=True, leave=SLATEPORT_FAN_FLOOR),
        piece("table", [(80, 94, 144, 128)], 10, leave=SLATEPORT_FAN_RUG, solid=True),
        piece("plant_w", [(48, 20, 64, 47)], 27, card=True, leave=wall_floor),
        piece("plant_e", [(160, 20, 176, 47)], 27, card=True, leave=wall_floor),
        # pilasters: the wall's own pattern 5 px forward, their tops its trim
        # (drawn in the wall's colours: only the floor is left round them)
        piece("pillar_w", [(32, 8, 48, 45)], 32, leave=SLATEPORT_FAN_FLOOR, back=40),
        piece("pillar_e", [(176, 8, 192, 45)], 32, leave=SLATEPORT_FAN_FLOOR, back=40),
        # the bookcases' sides are their green tops
        piece("bookcase_w", [(0, 26, 32, 47)], 12, leave=wall_floor, against=40,
              side=(6, 28, 22, 34)),
        piece("bookcase_e", [(192, 17, 224, 46)], 18, leave=wall_floor, against=40,
              side=(194, 19, 206, 25)),
        piece("wall", [(0, 8, 224, 40)], 32, fill=16, foot=40, side=side),
        piece("side_w", [], 32, side=side, walls=[((0, 176), (0, 40))]),
        piece("side_e", [], 32, side=side, walls=[((224, 40), (224, 176))]),
    ]



# The Harbor (Slateport's and Lilycove's): the building's wall along the
# back, the railing round the dock - the floor and the water seen between
# its bars, the runs down the west side and the cargo yard drawn as their
# top line, their sides the back railing's bars, as Rustboro's - and the
# crates stacked in the yard. The water stays the sea, level with the
# floor; the gangway, where the S.S. Tidal is boarded, and the stools are
# drawn on the floor.

HARBOR_SEE = ("8bb4ac", "a4cdbd", "9c8b94", "628b83", "739c8b", "62627b", "526ad5", "6a83d5",
              "29418b", "39529c", "acb494", "839473", "6a7b41", "dedec5")
HARBOR_BARS = (180, 67, 212, 74)
HARBOR_WALL_SIDE = (64, 0, 80, 64)


def harbor():
    see = HARBOR_SEE
    return [
        # the crates inside the dark line between them (their sides would be
        # that line), the line drawn on the floor beside them
        piece("crate_c", [(352, 192, 383, 239)], 14, leave=see),
        piece("crate_b", [(320, 176, 351, 207)], 14, leave=see),
        piece("crate_a", [(289, 160, 319, 207)], 14, leave=see),
        piece("rail_v", [(272, 176, 280, 240)], 15, through=True, leave=see, side=HARBOR_BARS),
        piece("rail_s", [(48, 160, 112, 176), (144, 160, 288, 176), (320, 160, 384, 176)], 15,
              through=True, leave=see),
        piece("rail_w", [(48, 80, 56, 160)], 15, through=True, leave=see, side=HARBOR_BARS),
        piece("rail_n", [(48, 64, 384, 80)], 15, through=True, leave=see),
        piece("wall", [(0, 0, 384, 64)], 56, fill=16, foot=64, side=HARBOR_WALL_SIDE),
        piece("side_w", [], 56, side=HARBOR_WALL_SIDE, walls=[((0, 240), (0, 64))]),
        piece("side_e", [], 56, side=HARBOR_WALL_SIDE, walls=[((384, 64), (384, 240))]),
    ]



# Stern's Shipyard 1F: the office west of its partition (the stairs up
# painted on the back wall), the workshop east of it - the board stacks in
# its corners, the girder towers, the enclosure round the submarine.

SHIPYARD_FLOOR = ("739c8b", "62627b", "628b83", "8bb4ac", "a4cdbd", "9c8b94", "acb494", "839473",
                  "6a7b41", "dedec5")
SHIPYARD_WALL = ("dedede", "ffffff", "8b94a4", "6a7b7b")
SHIPYARD_WALL_SIDE = (64, 0, 80, 32)


def shipyard_1f():
    fl = SHIPYARD_FLOOR
    side = SHIPYARD_WALL_SIDE
    return [
        # the submarine in its glass capsule, its nose out in front
        piece("sub_nose", [(192, 136, 224, 157)], 12, leave=fl),
        piece("capsule", [(176, 72, 240, 136)], 40, leave=fl),
        # the robot arm and its console, before the enclosure's wall
        piece("robot", [(144, 72, 176, 120)], 24, leave=fl),
        piece("girder_n", [(112, 64, 128, 112)], 44, leave=fl),
        # the enclosure's wall and the pillar at its east end
        piece("pillar_e", [(240, 56, 256, 99)], 29, leave=fl),
        piece("wall_enc", [(112, 60, 240, 96)], 27, fill=16, foot=96, side=side),
        # along the back wall: the three consoles and a pilaster
        piece("consoles", [(128, 16, 176, 40)], 17, back=32, leave=fl, side=(129, 17, 143, 21)),
        piece("pilaster", [(192, 0, 208, 35)], 35, back=32, leave=fl),
        # the workshop's girder towers and board stacks
        piece("girder_w", [(160, 144, 176, 207)], 59, leave=fl),
        piece("girder_e", [(224, 160, 240, 207)], 43, leave=fl),
        piece("boards_sw", [(112, 176, 160, 240)], 32, leave=fl),
        piece("boards_se", [(288, 176, 336, 225)], 32, leave=fl),
        piece("boards_ne", [(304, 48, 336, 113)], 40, leave=fl),
        # the office
        piece("table", [(64, 96, 96, 128)], 6, leave=fl, solid=True),
        # the desks with their computers, their sides their tops' lavender
        piece("desk_n", [(0, 66, 32, 97)], 10, leave=fl, solid=True, side=(18, 78, 30, 88)),
        piece("desk_m", [(0, 114, 32, 145)], 10, leave=fl, solid=True, side=(18, 126, 30, 136)),
        piece("desk_s", [(0, 162, 32, 193)], 10, leave=fl, solid=True, side=(18, 174, 30, 184)),
        # the model ship on the desk, with the grey strip of the desk's top
        # it stands on (outside the desk's outline, which closes its front)
        piece("model_ship", [(3, 24, 30, 38)], 10, base=8, leave=SHIPYARD_WALL[2:]),
        # the desk the model ship is on, back to the wall under the ship
        piece("desk_ship", [(0, 37, 32, 48)], 8, back=32),
        # the partition between office and workshop, its door between its
        # two runs; the south one runs on past the map's edge
        piece("partition_n", [(96, 0, 112, 146)], 28, fill=16, foot=146, side=side, against=32,
              walls=[((96, 32), (96, 146)), ((112, 146), (112, 32))]),
        piece("partition_s", [(96, 169, 112, 240)], 28, fill=16, foot=268, side=side,
              walls=[((96, 197), (96, 268)), ((112, 268), (112, 197))]),
        piece("wall", [(0, 0, 336, 32)], 32, fill=16, foot=32, side=side),
        piece("side_w", [], 32, side=side, walls=[((0, 240), (0, 32))]),
        piece("side_e", [], 32, side=side, walls=[((336, 32), (336, 240))]),
    ]



# Stern's Shipyard 2F: offices round a balcony over the workshop. 1F's desks
# with their computers, its blueprint tables, its pilasters and the Harbor's
# railing stand in it as they are (reuse_pieces); its own are the plain
# desks, the bin, and the railing round the balcony, the workshop seen
# through it. Inside the railing the floor below is drawn - 1F's girder
# and submarine among it: the room's `pit`, where nothing found again
# stands.

SHIPYARD_PIT = [(136, 96, 248, 192)]


def shipyard_2f():
    side = SHIPYARD_WALL_SIDE
    fl = SHIPYARD_FLOOR
    desk = lambda name, x, y: piece(name, [(x, y + 10, x + 32, y + 33)], 10, leave=fl,  # noqa: E731
                                    solid=True, side=(x + 18, y + 14, x + 30, y + 24))
    return [
        piece("bin", [(113, 176, 127, 192)], 15, round="lathe", leave=fl),
        desk("desk_nw", 0, 64), desk("desk_m", 32, 112), desk("desk_s", 32, 160),
        piece("rail_s", [(128, 192, 256, 208)], 15, through=True, leave=fl),
        piece("rail_e", [(248, 96, 256, 192)], 15, through=True, leave=fl, side=HARBOR_BARS),
        piece("rail_w", [(128, 160, 136, 192)], 15, through=True, leave=fl, side=HARBOR_BARS),
        piece("rail_n", [(128, 80, 256, 96)], 15, through=True, leave=fl),
        piece("wall", [(0, 0, 272, 32)], 32, fill=16, foot=32, side=side),
        piece("side_w", [], 32, side=side, walls=[((0, 240), (0, 32))]),
        piece("side_e", [], 32, side=side, walls=[((272, 32), (272, 240))]),
    ]



def room_shell(w, h, foot=32, side=None, top=0):
    """A room's shell: its back wall standing on `foot`, drawn from `top`,
    and its side walls."""
    side = side or (64, top, 80, foot)
    return [
        piece("wall", [(0, top, w, foot)], foot - top, fill=16, foot=foot, side=side),
        piece("side_w", [], foot - top, side=side, walls=[((0, h), (0, foot))]),
        piece("side_e", [], foot - top, side=side, walls=[((w, foot), (w, h))]),
    ]



# The Shop tileset's mint glass counters (Lavaridge's Herb Shop, the Seaside
# Cycling Road's gates): 7 px, their arms to the wall's foot. The Herb Shop's
# register is the Department Store's, found again (reuse_pieces).

MINT_SHOP_FLOOR = ("e69c39", "f6c539", "f6e673", "cd8320", "ac5a20")


def herb_shop():
    return [
        piece("counter", [(0, 44, 96, 63), (80, 28, 96, 44)], 7, leave=MINT_SHOP_FLOOR,
              plan=[(0, 52), (80, 52), (80, 35), (96, 35), (96, 63), (0, 63)]),
    ] + room_shell(176, 128)


def cycling_gate():
    return [
        piece("counter", [(80, 28, 96, 63), (160, 28, 176, 63), (96, 44, 160, 63)], 7,
              leave=MINT_SHOP_FLOOR,
              plan=[(80, 35), (96, 35), (96, 51), (160, 51), (160, 35), (176, 35), (176, 63),
                    (80, 63)]),
    ] + room_shell(240, 96)


def safari_rest_house():
    return [
        piece("table", [(49, 79, 79, 112)], 8, leave=("ded552", "bdb431", "8b8b8b"),
              solid=True),
    ] + room_shell(160, 144, foot=64, top=32)



# The Weather Institute on Route 119: the lab's furniture stands in it as it
# is (reuse_pieces) - desks, computers, chairs, plants; its own are its
# partitions, white-topped walls 29 px tall: on 1F two running down from the
# back wall, and one across the room, its doorway between two posts.

INSTITUTE_SIDE = (176, 133, 192, 160)
INSTITUTE_FLOOR = ("d5d5d5", "acacb4", "9c9ca4", "7b7b83")


def institute_desk(name, x, y):
    """A desk a column wide under one of the lab's computers (found again at
    cell (x, y) // 16): the lab's desk, drawn shorter - its rails beside the
    computer, its top back to 12 px into the computer's cell."""
    return piece(name, [(x, y + 20, x + 16, y + 49), (x, y + 4, x + 2, y + 20),
                        (x + 14, y + 4, x + 16, y + 20)], 8, leave=INSTITUTE_FLOOR, back=y + 12)


def institute_1f():
    side = INSTITUTE_SIDE
    return [
        piece("machine", [(4, 32, 28, 62)], 17, leave=INSTITUTE_FLOOR),
        piece("bookcases", [(128, 16, 192, 48)], 20, leave=INSTITUTE_FLOOR, against=32),
    ] + [institute_desk("desk_%d" % x, x, 64) for x in (176, 192, 256, 272)] + [
        piece("partition_ww", [(48, 0, 64, 99)], 29, fill=16, foot=99, side=side, against=32,
              walls=[((48, 32), (48, 99)), ((64, 99), (64, 32))]),
        piece("partition_we", [(112, 0, 128, 99)], 29, fill=16, foot=99, side=side, against=32,
              walls=[((112, 32), (112, 99)), ((128, 99), (128, 32))]),
        piece("partition_post_w", [(48, 121, 64, 163)], 29, fill=16, foot=163, side=side,
              walls=[((48, 134), (48, 163)), ((64, 163), (64, 134))]),
        piece("partition_post_e", [(112, 121, 128, 163)], 29, fill=16, foot=163, side=side,
              walls=[((112, 134), (112, 163)), ((128, 163), (128, 134))]),
        piece("partition_s_w", [(0, 124, 48, 160)], 28, fill=16, foot=160, side=side),
        piece("partition_s_e", [(128, 124, 320, 160)], 28, fill=16, foot=160, side=side),
    ] + room_shell(320, 208)



# 2F: the partition down its west side in two runs (the south one on past
# the map's edge), desks under the two computers at its west end (the eight
# in the middle stand on 1F's, found again), and the last of the bookcases
# (the four before it are 1F's pairs, found again).

def institute_2f():
    side = INSTITUTE_SIDE
    return [institute_desk("desk_16_%d" % y, 16, y) for y in (48, 128)] + [
        # the last bookcase, single (the pairs before it are 1F's)
        piece("bookcase_e", [(208, 16, 240, 48)], 20, leave=INSTITUTE_FLOOR, against=32),
        piece("partition_n", [(64, 0, 80, 99)], 29, fill=16, foot=99, side=side, against=32,
              walls=[((64, 32), (64, 99)), ((80, 99), (80, 32))]),
        piece("partition_s", [(64, 112, 80, 176)], 29, fill=16, foot=205, side=side,
              walls=[((64, 141), (64, 205)), ((80, 205), (80, 141))]),
    ] + room_shell(320, 176)



# The Safari Zone's entrance on Route 121: an L round a void. Its gate
# counter is the Seaside Cycling Road's, its planters the Department
# Store's, its plants the Center's, its chairs the store's rooftop's and the
# machine the Battle Tent's (reuse_pieces); its own are the counter down its
# east side, its two stubs either side of the clerk's floor, and its walls:
# the back one, the sides, and one down the void's edge facing the corridor
# (the void's south edge, a front wall the GBA never draws, stays open).

def safari_entrance():
    side = (64, 0, 80, 32)
    return [
        piece("counter_e", [(256, 104, 272, 191), (272, 104, 288, 127), (272, 168, 288, 191)], 7,
              leave=MINT_SHOP_FLOOR,
              plan=[(256, 111), (288, 111), (288, 127), (272, 127), (272, 175), (288, 175),
                    (288, 191), (256, 191)]),
        piece("wall", [(0, 0, 288, 32)], 32, fill=16, foot=32, side=side),
        # the west wall stops a pixel short of the void: a wall answers for the
        # cell its end reaches, and the void's first cell would lie on the
        # floor as a black patch, the rest of the void open
        piece("side_w", [], 32, side=side, walls=[((0, 95), (0, 32))]),
        piece("side_void", [], 32, side=side, walls=[((192, 224), (192, 96))]),
        piece("side_e", [], 32, side=side, walls=[((288, 32), (288, 224))]),
    ]



# The Cable Car Station (Route 112's and Mt. Chimney's): the back wall (its
# pilasters the Shipyard's, found again), and over the track two railings
# either side of the boarding gap - the far one along the track's back, the
# near one along the platform's edge, the runs down the gap drawn as their
# top line (the Harbor's bars on their sides) - the track seen through them;
# the ticket machine (its bin the Shipyard's). The benches stand on cells
# the player walks on and stay drawn.

CABLE_SEE = ("d5d539", "62627b")


def cable_car_station():
    see = CABLE_SEE

    def rail(name, shape, side=None):
        return piece(name, shape, 15, through=True, leave=see, side=side)

    return [
        piece("machine", [(192, 112, 208, 144)], 16, leave=("d5d539",)),
        rail("rail_gap_w", [(88, 88, 96, 120)], HARBOR_BARS),
        rail("rail_gap_e", [(112, 88, 120, 120)], HARBOR_BARS),
        rail("rail_near_w", [(0, 104, 88, 120)]),
        rail("rail_near_e", [(120, 104, 208, 120)]),
        rail("rail_far_w", [(0, 72, 96, 88)]),
        rail("rail_far_e", [(112, 72, 208, 88)]),
    ] + room_shell(208, 192)



# Mauville's Bike Shop: the bicycles in their racks down both sides
# (upright cards), the shelf in the corner, the counter (an L of wood, the
# printer on its arm), the pillar down to the display stand, the two
# stands, and the two desks against the back wall (the lab's red book found
# again on each: they stand on the wall's foot, 8 px into the room, as the
# Centers' computer). The wall is drawn 4 px down, the void above it.

BIKE_FLOOR = ("acbdac", "83948b", "d5ded5", "cdcdcd")
BIKE_WALL = ("eeeede", "cdcdb4", "acac8b", "947b6a", "000000")


def bike_shop():
    fl = BIKE_FLOOR
    side = (60, 4, 76, 32)
    bikes = [piece("bike_w%d" % k, [(0, y, 26, y + 16)], 16, card=True, leave=fl)
             for k, y in enumerate((40, 56, 72, 88, 104, 120))]
    # the east rack's: the last takes the rack's upright down to the wall's foot
    bikes += [piece("bike_e%d" % k, [(162, y0, 192, y1)], y1 - y0, card=True, leave=fl)
              for k, (y0, y1) in enumerate(((86, 104), (104, 120), (120, 144)))]
    return bikes + [
        piece("printer", [(32, 26, 48, 38)], 8, base=10, leave=BIKE_WALL),
        piece("counter", [(32, 38, 48, 69), (48, 46, 80, 69)], 10, leave=fl,
              plan=[(32, 48), (48, 48), (48, 56), (79, 56), (79, 57), (80, 57), (80, 69),
                    (32, 69)]),
        piece("box", [(80, 46, 96, 63)], 7, leave=fl),
        piece("stand_w", [(96, 60, 128, 88)], 8, leave=fl),
        piece("stand_e", [(160, 60, 192, 80)], 8, leave=fl),
        piece("desk_w", [(112, 20, 144, 41)], 9, leave=fl + BIKE_WALL, solid=True, against=32),
        piece("desk_e", [(160, 20, 192, 41)], 9, leave=fl + BIKE_WALL, solid=True, against=32),
        # the shelf, its sides its top's grey
        piece("shelf", [(0, 11, 32, 46)], 25, leave=fl, against=32, side=(4, 12, 28, 19)),
        piece("partition", [(96, 4, 104, 60)], 28, fill=16, foot=88, side=side, against=32,
              walls=[((96, 32), (96, 88)), ((104, 88), (104, 32))]),
    ] + room_shell(192, 144, top=4, side=side)



# Mauville's Game Corner: the two islands of slot machines, the two
# roulette tables, the prize counter (a U round the clerk's floor, its arms'
# backs slanting in, its rim behind it painted on the wall), the arcade
# cabinets, the plants and the bins; the back wall a cell forward at its
# east end. The benches stand on cells the player walks on and stay drawn.

GAME_CORNER_FLOOR = ("d55231", "f65252", "a44a41", "ac625a")
GAME_CORNER_WALL = ("31839c", "62accd", "bdcde6", "eee6f6", "cdcdc5", "7b7b8b", "ffffff")


def mauville_game_corner():
    fl = GAME_CORNER_FLOOR
    side = (200, 0, 216, 32)
    plants = [piece("plant_%d" % k, [(x, 20, x + 16, 47)], 27, card=True,
                    leave=fl + GAME_CORNER_WALL) for k, x in enumerate((80, 96, 112))]
    return plants + [
        piece("bin_w", [(33, 30, 47, 48)], 17, round="lathe", leave=fl + GAME_CORNER_WALL),
        piece("bin_e", [(337, 65, 351, 80)], 15, round="lathe", leave=fl),
        piece("arcade", [(0, 18, 32, 48)], 14, leave=fl + GAME_CORNER_WALL, against=32,
              side=(2, 19, 14, 26)),
        # the islands' sides are their side rails' light grey
        piece("slots_w", [(32, 80, 64, 160)], 10, leave=fl, solid=True,
              side=(35, 93, 36, 100)),
        piece("slots_e", [(112, 80, 144, 160)], 10, leave=fl, solid=True,
              side=(115, 93, 116, 100)),
        piece("roulette_w", [(224, 96, 256, 141)], 7, leave=fl, solid=True),
        piece("roulette_e", [(288, 96, 320, 141)], 7, leave=fl, solid=True),
        # the arms 15 px wide, their backs stepping in a pixel a row, the front
        # corners cut on the slant (their lighter blue faces)
        piece("counter", [(144, 27, 150, 28), (144, 28, 151, 29), (144, 29, 152, 30),
                          (250, 27, 256, 28), (249, 28, 256, 29), (248, 29, 256, 30),
                          (144, 30, 159, 73), (241, 30, 256, 73), (159, 45, 241, 73)], 13,
              leave=fl,
              plan=[(144, 40), (150, 40), (150, 41), (151, 41), (151, 42), (152, 42), (152, 43),
                    (159, 43), (159, 58), (241, 58), (241, 43), (248, 43), (248, 42), (249, 42),
                    (249, 41), (250, 41), (250, 40), (256, 40), (256, 65), (249, 72), (151, 72),
                    (144, 65)]),
        piece("wall_e", [(288, 0, 352, 48)], 30, fill=16, foot=48, side=side,
              walls=[((288, 32), (288, 48))]),
        piece("wall", [(0, 0, 288, 32)], 32, fill=16, foot=32, side=side),
        piece("side_w", [], 32, side=side, walls=[((0, 176), (0, 32))]),
        piece("side_e", [], 32, side=side, walls=[((352, 48), (352, 176))]),
    ]



# Mossdeep's Game Corner and Sootopolis's Mystery Events House. Much of
# their furniture already stands through reuse_pieces (House1's and House2's
# plants, chairs and television, the chest and case from Mr. Briney's house,
# the Battle Tent's machine); their own are below.

MOSSDEEP_GC_FLOOR = ("eecda4", "d5b473", "bda462", "7b839c")
EVENTS_FLOOR = ("8b8b8b", "b4b4a4", "73738b")


def mossdeep_game_corner_1f():
    """The counter, a U from the back wall on short legs (its arms' planks
    run back to the wall, the bar's across), in two round the yellow gate in
    its middle,
    the four posts against the wall either side of the doorway, and the two
    glass tables; the chairs round them stand on cells the player walks on
    and stay drawn."""
    fl = MOSSDEEP_GC_FLOOR
    posts = [piece("post_%d" % x, [(x, 15, x + 6, 34)], 16, leave=fl, against=31)
             for x in (57, 73, 98, 114)]
    tables = [piece("table_%s" % k, [(x, 93, x + 32, 128)], 8, leave=fl, solid=True)
              for k, x in (("w", 32), ("e", 112))]
    return posts + tables + [
        # in two, round its gate (5,2-3), which the cable club's scripts open
        # for link play (EventScript_OpenMossdeepGameCornerBarrier): it stays
        # drawn, so it opens - a piece's rectangle would lay it in its model
        piece("counter_w", [(32, 21, 48, 45), (32, 45, 80, 62)], 6, leave=fl, against=31,
              plan=[(32, 31), (48, 31), (48, 51), (80, 51), (80, 62), (32, 62)]),
        piece("counter_e", [(128, 21, 144, 45), (96, 45, 144, 62)], 6, leave=fl, against=31,
              plan=[(96, 51), (128, 51), (128, 31), (144, 31), (144, 62), (96, 62)]),
    ] + room_shell(192, 160, foot=31)


def mystery_events_house_1f():
    return [
        piece("table", [(114, 64, 142, 91)], 4, leave=EVENTS_FLOOR, solid=True),
    ] + room_shell(176, 128)



# The Pokémon League. The Elite Four's four rooms share one plan: a back
# wall with the door to the next room, ten spotlights along its foot, a
# pillar standing out from each side wall, and a front wall across the
# entry corridor. data/scripts/elite_four.inc opens the door and switches
# the spotlights off after the battle, and closes the entry door behind the
# player (setmetatile): a model keeps the drawing it was built from, so
# the spotlights and the entry door stay drawn - the back wall stands on row
# 2's top edge, the spotlights lying at its foot, and the front wall runs in
# two stretches, its top over the corridor drawn. The door is painted on the
# back wall (see below).

E4_WALL_SIDE = (32, 16, 48, 32)


def elite_four_room(floor):
    """`floor`: the room's own floor colours (the pillars' tops are the
    border's, so only the main floor is left round them)."""
    side = E4_WALL_SIDE
    return [
        piece("pillar_w", [(0, 68, 26, 147)], 69, leave=floor),
        piece("pillar_e", [(182, 68, 208, 147)], 69, leave=floor),
        # the door painted on the back wall, as the rooms' doors are: a
        # doorway cut through it shows, from the console's camera, the wall's
        # own drawing lying on the floor behind it; so the door stays shut
        # when elite_four.inc opens it. The front wall in two stretches, its
        # top over the corridor drawn, and the side walls from the spotlights'
        # row on: a piece's rectangle lays every cell in it in its model, and
        # the spotlights and the entry door stay drawn to change. Named as
        # walls, the room's own (own_shell)
        piece("wall", [(0, 0, 208, 32)], 32, fill=16, foot=32, side=side),
        piece("wall_front_w", [(0, 172, 80, 224)], 32, fill=16, foot=224),
        piece("wall_front_e", [(128, 172, 208, 224)], 32, fill=16, foot=224),
        piece("side_w", [], 32, side=side, walls=[((0, 204), (0, 48))]),
        piece("side_e", [], 32, side=side, walls=[((208, 48), (208, 204))]),
    ]


# Pokémon League 1F: the Center's, wider. The healing machine, the PC, the
# telephone, the balls on the counter's arms, the stools, tables, plants and
# escalator are the Center's, found again (reuse_pieces); its own are its
# two counters (their arms' back outline painted on the wall's foot) and its
# walls: the Center's 45-degree corners and side walls, the back wall
# between them, the gate to the Elite Four painted on it.

LEAGUE_WALL_SIDE = (112, 0, 128, 32)
LEAGUE_1F_OPEN = [[(0, 174), (18, 192), (0, 192)], [(304, 174), (286, 192), (304, 192)]]
LEAGUE_COUNTER_LEAVE = ("cdc58b", "eedea4", "de7b31", "f6b473", "bd5220")


def league_1f():
    side = LEAGUE_WALL_SIDE
    return [
        # each a bar from the map's edge and an arm back to the wall, its inner
        # front corner cut on the slant
        piece("counter_w", [(0, 45, 95, 63), (80, 25, 95, 45)], 7, leave=LEAGUE_COUNTER_LEAVE,
              plan=[(0, 52), (80, 52), (80, 32), (95, 32), (95, 61), (93, 63), (0, 63)]),
        piece("counter_e", [(225, 45, 304, 63), (225, 25, 240, 45)], 7,
              leave=LEAGUE_COUNTER_LEAVE,
              plan=[(225, 32), (240, 32), (240, 52), (304, 52), (304, 63), (227, 63),
                    (225, 61)]),
        facet("corner_w", (0, 15, 47), (16, -1, 31), 32, walls=[((0, 174), (0, 47))],
              side=side, cells=[(0, 10)]),
        facet("corner_e", (288, -1, 31), (304, 15, 47), 32, walls=[((304, 47), (304, 174))],
              side=side, cells=[(18, 10)]),
        piece("wall_e", [(160, 0, 288, 32)], 32, fill=16, side=side),
        piece("wall", [(16, 0, 160, 32)], 32, fill=16, side=side),
    ]


# The halls between the Elite Four's rooms: the back wall (its doorway
# painted on it), a landing, and the stairs down between two rails over the
# dark. The floor has one level: the landing's edge and the dark below it
# stay drawn, as the Shipyard's well; the rails stand along the stairs, 16
# px from the landing's edge to the map's.

LEAGUE_HALL_SIDE = (16, 16, 32, 48)


def league_hall(h, rail=80):
    """`rail`: the row the hall's own rails start at. Hall 4's first 13 rows
    are the short halls': their rails are found again there, and its own run
    on from them, their tops back to where those end."""
    side = LEAGUE_HALL_SIDE
    back = None if rail == 80 else rail
    return [
        piece("rail_w", [(51, rail, 64, h)], 16, back=back),
        piece("rail_e", [(112, rail, 125, h)], 16, back=back),
        piece("wall", [(0, 0, 176, 48)], 48, fill=16, foot=48, side=side),
        piece("side_w", [], 48, side=side, walls=[((0, h), (0, 48))]),
        piece("side_e", [], 48, side=side, walls=[((176, 48), (176, h))]),
    ]


# The Hall of Fame: the back wall with its screens, its middle a cell
# forward round the great screen, and the record machine before it; the
# dais under the machine and the trim along the wall's foot are steps the
# floor's one level can't take, and stay drawn. The side walls are edge on
# to the GBA's camera (a pale line of their top drawn beside the floor).

HOF_FLOOR = ("524ab4", "524a62", "31394a")
HOF_WALL_SIDE = (54, 0, 60, 29)


def hall_of_fame():
    side = HOF_WALL_SIDE
    return [
        piece("machine", [(98, 56, 142, 88)], 29, leave=HOF_FLOOR),
        piece("wall_c", [(88, 0, 152, 51)], 51, fill=16, foot=51, side=side),
        piece("wall_w", [(16, 0, 88, 29)], 29, fill=16, foot=29, side=side),
        piece("wall_e", [(152, 0, 224, 29)], 29, fill=16, foot=29, side=side),
        # the corners' cells beside the wall's ends with them
        piece("side_w", [], 29, side=side, walls=[((16, 256), (16, 29))],
              cells=[(1, 2)] + [(0, y) for y in range(2, 16)]),
        piece("side_e", [], 29, side=side, walls=[((224, 29), (224, 256))],
              cells=[(13, 2)] + [(14, y) for y in range(2, 16)]),
    ]



# Mauville's gym: the blocks either side of the corridor up to the back
# wall, their tops the dark beyond the room and the rims round it, the block
# further down the east side, the two either side of the entrance hall, and
# the back wall; the coils stand, turned, one drawn whole standing for all
# twelve (three in a row up each side of the corridor, each hiding most of
# the one behind it). The puzzle's barriers stand and go as the game
# switches them (setmetatile: MauvilleGymPressSwitch, the barriers' two
# states, MauvilleGymDeactivatePuzzle once Wattson is beaten): each beam a
# piece that stands only while its cell holds its lit metatile (`when`),
# its texels following the electric gates' animation (tileset_anims.c,
# secondary tiles 144-159) - a sheet between two coils, or a strip from
# the foot of one coil up to the head of the next where it runs down the
# room. The switches stay drawn, raised or pressed as the game has them: a
# plate in an S of three strokes, which a piece read column by column would
# stand as a block with the floor showing through its top. The red
# beams are lit only in the barriers' other state: they are read off the
# room as MauvilleCity_Gym_EventScript_SetAltBarriers leaves it
# (mauville_gym_alt). The leader's dais, a step the floor can't take,
# stays drawn.

MAUVILLE_GYM_SIDE = (0, 120, 16, 142)
# the floor and its checks, and the walls' shadow across it
MAUVILLE_GYM_FLOOR = ("f6f6ac", "cdcd83", "f6e641", "ffff10", "b4b483", "dec541")
# every coil by its head's cell, the one drawn whole first: drawn from 3
# rows into it to the foot of the cell below. The heads a vertical beam runs
# down to are drawn with its end on them while it is lit (PoleTop_On):
# those two coils stand twice, each the coil the drawing has in that state.
# Those whose feet are drawn on the lower layer (PoleBottom: the feet a
# vertical beam runs from) stand as drawn, a pixel or two of their own -
# each read where its beam is out, which is drawn over it
MAUVILLE_GYM_COILS = [(0, 10), (6, 13), (9, 10), (3, 7), (3, 6), (6, 6), (3, 5), (6, 5)]
MAUVILLE_GYM_POLE_TOP = (0x251, 0x250)      # PoleTop_Off, PoleTop_On
MAUVILLE_GYM_BEAM_TILES = (512 + 144, 512 + 159)
MAUVILLE_GYM_GATES = (["data/tilesets/secondary/mauville_gym/anim/electric_gates/%d.4bpp" % f
                       for f in (0, 1)], 512 + 144)
# The metatiles (include/constants/metatile_labels.h): a horizontal beam
# by its top-left cell lit, green or red; the middle of a vertical one
MAUVILLE_GYM_GREEN_H, MAUVILLE_GYM_RED_H = 0x220, 0x222
MAUVILLE_GYM_GREEN_V, MAUVILLE_GYM_RED_V = 0x248, 0x249
# the barriers' other state (MauvilleCity_Gym_EventScript_SetAltBarriers):
# the red beams lit, the green ones out
MAUVILLE_GYM_ALT = {(3, 11): 0x241, (3, 12): 0x249, (3, 13): 0x250,
                    (4, 10): 0x222, (5, 10): 0x223, (4, 11): 0x22A, (5, 11): 0x22B,
                    (7, 10): 0x222, (8, 10): 0x223, (7, 11): 0x22A, (8, 11): 0x22B,
                    (4, 13): 0x230, (5, 13): 0x231, (4, 14): 0x238, (5, 14): 0x239,
                    (1, 10): 0x230, (2, 10): 0x231, (1, 11): 0x238, (2, 11): 0x239,
                    (6, 8): 0x242, (6, 9): 0x21A, (6, 10): 0x251,
                    (4, 6): 0x230, (5, 6): 0x231, (4, 7): 0x238, (5, 7): 0x239}


def mauville_beam(x, y, lit):
    """The horizontal beam whose top-left cell is (x, y): a sheet standing
    at its foot, between the two coils' axes."""
    return piece("beam_%d_%d" % (x, y), [(x * 16, y * 16, x * 16 + 32, y * 16 + 32)], 16,
                 card=True, tiles=MAUVILLE_GYM_BEAM_TILES, anim=MAUVILLE_GYM_GATES,
                 when=(x, y, lit))


def mauville_beam_down(x, y, lit):
    """The vertical beam from the coil whose foot is cell (x, y) down to the
    head of the next: a strip from the first's axis, low on it, up to the
    second's, at its head - as the drawing runs it, 32 rows from (x, y)."""
    # from the first coil's front, its drawn foot at its axis, to the
    # second's axis at its head
    za, zb = y * 16 + 16, y * 16 + 57
    return piece("beam_%d_%d" % (x, y), [(x * 16, y * 16, x * 16 + 16, y * 16 + 32)], 1,
                 tiles=MAUVILLE_GYM_BEAM_TILES, anim=MAUVILLE_GYM_GATES,
                 when=(x, y + 1, lit), ribbon=((za, za - y * 16), (zb, zb - y * 16 - 32)),
                 skip=[(x, y)])


def mauville_coil(x, y, head=None):
    """The coil whose head is cell (x, y), as the drawing has it (while that
    cell holds `head`)."""
    return piece("coil_%d_%d" % (x, y), [(x * 16, y * 16 + 3, x * 16 + 16, y * 16 + 32)], 22,
                 round="lathe", leave=MAUVILLE_GYM_FLOOR,
                 when=head and (x, y, head))


def mauville_gym():
    side = MAUVILLE_GYM_SIDE
    (cx, cy), copies = MAUVILLE_GYM_COILS[0], MAUVILLE_GYM_COILS[1:]
    return [
        # front first: the beams over the coils' feet
        mauville_beam(4, 13, MAUVILLE_GYM_GREEN_H),
        mauville_beam(1, 10, MAUVILLE_GYM_GREEN_H),
        mauville_beam(4, 6, MAUVILLE_GYM_GREEN_H),
        mauville_beam_down(6, 8, MAUVILLE_GYM_GREEN_V),
        piece("coil", [(cx * 16, cy * 16 + 3, cx * 16 + 16, cy * 16 + 32)], 22, round="lathe",
              leave=MAUVILLE_GYM_FLOOR, at=copies),
        mauville_coil(3, 10),
        # the green beam's end on its head, and the red one's not
        mauville_coil(6, 10, MAUVILLE_GYM_POLE_TOP[1]),
        mauville_coil(3, 13, MAUVILLE_GYM_POLE_TOP[0]),
    ] + [
        # each block solid, its sides the wall's face, standing as drawn
        piece("block_w", [(0, 0, 48, 144)], 24, side=side, solid=True),
        piece("block_e", [(112, 0, 160, 128)], 24, side=side, solid=True),
        piece("block_se", [(112, 200, 160, 240)], 24, side=side, solid=True),
        # the dark either side of the entrance hall: its rims are drawn at
        # the floor's level, round its north and inner edges, where its walls
        # end - so its top is its drawing as drawn
        piece("block_hall_w", [(0, 272, 32, 336)], 24, side=side, flat=True,
              walls=[((32, 336), (32, 272))]),
        piece("block_hall_e", [(128, 272, 160, 336)], 24, side=side, flat=True,
              walls=[((128, 272), (128, 336))]),
        piece("wall", [(48, 0, 112, 31)], 31, fill=16, foot=31, side=side),
        # the switch at (0, 15) beside the west wall is the map's
        piece("side_w", [], 24, side=side, walls=[((0, 272), (0, 144))], skip=[(0, 15)]),
        # the east wall whole, behind the block down that side too: no gap
        # between the wall and the block's north face
        piece("side_e", [], 24, side=side, walls=[((160, 128), (160, 272))]),
    ]


def mauville_gym_alt():
    """The red beams, lit in the barriers' other state, and the two coils
    as it draws them."""
    return [
        mauville_beam(4, 10, MAUVILLE_GYM_RED_H),
        mauville_beam(7, 10, MAUVILLE_GYM_RED_H),
        mauville_beam_down(3, 11, MAUVILLE_GYM_RED_V),
        mauville_coil(6, 10, MAUVILLE_GYM_POLE_TOP[0]),
        mauville_coil(3, 13, MAUVILLE_GYM_POLE_TOP[1]),
        mauville_coil(6, 7),
    ]



# Lavaridge's gym, 1F and B1F: rooms walled off from each other, the walls
# a cell thick or a row of them, each column's run of blocked cells (read
# off the collision) a block drawn from its first row to its foot - its pale
# top, and its brick face 16 px - a piece to each rectangle of them
# (run_rects). The dark ledges across some rooms' floors stand as curbs,
# 7 px. The sand the trainers hide in, the holes down to B1F and the
# leader's dais lie on the floor, and stay drawn; the statues are Rustboro's.

LAVARIDGE_GYM_SIDE = (16, 8, 32, 24)       # a stretch of the top wall's face
LAVARIDGE_GYM_1F_RUNS = [
    (0, 0, 1), (0, 7, 8), (1, 0, 1), (1, 7, 8), (2, 0, 1), (2, 7, 8), (3, 0, 1), (3, 7, 8),
    (4, 0, 1), (4, 7, 16), (5, 0, 1), (5, 7, 8), (6, 0, 8), (7, 0, 1), (7, 7, 18), (8, 0, 1),
    (8, 7, 8), (9, 0, 1), (9, 7, 8), (10, 0, 1), (10, 7, 13), (11, 0, 8), (12, 0, 1),
    (12, 7, 8), (13, 0, 1), (13, 7, 8), (14, 0, 1), (14, 7, 8), (15, 0, 1), (15, 7, 8),
    (16, 0, 13),
]
LAVARIDGE_GYM_B1F_RUNS = [
    (0, 0, 1), (0, 4, 5), (1, 0, 1), (1, 4, 5), (2, 0, 1), (2, 4, 5), (3, 0, 1), (3, 4, 5),
    (4, 0, 2), (4, 4, 15), (5, 0, 1), (5, 4, 5), (6, 0, 1), (6, 4, 5), (7, 0, 1), (7, 4, 15),
    (8, 0, 1), (8, 4, 5), (9, 0, 1), (9, 4, 18), (10, 0, 2), (10, 4, 5), (11, 0, 1),
    (11, 4, 5), (12, 0, 1), (12, 4, 5), (13, 0, 1), (13, 4, 16), (14, 0, 1), (14, 4, 5),
    (15, 0, 1), (15, 4, 5), (16, 0, 1), (16, 4, 5),
]
# the side walls down the room's west and east edges, where no wall is drawn
# (where one is, a block)
LAVARIDGE_GYM_1F_SIDES = [(((0, 304), (0, 32)), ()),
                          (((272, 224), (272, 304)), [(16, 14), (16, 15)])]
LAVARIDGE_GYM_B1F_SIDES = [(((0, 304), (0, 32)), ()), (((272, 32), (272, 304)), ())]
# the ledges, (x0, x1, y) in cells: drawn in the cell's lower half
LAVARIDGE_GYM_1F_CURBS = [(11, 16, 13), (5, 7, 15)]
LAVARIDGE_GYM_B1F_CURBS = [(0, 4, 12), (5, 7, 11), (10, 13, 10), (14, 17, 10), (10, 13, 15)]
LAVARIDGE_GYM_FLOOR = ("a47b62", "c59c7b", "e6c594", "835a4a", "624a41")


def lavaridge_gym(runs, curbs, sides):
    """`sides`: the room's side walls, (wall, cells it leaves) - a wall as a
    piece's `walls`, the cells the statue beside it stands on."""
    side = LAVARIDGE_GYM_SIDE
    return [
        # front first: the curbs, then the walls. A curb is the room's own
        # (a block's, own_shell): its dark band and outline are found by
        # their pixels in half the rooms of Hoenn
        piece("block_curb_%d_%d" % (x0, y), [(x0 * 16, y * 16 + 8, x1 * 16, y * 16 + 16)], 7,
              leave=LAVARIDGE_GYM_FLOOR)
        for (x0, x1, y) in curbs
    ] + rect_pieces("block", runs, 16, side=lambda *r: side, solid=True) + [
        piece("side_%d" % n, [], 16, side=side, walls=[wall], skip=leaves)
        for n, (wall, leaves) in enumerate(sides)
    ]



# Mossdeep's gym: islands in the dark, joined by the warp pads. Each
# island's walls stand from their cells, 24 px tall - their 8-px tops, the
# long strips of the walls running back between them and their panelled
# faces - a piece to each rectangle of them (run_rects), and the coloured
# boxes on the floor and the
# leader's sofa stand. The arrows and switches stay drawn: the gym's scripts
# turn them (setmetatile), and they lie on the floor anyway.

MOSSDEEP_GYM_FLOOR = ("9c9cac", "bdb4c5", "ded5de", "000000")


def cells_rects(cells):
    """16-px cells (x, y) as rectangles in pixels."""
    return [(x * 16, y * 16, x * 16 + 16, y * 16 + 16) for (x, y) in cells]


def cell_runs(*runs):
    """Cells from (x, y0, y1) runs, y1 inclusive."""
    return [(x, y) for (x, y0, y1) in runs for y in range(y0, y1 + 1)]


def run_rects(runs):
    """(x, y0, y1) runs of cells, y1 inclusive, as rectangles of pixels:
    neighbouring columns with the same run in one. Every cell inside a
    piece's rectangle that no piece takes is laid with the piece (the floor
    cells between two walls, a bridge, a floor arrow, stood up with it), so a
    wall of several runs stands as one piece per rectangle; cut only between
    columns or at a gap of floor, no drawn run is split."""
    rows = {}
    for (x, y0, y1) in runs:
        rows.setdefault((y0, y1), []).append(x)
    out = []
    for (y0, y1), xs in sorted(rows.items()):
        xs.sort()
        start = prev = xs[0]
        for x in xs[1:] + [None]:
            if x is None or x != prev + 1:
                out.append((start * 16, y0 * 16, (prev + 1) * 16, (y1 + 1) * 16))
                start = x
            if x is not None:
                prev = x
    return sorted(out)


def runs_of(cells):
    """Cells as (x, y0, y1) runs down each column, y1 inclusive."""
    out = []
    for x in sorted({cx for (cx, _) in cells}):
        ys = sorted(cy for (cx, cy) in cells if cx == x)
        start = prev = ys[0]
        for y in ys[1:] + [None]:
            if y is None or y != prev + 1:
                out.append((x, start, prev))
                start = y
            if y is not None:
                prev = y
    return out


def rect_pieces(prefix, runs, height, side=None, whole=False, per=None, **kw):
    """A piece for each of run_rects(runs), named by its corner. A run a
    single cell tall stands 4 px lower than its cell, or it would be all
    front and no top - but for `whole`: a wall cut where the floor beside
    it changes level stands its level's whole height, on the cut below it;
    `side`, if given, is a function of the rectangle, as is `per`: more of
    its piece's options (its height among them)."""
    out = []
    for (x0, y0, x1, y1) in run_rects(runs):
        opts = dict(kw, **(per(x0, y0, x1, y1) if per else {}))
        h = opts.pop("height", height)
        out.append(piece("%s_%d_%d" % (prefix, x0 // 16, y0 // 16), [(x0, y0, x1, y1)],
                         h if whole else min(h, y1 - y0 - 4),
                         side=side and side(x0, y0, x1, y1), **opts))
    return out


def mossdeep_gym():
    fl = MOSSDEEP_GYM_FLOOR
    islands = {
        "nw": cell_runs((4, 2, 3), (5, 1, 3), (6, 2, 2), (7, 1, 3), (8, 2, 3), (9, 2, 7),
                        (10, 1, 7), (11, 1, 2), (12, 1, 4)),
        "ne": cell_runs((21, 4, 5), (22, 4, 6), (23, 5, 6), (24, 5, 6), (25, 5, 6)),
        "w": cell_runs((1, 18, 20), (2, 18, 19), (3, 18, 19), (4, 18, 19), (5, 18, 19),
                       (6, 16, 19), (7, 16, 17), (8, 16, 17), (9, 16, 17)),
        "sw": cell_runs((1, 27, 28), (2, 26, 28), (3, 26, 27), (4, 26, 28), (5, 27, 28),
                        (6, 27, 30), (7, 28, 29), (8, 28, 31), (9, 30, 31)),
        "s": cell_runs((11, 31, 32), (12, 30, 32), (13, 30, 31)),
        "se": cell_runs((15, 32, 33), (16, 32, 33), (17, 32, 33), (18, 32, 33), (19, 32, 33),
                        (20, 31, 33), (21, 30, 32), (22, 29, 31), (23, 29, 30)),
        "e": cell_runs((20, 20, 23), (21, 20, 21), (22, 18, 21), (23, 19, 19), (24, 18, 21),
                       (25, 20, 21)),
    }
    pieces = []
    for k, cells in islands.items():
        # (each one's sides a plain patch of its own panels: not the dark
        # the step's top runs back over)
        pieces += rect_pieces("walls_" + k, runs_of(cells), 24, leave=fl,
                              side=lambda x0, y0, x1, y1: (x0 + 4, y1 - 16, x0 + 10, y1 - 10))
    boxes = [piece("box_%d_%d" % c, cells_rects([c]), 8, leave=fl, solid=True)
             for c in ((5, 7), (8, 7), (8, 8), (3, 21), (7, 19), (9, 19))]
    sofa = piece("sofa", cells_rects([(22, 7), (23, 7), (24, 7), (25, 7), (22, 8), (25, 8)]),
                 12, leave=fl)
    return pieces + boxes + [sofa]



# Fortree's gym: a maze of green columns on a grass floor, the rotating
# gates between them (sprites, src/rotating_gate.c: no tile of the floor
# changes). The three rails across the east side stand 4 px; the leader's
# dais is a step the floor can't take and stays drawn.

FORTREE_GYM_FLOOR = ("b4bd5a", "dede8b", "f6f6bd")

# The green columns stand as cylinders, each a turned piece: one drawn whole
# standing for every column, each textured as drawn where it stands: round,
# its cell wide and deep, its lid most of its drawing. Two heights of them,
# each by its cell: 6 px, its top drawn from 6 rows into the cell north of
# it (FORTREE_GYM_COLUMNS_A, drawn whole at (7, 11)), and 14 px, from 14
# rows into it (FORTREE_GYM_COLUMNS_B, down the room's edges
# and here and there between, drawn whole at (18, 24)) - each column by the
# white crest of its top. The small ones, two to a cell and two down it,
# are turned too, each on its own; those painted on the back wall stay on
# it.
FORTREE_GYM_COLUMNS_A = [
    (0, 24), (1, 11), (1, 12), (1, 15), (1, 16), (1, 17), (1, 18), (1, 19), (1, 20), (1, 21),
    (1, 22), (1, 23), (1, 24), (2, 9), (2, 10), (2, 11), (2, 12), (2, 15), (2, 16), (2, 22),
    (2, 23), (2, 24), (3, 7), (3, 9), (3, 10), (3, 13), (3, 14), (3, 15), (3, 16), (3, 23),
    (3, 24), (4, 6), (4, 7), (4, 9), (4, 10), (4, 13), (4, 24), (5, 10), (5, 11), (5, 12),
    (5, 13), (5, 14), (5, 15), (5, 21), (5, 24), (6, 2), (6, 3), (6, 5), (6, 6), (6, 12),
    (6, 13), (6, 14), (6, 15), (6, 20), (6, 21), (6, 24), (7, 2), (7, 3), (7, 5), (7, 6),
    (7, 11), (7, 20), (7, 24), (8, 2), (8, 3), (8, 5), (8, 6), (8, 7), (8, 8), (8, 11),
    (8, 20), (8, 23), (8, 24), (9, 2), (9, 3), (9, 5), (9, 6), (9, 7), (9, 20), (9, 23),
    (9, 24), (10, 2), (10, 5), (10, 6), (10, 7), (10, 8), (10, 11), (10, 12), (10, 13),
    (10, 14), (10, 15), (10, 16), (10, 17), (10, 18), (10, 19), (10, 20), (10, 21), (10, 23),
    (10, 24), (11, 5), (11, 6), (11, 7), (11, 10), (11, 13), (11, 16), (11, 19), (11, 20),
    (11, 23), (11, 24), (12, 5), (12, 6), (12, 7), (12, 8), (12, 9), (12, 10), (12, 11),
    (12, 12), (12, 13), (12, 14), (12, 15), (12, 16), (12, 17), (12, 18), (12, 19), (12, 20),
    (12, 23), (12, 24), (13, 24), (14, 24), (17, 24), (19, 24),
]
FORTREE_GYM_COLUMNS_B = [
    (0, 3), (0, 4), (0, 5), (0, 6), (0, 7), (0, 8), (0, 9), (0, 10), (0, 11), (0, 12), (0, 13),
    (0, 14), (0, 15), (0, 16), (0, 17), (0, 18), (0, 19), (0, 20), (0, 21), (0, 22), (0, 23),
    (1, 2), (1, 3), (1, 4), (1, 5), (1, 14), (2, 14), (3, 12), (4, 12), (6, 11), (11, 9), (11, 12),
    (11, 15), (11, 18), (18, 3), (18, 4), (18, 5), (18, 6), (18, 7), (18, 8), (18, 9), (18, 10),
    (18, 11), (18, 12), (18, 13), (18, 14), (18, 15), (18, 16), (18, 17), (18, 18), (18, 19),
    (18, 20), (18, 21), (18, 22), (18, 23), (18, 24), (19, 4), (19, 5), (19, 6), (19, 7), (19, 8),
    (19, 9), (19, 10), (19, 11), (19, 12), (19, 13), (19, 14), (19, 15), (19, 16), (19, 17),
    (19, 18), (19, 19), (19, 20), (19, 21), (19, 22), (19, 23),
]
FORTREE_GYM_SMALL = [(3, 3, 7), (3, 18, 18), (5, 19, 21), (6, 17, 17), (6, 19, 21), (7, 19, 20),
                     (8, 13, 13), (8, 17, 20), (9, 15, 20)]


def fortree_gym():
    fl = FORTREE_GYM_FLOOR + ("7b839c",)       # the columns' shadow on the grass

    def columns(name, cells, ref, height):
        """A column `height` tall drawn whole at `ref`, standing again at
        every one of `cells`, each textured as drawn there (`variants`)."""
        top = -height        # its top's first row, from its cell's: round, 16 deep
        # a copy stands by its rectangle's corner, the cell its top is in
        corner = lambda x, y: (x, y + (top // 16))   # noqa: E731
        rx, ry = ref
        return piece(name, [(rx * 16, ry * 16 + top, rx * 16 + 16, ry * 16 + 16)], height,
                     round="lathe", squash=1.0, turn=(8, 1), leave=fl, variants=True,
                     at=[corner(*c) for c in sorted(set(cells) - {ref})])

    out = [columns("column", FORTREE_GYM_COLUMNS_A, (7, 11), 6),
           columns("column_tall", FORTREE_GYM_COLUMNS_B, (18, 24), 14)]
    # the small ones, two to a cell and two down it, each turned
    out += [piece("small_%d_%d" % (x, y0), [(x * 16, y0 * 16, x * 16 + 16, y1 * 16 + 16)], 5,
                  round="lathe", posts=(8, 8), turn=(6, 1), leave=fl)
            for (x, y0, y1) in FORTREE_GYM_SMALL]
    rails = [piece("rail_%d" % y, [(208, y * 16, 288, y * 16 + 16)], 4, leave=fl, solid=True)
             for y in (6, 12, 17)]
    return out + rails + [
        piece("wall", [(0, 0, 320, 32)], 32, fill=16, foot=32, side=(32, 0, 48, 32)),
    ]



# Sootopolis's gym. 1F: three tiers of ice, each ringed by its wall - their
# light tops and lilac faces, the crystal fountains at the back with the
# back wall - a piece to each rectangle of them (run_rects), and the
# rocks on the ice. The ice cracks under the player
# (SetSootopolisGymCrackedIceMetatiles) and the script lays the stairs in the
# walls' gaps (setmetatile): both lie on the floor, and stay drawn, as do the
# two holes in it. B1F: the ice cave's rock walls, the same way. The floor's
# white is the walls' tops' too: only its blues are left round them.

SOOTOPOLIS_GYM_ICE = ("5abdd5", "83cdde", "acdee6", "d5eeee")
SOOTOPOLIS_GYM_1F_WALLS = [
    [(0, 0, 25), (1, 0, 11), (1, 18, 20), (2, 0, 11), (2, 18, 20)],
    [(3, 0, 1), (3, 4, 5), (3, 10, 15), (3, 18, 25), (4, 0, 2), (4, 4, 5), (4, 10, 15),
     (4, 18, 25), (5, 0, 2), (5, 4, 5), (5, 10, 11), (5, 15, 21)],
    [(6, 0, 1), (6, 4, 5), (6, 10, 11), (6, 15, 21), (7, 0, 1), (7, 4, 5), (7, 10, 11),
     (7, 15, 16), (7, 20, 21), (8, 0, 1)],
    [(9, 0, 1), (9, 4, 5), (9, 10, 11), (9, 15, 16), (9, 20, 21), (10, 0, 1), (10, 4, 5),
     (10, 10, 11), (10, 15, 21), (11, 0, 2), (11, 4, 5), (11, 10, 11), (11, 15, 21)],
    [(12, 0, 2), (12, 4, 5), (12, 10, 15), (12, 18, 25), (13, 0, 1), (13, 4, 5), (13, 10, 15),
     (13, 18, 25), (14, 0, 11), (14, 18, 20)],
    [(15, 0, 11), (15, 18, 20), (16, 2, 3), (16, 9, 11), (16, 14, 15), (16, 18, 20)],
]
SOOTOPOLIS_GYM_ROCKS = [(4, 8), (6, 13), (7, 8), (9, 7), (9, 19), (10, 13), (11, 9), (12, 7)]
SOOTOPOLIS_GYM_B1F_WALLS = [
    [(0, 4, 11), (0, 14, 16), (0, 18, 20), (1, 4, 11), (1, 14, 16), (1, 18, 20), (2, 4, 5),
     (2, 10, 16), (2, 18, 20)],
    [(3, 4, 5), (3, 10, 16), (3, 18, 25), (4, 4, 5), (4, 10, 11), (4, 15, 25), (5, 4, 5),
     (5, 10, 11), (5, 15, 16), (5, 19, 21)],
    [(6, 4, 5), (6, 10, 11), (6, 15, 21), (7, 4, 5), (7, 10, 11), (7, 15, 16), (7, 20, 21),
     (8, 4, 5)],
    [(9, 4, 5), (9, 10, 11), (9, 15, 16), (9, 20, 21), (10, 4, 5), (10, 10, 11), (10, 15, 21),
     (11, 4, 5), (11, 10, 11), (11, 15, 16), (11, 19, 20)],
    [(12, 4, 5), (12, 10, 11), (12, 15, 25), (13, 4, 5), (13, 10, 16), (13, 18, 25),
     (14, 4, 5), (14, 10, 16), (14, 18, 20)],
    [(15, 4, 11), (15, 14, 16), (15, 18, 20), (16, 4, 11), (16, 14, 16), (16, 18, 20)],
]


# The 1F's tiers rise to the north, a level (16 px) each, from the entrance
# to the leader (relief_fixes.json): its walls stand on the tier each
# reaches down to, so a run of them is cut where the floor beside it changes
# level - the first rows of the tiers above the entrance's.
SOOTOPOLIS_GYM_1F_TIERS = (4, 10, 15, 20)
SOOTOPOLIS_GYM_B1F_TIERS = (10, 15, 20)


def tier_tops(tiers):
    """`per` for a tier's walls (rect_pieces): a wall rising from one tier to
    the next leaves its top's row, the next tier's level, to the map - drawn
    there as the game draws it, it closes the floor beyond onto the wall's
    top (the relief lifts it, relief_fixes.json)."""
    def per(x0, y0, x1, y1):
        out = {}
        if y0 // 16 in tiers and y1 - y0 > 16:
            out["skip"] = [(x, y0 // 16) for x in range(x0 // 16, x1 // 16)]
        # (a run whose foot is a tier's first row: a level taller for it)
        if y1 // 16 - 1 in tiers and y1 - y0 > 16:
            out["height"] = 32
        return out
    return per


def cut_runs(runs, rows):
    """(x, y0, y1) runs cut before each of `rows` - but not where that would
    leave a single row at the run's foot: that row is the riser of the run
    above it, which stands on it a level taller (tier_tops)."""
    out = []
    for (x, y0, y1) in runs:
        for r in sorted(rows):
            if y0 < r < y1:
                out.append((x, y0, r - 1))
                y0 = r
        out.append((x, y0, y1))
    return out


def sootopolis_gym_1f():
    ice = SOOTOPOLIS_GYM_ICE
    walls = rect_pieces("walls", cut_runs([r for runs in SOOTOPOLIS_GYM_1F_WALLS for r in runs],
                                          SOOTOPOLIS_GYM_1F_TIERS), 16, leave=ice,
                        whole=True, per=tier_tops(SOOTOPOLIS_GYM_1F_TIERS))
    rocks = [piece("rock_%d_%d" % c, cells_rects([c]), 10, leave=ice)
             for c in SOOTOPOLIS_GYM_ROCKS]
    return walls + rocks


def sootopolis_gym_b1f():
    # the cave's snow, and the dark round its rock. Solid, each wall's
    # sides - and the steps of its snowy edge's outline - its own rock, a
    # whole cell of it: not its edge's column drawn down them in stripes,
    # nor its foot, the light floor below the cave's walls, in bands
    # (its rooms under the 1F's tiers, at their levels: its walls cut as theirs)
    return rect_pieces("wall", cut_runs([r for runs in SOOTOPOLIS_GYM_B1F_WALLS for r in runs],
                                        SOOTOPOLIS_GYM_B1F_TIERS), 16,
                       leave=("acdee6", "d5eeee", "ffffff", "000000"), solid=True,
                       side=lambda x0, y0, x1, y1: "cell", whole=True,
                       per=tier_tops(SOOTOPOLIS_GYM_B1F_TIERS))



# The Battle Frontier. Scott's house and the lounges: their back walls and
# side walls; their plants are the Center's and the lounges' television
# House2's, found again (reuse_pieces). Their front corners, black in the
# drawing, are floor in the round.


# The Battle Frontier's lobbies: (layout, width, height, the back wall's
# foot, floor). Their doors are painted on the back wall, as the rooms'
# doors are: the receptionist walks the player into them.
FRONTIER_LOBBIES = {
    "arena": ("LAYOUT_BATTLE_FRONTIER_BATTLE_ARENA_LOBBY", 256, 208, 48, 0x202),
    "dome": ("LAYOUT_BATTLE_FRONTIER_BATTLE_DOME_LOBBY", 368, 272, 80, 0x340),
    "factory": ("LAYOUT_BATTLE_FRONTIER_BATTLE_FACTORY_LOBBY", 304, 192, 80, 0x201),
    "palace": ("LAYOUT_BATTLE_FRONTIER_BATTLE_PALACE_LOBBY", 400, 192, 80, 0x201),
    "pike": ("LAYOUT_BATTLE_FRONTIER_BATTLE_PIKE_LOBBY", 176, 208, 64, 0x2c2),
    "pyramid": ("LAYOUT_BATTLE_FRONTIER_BATTLE_PYRAMID_LOBBY", 240, 288, 80, 0x20b),
    "tower": ("LAYOUT_BATTLE_FRONTIER_BATTLE_TOWER_LOBBY", 400, 160, 32, 0x200),
}


def frontier_lobby(name):
    _, w, h, foot, _ = FRONTIER_LOBBIES[name]
    return FRONTIER_LOBBY_PIECES.get(name, list)() + room_shell(
        w, h, foot, side=FRONTIER_LOBBY_SIDES.get(name))


# the Battle Tower's: five counters along the back, a glass terminal on
# four of them; a low yellow divider runs from the wall to each terminal,
# and to the fifth counter's east end, as tall as the counters.
def frontier_tower_lobby():
    pieces = []
    for c in (4, 8, 12, 16):
        x = c * 16
        pieces += [piece("divider_%d" % c, [(x, 22, x + 16, 68)], 10),
                   piece("terminal_%d" % c, [(x, 68, x + 16, 96)], 18),
                   piece("counter_%d_e" % c, [(x + 16, 76, x + 32, 96)], 10)]
        if c > 4:
            pieces.append(piece("counter_%d_w" % c, [(x - 16, 76, x, 96)], 10))
    return pieces + [piece("divider_20", [(320, 22, 336, 96)], 10),
                     piece("counter_20_w", [(304, 76, 320, 96)], 10)]


# the Battle Dome's: an L of counter each side, its arm running back, and
# a counter between it and the middle, 6 px tall; on each a terminal, or
# the results monitor, and red ropes on posts between them
DOME_FLOOR = ("8bcdac", "9ceecd", "7bac8b")
# the west arm's top, the row its outline starts on in each column from
# x 32 (slanting down to the west, its corners rounded)
DOME_ARM_TOP = [134, 132, 130, 128, 126, 124, 122, 120, 114, 113] + [112] * 12 + [113, 114]


def dome_counter_arm():
    """The west counter's plan: the arm's back stepped column by column
    along its drawn outline (a slanted edge is a face the GBA's camera
    would see beside the top), then the bar to the terminal."""
    plan = [(32, 176)]
    for i, top in enumerate(DOME_ARM_TOP):
        for pt in ((32 + i, top + 6), (33 + i, top + 6)):
            if len(plan) > 1 and plan[-1][1] == pt[1] == plan[-2][1]:
                plan[-1] = pt     # along a run of the same row
            elif pt != plan[-1]:
                plan.append(pt)
    return plan + [(56, 166), (64, 166), (64, 176)]


def frontier_dome_lobby():
    def ropes(name, x0, y0, y1):
        return piece(name, [(x0, y0, x0 + 32, y1)], y1 - y0, card=True, leave=DOME_FLOOR)
    return [
        piece("terminal_w", [(64, 148, 80, 176)], 22),
        piece("terminal_red", [(112, 148, 128, 176)], 22),
        piece("monitor", [(232, 144, 264, 176)], 24),
        piece("terminal_e", [(288, 148, 304, 176)], 22),
        piece("counter_w", [(32, 112, 64, 176)], 6, leave=DOME_FLOOR, plan=dome_counter_arm()),
        piece("counter_e", [(304, 112, 336, 176)], 6, leave=DOME_FLOOR,
              plan=[(368 - x, z) for x, z in reversed(dome_counter_arm())]),
        piece("counter_mid_w1", [(96, 148, 112, 176)], 6, leave=DOME_FLOOR),
        piece("counter_mid_w2", [(128, 148, 144, 176)], 6, leave=DOME_FLOOR),
        piece("counter_mid_e1", [(224, 148, 232, 176)], 6, leave=DOME_FLOOR),
        piece("counter_mid_e2", [(264, 148, 272, 176)], 6, leave=DOME_FLOOR),
        ropes("ropes_w", 0, 158, 176),
        ropes("ropes_mw1", 144, 126, 144),
        ropes("ropes_mw2", 144, 158, 176),
        ropes("ropes_me1", 192, 126, 144),
        ropes("ropes_me2", 192, 158, 176),
        ropes("ropes_e", 336, 158, 176),
    ]


# the Battle Factory's: as the Tower's, a divider 10 px tall from the wall
# to a terminal or to a counter's end, the counters round them; and two
# bins by the door. Its back wall stands to row 5, its base band and the
# doorways into the back painted on it.
def frontier_factory_lobby():
    return [
        piece("divider_2", [(32, 72, 48, 100)], 10),
        piece("terminal_2", [(32, 100, 48, 128)], 18),
        piece("counter_3", [(48, 108, 64, 128)], 10),
        piece("counter_5", [(80, 108, 112, 128)], 10),
        piece("divider_7", [(112, 72, 128, 128)], 10),
        piece("divider_11", [(176, 72, 192, 128)], 10),
        piece("counter_12", [(192, 108, 224, 128)], 10),
        piece("counter_15", [(240, 108, 256, 128)], 10),
        piece("divider_16", [(256, 72, 272, 100)], 10),
        piece("terminal_16", [(256, 100, 272, 128)], 18),
        piece("bin_15", [(240, 166, 256, 192)], 16),
        piece("bin_17", [(272, 166, 288, 192)], 16),
    ]


# the Battle Palace's: two low white tables each side of the receptionist,
# on gold legs, and a glass terminal on a post in front of them
PALACE_FLOOR = ("a46283", "bd8ba4", "944162")


def frontier_palace_lobby():
    out = []
    for side, dx in (("w", 0), ("e", 224)):
        out += [
            piece("terminal_" + side, [(32 + dx, 108, 48 + dx, 136)], 28, card=True,
                  leave=PALACE_FLOOR),
            piece("table_%s1" % side, [(32 + dx, 80, 56 + dx, 118)], 6, leave=PALACE_FLOOR),
            piece("table_%s2" % side, [(56 + dx, 96, 80 + dx, 118)], 6, leave=PALACE_FLOOR),
            piece("table_%s3" % side, [(96 + dx, 96, 120 + dx, 118)], 6, leave=PALACE_FLOOR),
            piece("table_%s4" % side, [(120 + dx, 80, 144 + dx, 118)], 6, leave=PALACE_FLOOR),
        ]
    return out


# the Battle Pike's: fourteen copper pillars, a gold ball on each, 21 px
# tall on their cells - those at the side walls too - and a glass terminal
# on a post before the curtain
PIKE_CURTAIN = ("c50000", "ff0000", "7b3100", "523131", "ff9462")
PIKE_FLOOR = ("7b6a31", "a48b31", "cdac31")


def frontier_pike_lobby():
    cells = [(3, 4), (7, 4), (4, 5), (6, 5), (1, 7), (9, 7), (0, 8), (3, 8), (7, 8), (10, 8),
             (1, 10), (9, 10), (3, 12), (7, 12)]
    return [piece("terminal", [(128, 44, 144, 72)], 28, card=True,
                  leave=PIKE_CURTAIN + PIKE_FLOOR)] + [
        piece("pillar_%d_%d" % (x, y), [(x * 16, y * 16 - 15, x * 16 + 16, y * 16 + 16)], 21)
        for (x, y) in cells]


# the Battle Arena's: its tatami, the staff's floor, raised by no more
# than its edge, stays drawn; the results television in its cabinet, the
# plants on stands, the terminal and the shelf down the east side stand
def frontier_arena_lobby():
    return [
        piece("terminal", [(80, 128, 96, 160)], 24),
        piece("television", [(0, 92, 48, 128)], 28),
        piece("plant_12", [(192, 96, 208, 128)], 24),
        piece("plant_14", [(224, 96, 240, 128)], 24),
        piece("plant_sw", [(0, 160, 16, 192)], 24),
        piece("shelf_e", [(240, 96, 256, 208)], 16),
    ]


# the Battle Pyramid's: the columns either side and the braziers stand
# before the back wall, its statues, the models of the pyramid and the
# square into it painted on it (it stands to row 5); its glass terminal is
# the Pike's, found again. The terrace and its stairs, rows 8 to 12, are a
# level below the room's floor the console's floor does not have: they
# stay drawn
def frontier_pyramid_lobby():
    return [
        piece("pillar_w", [(32, 0, 48, 104)], 80),
        piece("pillar_e", [(192, 0, 208, 104)], 80),
        piece("brazier_w", [(64, 72, 80, 96)], 20),
        piece("brazier_e", [(160, 72, 176, 96)], 20),
    ]


FRONTIER_LOBBY_PIECES = {
    "arena": frontier_arena_lobby,
    "dome": frontier_dome_lobby,
    "factory": frontier_factory_lobby,
    "palace": frontier_palace_lobby,
    "pike": frontier_pike_lobby,
    "pyramid": frontier_pyramid_lobby,
    "tower": frontier_tower_lobby,
}
# the stretch of each back wall that dresses its side walls: one with
# nothing standing before it, as tall as the wall
FRONTIER_LOBBY_SIDES = {
    "factory": (96, 0, 112, 80),
    "palace": (128, 0, 144, 80),
    "tower": (112, 0, 128, 32),
}


# The Battle Frontier's battle rooms and corridors: their walls, the doors
# painted on them (the Tower corridor's two, which open by script, stay shut
# as the Elite Four's back door does: a gap in a wall shows the floor behind
# it, where the map draws the door lying flat). The trainers stand on the
# battlefields and stages, which stay drawn.

def pike_pillar(x, y):
    """A Battle Pike pillar on cell (x, y): its cap from 15 px above the
    cell, 21 px tall, as the lobby's."""
    return piece("pillar_%d_%d" % (x, y), [(x * 16, y * 16 - 15, x * 16 + 16, y * 16 + 16)], 21)


def tower_battle_room():
    # the four lamps in its corners, 16 px tall
    return [piece("lamp_%d_%d" % (x, y), [(x * 16, y * 16 - 8, x * 16 + 16, y * 16 + 16)], 16)
            for (x, y) in ((0, 2), (9, 2), (0, 8), (9, 8))] + room_shell(160, 144, 32)


def dome_pre_battle_room():
    # the black down its west side is beyond the room
    side = (64, 0, 80, 32)
    return [
        piece("wall", [(32, 0, 144, 32)], 32, fill=16, foot=32, side=side),
        piece("side_w", [], 32, side=side, walls=[((32, 128), (32, 32))]),
        piece("side_e", [], 32, side=side, walls=[((144, 32), (144, 128))]),
    ]


def dome_battle_room():
    # a wall's top is filled from its own top rows: 8 of them, the banner
    # being 16 px tall. In two halves: in one, 320 px wide and a cell tall,
    # its faces read a row past its drawing, and the page has no room for it
    side = (64, 48, 80, 64)
    return [
        piece("wall_w", [(0, 48, 160, 64)], 16, fill=8, foot=64, side=side),
        piece("wall_e", [(160, 48, 320, 64)], 16, fill=8, foot=64, side=side),
        piece("side_w", [], 16, side=side, walls=[((0, 160), (0, 64))]),
        piece("side_e", [], 16, side=side, walls=[((320, 64), (320, 160))]),
    ]


def pike_three_path_room():
    # its pillars, silver, stand here; the other Pike rooms' are these,
    # found again (a pillar defined in two rooms would be found in each)
    return [pike_pillar(x, y) for (x, y) in ((0, 4), (4, 4), (8, 4), (12, 4), (3, 10), (8, 10))]


def pike_wild_mons_room():
    # the gold pillars in rows across the room; its silver ones are the
    # three-path room's, found again
    cells = cell_runs((0, 6, 6), (1, 6, 6), (2, 6, 6), (6, 6, 6), (7, 6, 6), (8, 6, 6))
    cells += [(x, 8) for x in range(0, 8)] + [(x, 10) for x in range(1, 9)]
    cells += [(x, 12) for x in range(0, 7)] + [(8, 12), (0, 14)] + [(x, 14) for x in range(2, 9)]
    cells += [(x, 16) for x in range(0, 7)] + [(8, 16), (0, 18), (1, 18), (7, 18), (8, 18)]
    return [pike_pillar(x, y) for (x, y) in cells]


def arena_corridor():
    """The Battle Arena's corridor runs round a block: its face to the south,
    shoji 64 px tall, its sides along the corridors; the black over it, and
    between the two stretches of the back wall, is beyond the room."""
    side = (64, 0, 80, 48)
    shoji = (112, 128, 128, 192)
    return [
        piece("wall_w", [(0, 0, 96, 48)], 48, fill=16, foot=48, side=side),
        piece("wall_e", [(192, 0, 288, 48)], 48, fill=16, foot=48, side=side),
        piece("block", [(48, 128, 240, 192)], 64, fill=16, foot=192, side=shoji),
        piece("block_sides", [], 64, side=shoji,
              walls=[((96, 48), (96, 80)), ((48, 80), (48, 192)), ((192, 80), (192, 48)),
                     ((240, 192), (240, 80))]),
        piece("side_w", [], 48, side=side, walls=[((0, 224), (0, 48))]),
        piece("side_e", [], 48, side=side, walls=[((288, 48), (288, 224))]),
    ]


# (name, layout, ground, pieces[, more of the room's keys])
FRONTIER_ROOMS = [
    ("frontier_tower_corridor", "LAYOUT_BATTLE_FRONTIER_BATTLE_TOWER_CORRIDOR", 0x229,
     lambda: room_shell(272, 80, 32)),
    ("frontier_tower_multi_corridor", "LAYOUT_BATTLE_FRONTIER_BATTLE_TOWER_MULTI_CORRIDOR", 0x229,
     lambda: room_shell(272, 80, 32)),
    ("frontier_tower_battle_room", "LAYOUT_BATTLE_FRONTIER_BATTLE_TOWER_BATTLE_ROOM", 0x23a,
     tower_battle_room),
    # the Tower's elevator, Trainer Hill's too
    ("battle_elevator", "LAYOUT_BATTLE_ELEVATOR", 0x265, lambda: room_shell(80, 112, 48)),
    ("frontier_tower_multi_partner_room", "LAYOUT_BATTLE_FRONTIER_BATTLE_TOWER_MULTI_PARTNER_ROOM",
     0x200, lambda: room_shell(336, 240, 32)),
    ("frontier_dome_corridor", "LAYOUT_BATTLE_FRONTIER_BATTLE_DOME_CORRIDOR", 0x341,
     lambda: room_shell(768, 112, 64)),
    ("frontier_dome_pre_battle_room", "LAYOUT_BATTLE_FRONTIER_BATTLE_DOME_PRE_BATTLE_ROOM", 0x346,
     dome_pre_battle_room),
    # the banner along the foot of the stands, 16 px; the stands stay drawn,
    # the audience standing in them (as a wall they would hide it)
    ("frontier_dome_battle_room", "LAYOUT_BATTLE_FRONTIER_BATTLE_DOME_BATTLE_ROOM", 0x309,
     dome_battle_room),
    ("frontier_palace_corridor", "LAYOUT_BATTLE_FRONTIER_BATTLE_PALACE_CORRIDOR", 0x201,
     lambda: room_shell(272, 224, 64)),
    ("frontier_palace_battle_room", "LAYOUT_BATTLE_FRONTIER_BATTLE_PALACE_BATTLE_ROOM", 0x206,
     lambda: room_shell(240, 160, 48)),
    # the pipes, the shutters, the doorways and the desks in the alcoves,
    # all above row 6, painted on its wall
    ("frontier_factory_pre_battle_room", "LAYOUT_BATTLE_FRONTIER_BATTLE_FACTORY_PRE_BATTLE_ROOM",
     0x202, lambda: room_shell(272, 224, 96)),
    ("frontier_factory_battle_room", "LAYOUT_BATTLE_FRONTIER_BATTLE_FACTORY_BATTLE_ROOM", 0x2f1,
     lambda: room_shell(208, 192, 64)),
    # the Pike's: their pillars copper (the lobby's), silver (the three-path
    # room's) and gold (the wild Pokemon's room's); the normal room's
    # curtain, which opens by script, is painted on its wall
    # the corridor's two silver pillars by the curtain are its own (the
    # curtain behind them is not the three-path room's)
    ("frontier_pike_corridor", "LAYOUT_BATTLE_FRONTIER_BATTLE_PIKE_CORRIDOR", 0x2c1,
     lambda: [pike_pillar(3, 4), pike_pillar(9, 4)] + room_shell(224, 128, 64)),
    ("frontier_pike_three_path_room", "LAYOUT_BATTLE_FRONTIER_BATTLE_PIKE_THREE_PATH_ROOM", 0x2c1,
     lambda: pike_three_path_room() + room_shell(208, 176, 64)),
    ("frontier_pike_room_normal", "LAYOUT_BATTLE_FRONTIER_BATTLE_PIKE_ROOM_NORMAL", 0x2c1,
     lambda: room_shell(144, 128, 64)),
    # its gold pillars, at the room's edges, are drawn a pixel narrower than
    # the wild room's: its own, and named as its edge's (own_shell) - found
    # again in the wild room, they would take its pillars' pixels
    ("frontier_pike_room_final", "LAYOUT_BATTLE_FRONTIER_BATTLE_PIKE_ROOM_FINAL", 0x340,
     lambda: [dict(pike_pillar(x, y), name="edge_pillar_%d_%d" % (x, y))
              for (x, y) in ((0, 4), (4, 4), (0, 7), (4, 7))] + room_shell(80, 128, 64),
     {"shade": [0x341]}),
    # (its floor's shaded cells run on beside the pillars at the ends of
    # their rows: floor, not theirs)
    ("frontier_pike_room_wild_mons", "LAYOUT_BATTLE_FRONTIER_BATTLE_PIKE_ROOM_WILD_MONS", 0x2c1,
     lambda: pike_wild_mons_room() + room_shell(144, 320, 64), {"shade": [0x2da, 0x2d9]}),
    ("frontier_arena_corridor", "LAYOUT_BATTLE_FRONTIER_BATTLE_ARENA_CORRIDOR", 0x201,
     arena_corridor),
    ("frontier_arena_battle_room", "LAYOUT_BATTLE_FRONTIER_BATTLE_ARENA_BATTLE_ROOM", 0x202,
     lambda: room_shell(256, 176, 48)),
]


def front_corners(w, h):
    """A room's two bottom corner cells the GBA leaves black, as `open`."""
    return [[(0, h - 16), (16, h - 16), (16, h), (0, h)],
            [(w - 16, h - 16), (w, h - 16), (w, h), (w - 16, h)]]


# Fortree (voxel_fortree.py). The forest floor the tree houses stand on,
# and what their cells draw that is the wood's: the canopy (both layers of
# 0x22A, 0x244, 0x237's bottom), the path (0x261, 0x253)
FORTREE_GROUND = [GRASS, 0x22A, 0x244, 0x237, 0x261, 0x253]
# a house's cells: not its top corners (small trees of the wood) nor the
# row under its deck but the ladder's foot
FORTREE_OWNED = [(i, j) for j in range(5) for i in range(5) if (i, j) not in ((0, 0), (4, 0))] + [(2, 5)]
FORTREE_OWNED_DEEP = [(i, j) for j in range(6) for i in range(5) if (i, j) not in ((0, 0), (4, 0))] + [(2, 6)]


def flat_art(make):
    """flat() for parts read off the art (voxel_fortree): their hidden faces
    read the grown band."""
    def parts(art):
        tex = Mound.with_ring(art, ())
        return [Grown(p, 2 * art.size[1]) for p in make(art)], tex
    return parts


SPECS = [
    {
        "name": "littleroot_house_w",
        "layout": "LAYOUT_LITTLEROOT_TOWN",
        "rect": (2, 4, 5, 5),
        "ground": [GRASS],
        "parts": lambda: littleroot_house(8),
        "exact": HOUSE_EXACT,
    },
    {
        "name": "littleroot_house_e",
        "layout": "LAYOUT_LITTLEROOT_TOWN",
        "rect": (13, 4, 5, 5),
        "ground": [GRASS],
        "parts": lambda: littleroot_house(64),
        "exact": HOUSE_EXACT,
    },
    {
        "name": "littleroot_lab",
        "layout": "LAYOUT_LITTLEROOT_TOWN",
        "rect": (3, 12, 7, 5),
        "ground": [GRASS],
        "parts": littleroot_lab,
        "exact": LAB_EXACT,
    },
    {
        # The canonical copy: Oldale paints its path into the top row.
        "name": "pokemon_center",
        "layout": "LAYOUT_PETALBURG_CITY",
        "rect": (19, 13, 4, 4),
        "match_rows": (1, 4),
        "ground": [GRASS],
        "parts": lambda: center_or_mart((9, 16), crown=True),
        "exact": CROWN_EXACT,
    },
    {
        # Oldale's copy has a tree's crown over its top-right corner.
        "name": "poke_mart",
        "layout": "LAYOUT_MAUVILLE_CITY",
        "rect": (22, 11, 4, 4),
        "match_rows": (1, 4),
        "ground": [GRASS],
        "parts": lambda: center_or_mart((12, 16)),
        "exact": CENTER_EXACT,
    },
    {
        "name": "oldale_house",
        "layout": "LAYOUT_OLDALE_TOWN",
        "rect": (4, 4, 4, 4),
        "ground": [GRASS],
        "parts": oldale_house,
        "exact": OLDALE_HOUSE_EXACT,
    },
    {
        "name": "dewford_house",
        "layout": "LAYOUT_DEWFORD_TOWN",
        "rect": (16, 11, 4, 4),
        "match_rows": (1, 4),
        "ground": [0x124],
        "parts": lambda: dewford_house(64),
        "exact": kit_house_exact(64),
    },
    {
        "name": "dewford_hall",
        "layout": "LAYOUT_DEWFORD_TOWN",
        "rect": (1, 0, 5, 4),
        "ground": [0x124],
        "parts": lambda: dewford_house(80),
        "exact": kit_house_exact(80),
    },
    {
        "name": "dewford_gym_exterior",
        "layout": "LAYOUT_DEWFORD_TOWN",
        "rect": (5, 13, 6, 5),
        "ground": [0x124],
        "parts": gym,
        "exact": GYM_EXACT,
    },
    {
        "name": "briney_house",
        "layout": "LAYOUT_ROUTE104",
        "rect": (15, 47, 5, 4),
        "ground": [GRASS],
        "parts": briney_house,
        "exact": BRINEY_HOUSE_EXACT,
    },
    {
        "name": "flower_shop",
        "layout": "LAYOUT_ROUTE104",
        "rect": (3, 15, 6, 4),
        # the cobbled path round it
        "ground": [GRASS, 0x206, 0x207],
        "parts": flower_shop,
        "exact": FLOWER_SHOP_EXACT,
    },
    {
        "name": "kit_house_4",
        "layout": "LAYOUT_PETALBURG_CITY",
        "rect": (9, 16, 4, 4),
        "ground": [GRASS],
        "parts": lambda: kit_house(64),
        "exact": kit_house_exact(64),
    },
    {
        "name": "kit_house_5",
        "layout": "LAYOUT_PETALBURG_CITY",
        "rect": (5, 2, 5, 4),
        "ground": [GRASS],
        "parts": lambda: kit_house(80),
        "exact": kit_house_exact(80),
    },
    {
        # The kit house in Slateport's purple: the same rows, its own tiles.
        # The copy by the harbour (20, 41) is found from this one.
        "name": "slateport_house_4",
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (4, 16, 4, 4),
        "ground": [GRASS],
        "keep": kit_house_keep(64),
        "patch_own": True,
        "parts": lambda: kit_house(64),
        "exact": kit_house_exact(64),
    },
    {
        # the same house five cells wide: Route 109's Seashore House, on sand
        "name": "slateport_house_5",
        "layout": "LAYOUT_ROUTE109",
        "rect": (10, 2, 5, 4),
        "ground": [0x124],
        "parts": lambda: kit_house(80),
        "exact": kit_house_exact(80),
    },
    {
        "name": "slateport_house_6",
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (24, 41, 6, 4),
        "ground": [0x202],
        "keep": kit_house_keep(96),
        "patch_own": True,
        "parts": lambda: kit_house(96),
        "exact": kit_house_exact(96),
    },
    {
        # Slateport's awning over the way to Route 110; the market's gate is
        # the same awning over the market's paving (found from this one).
        # Lifted 12 so that people walk under it.
        "name": "slateport_awning",
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (16, 7, 5, 3),
        "match_rows": (0, 2),
        "ground": [GRASS],
        "ground_px": True,
        # the market's gate stands over the market's path, not on the grass
        # round it: the path runs on under the roof, in its shade as the row
        # of the posts draws it (the roof's back rows show the path still in
        # the sun above it)
        "cell_ground_at": {("LAYOUT_SLATEPORT_CITY", 8, 30): {
            (1, 0): 0x210, (2, 0): 0x211, (3, 0): 0x212,
            (1, 1): 0x286, (2, 1): 0x285, (3, 1): 0x287,
            (1, 2): 0x286, (2, 2): 0x285, (3, 2): 0x287}},
        "parts": lambda: [Awning("awning", 8, 72, top_v=1, valance_v=24, post_v=32,
                                 foot_v=47, back=0, lift=12, repeat=(3, 11),
                                 posts=[(8, 14), (66, 72)], shaft=(34, 43), foot=(43, 47))],
        "exact": [(8, 3, 72, 32)],
    },
    {
        "name": "fan_club",
        # its projected shapes stand past its drawing: cut to it
        "clip_art": True,
        "layout": "LAYOUT_SLATEPORT_CITY",
        # with the row of cells under it, where the door posts' feet are
        # drawn: as the ground's they lay flat on the paving beside the posts
        "rect": (2, 22, 5, 6),
        "ground": [GRASS, 0x209, 0x218, 0x219],
        "patch_cells": [(i, 5) for i in range(5)],
        # of that row only the posts' feet are its own: the path's edge
        # drawn there is the map's (as the model's, nothing drew it)
        "keep": [(0, 0, 80, 80), (29, 80, 33, 96), (47, 80, 51, 96)],
        "parts": fan_club,
        "exact": FAN_CLUB_EXACT,
    },
    {
        "name": "museum",
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (25, 7, 7, 6),
        "ground": [GRASS],
        # the trees' feet in its top row are not its own, nor the leaves and
        # grass beside its roof's rounded back corners (rows 2-7)
        "keep": [(6, 2, 106, 3), (5, 3, 107, 4), (4, 4, 108, 5), (3, 5, 109, 6), (2, 6, 110, 7),
                 (1, 7, 111, 8), (0, 8, 112, 96)],
        "parts": museum,
        "exact": MUSEUM_EXACT,
    },
    {
        "name": "contest_hall",
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (28, 22, 6, 6),
        "ground": [GRASS],
        # the seawall drawn behind it is not its own
        "keep": [(0, 9, 96, 96)],
        "parts": contest_hall,
        "exact": CONTEST_EXACT,
    },
    {
        # The battle tent: a smooth dome and its door (battle_tent). The same
        # drawing stands in Verdanturf and Fallarbor in their own tiles.
        "name": "battle_tent",
        # a coarser grid for its round parts (2026-10-10: the console's
        # budgets), the drawing still projected wherever the view sees it
        "lod": (6, 6, 3),
        # the coarse grid misses 6 pixels of its outline at 45 degrees
        "tolerance": 6,
        "anywhere": True,
        # Verdanturf paints its cliff into the top row
        "match_rows": (1, 5),
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (8, 8, 5, 5),
        "ground": [GRASS],
        # on its lawn: the paving in front of it is commoner round it; and
        # every cell as the map paints it round the tent (its grass's dots)
        "ground_fixed": {"LAYOUT_SLATEPORT_CITY": GRASS},
        "patch_own": True,
        "ground_px": True,
        "parts_art": battle_tent,
        "exact": [(0, 0, 80, 80)],
    },
    {
        # Slateport's sailboat; the copies the pier hides the hull of are
        # found from its top rows (the drawing cuts the hull there)
        "name": "sailboat",
        # its projected shapes stand past its drawing: cut to it
        "clip_art": True,
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (34, 44, 3, 3),
        "ground": [0x170],
        # on the sea, not on the pier round it
        "ground_fixed": True,
        "ground_px": True,
        "parts_art": sailboat,
        "exact": [(0, 0, 48, 48)],
    },
    {
        # the same boat with its hull behind the pier's edge
        "name": "sailboat_cut",
        # its projected shapes stand past its drawing: cut to it
        "clip_art": True,
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (36, 37, 3, 2),
        "ground": [0x170],
        # on the sea, not on the pier round it
        "ground_fixed": True,
        "ground_px": True,
        "parts_art": lambda art: sailboat(art, keel=-6),
        "exact": [(0, 0, 48, 32)],
    },
    {
        # two of them moored one behind the other, drawn 8 and 24 rows lower
        # than the whole one; the seawall's shadow across the top is not theirs
        "name": "sailboat_pair",
        # its projected shapes stand past its drawing: cut to it
        "clip_art": True,
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (33, 35, 3, 4),
        "ground": [0x170],
        # on the sea, not on the pier round it
        "ground_fixed": True,
        "ground_px": True,
        "keep": [(0, 8, 48, 64)],
        # the seawall's shadow on its top row stays as the map paints it
        "patch_cells": [(0, 0), (1, 0), (2, 0)],
        "parts_art": lambda art: sailboat(art, (8, 24)),
        "exact": [(0, 8, 48, 64)],
    },
    {
        "name": "rowboat",
        # its projected shapes stand past its drawing: cut to it
        "clip_art": True,
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (21, 35, 3, 2),
        "ground": [GRASS],
        "ground_px": True,
        "parts_art": rowboat,
        "exact": [(0, 0, 48, 32)],
    },
    {
        # the square's stone lantern; the plot's border round it is not its own
        "name": "slateport_lantern",
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (32, 51, 2, 4),
        "ground": [GRASS, 0x202],
        "ground_px": True,
        "keep": [(4, 8, 28, 15), (0, 15, 32, 64)],
        "parts_art": lantern,
        # true cubes, thousands of them: drawn from its own mesh, not copied
        # into the chunk it stands in
        "static": True,
        # judged against its drawing moved back with it
        "drawing": lantern_drawing,
        "exact": [(0, 0, 32, 64 - LANTERN_BACK)],
        # its outline is a 32-sided polygon, not the drawing's pixel stair
        # (2 wrong, 8 missing, 16 extra), and its cap's far half takes the
        # near half's colours, not the thick outline the GBA draws there (66
        # wrong) - measured 2026-10-10
        "tolerance": 92,
        # under it: the plaza's bricks and the lawn's border in its top row
        # (the drawing's cap stood there), its lawn in the others
        "cell_ground": {(0, 0): 0x34C, (1, 0): 0x34C},
    },
    {
        # Slateport's round bush: a low dome of leaves
        "name": "slateport_bush",
        # its projected shapes stand past its drawing: cut to it
        "clip_art": True,
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (7, 10, 1, 1),
        "ground": [GRASS],
        # on its lawn, whatever paving is round it
        "ground_fixed": True,
        # and its cell as the map paints it round the bush (the grass
        # dots its outline leaves out)
        "patch_own": True,
        "ground_px": True,
        # the bush's own outline: the grass dots drawn round it (its top
        # row, beside its crown and foot) would stand up as part of it
        "keep": [(6, 1, 10, 2), (4, 2, 12, 3), (3, 3, 13, 4), (2, 4, 14, 6), (1, 6, 14, 10),
                 (2, 10, 14, 12), (3, 12, 13, 14), (5, 14, 11, 15)],
        # a smooth dome under its drawing: the height field cut column by
        # column left fins on its flanks and stood in steps
        "parts": lambda: [ProjDome("bush", 7.5, 8.6, 7.6, 7.0, 8.0)],
        "exact": [(0, 0, 16, 16)],
    },
    {
        # the harbour's bollard: a white cap on a short post, its shadow a
        # disc on the paving
        "name": "slateport_bollard",
        # its projected shapes stand past its drawing: cut to it
        "clip_art": True,
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (33, 34, 1, 1),
        "ground": [0x202],
        # on the pier, not on the sea beside it
        "ground_fixed": True,
        "ground_px": True,
        # its plan half as deep as wide, as the drawing's rings are (round,
        # it stood past the drawing's top and was cut flat behind)
        # turned smooth (ProjLathe): a height field cut column by column had
        # left a fin on its flank
        "parts": lambda: [ProjLathe("bollard", 8, 11.5, BOLLARD, squash=0.5, seg=10)],
        "exact": [(0, 0, 16, 16)],
    },
    {
        # Route 109's beach parasols (red, blue and green: found by their own
        # pixels from this one when the drawing matches)
        "name": "parasol",
        "layout": "LAYOUT_ROUTE109",
        "rect": (9, 6, 2, 3),
        "ground": [0x124],
        "ground_px": True,
        "ground_rgb": PARASOL_SHADOW,
        "parts_art": parasol,
        "exact": [(0, 0, 32, 48)],
    },
    {
        "name": "parasol_blue",
        "layout": "LAYOUT_ROUTE109",
        "rect": (18, 4, 2, 3),
        "ground": [0x124],
        "ground_px": True,
        "ground_rgb": PARASOL_SHADOW,
        "parts_art": parasol,
        "exact": [(0, 0, 32, 48)],
    },
    {
        "name": "parasol_green",
        "layout": "LAYOUT_ROUTE109",
        "rect": (13, 14, 2, 3),
        "ground": [0x124],
        "ground_px": True,
        "ground_rgb": PARASOL_SHADOW,
        "parts_art": parasol,
        "exact": [(0, 0, 32, 48)],
    },
    {
        # a sun lounger (art 16x48, v 9-37): a frame on legs, its back
        # raised to the north
        "name": "sun_lounger",
        "layout": "LAYOUT_ROUTE109",
        "rect": (17, 8, 1, 3),
        "ground": [0x124],
        "ground_px": True,
        "parts_art": sun_lounger,
        "exact": [(0, 0, 16, 48)],
    },
    {
        # an air mattress (art 16x48, v 9-36), flat on the sand
        "name": "beach_mat",
        "layout": "LAYOUT_ROUTE109",
        "rect": (9, 9, 1, 3),
        "ground": [0x124],
        "ground_px": True,
        "parts_art": beach_mat,
        "exact": [(0, 0, 16, 48)],
        # the pillow's square top corners (row 9), which the drawing rounds
        "tolerance": 5,
    },
    {
        # the Abandoned Ship; the rock beside its bow is the sea's (a prop)
        "name": "abandoned_ship",
        "layout": "LAYOUT_ROUTE108",
        "rect": (27, 3, 6, 4),
        "ground": [0x19E, 0x194, 0x1BE, 0x1BD, 0x1A3, 0x1A4, 0x170],
        "ground_px": True,
        # the shallows (0x19E) under it, and its bottom row as the map paints
        # it: the shallows' edge on the deep sea
        "patch_cells": [(i, 3) for i in range(6)],
        "keep": [(0, 0, 80, 64), (80, 19, 96, 64)],
        "parts_art": abandoned_ship,
        "exact": [(0, 0, 96, 64)],
    },
    {
        "name": "trick_house",
        "layout": "LAYOUT_ROUTE110",
        "rect": (9, 61, 5, 6),
        "ground": [GRASS, 0x171, 0x179, 0x190],
        # the pond's water under its back; west of its walls, two columns of
        # the lawn drawn in its own cells
        "cell_ground": {(0, 4): GRASS, (0, 5): GRASS},
        "parts": trick_house,
        "exact": TRICK_EXACT,
    },
    {
        # Route 110's cycling road gatehouses: a flat-roofed block, its roof
        # in 8-row courses (rows 10-32) behind a parapet (back rows 0-10,
        # front 32-40), the yellow facade below with its two doors
        "name": "cycling_gate",
        "layout": "LAYOUT_ROUTE110",
        "rect": (14, 12, 6, 5),
        "ground": [GRASS, 0x149],
        # the southern gate's east column, one pixel of the sea beside it
        # (the ground under it is the lawn round it)
        "cell_ground_at": {("LAYOUT_ROUTE110", 15, 84): {(5, 1): 0x170}},
        # drawn over columns 1-94 (95: its shadow)
        "parts": lambda: flat_block(96, 80, ((10, 32), (10, 18), (0, 10)), (32, 40), 40, x0=1, x1=95),
        "exact": [(1, 10, 95, 80)],
    },
    {
        "name": "pink_house_4",
        "layout": "LAYOUT_MAUVILLE_CITY",
        "rect": (18, 11, 4, 4),
        "ground": [GRASS, 0x004],
        "parts": lambda: pink_house(64),
        "exact": pink_house_exact(64),
    },
    {
        # Route 118's house by the sea: the same, plain wall where the door is
        "name": "pink_house_4w",
        "layout": "LAYOUT_ROUTE118",
        "rect": (0, 14, 4, 4),
        "ground": [GRASS, 0x004],
        "parts": lambda: pink_house(64),
        "exact": pink_house_exact(64),
    },
    {
        "name": "pink_house_5",
        "layout": "LAYOUT_MAUVILLE_CITY",
        "rect": (34, 2, 5, 4),
        "ground": [GRASS, 0x004],
        "parts": lambda: pink_house(80),
        "exact": pink_house_exact(80),
    },
    {
        # Rydel's cycles: the same house, its facade with a shop window
        "name": "pink_house_5b",
        "layout": "LAYOUT_MAUVILLE_CITY",
        "rect": (31, 11, 5, 4),
        "ground": [GRASS, 0x004],
        "parts": lambda: pink_house(80, x0=2),
        "exact": pink_house_exact(80),
    },
    {
        # the cycling road's gatehouse four cells wide, one door
        "name": "cycling_gate_4",
        "layout": "LAYOUT_MAUVILLE_CITY",
        "rect": (36, 11, 4, 5),
        "ground": [GRASS, 0x00E],
        "parts": lambda: flat_block(64, 80, ((10, 32), (10, 18), (0, 10)), (32, 40), 40, x0=1, x1=63),
        "exact": [(1, 10, 63, 80)],
    },
    {
        # the flowers in its bottom row are the town's
        "name": "game_corner",
        "layout": "LAYOUT_MAUVILLE_CITY",
        "rect": (5, 10, 7, 5),
        "ground": [GRASS, 0x004],
        "keep": [(0, 0, 112, 64), (40, 64, 48, 72), (64, 64, 72, 72)],
        "parts_art": game_corner,
        # its front corners are drawn rounded off at the top
        "exact": [(8, 3, 104, 9), (0, 9, 112, 64), (40, 64, 48, 72), (64, 64, 72, 72)],
    },
    {
        # Rydel's parked bicycles, two to a three-cell stand, drawn side on:
        # a card standing at their wheels' foot, like a potted plant (the
        # wheels keep their holes)
        "name": "parked_bikes",
        "layout": "LAYOUT_MAUVILLE_CITY",
        "rect": (26, 3, 3, 1),
        "ground": [GRASS, 0x004],
        "ground_px": True,
        "parts_art": lambda art: [Card("bikes", art, 15)],
        "exact": [(0, 0, 48, 16)],
    },
    {
        "name": "verdanturf_house_4",
        "layout": "LAYOUT_VERDANTURF_TOWN",
        "rect": (0, 11, 4, 4),
        "ground": [GRASS, 0x204, 0x205],
        "parts": lambda: verdanturf_house(64),
        "exact": verdanturf_house_exact(64),
    },
    {
        "name": "verdanturf_house_5",
        "layout": "LAYOUT_VERDANTURF_TOWN",
        "rect": (8, 11, 5, 4),
        "ground": [GRASS, 0x204, 0x205],
        "parts": lambda: verdanturf_house(80),
        "exact": verdanturf_house_exact(80),
    },
    {
        "name": "battle_tent_verdanturf",
        # a coarser grid for its round parts (2026-10-10: the console's
        # budgets), the drawing still projected wherever the view sees it
        "lod": (6, 6, 3),
        # the coarse grid misses 6 pixels of its outline at 45 degrees
        "tolerance": 6,
        "layout": "LAYOUT_VERDANTURF_TOWN",
        # its top row is the foot of the cliff behind it, the dome's top
        # drawn over it: the relief's, drawn less the dome (bare_quarters)
        "relief_bare": True,
        "rect": (1, 3, 5, 5),
        "ground": [GRASS],
        "ground_px": True,
        "parts_art": battle_tent_elsewhere,
        "exact": [(0, 0, 80, 80)],
    },
    {
        # the flower box by Verdanturf's west edge (flower_box)
        "name": "verdanturf_flower_box",
        "layout": "LAYOUT_VERDANTURF_TOWN",
        "rect": (0, 15, 1, 2),
        "ground": [GRASS, 0x204, 0x205],
        "ground_px": True,
        "parts_art": flower_box,
        "exact": [(0, 0, 16, 32)],
    },
    {
        "name": "fallarbor_house_4",
        "layout": "LAYOUT_FALLARBOR_TOWN",
        "rect": (0, 3, 4, 4),
        "ground": [0x279, 0x269],
        "parts": lambda: fallarbor_house(64),
        "exact": fallarbor_house_exact(64),
    },
    {
        "name": "fallarbor_cozmo_house",
        "layout": "LAYOUT_FALLARBOR_TOWN",
        "rect": (5, 13, 4, 5),
        "ground": [0x279],
        "parts_art": cozmo_house,
        # the real depth raises the roof behind the antenna (behind: True)
        "exact": [(3, 55, 62, 78), (10, 29, 54, 55), (32, 2, 57, 18, True)],
    },
    {
        # a bather buried in Lavaridge's hot sand: a mound
        "name": "sand_bath",
        "layout": "LAYOUT_LAVARIDGE_TOWN",
        "rect": (3, 8, 1, 2),
        "ground": [0x2A8],
        "ground_px": True,
        "parts_art": small_round("sand_bath", Volume().dome(8, 20, 7.5, 7, 7), depth=32.0),
        "exact": [(0, 0, 16, 32)],
    },
    {
        # Route 113's glass workshop: Fallarbor's house with a roof of glass
        # (the trees' crowns overlap its top row)
        "name": "glass_workshop",
        "layout": "LAYOUT_ROUTE113",
        "rect": (32, 2, 4, 4),
        "ground": [0x218, 0x26D, 0x26E, 0x20C, 0x20D],
        "parts": lambda: fallarbor_house(64),
        "exact": fallarbor_house_exact(64),
    },
    {
        # Route 114's Fossil Maniac's house: Fallarbor's, its ridge drawn
        # against the brick cliff behind it
        "name": "fossil_maniac_house",
        "layout": "LAYOUT_ROUTE114",
        "rect": (28, 2, 4, 4),
        "ground": [0x279, 0x269],
        "parts": lambda: fallarbor_house(64),
        "exact": fallarbor_house_exact(64),
    },
    {
        # Route 111's Mirage Tower (art 48 x 96): three round stone tiers,
        # each set back on the one below - the base (r 24, rows 72-96), the
        # middle (r 22, rows 26-70) and the top with its rounded crown (r 18,
        # rows 0-44) - its door in the base
        "name": "mirage_tower",
        "layout": "LAYOUT_ROUTE111",
        "rect": (18, 53, 3, 6),
        "ground": [0x251, 0x259, 0x121, 0x25A],
        "parts_art": flat(lambda: [PlanPrism("base", rounded_plan(24, 80, 25.0, 16.6, p=3.5, n=40), 0, 24, (2, 8, 96)),
                          PlanPrism("mid", rounded_plan(24, 80, 23.6, 14.4, p=2.0, n=40), 24, 47, (2, 8, 72)),
                          PlanPrism("top", rounded_plan(24, 80, 18.4, 11.0, p=2.0, n=40), 47, 72, (8, 14, 49),
                                    shrink=1.0),
                          ] + [PlanPrism("crown", rounded_plan(24, 80, 18.4 * k, 11.0 * k, p=2.0, n=40), y, y + 2,
                                         (8, 14, 49))
                               for y, k in zip(range(72, 86, 2), (0.97, 0.91, 0.84, 0.76, 0.68, 0.61, 0.55))] + [
                          PlanPyramid("tip", rounded_plan(24, 80, 10.1, 6.05, p=2.0, n=24), 86, (24, 92, 80))]),
        "exact": [(0, 0, 48, 96)],
    },
    {
        # Route 112's cable car station (80 x 80): a block 34 high, its flat
        # roof drawn rows 0-46 (z 34-80), its east end a slope falling 24 to
        # the east - the drawing's diagonal edges at the top right and along
        # the facade's right end
        "name": "cable_car_station",
        "layout": "LAYOUT_ROUTE112",
        "rect": (27, 23, 5, 5),
        "ground": [0x271, 0x210, 0x218, 0x21A],
        "parts_art": flat(lambda: [PlatePrism("station", [(0, 0), (0, 34), (56, 34), (80.5, 9.5), (80.5, 0)], 34, 80, (2, 10, 80))]),
        "exact": [(0, 0, 80, 80)],
    },
    {
        # Mt Chimney's cable car station: Route 112's drawn the other way
        # round (96 x 80), its slope falling 31 to the west
        "name": "cable_car_station_top",
        "layout": "LAYOUT_MT_CHIMNEY",
        "rect": (14, 32, 6, 5),
        "ground": [0x271],
        "parts_art": flat(lambda: [PlatePrism("station", [(96, 0), (96, 34), (40, 34), (8.5, 2.5), (8.5, 0)], 34, 80, (86, 94, 80))]),
        "exact": [(0, 0, 96, 80)],
    },
    {
        # the Winstrates' house on Route 111: the flat-roofed block five wide
        "name": "cycling_gate_5",
        "layout": "LAYOUT_ROUTE111",
        "rect": (11, 109, 5, 5),
        "ground": [GRASS, 0x149],
        "parts": lambda: flat_block(80, 80, ((10, 32), (10, 18), (0, 10)), (32, 40), 40, x0=1, x1=79),
        "exact": [(1, 10, 79, 80)],
    },
    {
        # Route 111's Trainer Hill entrance, on the plateau (80 x 96; below
        # row 82 the cliff's own drawing): a block 36 high, its flat roof
        # drawn rows 0-46 with a hexagonal lantern in its middle
        "name": "trainer_hill_entrance",
        "layout": "LAYOUT_ROUTE111",
        "rect": (29, 106, 5, 6),
        "ground": [0x070, 0x073, 0x071, 0x07B, 0x075, 0x089],
        "keep": [(0, 0, 80, 82)],
        "parts_art": flat(lambda: [PlanPrism("hill", [(0, 36), (80, 36), (80, 82), (0, 82)], 0, 36, (2, 10, 82)),
                          PlanPyramid("lantern", [(57, 56), (48.5, 66), (31.5, 66), (23, 56), (31.5, 46),
                                                  (48.5, 46)], 36, (40, 48, 56))]),
        "exact": [(0, 0, 80, 82)],
    },
    {
        "name": "lilycove_house_4",
        "layout": "LAYOUT_LILYCOVE_CITY",
        "rect": (35, 3, 4, 4),
        "ground": [GRASS, 0x1D4, 0x1D5, 0x1DC, 0x1DD, 0x1E4, 0x1E5, 0x1E6, 0x1E7],
        # the houses below the cliff paint the cliff's foot into the roof's
        # top rows, the fenced ones the fence's ends into the eave's: the
        # facade and the eave say which house it is
        "match_rows": (2, 4),
        "parts": lambda: lilycove_house(64),
        "exact": lilycove_house_exact(64),
    },
    {
        "name": "lilycove_house_5",
        "layout": "LAYOUT_LILYCOVE_CITY",
        "rect": (37, 11, 5, 4),
        "ground": [GRASS, 0x004],
        "match_rows": (2, 4),
        "parts": lambda: lilycove_house(80),
        "exact": lilycove_house_exact(80),
    },
    {
        # Lilycove's department store (144 x 112): drawn as its tall front
        # alone, the roof above the map's edge - a block 112 high and 64
        # deep, its flat roof standing above the map's top (open_top)
        "name": "department_store",
        "layout": "LAYOUT_LILYCOVE_CITY",
        "rect": (23, 0, 9, 7),
        "ground": [GRASS, 0x233, 0x22B],
        "parts_art": flat(lambda: [PlanPrism("store", [(0, 48), (144, 48), (144, 112), (0, 112)], 0, 112, (2, 10, 112),
                                    top=Tile(4, 4, 20, 20))]),
        "exact": [(0, 0, 144, 112)],
    },
    {
        # Lilycove's museum (160 x 96): a long block 44 high, its flat roof
        # drawn rows 16-52, and its middle (x 40-120) a storey higher, 68,
        # its windowed front standing on the lower block's front edge
        "name": "lilycove_museum",
        "layout": "LAYOUT_LILYCOVE_CITY",
        "rect": (7, 1, 10, 6),
        "ground": [GRASS, 0x002, 0x208, 0x207, 0x004],
        "parts_art": flat(lambda: [PlanPrism("museum", [(0, 52), (160, 52), (160, 96), (0, 96)], 0, 44, (2, 10, 96)),
                          PlanPrism("museum_hall", [(40, 60), (120, 60), (120, 94), (40, 94)], 0, 68, (42, 50, 94))]),
        "exact": [(0, 0, 160, 96)],
    },
    {
        # Lilycove's contest hall (112 x 112): a block 30 high on a plan with
        # its four corners cut off 8, its studded flat roof drawn rows 8-82
        "name": "lilycove_contest_hall",
        "layout": "LAYOUT_LILYCOVE_CITY",
        "rect": (20, 18, 7, 7),
        "ground": [GRASS, 0x233, 0x22B, 0x23B],
        "ground_px": True,
        "parts_art": flat(lambda: [PlanPrism("hall", [(8, 38), (104, 38), (112, 46), (112, 104), (104, 112), (8, 112),
                                                (0, 104), (0, 46)], 0, 30, (16, 40, 112))]),
        "exact": [(0, 0, 112, 112)],
    },
    {
        # Lilycove's harbour (112 x 96): a white block 34 high under a
        # corrugated roof of low pitch - its red eave rows 54-62 standing 2
        # out, the slope drawn rows 1-54 up to the ridge over z 56 (the back
        # half hidden behind it)
        "name": "lilycove_harbor",
        "layout": "LAYOUT_LILYCOVE_CITY",
        "rect": (9, 27, 7, 6),
        "ground": [GRASS, 0x233, 0x22B, 0x23B],
        "parts_art": flat(lambda: [SectionPrism("harbor", [(96, 0), (96, 34), (98, 36), (98, 44), (56, 55), (14, 44),
                                                (14, 36), (16, 34), (16, 0)], 0, 112, (2, 10, 96))]),
        "exact": [(0, 0, 112, 96)],
    },
    {
        "name": "safari_gate",
        "layout": "LAYOUT_ROUTE121",
        "rect": (35, 0, 5, 6),
        "ground": [GRASS, 0x004],
        "parts": safari_gate,
        "exact": [(0, 0, 80, 96)],
    },
    {
        "name": "safari_south_wing",
        "layout": "LAYOUT_SAFARI_ZONE_SOUTH",
        "rect": (30, 29, 10, 5),
        "ground": [GRASS, 0x004],
        "parts": safari_south,
        "exact": [(0, 7, 160, 80)],
    },
    {
        # its first column only down to the long wing's foot: the trees
        # below are the wood's
        "name": "safari_southeast_wing",
        "layout": "LAYOUT_SAFARI_ZONE_SOUTHEAST",
        "rect": (0, 29, 6, 11),
        "owned": {(0, j) for j in range(5)} | {(i, j) for i in range(1, 6) for j in range(11)},
        "ground": [GRASS, 0x004],
        "parts": safari_southeast,
        "exact": [(0, 7, 16, 80), (16, 7, 96, 176)],
    },
    {
        "name": "lilycove_motel",
        "layout": "LAYOUT_LILYCOVE_CITY",
        "rect": (36, 20, 6, 5),
        "ground": [GRASS, 0x004, 0x233],
        "parts": lilycove_motel,
        "exact": [(3, 56, 93, 80), (22, 50, 74, 56), (22, 40, 72, 50), (26, 16, 58, 40)],
    },
    {
        # Mossdeep's red-roofed house (64 x 64): a block 22 high (its facade
        # rows 40-62) under a hipped roof drawn rows 0-37
        "name": "mossdeep_house",
        "layout": "LAYOUT_MOSSDEEP_CITY",
        "rect": (18, 7, 4, 4),
        "ground": [GRASS, 0x004, 0x002],
        "parts": lambda: mossdeep_house_parts(64),
        "exact": [(4, 38, 60, 62), (12, 8, 52, 33)],
    },
    {
        # the same house without the reference's chimney, its roof's top
        # corners drawn over the sea, a cliff or grass (rows 1-3 must match)
        "name": "mossdeep_house_b",
        "layout": "LAYOUT_MOSSDEEP_CITY",
        "rect": (27, 6, 4, 4),
        "ground": [GRASS, 0x004, 0x002],
        "match_rows": (1, 4),
        "parts": lambda: mossdeep_house_parts(64),
        "exact": [(4, 38, 60, 62), (12, 8, 52, 33)],
    },
    {
        # Mossdeep's wide house (80 x 80), built as Oldale's: a hipped roof
        # railed in white (ridge rows 8-20, slope 20-48, fascia 48-53) over
        # the facade down to row 79, its sides the vine-grown stone
        "name": "mossdeep_house_wide",
        "layout": "LAYOUT_MOSSDEEP_CITY",
        "rect": (35, 20, 5, 5),
        "ground": [GRASS, 0x004, 0x120, 0x121, 0x129],
        "ground_px": True,
        "parts": lambda: pitched_house(80, (8, 14), (14, 21), (21, 48), (22, 38), (48, 53), 79,
                                       (22, 38), (22, 38), (1, 7)),
        "exact": [(4, 53, 76, 78), (16, 21, 64, 48)],
    },
    {
        # Pacifidlog's round hut on its raft (48 x 64): a round wall 22 high
        # (rows 41-63), a conical thatch over it (its eave r 25) and the
        # finial on its top (rows 0-14); round things drawn as the GBA draws
        # them, their plan about half as deep as wide. The raft's planks
        # round it are the ground
        "name": "pacifidlog_hut",
        # a coarser grid for its round parts (2026-10-10: the console's
        # budgets), the drawing still projected wherever the view sees it
        "lod": (4, 4, 2),
        "layout": "LAYOUT_PACIFIDLOG_TOWN",
        "rect": (1, 9, 3, 4),
        "ground": [0x221, 0x170],
        "ground_px": True,
        "keep": [(0, 0, 48, 35), (2, 35, 46, 37), (3, 37, 45, 64)],
        "parts_art": pacifidlog_hut,
        # every cell: the raft's planks and the sea round the hut as the
        # map paints them, whatever ground the placement takes
        "patch_cells": [(i, j) for j in range(4) for i in range(3)],
        "exact": [(0, 0, 48, 64)],
    },
    {
        # Sootopolis's spire house (48 x 80): an octagonal white base 22 high
        # (its front rows 56-78, its flat top rows 18-56: z 40-78, the
        # corners cut 8) under an eight-sided pyramid of dark stone rising
        # 29 from it (its apex row 8)
        "name": "sootopolis_spire_house",
        "layout": "LAYOUT_SOOTOPOLIS_CITY",
        "relief_bare": True,
        "rect": (22, 1, 3, 5),
        "ground": [0x2D9, 0x2E1, 0x2E2, 0x2D3, 0x2DA],
        "ground_px": True,
        "parts_art": sootopolis_spire,
        "patch_cells": [(i, j) for j in range(5) for i in range(3)],
        "exact": [(0, 0, 48, 80)],
    },
    {
        # Sootopolis's flat-roofed white house (48 x 64): a block 30 high
        # under a flat roof with its skylight
        "name": "sootopolis_house",
        "layout": "LAYOUT_SOOTOPOLIS_CITY",
        "relief_bare": True,
        "rect": (2, 6, 3, 4),
        "ground": [0x2D9, 0x2E1, 0x2E2, 0x2D3, 0x2DA],
        "ground_px": True,
        "parts_art": sootopolis_flat_house,
        "patch_cells": [(i, j) for j in range(4) for i in range(3)],
        "exact": [(0, 0, 48, 64)],
    },
    {
        # the same with a window where the other has its door
        "name": "sootopolis_house_window",
        "layout": "LAYOUT_SOOTOPOLIS_CITY",
        "relief_bare": True,
        "rect": (30, 1, 3, 4),
        "ground": [0x2D9, 0x2E1, 0x2E2, 0x2D3, 0x2DA],
        "ground_px": True,
        "parts_art": sootopolis_flat_house,
        "patch_cells": [(i, j) for j in range(4) for i in range(3)],
        "exact": [(0, 0, 48, 64)],
    },
    {
        # Sootopolis's flat-roofed house with a gate (48 x 64): its porch
        # stands out of the front in two steps (sootopolis_flat_house)
        "name": "sootopolis_gate_house",
        "layout": "LAYOUT_SOOTOPOLIS_CITY",
        "relief_bare": True,
        "rect": (47, 22, 3, 4),
        "ground": [0x2D9, 0x2E1, 0x2E2, 0x2D3, 0x2DA],
        "ground_px": True,
        "parts_art": sootopolis_gate_house,
        "patch_cells": [(i, j) for j in range(4) for i in range(3)],
        "exact": [(0, 0, 48, 64)],
    },
    {
        # the spire house with its door (the diamond) for a window
        "name": "sootopolis_spire_house_door",
        "layout": "LAYOUT_SOOTOPOLIS_CITY",
        "relief_bare": True,
        "rect": (53, 15, 3, 5),
        "ground": [0x2D9, 0x2E1, 0x2E2, 0x2D3, 0x2DA],
        "ground_px": True,
        "parts_art": sootopolis_spire,
        "patch_cells": [(i, j) for j in range(5) for i in range(3)],
        "exact": [(0, 0, 48, 80)],
    },
    {
        # the spire house whose gate stands back between two dark pillars
        "name": "sootopolis_spire_house_gate",
        "layout": "LAYOUT_SOOTOPOLIS_CITY",
        "relief_bare": True,
        "rect": (43, 13, 3, 5),
        "ground": [0x2D9, 0x2E1, 0x2E2, 0x2D3, 0x2DA],
        "ground_px": True,
        "parts_art": sootopolis_spire_gate,
        "patch_cells": [(i, j) for j in range(5) for i in range(3)],
        "exact": [(0, 0, 48, 80)],
    },
    {
        # the stone posts before the Cave of Origin (sootopolis_posts), a
        # pair of cells drawn alike either side of the path to it
        "name": "sootopolis_posts",
        "layout": "LAYOUT_SOOTOPOLIS_CITY",
        "rect": (30, 19, 1, 2),
        "ground": CAVE_ORIGIN_GROUND,
        "ground_px": True,
        "parts_art": sootopolis_posts,
        "exact": [(0, 0, 16, 32)],
    },
] + [
    {
        # the Cave of Origin's doorway (cave_origin), in the city and in the
        # city of the legends' battle: it owns the plaza's cells only, the
        # cliff's stay the relief's
        "name": "cave_origin" + suffix,
        "layout": layout,
        "rect": CAVE_ORIGIN[1:],
        "owned": {(i, j) for j in (2, 3) for i in range(5)},
        "ground": CAVE_ORIGIN_GROUND,
        "ground_px": True,
        "parts_art": cave_origin,
        "patch_cells": [(i, j) for j in (2, 3) for i in range(5)],
        "exact": [(0, 0, 80, 64)],
    } for (layout, suffix) in (("LAYOUT_SOOTOPOLIS_CITY", ""),
                               ("LAYOUT_SOOTOPOLIS_CITY_LEGENDS_BATTLE", "_legends"))
] + [
    {
        # Ever Grande's Pokemon League: see pokemon_league()
        "name": "pokemon_league",
        # a coarser grid for its round parts (2026-10-10: the console's
        # budgets), the drawing still projected wherever the view sees it
        "lod": (6, 6, 3),
        "layout": "LAYOUT_EVER_GRANDE_CITY",
        "rect": (13, 0, 11, 8),
        "ground": [0x001, 0x222, 0x221, 0x270, 0x271, 0x272],
        "ground_px": True,
        "parts_art": pokemon_league,
        "exact": [(0, 0, 176, 128)],
    },
    {
        # Ever Grande's round bushes along the paths (the League's own four
        # are in its model): a low dome of leaves, on grass and on shade
        "name": "ever_grande_bush",
        # a coarser grid for its round parts (2026-10-10: the console's
        # budgets), the drawing still projected wherever the view sees it
        "lod": (6, 6, 3),
        "layout": "LAYOUT_EVER_GRANDE_CITY",
        "rect": (14, 8, 1, 1),
        "ground": [0x001, 0x222, 0x221],
        "ground_px": True,
        "owned": {(0, 0)},
        "repeat_at": [(14, 8), (15, 8), (14, 9), (15, 9), (14, 10), (15, 10), (21, 10), (22, 10),
                      (23, 24), (29, 24), (23, 25), (24, 25), (28, 25), (29, 25)],
        "parts_art": small_round("bush", Volume().dome(8, 9, 8, 6.5, 8.5)),
        "exact": [(0, 0, 16, 16)],
    },
    {
        "name": "ever_grande_bush_shade",
        # a coarser grid for its round parts (2026-10-10: the console's
        # budgets), the drawing still projected wherever the view sees it
        "lod": (6, 6, 3),
        "layout": "LAYOUT_EVER_GRANDE_CITY",
        "rect": (21, 8, 1, 1),
        "ground": [0x221, 0x222, 0x001],
        "ground_px": True,
        "owned": {(0, 0)},
        "repeat_at": [(21, 8), (22, 8), (21, 9), (22, 9)],
        "parts_art": small_round("bush", Volume().dome(8, 9, 8, 6.5, 8.5)),
        "exact": [(0, 0, 16, 16)],
    },
    {
        # the Battle Frontier's Battle Dome: see battle_dome()
        "name": "battle_dome",
        # a coarser grid for its round parts (2026-10-10: the console's
        # budgets), the drawing still projected wherever the view sees it
        "lod": (8, 8, 3),
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_WEST",
        "rect": (13, 4, 13, 14),
        "ground": [0x001, 0x268, 0x269, 0x26A, 0x26B, 0x270, 0x271, 0x272, 0x273, 0x003],
        "ground_px": True,
        "keep": DOME_KEEP,
        "parts_art": battle_dome,
        # the shell shows through the gaps of the glass base, as the dome behind it
        "exact": [(0, 0, 208, 160), (0, 160, 208, 224, True)],
    },
    {
        # the Battle Frontier's Battle Factory: see battle_factory()
        "name": "battle_factory",
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_WEST",
        "rect": (4, 30, 15, 9),
        "ground": [0x189, 0x170, 0x172, 0x19A, 0x198, 0x180, 0x181, 0x05E],
        "ground_px": True,
        # its platform, which its front row shows, not the sea of its back
        # corners (more of its cells, but behind it)
        "ground_fixed": {"LAYOUT_BATTLE_FRONTIER_OUTSIDE_WEST": 0x05E},
        "keep": [(0, 4, 232, 144), (88, 0, 152, 4), (232, 44, 240, 144)],
        "parts_art": flat(battle_factory),
        "exact": [(0, 0, 240, 144)],
    },
    {
        # the Battle Frontier's small houses (48 x 64): a rounded block 36
        # high (its front z 62, its flat orange roof rows 0-26, the red band
        # and the walls its front), its plan a superellipse
        "name": "frontier_house",
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_WEST",
        "rect": (43, 2, 3, 4),
        "ground": [0x001, 0x170, 0x005, 0x0E0, 0x002, 0x007, 0x101],
        "ground_px": True,
        # drawn alike in the East's own tiles (its houses by the Tower, the
        # Pyramid and the walk to the Palace)
        "anywhere": True,
        "parts_art": flat(lambda: [PlanPrism("house", rounded_plan(24, 49.5, 24.6, 14.2, p=10.0), 0, 36, (10, 18, 63))]),
        "exact": [(0, 0, 48, 64)],
    },
    {
        # the glass-fronted one (rows 1-3 must match: a cliff's foot shows
        # above its roof at 4,17)
        "name": "frontier_house_glass",
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_WEST",
        "rect": (52, 41, 3, 4),
        "ground": [0x001, 0x170, 0x005, 0x0E0, 0x002, 0x007, 0x101],
        "ground_px": True,
        # drawn alike in the East's own tiles (its houses by the Tower, the
        # Pyramid and the walk to the Palace)
        "anywhere": True,
        "match_rows": (1, 4),
        "parts_art": flat(lambda: [PlanPrism("house", rounded_plan(24, 49.5, 24.6, 14.2, p=10.0), 0, 36, (20, 28, 63))]),
        "exact": [(0, 0, 48, 64)],
    },
    {
        "name": "frontier_house_long",
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_WEST",
        "rect": (44, 41, 6, 4),
        "ground": [0x001, 0x170, 0x005, 0x0E0, 0x002, 0x007, 0x101],
        "ground_px": True,
        # drawn alike in the East's own tiles (its houses by the Tower, the
        # Pyramid and the walk to the Palace)
        "anywhere": True,
        "parts_art": flat(lambda: [PlanPrism("house", rounded_plan(48, 49.5, 48.6, 14.2, p=14.0, n=48), 0, 36,
                                    (10, 18, 63))]),
        "exact": [(0, 0, 96, 64)],
    },
    {
        # the Battle Frontier's halls (frontier_hall): the one by the Tower's
        # stairs, its door in the middle
        "name": "frontier_hall",
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_EAST",
        "rect": (7, 24, 7, 5),
        "ground": [0x001, 0x079, 0x0C7, 0x016, 0x3B6],
        "ground_px": True,
        # the cliff behind it, either side of its crown, is the relief's
        "owned": {(i, j) for j in range(1, 5) for i in range(7)} | {(2, 0), (3, 0), (4, 0)},
        "keep": HALL_KEEP,
        "parts_art": frontier_hall,
        "exact": [(0, 0, 112, 80)],
    },
    {
        # the one below the Pyramid's cliff, a porch on two posts before its
        # door
        "name": "frontier_hall_porch",
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_EAST",
        "rect": (32, 8, 7, 5),
        "ground": [0x0C6, 0x0A9, 0x00F, 0x001],
        "ground_px": True,
        "owned": {(i, j) for j in range(1, 5) for i in range(7)} | {(2, 0), (3, 0), (4, 0)},
        "keep": HALL_KEEP,
        "parts_art": lambda art: frontier_hall(art, vault=HALL_VAULT_PORCH),
        "exact": [(0, 0, 112, 80)],
    },
    {
        # the gate the boat brings you to, the walk through its middle
        "name": "frontier_hall_gate",
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_WEST",
        "rect": (23, 61, 7, 5),
        "ground": [0x05E, 0x05D, 0x055, 0x170],
        "ground_px": True,
        "owned": {(i, j) for j in range(1, 5) for i in range(7)} | {(2, 0), (3, 0), (4, 0)},
        "keep": HALL_KEEP[:1] + [(0, 16, 112, 56), (0, 56, 40, 80), (72, 56, 112, 80)],
        "parts_art": lambda art: frontier_hall(art, gate=True),
        "exact": [(0, 0, 112, 80)],
    },
    {
        # the Battle Frontier's flowerbeds on the plazas: a bed raised in an
        # orange stone rim, its front 6 rows (the red course under the rim),
        # the flowers its top
        "name": "frontier_planter_west",
        "components": {
            "secondary": "gTileset_BattleFrontierOutsideWest",
            "tiles": {0x208, 0x209, 0x20A, 0x210, 0x211, 0x212, 0x218, 0x219, 0x21A},
            "height": 6, "block": 4,
        },
        "ground": [0x05E, 0x05D, 0x05F],
    },
    {
        "name": "frontier_planter_east",
        "components": {
            "secondary": "gTileset_BattleFrontierOutsideEast",
            "tiles": {0x2C8, 0x2CB, 0x2F7, 0x2D0, 0x2CC, 0x2FF, 0x2C9, 0x2CD, 0x307},
            "height": 6, "block": 4,
        },
        "ground": [0x05E, 0x101, 0x109],
    },
    {
        # the arches over the walk from the gate to the plaza (three, drawn
        # alike): see frontier_arch
        "name": "frontier_arch",
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_WEST",
        "rect": (24, 46, 5, 5),
        "ground": [0x05E, 0x05D, 0x05F],
        "ground_px": True,
        "parts_art": lambda art: flat(lambda: frontier_arch(art))(art),
        # the arch's own few pixels under its band (its underside's shadow)
        # are stood behind by its front
        "exact": [(0, 0, 80, 80, True)],
    },
    {
        # the Battle Frontier's Battle Pike: see battle_pike()
        "name": "battle_pike",
        # a coarser grid for its round parts (2026-10-10: the console's
        # budgets), the drawing still projected wherever the view sees it
        "lod": (8, 8, 3),
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_WEST",
        "rect": (40, 17, 5, 13),
        "ground": [0x001, 0x204, 0x017, 0x004, 0x003],
        "ground_px": True,
        "keep": [(16, 0, 80, 208), (0, 36, 16, 208)],
        "parts_art": battle_pike,
        "exact": [(0, 0, 80, 208)],
    },
    {
        # the Battle Tower (272 x 256), its glass shaft cut by the map's top
        # edge: two wings of brick and glass (x 3-46, 226-270) standing to
        # z 248, the middle set back to z 232 above the lobby, a block 36
        # high before it whose dark roof is drawn rows 196-212; the roofs
        # above the map laid with the brickwork
        "name": "battle_tower",
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_EAST",
        "rect": (8, 0, 17, 16),
        "ground": [0x001, 0x201, 0x203, 0x202],
        "ground_px": True,
        # the doormat before the doors is the plaza's paint
        "keep": [(0, 0, 272, 248), (0, 248, 100, 256), (172, 248, 272, 256)],
        "patch_cells": [(i, 15) for i in range(6, 11)],
        "parts_art": flat(lambda: [
            PlanPrism("wing", [(0, 136), (46, 136), (46, 248), (0, 248)], 0, 320, (20, 40, 248),
                      top=Tile(20, 0, 40, 16)),
            PlanPrism("wing", [(226, 136), (272, 136), (272, 248), (226, 248)], 0, 320, (232, 252, 248),
                      top=Tile(20, 0, 40, 16)),
            PlanPrism("shaft", [(46, 136), (226, 136), (226, 232), (46, 232)], 0, 320, (60, 76, 196),
                      top=Tile(20, 0, 40, 16)),
            PlanPrism("lobby", [(46, 232), (226, 232), (226, 248), (46, 248)], 0, 36, (60, 76, 248))]),
        "exact": [(0, 0, 272, 256)],
    },
    {
        # the Battle Frontier's Battle Pyramid: see battle_pyramid()
        "name": "battle_pyramid",
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_EAST",
        "rect": (51, 5, 15, 11),
        "ground": [0x121, 0x129, 0x120, 0x128, 0x001, 0x003],
        "ground_px": True,
        "keep": PYRAMID_KEEP,
        "parts_art": sand_specks(flat(battle_pyramid)),
        "exact": [(0, 0, 240, 176)],
    },
    {
        # the Battle Arena's hall (144 x 112), built as Oldale's: a hipped
        # roof of dark boards (ridge rows 0-4, the slope 8-64 with its seams
        # every 8 columns, the eave 64-69) reaching 8 over the deck either
        # side; the white walls with their posts down to row 112, 8 in
        "name": "battle_arena",
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_EAST",
        "rect": (35, 23, 9, 7),
        "ground": [0x286, 0x285, 0x170, 0x189],
        "ground_px": True,
        "parts": lambda: pitched_house(144, (0, 4), (4, 8), (8, 64), (8, 16), (64, 69), 112,
                                       (18, 26), (18, 26), (16, 32), inset=8, overhang=2, back_z=32),
        "exact": [(17, 69, 127, 112), (17, 8, 127, 64)],
    },
    {
        # the Battle Arena's gate (112 x 64): two blocks of pillars and white
        # walls (x 0-32, 80-112) 36 high, and between them the lintel over
        # the path, 24 up; the two-tier roof drawn rows 0-28 their tops
        "name": "battle_arena_gate",
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_EAST",
        "rect": (36, 36, 7, 4),
        "ground": [0x0C6, 0x001, 0x003, 0x00E, 0x1D9, 0x1D8, 0x1DA],
        "ground_px": True,
        "parts_art": flat(lambda: [
            PlanPrism("gate", [(0, 36), (32, 36), (32, 64), (0, 64)], 0, 36, (10, 20, 64)),
            PlanPrism("gate", [(80, 36), (112, 36), (112, 64), (80, 64)], 0, 36, (90, 100, 64)),
            PlanPrism("lintel", [(32, 36), (80, 36), (80, 64), (32, 64)], 24, 36, (40, 48, 64))]),
        "exact": [(0, 0, 112, 64)],
    },
    {
        # the Battle Palace's side pavilions (48 x 96, the flowerbeds above
        # and below them the ground's paint), built as Oldale's: a golden
        # hipped roof (ridge rows 8-16, the slope 16-46 in courses of 8,
        # the eave 46-48) over red walls with their columns down to row 80
        "name": "battle_palace_wing",
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_EAST",
        "rect": (40, 50, 3, 6),
        "ground": [0x004, 0x017, 0x00E, 0x0C6, 0x003],
        "ground_px": True,
        "keep": [(0, 8, 48, 80)],
        "patch_cells": [(i, j) for j in (0, 5) for i in range(3)],
        "parts": lambda: pitched_house(48, (8, 16), (16, 20), (20, 46), (23, 31), (46, 48), 80,
                                       (16, 24), (16, 24), (2, 8), inset=2, overhang=2, back_z=40),
        "exact": [(4, 48, 36, 80), (16, 20, 24, 46)],
    },
    {
        # the Battle Palace's middle pavilion (80 x 128): the hall (walls 18
        # high to z 104) under its golden hipped roof - flat faces, the
        # drawing projected: eave z 34-106 at 20, ridge x 23-57 over z 71 at
        # 47 (the back eave drawn row 16, the ridge row 24) - and before it
        # the porch (x 18-62, 42 high to z 128) under its own (eave z 90-130
        # at 44, ridge x 31-49 at 55), the two stone plinths either side
        "name": "battle_palace",
        "layout": "LAYOUT_BATTLE_FRONTIER_OUTSIDE_EAST",
        "rect": (43, 49, 5, 8),
        "ground": [0x004, 0x017, 0x00E, 0x0C6, 0x003],
        "ground_px": True,
        "keep": [(0, 16, 80, 96), (8, 96, 72, 128)],
        "patch_cells": [(i, j) for i in range(5) for j in (0, 6, 7)],
        "parts_art": flat(lambda: [
            PlanPrism("hall", [(0, 40), (80, 40), (80, 104), (0, 104)], 0, 18, (72, 78, 104)),
            PlanPrism("eave", [(-2, 34), (82, 34), (82, 106), (-2, 106)], 18, 20, (72, 78, 106)),
            PlanHip("hall_roof", -2, 82, 34, 106, 20, 23, 57, 71, 47),
            PlanPrism("porch", [(18, 88), (62, 88), (62, 128), (18, 128)], 0, 42, (20, 26, 128)),
            PlanPrism("plinth", [(8, 116), (18, 116), (18, 128), (8, 128)], 0, 32, (10, 16, 128)),
            PlanPrism("plinth", [(62, 116), (72, 116), (72, 128), (62, 128)], 0, 32, (64, 70, 128)),
            PlanPrism("porch_eave", [(14, 90), (66, 90), (66, 130), (14, 130)], 42, 44, (20, 26, 130)),
            PlanHip("porch_roof", 14, 66, 90, 130, 44, 31, 49, 111, 55)]),
        "exact": [(0, 16, 80, 128)],
    },
] + [
    # The Battle Frontier's flags, on every wall they stand on (voxel_props
    # finds them by their tiles): a pole and its cloth, flat - a card
    # standing at the pole's foot, facing the view - the cloth waving as the
    # game animates its tiles (their texels live). The wall or the cliff
    # under them is the map's own.
    {"name": name, "props": name, "ground": [0x170], "shape": "flag", "static": True,
     "live_tiles": (730, 736)}
    for name in ("frontier_flag_east", "frontier_flag_west")
] + [
    {
        # Mossdeep's Space Center (144 x 224): see space_center(); the sea
        # behind the rocket stays the map's paint
        "name": "space_center",
        # a coarser grid for its round parts (2026-10-10: the console's
        # budgets), the drawing still projected wherever the view sees it
        "lod": (4, 4, 2),
        "layout": "LAYOUT_MOSSDEEP_CITY",
        "rect": (60, 2, 9, 14),
        "ground": [0x170, GRASS, 0x002, 0x004],
        "ground_px": True,
        "parts_art": space_center,
        # the sea and the shore behind the rocket, as the map paints them
        "patch_cells": [(i, j) for j in range(6) for i in range(9)],
        "exact": [(0, 0, 144, 224)],
    },
    {
        "name": "shipyard",
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (24, 32, 8, 7),
        "ground": [0x202, GRASS],
        # its top row is the sea behind it, not its roof's
        "keep": [(0, 1, 128, 112)],
        "parts": shipyard,
        "exact": SHIPYARD_EXACT,
    },
    {
        # Slateport market's stalls, six of them, half a cell off the grid
        # (the east row's metatiles differ, its drawing does not). Lifted 12
        # like the awnings. Only the stall is its own: the pots and baskets
        # standing under it are not.
        "name": "slateport_stall",
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (2, 41, 5, 3),
        # the paving and its shade under the stall, told apart pixel by
        # pixel from the posts drawn over it
        "ground": [0x211, 0x285],
        "ground_px": True,
        "keep": stall_keep(72),
        # its outer column as the map paints it (the paving's edge): under
        # the roof the goods stand on their own, not laid flat again
        "patch_cells": [(0, 0), (0, 1), (0, 2)],
        "parts": lambda: [stall(72, 57.5, ((24, 3, 55, 8), (9, 12, 12, 20), (60, 12, 69, 20)))],
        "exact": [(8, 11, 72, 29)],
    },
    {
        # the east row's stalls are a cell narrower
        "name": "slateport_stall_e",
        "layout": "LAYOUT_SLATEPORT_CITY",
        "rect": (10, 41, 4, 3),
        "ground": [0x211, 0x285],
        "ground_px": True,
        "keep": stall_keep(56),
        "patch_cells": [(3, 0), (3, 1), (3, 2)],
        "parts": lambda: [stall(56, 41.5, ((24, 3, 39, 8), (9, 12, 12, 20), (44, 12, 53, 20)))],
        "exact": [(8, 11, 56, 29)],
    },
    {
        "name": "gym",
        "layout": "LAYOUT_PETALBURG_CITY",
        "rect": (12, 4, 6, 5),
        # the bottom row is porch and the town's own ground
        "match_rows": (0, 4),
        "ground": [GRASS],
        "parts": gym,
        "exact": GYM_EXACT,
    },
    {
        # Petalburg's hedges. Not a building: a run of metatiles of any shape,
        # so each connected run becomes its own model, read column by column
        # off its drawing. The front, where a run ends to the south, is
        # drawn 11 rows tall. 23F is the rounded north end of a run.
        "name": "hedge",
        "components": {
            "secondary": "gTileset_Petalburg",
            "tiles": {0x23c, 0x23d, 0x23e, 0x23f, 0x244, 0x245, 0x246, 0x24c, 0x24d, 0x24e,
                      0x254, 0x255, 0x256, 0x264, 0x265, 0x266},
            "height": 11,
            # the row under the woods carries the crowns' lower leaves: the
            # trees draw those (voxel_tree.c EmitMissingRow), the hedge is
            # its plain twin
            "clean": {0x24d: 0x23d, 0x24e: 0x23d, 0x255: 0x23c, 0x256: 0x23e,
                      # an arm's inner corner (244, 246): the run turning into
                      # it draws its front across the arm's top; in 3D that
                      # front ends at the arm's side, and the arm is straight
                      0x244: 0x24c, 0x246: 0x24c},
            # and the run turning into those corners, from the east (244)
            # or the west (246), meets the arm with no slit
            "joins": {0x244: 1, 0x246: -1},
            # a run that goes on behind a house: drawn on under its roof as
            # the bare hedge is (23D over 245)
            "under": {0x23d: 0x245},
            "side_longest": True,
            # its shaded east band has texels of the lawn's colours
            "close": 2,
            # its shadow on the lawn, past its dark outline, is no hedge
            "outline": (57, 82, 0),
        },
        "ground": [GRASS],
    },
    {
        # Rustboro's stone blocks, any width and any number of storeys: every
        # one in the map is found by its corners and modelled from its own art.
        "name": "rustboro_stone",
        "kit": {"layout": "LAYOUT_RUSTBORO_CITY", "corner": 0x224, "top": {0x225},
                "end": {0x226}, "foot": 0x21c},
        "ground": [0x2BB, 0x2C3, GRASS],
        "parts": stone_block,
        "exact": lambda w, h, meta: flat_block_exact(w, h, 7),
    },
    {
        "name": "rustboro_olive",
        "kit": {"layout": "LAYOUT_RUSTBORO_CITY", "corner": 0x220, "top": {0x221, 0x222},
                "end": {0x223, 0x23F}, "foot": 0x240},
        "ground": [0x2BB, 0x2C3, GRASS],
        "parts": olive_block,
        "exact": lambda w, h, meta: flat_block_exact(w, h, 8),
    },
    {
        # The gym again: Rustboro's copy has its own roof and flanks.
        "name": "gym_rustboro",
        "layout": "LAYOUT_RUSTBORO_CITY",
        "rect": (24, 15, 6, 5),
        "ground": [0x2BB, 0x2C3, GRASS],
        "parts": gym,
        "exact": GYM_EXACT,
    },
    {
        # Route 110's Seaside Cycling Road: a deck on piers over the sea and
        # the paths it spans, read off its drawing block by block
        # (voxel_bridge.py)
        "name": "cycling_road",
        "bridge": True,
    },
    {
        # Rustboro's iron railings, read column by column like the hedges: a
        # railing along a row is all front, one down a column is a line on top
        # with its front where it ends. The bars keep their gaps (alpha).
        "name": "railing",
        "components": {
            "secondary": "gTileset_Rustboro",
            "tiles": {0x2A7, 0x2FC, 0x318, 0x319, 0x31A, 0x315, 0x31D, 0x320, 0x321,
                      0x32C, 0x32D, 0x2BE, 0x2BF, 0x2CD, 0x352, 0x2E9,
                      # the same railings over grass or a building's shade
                      0x2B7, 0x2C6, 0x2C7, 0x2D5, 0x2D7, 0x2DC, 0x2DE, 0x2DF,
                      0x2E6, 0x2E7, 0x2EC, 0x31B},
            "height": 12, "hull": 12, "bridge": 3, "block": 4,
            # drawn whole on the upper layer; the bottom one is the ground
            # with its grass edges, which the map paints under it
            "upper": True,
            # a railing down a column shows its bars on its sides: those of
            # the railing along a row
            "flank": 0x2A7,
        },
        "ground": [0x2BB, 0x2C3, GRASS],
    },
    {
        # The General tileset's grey picket fence, built post by post
        # (voxel_fence.py): every post found by its light 4x2 cap, standing
        # where its front ends on the ground (8 rows along a row, 10 down a
        # column, where the next post hides its foot), the rails 2 rows thick
        # between the posts of a row and down a column. Every town and route
        # that draws it shares it, in the General's metatiles and in the
        # secondary tilesets' own copies over their own ground (`also`): the
        # Battle Frontier's sand and walls, Verdanturf's tall grass, Route
        # 112's and Mt Chimney's ash.
        "name": "fence",
        "components": {
            "primary": "gTileset_General",
            "tiles": {0x132, 0x133, 0x134, 0x138, 0x139, 0x13A, 0x140, 0x141, 0x142,
                      0x148, 0x149, 0x14A},
            "also": {
                "gTileset_BattleFrontierOutsideEast": {0x24D, 0x255, 0x25D, 0x26D, 0x27D, 0x320,
                                                       0x322, 0x324, 0x3D7, 0x3E5, 0x3E7},
                "gTileset_BattleFrontierOutsideWest": {0x22F, 0x286, 0x28E, 0x296, 0x29E, 0x2A6,
                                                       0x2E5, 0x2E8, 0x2ED, 0x2F0, 0x2F4, 0x2F5,
                                                       0x2F8, 0x2FC, 0x2FD, 0x300, 0x304, 0x308,
                                                       0x30C},
                "gTileset_Mauville": {0x323, 0x34D, 0x34E, 0x381, 0x3B0},
                "gTileset_Lavaridge": {0x285, 0x286, 0x287, 0x28D, 0x28E, 0x28F, 0x293, 0x294,
                                       0x296, 0x297},
            },
            "block": 4, "upper": True,
            "posts": {
                "kind": "box",
                # the post along a row: its cap at (2, 3), 4 wide and 2 deep
                "ref": 0x149, "find": (2, 3, 4, 2), "post": (2, 6), "top": 3, "depth": 2,
                "scan": 3, "hidden": 10, "rail": (4, 6),
                # a post's own colours (cap and front), its front's
                "body": (2, 3, 6, 13), "trunk": (2, 5, 6, 13), "strip": (3, 5, 5, 13),
            },
        },
        # under the fence its own bottom layer, the same in every one of the
        # General's metatiles and drawn whole by the grass: the patches lay
        # that layer a hair over the ground, and the commonest cell round a
        # run (a path beside it, the sea's edge along Slateport's market, the
        # grass with flowers) showed in a line at their edges in the game's
        # view
        "ground": [GRASS],
        "ground_fixed": True,
    },
    {
        # Lilycove's blue iron railings, read column by column like the fence
        "name": "railing_lilycove",
        "components": {
            "secondary": "gTileset_Lilycove",
            "tiles": {0x249, 0x250, 0x251, 0x252, 0x258, 0x25A},
            "height": 12, "hull": 12, "bridge": 3, "block": 4,
            "upper": True,
            "flank": 0x251,
        },
        "ground": [GRASS],
    },
    {
        # The General tileset's white curb stones (Route 110's paths, the
        # edges of lawns and piers in many towns): the same slabs as
        # Slateport's, a top in a grey frame and 3 rows of front.
        "name": "curb",
        "components": {
            "primary": "gTileset_General",
            "tiles": {0x03D, 0x03E, 0x03F, 0x045, 0x047, 0x04D, 0x04E, 0x04F,
                      0x055, 0x056, 0x057, 0x05D, 0x05E, 0x05F},
            "height": 3, "block": 4,
            # the slab's middle (05E) has no stone drawn in it: it is the
            # plain paving inside a frame of curb (Southern Island, Route
            # 121's landing), and on its own the road itself - Rustboro's
            # and Mauville's streets, which stood up in curb blocks
            "fill": {0x05E},
            # the Battle Frontier paves its walks with the same stones: a
            # floor to walk on, not a curb
            "skip": ("LAYOUT_BATTLE_FRONTIER_OUTSIDE_WEST", "LAYOUT_BATTLE_FRONTIER_OUTSIDE_EAST"),
        },
        # every cell as the map paints it under the slabs: one ground for a
        # whole run laid paving over the lawn drawn beside it (Slateport's
        # beach steps)
        "patch_own": True,
        "ground": [GRASS, 0x001],
    },
    {
        # Mauville's fence of short logs (Mauville, Routes 111 and 117), log
        # by log (voxel_fence.py): each an upright log 5 pixels across, its
        # cut face the drawn ellipse, standing where its bark ends on the
        # ground (7 rows; down a column, where the next log hides its foot,
        # as tall)
        "name": "stakes",
        "components": {
            "secondary": "gTileset_Mauville",
            "tiles": {0x273, 0x27A, 0x27B, 0x27C, 0x27D, 0x280, 0x282, 0x288, 0x28A, 0x290,
                      0x291, 0x292, 0x2B1, 0x2B2, 0x2B9, 0x2BA},
            "block": 4, "upper": True,
            "posts": {
                "kind": "log",
                # the cut face's two full rows at (1, 5); its middle row 6,
                # its centre column 3.5, 2.5 across and 2 deep
                "ref": 0x291, "find": (1, 5, 5, 2), "log": (3.5, 2.5, 2.0), "top": 6,
                "scan": 3, "trunk_row": 8, "hidden": 7, "sides": 16,
                "body": (1, 4, 6, 14), "trunk": (1, 8, 6, 14), "strip": (1, 8, 6, 14),
            },
        },
        "ground": [GRASS],
    },
    {
        # Sootopolis's white walls along the terraces' edges and the stairs,
        # read column by column like the hedges: along a row a top 4 rows
        # deep (its outline, two rows of top, a white edge) over 8 rows of
        # front; down a column a line of top, its front where it ends.
        "name": "sootopolis_wall",
        "components": {
            "secondary": "gTileset_Sootopolis",
            "tiles": SOOTOPOLIS_WALL,
            "height": 8, "block": 4,
        },
        "ground": SOOTOPOLIS_GROUND,
        # the rock round a run, and the wall's shadow cast on it, drawn in
        # the run's own cells: the map's, laid as patches
        "ground_rgb": SOOTOPOLIS_ROCK,
    },
    {
        # Lavaridge's hot spring: its rim of round stones, 4 rows of front
        "name": "spring_rim",
        "components": {
            "secondary": "gTileset_Lavaridge",
            "tiles": {0x299, 0x29A, 0x29B, 0x2A1, 0x2A5, 0x2A9, 0x2AD, 0x2B1, 0x2B2, 0x2B3,
                      0x2B4, 0x2B5},
            "height": 4,
        },
        "ground": [0x2A3, 0x31D, 0x2A8, 0x2BC],
    },
    {
        # the wooden benches by the spring, two to a cell, 4 rows of front
        "name": "spring_bench",
        "components": {
            "secondary": "gTileset_Lavaridge",
            "tiles": {0x29C, 0x29E},
            "height": 4, "block": 1,
        },
        "ground": [0x2A8, 0x274, 0x2AE],
    },
    {
        # Slateport's harbour curbs: white stone slabs along the pier's edge,
        # a top in a grey frame and 3 rows of front, read column by column
        # like the hedges; a run down a column is a line of slab tops.
        "name": "slateport_curb",
        "components": {
            "secondary": "gTileset_Slateport",
            "tiles": {0x245, 0x24D, 0x255, 0x25D, 0x2FF, 0x2FE, 0x23C, 0x234, 0x2CF},
            "height": 3, "block": 4,
        },
        # every cell as the map paints it under the slabs (the sea beside a
        # corner, the pier's bricks under a row): one ground for a whole run
        # was the sea under a pier's edge
        "patch_own": True,
        "ground": [0x202],
        # and the pier's bricks under the slabs themselves: the commonest
        # ground round a run along the edge is the sea or the pier's rocky
        # side, and a patch, lying a hair above the ground, shows a line of
        # whatever is under it along its edges in the game's view
        "ground_fixed": True,
        # the paving and the lawn beside a run (the Contest Hall's east
        # edge), drawn in the run's own cells, rose with it as a ragged
        # lump: the slabs are white, grey and their outline, never these
        "ground_rgb": {(65, 180, 131), (115, 197, 164), (164, 213, 197), (205, 205, 222),
                       (230, 230, 238)},
    },
    {
        # The grey rock in the sea, found by its four tiles wherever they are
        # drawn (voxel_props.py): over the water round it, by a sandbank, by
        # a shore. A low dome, as deep as it is drawn tall below its crest.
        # The water round it stays the map's own, animated.
        "name": "sea_rock",
        "props": "sea_rock",
        "ground": [0x170],
        "rise": 1.0,
        "step": 2,
        # its foam on the water
        "ring": [(222, 230, 238)],
    },
    {
        # A boulder on the sand or the grass, a cell across (Route 106's beach,
        # Route 111's desert, the Safari Zone).
        "name": "sand_boulder",
        "props": "sand_boulder",
        "ground": [0x124],
        "rise": 1.0,
        "step": 2,
    },
    {
        # Slateport market's crates, slatted or labelled: a lid and a front
        # of 8 rows, drawn whole on the upper layer over the paving. One
        # model per cell, placed at every crate drawn alike.
        "name": "crate",
        "components": {
            "secondary": "gTileset_Slateport",
            "tiles": {0x33E, 0x33F},
            "height": 8, "block": 1, "upper": True,
        },
        "ground": [0x211],
    },
    {
        # three crates one on another: the lid, then three fronts (24 rows)
        # down into the cell below
        "name": "crate_stack",
        "components": {
            "secondary": "gTileset_Slateport",
            "tiles": {0x32E, 0x32F, 0x336, 0x337},
            "height": 24, "upper": True,
        },
        "ground": [0x211],
    },
    # Fortree's tree houses, decks and rope bridges in the treetops
    # (voxel_fortree.py): lifted 28 px over the forest floor, the console
    # lifting whoever walks on them. The house at (10, 11) is the one at
    # (8, 1) again.
    {
        "name": "fortree_treehouse",
        "layout": "LAYOUT_FORTREE_CITY",
        "rect": (8, 1, 5, 6),
        "owned": FORTREE_OWNED,
        "repeat_at": [(8, 1), (10, 11)],
        "ground": FORTREE_GROUND,
        "ground_fixed": True,
        "parts_art": flat_art(lambda art: voxel_fortree.treehouse(art, 64, 32)),
        "exact": voxel_fortree.treehouse_exact(64, 32),
    },
    {
        # bridges east and west, a ladder
        "name": "fortree_treehouse_c",
        "layout": "LAYOUT_FORTREE_CITY",
        "rect": (23, 1, 5, 6),
        "owned": FORTREE_OWNED,
        "ground": FORTREE_GROUND,
        "ground_fixed": True,
        "parts_art": flat_art(lambda art: voxel_fortree.treehouse(art, 64, 32)),
        "exact": voxel_fortree.treehouse_exact(64, 32),
    },
    {
        # bridges east and west, no ladder
        "name": "fortree_treehouse_b",
        "layout": "LAYOUT_FORTREE_CITY",
        "rect": (15, 1, 5, 5),
        "owned": FORTREE_OWNED[:-1],
        "ground": FORTREE_GROUND,
        "ground_fixed": True,
        "parts_art": flat_art(lambda art: voxel_fortree.treehouse(art, 64)),
        "exact": voxel_fortree.treehouse_exact(64),
    },
    {
        # the deeper deck in the north-east, one row more of it
        "name": "fortree_treehouse_d",
        "layout": "LAYOUT_FORTREE_CITY",
        "rect": (30, 0, 5, 7),
        "owned": FORTREE_OWNED_DEEP,
        "ground": FORTREE_GROUND,
        "ground_fixed": True,
        "parts_art": flat_art(lambda art: voxel_fortree.treehouse(art, 80, 32)),
        "exact": voxel_fortree.treehouse_exact(80, 32),
    },
    {
        # the east one: a bridge west, a ladder without its hook
        "name": "fortree_treehouse_f",
        "layout": "LAYOUT_FORTREE_CITY",
        "rect": (35, 11, 5, 6),
        "owned": FORTREE_OWNED,
        "ground": FORTREE_GROUND,
        "ground_fixed": True,
        "parts_art": flat_art(lambda art: voxel_fortree.treehouse(art, 64, 32)),
        "exact": voxel_fortree.treehouse_exact(64, 32),
    },
    {
        # the tall palms, their trunks two cells (the other at (28, 6))
        "name": "fortree_palm",
        "layout": "LAYOUT_FORTREE_CITY",
        "rect": (12, 6, 3, 4),
        "repeat_at": [(12, 6), (28, 6)],
        "owned": [(i, j) for j in range(2) for i in range(3)] + [(1, 2), (1, 3)],
        "ground": FORTREE_GROUND,
        "ground_fixed": True,
        "parts_art": lambda art: voxel_fortree.palm(art, [(1, 2, 0x25C), (1, 3, 0x273)]),
        "exact": [(0, 0, 48, 64)],
    },
    {
        # the short ones, one cell of trunk (also at (14, 16) and (27, 17))
        "name": "fortree_palm_short",
        "layout": "LAYOUT_FORTREE_CITY",
        "rect": (1, 1, 3, 3),
        "repeat_at": [(1, 1), (14, 16), (27, 17)],
        "owned": [(i, j) for j in range(2) for i in range(3)] + [(1, 2)],
        "ground": FORTREE_GROUND,
        "ground_fixed": True,
        "parts_art": lambda art: voxel_fortree.palm(art, [(1, 2, 0x25C)]),
        "exact": [(0, 0, 48, 48)],
    },
    {
        "name": "fortree_bridge",
        "layout": "LAYOUT_FORTREE_CITY",
        "rect": (13, 4, 1, 1),
        "ground": FORTREE_GROUND,
        "ground_fixed": True,
        "parts_art": flat_art(voxel_fortree.bridge_cell),
        "exact": [(0, 0, 16, 16)],
    },
    {
        # over the path, which runs on under it
        "name": "fortree_bridge_path",
        "layout": "LAYOUT_FORTREE_CITY",
        "rect": (30, 14, 1, 1),
        "ground": [0x261] + FORTREE_GROUND,
        "ground_fixed": True,
        "parts_art": flat_art(voxel_fortree.bridge_cell),
        "exact": [(0, 0, 16, 16)],
    },
] + [
    # Slateport market's goods: round things, each a dome off its own
    # drawing like the rocks, on the paving's own cells
    # a pot, turned smooth (ProjLathe): its foot, its belly, a neck under the
    # lip round its dark mouth. As a Mound it was a lump in steps.
    # Static: one mesh drawn at each of its places, not copied into the
    # market's chunk at every one (it was, as a Mound, for its triangles).
    {"name": "urn", "props": "urn", "clip_art": True, "ground": [0x211], "shape": "lathe", "static": True,
     "lathe": {"cx": 8, "cz": 13, "squash": 0.7,
               "profile": [(0, 6), (3.5, 8.5), (7, 7.5), (9, 5.1), (10, 5.5)]}},
] + [
    # the baskets: an oval drawn round, its rim and goods on top, 5 rows of
    # wicker in front. A smooth drum under its drawing (ProjLathe), cut to
    # it: exact to its outline (a 24-sided cylinder missed it by a pixel)
    {"name": name, "props": name, "ground": [0x211], "shape": "lathe", "clip_art": True,
     "lathe": {"cx": 8, "cz": 11, "squash": 0.7, "seg": 16, "profile": [(0, 8.5), (5, 8.5)]}}
    for name in ("basket_green", "basket_orange", "basket_red")
] + [
    # a pile of fruit: a smooth dome under its drawing (ProjDome)
    {"name": "fruit", "props": "fruit", "clip_art": True, "ground": [0x211], "shape": "dome", "static": True,
     "dome": {"cx": 8.5, "cz": 9, "rx": 8.5, "rz": 7.5, "h": 7}},
    {
        # flowers in a low box: the box's front 3 rows, the flowers on its top
        "name": "flower_crate",
        "components": {
            "secondary": "gTileset_Slateport",
            "tiles": {0x20B, 0x28D, 0x213, 0x214},
            "height": 3, "block": 1, "upper": True,
        },
        "ground": [0x211],
    },
] + [
    {
        # The brown stack in the sea: a peak, taller than it is deep.
        "name": "sea_stack",
        "props": "sea_stack",
        "ground": [0x170],
        "rise": 1.6,
        "step": 4,
        # its grey shadow on the water
        "ring": [(131, 131, 139)],
    },
    {
        # The same stack in Fallarbor's beige (Routes 114 and 115): in the
        # sea, and on Route 115's terraces as their mounds (the cells under
        # it are the terrace's top, flat: voxel_terraces.py)
        "name": "sea_stack_fallarbor",
        "props": "sea_stack_fallarbor",
        "ground": [0x170],
        "rise": 1.6,
        "step": 4,
        "ring": [(131, 131, 139)],
    },
    {
        # the boulder in Fallarbor's beige, on the mountain's top
        "name": "sand_boulder_fallarbor",
        "props": "sand_boulder_fallarbor",
        "ground": [0x241],
        "rise": 1.0,
        "step": 2,
    },
    {
        "name": "devon_corporation",
        "layout": "LAYOUT_RUSTBORO_CITY",
        "rect": (7, 7, 10, 9),
        "ground": [0x2BB, 0x2C3, GRASS],
        "parts": devon,
        "exact": DEVON_EXACT,
    },
    {
        "name": "rustboro_fountain",
        "layout": "LAYOUT_RUSTBORO_CITY",
        "rect": FOUNTAIN_RECT + (3, 3),
        "ground": [0x2BB, 0x2C3, GRASS],
        "parts_art": fountain,
        # judged against its drawing, jet included (its texture keeps the
        # jet on a band of its own)
        "drawing": fountain_drawing,
        "exact": [(0, 0, 48, 48)],
    },
    {
        # Every town's Pokemon Center has this one ground floor.
        "name": "pc1f",
        "interior": {"layout": "LAYOUT_POKEMON_CENTER_1F", "ground": [0x202],
                     "pieces": POKEMON_CENTER_1F, "open": CENTER_1F_OPEN},
    },
    {
        "name": "pc2f",
        "interior": {"layout": "LAYOUT_POKEMON_CENTER_2F", "ground": [0x202],
                     "pieces": POKEMON_CENTER_2F, "open": CENTER_2F_OPEN},
    },
    {
        # Every Poke Mart but the department store has this one room.
        "name": "mart",
        "interior": {"layout": "LAYOUT_MART", "ground": [0x201], "pieces": MART,
                     "open": MART_OPEN,
                     "shade": [0x202, 0x204, 0x206, 0x208, 0x20A]},
    },
    {
        "name": "brendan_1f",
        "interior": {"layout": "LAYOUT_LITTLEROOT_TOWN_BRENDANS_HOUSE_1F", "ground": [0x201],
                     "pieces": brendan_1f()},
    },
    {
        "name": "brendan_2f",
        "interior": {"layout": "LAYOUT_LITTLEROOT_TOWN_BRENDANS_HOUSE_2F", "ground": [0x201],
                     "pieces": brendan_2f()},
    },
    {
        "name": "may_1f",
        "interior": {"layout": "LAYOUT_LITTLEROOT_TOWN_MAYS_HOUSE_1F", "ground": [0x201],
                     "pieces": may_1f()},
    },
    {
        "name": "may_2f",
        "interior": {"layout": "LAYOUT_LITTLEROOT_TOWN_MAYS_HOUSE_2F", "ground": [0x201],
                     "pieces": may_2f()},
    },
    {
        "name": "lab",
        "interior": {"layout": "LAYOUT_LITTLEROOT_TOWN_PROFESSOR_BIRCHS_LAB", "ground": [0x202],
                     "pieces": lab()},
    },
    {
        # after the starter: the boxes by the machine give way to a table.
        # The rest of the furniture is the lab's, tile for tile, and stands
        # here as it is, its back wall too; the side walls are its own.
        "name": "lab_table",
        "interior": {"layout": "LAYOUT_LITTLEROOT_TOWN_PROFESSOR_BIRCHS_LAB_WITH_TABLE",
                     "ground": [0x202],
                     "pieces": [piece("table", [(128, 64, 176, 89)], 8, leave=LAB_SHADOW,
                                      solid=True)]
                     + [pc for pc in lab() if pc["name"] in ("side_w", "side_e")]},
    },
    {
        "name": "lavaridge_pc1f",
        "interior": {"layout": "LAYOUT_LAVARIDGE_TOWN_POKEMON_CENTER_1F", "ground": [0x202],
                     "pieces": LAVARIDGE_CENTER_1F, "open": CENTER_1F_OPEN},
    },
    {
        # Oldale's first house, and eight more maps'
        "name": "house1",
        "interior": {"layout": "LAYOUT_HOUSE1", "ground": [0x223], "pieces": house1()},
    },
    {
        # Oldale's second house, and eleven more maps'
        "name": "house2",
        "interior": {"layout": "LAYOUT_HOUSE2", "ground": [0x223], "pieces": house2()},
    },
    {
        # Petalburg's second house, and two more maps'
        "name": "house_with_bed",
        "interior": {"layout": "LAYOUT_HOUSE_WITH_BED", "ground": [0x223], "pieces": own_shell(house_with_bed())},
    },
] + [
    {
        "name": layout[len("LAYOUT_"):].lower(),
        "interior": {"layout": layout, "ground": [ground],
                     "pieces": own_shell(plain_room(width, height, plain_x, sides))},
    }
    for layout, width, height, ground, plain_x, sides in PLAIN_ROOMS
] + [
    {
        # Mr. Briney's house, by the dock on Route 104
        "name": "briney_room",
        "interior": {"layout": "LAYOUT_ROUTE104_MR_BRINEYS_HOUSE", "ground": [0x229],
                     "pieces": own_shell(briney_room())},
    },
] + [
    {
        "name": layout[len("LAYOUT_"):].lower(),
        "interior": {"layout": layout, "ground": [ground],
                     "pieces": own_shell(stair_room(width, height, plain_x, doors, *extra))},
    }
    for layout, width, height, ground, plain_x, doors, *extra in STAIR_ROOMS
] + [
    {
        # Rustboro's Pokemon school
        "name": "school_room",
        "interior": {"layout": "LAYOUT_RUSTBORO_CITY_POKEMON_SCHOOL", "ground": [0x201],
                     "pieces": own_shell(school_room())},
    },
    {
        # The Pretty Petal flower shop on Route 104
        "name": "flower_shop_room",
        "interior": {"layout": "LAYOUT_ROUTE104_PRETTY_PETAL_FLOWER_SHOP", "ground": [0x201],
                     "pieces": own_shell(flower_shop_room())},
    },
    {
        # Rustboro's gym: Roxanne's maze. Before the other gyms: a room stands
        # only the pieces of the rooms modelled before it (reuse_pieces), and
        # the statues every gym has by its door are modelled here
        "name": "rustboro_gym",
        "interior": {"layout": "LAYOUT_RUSTBORO_CITY_GYM", "ground": [0x201],
                     "shade": [0x202, 0x203, 0x204, 0x216, 0x22f, 0x237],
                     "pieces": rustboro_gym()},
    },
    {
        # Dewford's gym: Brawly's maze
        "name": "dewford_gym",
        "interior": {"layout": "LAYOUT_DEWFORD_TOWN_GYM", "ground": [0x210],
                     "shade": DEWFORD_GYM_FLOOR, "pieces": own_shell(dewford_gym())},
    },
    {
        # Petalburg's gym: Norman's rooms
        "name": "petalburg_gym",
        "interior": {"layout": "LAYOUT_PETALBURG_CITY_GYM", "ground": [0x201, 0x22B],
                     "shade": [0x209, 0x212, 0x213, 0x214, 0x22A, 0x232,
                               0x216, 0x22C],
                     "pieces": own_shell(petalburg_gym())},
    },
    {
        # its doors at frame 1 of their sliding open
        "name": "petalburg_gym_door1",
        "interior": {"layout": "LAYOUT_PETALBURG_CITY_GYM", "ground": [0x201, 0x22B],
                     "shade": [0x209, 0x212, 0x213, 0x214, 0x22A, 0x232,
                               0x216, 0x22C],
                     "draw": petalburg_door_draw(1), "pieces": petalburg_gym_doors(1)},
    },
    {
        # its doors at frame 2 of their sliding open
        "name": "petalburg_gym_door2",
        "interior": {"layout": "LAYOUT_PETALBURG_CITY_GYM", "ground": [0x201, 0x22B],
                     "shade": [0x209, 0x212, 0x213, 0x214, 0x22A, 0x232,
                               0x216, 0x22C],
                     "draw": petalburg_door_draw(2), "pieces": petalburg_gym_doors(2)},
    },
    {
        # its doors at frame 3 of their sliding open
        "name": "petalburg_gym_door3",
        "interior": {"layout": "LAYOUT_PETALBURG_CITY_GYM", "ground": [0x201, 0x22B],
                     "shade": [0x209, 0x212, 0x213, 0x214, 0x22A, 0x232,
                               0x216, 0x22C],
                     "draw": petalburg_door_draw(3), "pieces": petalburg_gym_doors(3)},
    },
    {
        # its doors at frame 4 of their sliding open
        "name": "petalburg_gym_door4",
        "interior": {"layout": "LAYOUT_PETALBURG_CITY_GYM", "ground": [0x201, 0x22B],
                     "shade": [0x209, 0x212, 0x213, 0x214, 0x22A, 0x232,
                               0x216, 0x22C],
                     "draw": petalburg_door_draw(4), "pieces": petalburg_gym_doors(4)},
    },
    {
        # the contest stage: its pieces stand on every stage drawn again,
        # in the five coloured halls and three times in Lilycove's
        "name": "contest_stage",
        "interior": {"layout": "LAYOUT_CONTEST_HALL", "ground": [0x271, 0x225],
                     "pieces": own_shell(contest_stage())},
    },
    {
        # Lilycove's contest lobby: the reception, the Berry Blenders
        "name": "contest_lobby",
        "interior": {"layout": "LAYOUT_LILYCOVE_CITY_CONTEST_LOBBY", "ground": [0x260],
                     "pieces": own_shell(contest_lobby())},
    },
    {
        # Lilycove's contest hall: its walls and palms (its stages stand the
        # contest stage's pieces)
        "name": "lilycove_contest_room",
        "interior": {"layout": "LAYOUT_LILYCOVE_CITY_CONTEST_HALL", "ground": [0x260],
                     "pieces": own_shell(lilycove_contest_room())},
    },
    {
        # Lilycove's Department Store, 1F
        "name": "dept_store_1f",
        "interior": {"layout": "LAYOUT_LILYCOVE_CITY_DEPARTMENT_STORE_1F", "ground": [0x268],
                     "pieces": own_shell(dept_store_1f())},
    },
    {
        "name": "dept_store_2f",
        "interior": {"layout": "LAYOUT_LILYCOVE_CITY_DEPARTMENT_STORE_2F", "ground": [0x201],
                     "pieces": own_shell(dept_store_2f())},
    },
    {
        "name": "dept_store_3f",
        "interior": {"layout": "LAYOUT_LILYCOVE_CITY_DEPARTMENT_STORE_3F", "ground": [0x201],
                     "pieces": own_shell(dept_store_3f())},
    },
    {
        "name": "dept_store_4f",
        "interior": {"layout": "LAYOUT_LILYCOVE_CITY_DEPARTMENT_STORE_4F", "ground": [0x201],
                     "pieces": own_shell(dept_store_4f())},
    },
    {
        "name": "dept_store_5f",
        "interior": {"layout": "LAYOUT_LILYCOVE_CITY_DEPARTMENT_STORE_5F", "ground": [0x2f2],
                     "pieces": own_shell(dept_store_5f())},
    },
    {
        "name": "dept_store_elevator",
        "interior": {"layout": "LAYOUT_LILYCOVE_CITY_DEPARTMENT_STORE_ELEVATOR", "ground": [0x265],
                     "pieces": own_shell(dept_store_elevator())},
    },
    {
        "name": "dept_store_rooftop",
        "interior": {"layout": "LAYOUT_LILYCOVE_CITY_DEPARTMENT_STORE_ROOFTOP", "ground": [0x24a],
                     "pieces": dept_store_rooftop()},
    },
    {
        # Lilycove's museum, 1F
        "name": "museum_1f",
        "interior": {"layout": "LAYOUT_LILYCOVE_CITY_LILYCOVE_MUSEUM_1F", "ground": [0x202],
                     "pieces": own_shell(museum_1f())},
    },
    {
        "name": "museum_2f",
        "interior": {"layout": "LAYOUT_LILYCOVE_CITY_LILYCOVE_MUSEUM_2F", "ground": [0x268],
                     "pieces": own_shell(museum_2f())},
    },
    {
        # Lilycove's Pokemon Trainer Fan Club
        "name": "trainer_fan_club",
        "interior": {"layout": "LAYOUT_LILYCOVE_CITY_POKEMON_TRAINER_FAN_CLUB", "ground": [0x223],
                     "pieces": own_shell(trainer_fan_club())},
    },
    {
        # Lanette's house, Route 114
        "name": "lanettes_house",
        "interior": {"layout": "LAYOUT_ROUTE114_LANETTES_HOUSE", "ground": [0x202],
                     "pieces": own_shell(lanettes_house())},
    },
    {
        # the Pokemon Day Care, Route 117
        "name": "day_care",
        "interior": {"layout": "LAYOUT_ROUTE117_POKEMON_DAY_CARE", "ground": [0x201],
                     "pieces": own_shell(day_care())},
    },
    {
        # the Seashore House, Route 109
        "name": "seashore_house",
        "interior": {"layout": "LAYOUT_ROUTE109_SEASHORE_HOUSE", "ground": [0x225],
                     "pieces": own_shell(seashore_house())},
    },
    {
        # Devon Corporation, 1F
        "name": "devon_corp_1f",
        "interior": {"layout": "LAYOUT_RUSTBORO_CITY_DEVON_CORP_1F", "ground": [0x380],
                     "pieces": own_shell(devon_corp_1f())},
    },
    {
        # the Battle Tents' lobby (Slateport, Verdanturf, Fallarbor)
        "name": "tent_lobby",
        "interior": {"layout": "LAYOUT_BATTLE_TENT_LOBBY", "ground": [0x210],
                     "pieces": own_shell(tent_lobby())},
    },
    {
        "name": "tent_corridor",
        "interior": {"layout": "LAYOUT_BATTLE_TENT_CORRIDOR", "ground": [0x23b],
                     "pieces": own_shell(tent_room(80, 128))},
    },
    {
        # Slateport's and Fallarbor's battle room
        "name": "tent_battle_room",
        "interior": {"layout": "LAYOUT_BATTLE_TENT_BATTLE_ROOM", "ground": [0x210],
                     "pieces": own_shell(tent_room(160, 144))},
    },
    {
        "name": "verdanturf_tent_battle_room",
        "interior": {"layout": "LAYOUT_VERDANTURF_TOWN_BATTLE_TENT_BATTLE_ROOM", "ground": [0x1],
                     "pieces": own_shell(tent_room(208, 144))},
    },
    {
        # Slateport's Pokemon Fan Club
        "name": "slateport_fan_club",
        "interior": {"layout": "LAYOUT_SLATEPORT_CITY_POKEMON_FAN_CLUB", "ground": [0x201],
                     "pieces": own_shell(slateport_fan_club())},
    },
    {
        # the Harbor, Slateport's and Lilycove's
        "name": "harbor",
        "interior": {"layout": "LAYOUT_HARBOR", "ground": [0x202],
                     "pieces": own_shell(harbor())},
    },
    {
        # Stern's Shipyard, Slateport, 1F
        "name": "shipyard_1f",
        "interior": {"layout": "LAYOUT_SLATEPORT_CITY_STERNS_SHIPYARD_1F", "ground": [0x229],
                     "pieces": own_shell(shipyard_1f())},
    },
    {
        "name": "shipyard_2f",
        "interior": {"layout": "LAYOUT_SLATEPORT_CITY_STERNS_SHIPYARD_2F", "ground": [0x202],
                     "pieces": own_shell(shipyard_2f()), "pit": SHIPYARD_PIT},
    },
    {
        "name": "herb_shop",
        "interior": {"layout": "LAYOUT_LAVARIDGE_TOWN_HERB_SHOP", "ground": [0x268],
                     "pieces": own_shell(herb_shop())},
    },
    {
        # the Seaside Cycling Road's entrances, south and north
        "name": "cycling_gate_room",
        "interior": {"layout": "LAYOUT_ROUTE110_SEASIDE_CYCLING_ROAD_ENTRANCE", "ground": [0x268],
                     "pieces": own_shell(cycling_gate())},
    },
    {
        "name": "safari_rest_house",
        "interior": {"layout": "LAYOUT_SAFARI_ZONE_REST_HOUSE", "ground": [0x223],
                     "pieces": own_shell(safari_rest_house())},
    },
    {
        "name": "institute_1f",
        "interior": {"layout": "LAYOUT_ROUTE119_WEATHER_INSTITUTE_1F", "ground": [0x252],
                     "pieces": own_shell(institute_1f())},
    },
    {
        "name": "institute_2f",
        "interior": {"layout": "LAYOUT_ROUTE119_WEATHER_INSTITUTE_2F", "ground": [0x252],
                     "pieces": own_shell(institute_2f())},
    },
    {
        "name": "safari_entrance",
        "interior": {"layout": "LAYOUT_ROUTE121_SAFARI_ZONE_ENTRANCE", "ground": [0x268],
                     "pieces": own_shell(safari_entrance())},
    },
    {
        # the Cable Car Station, Route 112's and Mt. Chimney's
        "name": "cable_car_room",
        "interior": {"layout": "LAYOUT_CABLE_CAR_STATION", "ground": [0x2f0],
                     "pieces": own_shell(cable_car_station())},
    },
    {
        "name": "bike_shop",
        "interior": {"layout": "LAYOUT_MAUVILLE_CITY_BIKE_SHOP", "ground": [0x23f],
                     "pieces": own_shell(bike_shop())},
    },
    {
        "name": "game_corner_room",
        "interior": {"layout": "LAYOUT_MAUVILLE_CITY_GAME_CORNER", "ground": [0x202],
                     "pieces": own_shell(mauville_game_corner())},
    },
    {
        "name": "mossdeep_game_corner_1f",
        "interior": {"layout": "LAYOUT_MOSSDEEP_CITY_GAME_CORNER_1F", "ground": [0x20a],
                     "pieces": own_shell(mossdeep_game_corner_1f())},
    },
    {
        "name": "mossdeep_game_corner_b1f",
        "interior": {"layout": "LAYOUT_MOSSDEEP_CITY_GAME_CORNER_B1F", "ground": [0x223],
                     "pieces": own_shell(room_shell(192, 144, foot=31))},
    },
    {
        "name": "mystery_events_house_1f",
        "interior": {"layout": "LAYOUT_SOOTOPOLIS_CITY_MYSTERY_EVENTS_HOUSE_1F",
                     "ground": [0x235], "pieces": own_shell(mystery_events_house_1f())},
    },
    {
        # while a Frontier trainer is visiting, the map's script opens the
        # stairs down (setmaplayoutindex): the plants stand either side of
        # them, not in front. Its back wall is drawn round the open stairs,
        # so it is its own; the rest is 1F's, found again
        "name": "mystery_events_house_1f_open",
        "interior": {"layout": "LAYOUT_SOOTOPOLIS_CITY_MYSTERY_EVENTS_HOUSE_1F_STAIRS_UNBLOCKED",
                     "ground": [0x235], "pieces": own_shell(room_shell(176, 128))},
    },
    {
        "name": "mystery_events_house_b1f",
        "interior": {"layout": "LAYOUT_SOOTOPOLIS_CITY_MYSTERY_EVENTS_HOUSE_B1F",
                     "ground": [0x235], "pieces": own_shell(room_shell(192, 144, foot=31))},
    },
    {
        "name": "sidneys_room",
        "interior": {"layout": "LAYOUT_EVER_GRANDE_CITY_SIDNEYS_ROOM", "ground": [0x22c],
                     "pieces": own_shell(elite_four_room(("c59462", "cda473")))},
    },
    {
        "name": "phoebes_room",
        "interior": {"layout": "LAYOUT_EVER_GRANDE_CITY_PHOEBES_ROOM", "ground": [0x25c],
                     "pieces": own_shell(elite_four_room(("a47bac", "b48bbd")))},
    },
    {
        "name": "glacias_room",
        "interior": {"layout": "LAYOUT_EVER_GRANDE_CITY_GLACIAS_ROOM", "ground": [0x28c],
                     "pieces": own_shell(elite_four_room(("bdc5ff", "cdd5ff")))},
    },
    {
        "name": "drakes_room",
        "interior": {"layout": "LAYOUT_EVER_GRANDE_CITY_DRAKES_ROOM", "ground": [0x2bc],
                     "pieces": own_shell(elite_four_room(("e67341", "ff7341")))},
    },
    {
        "name": "league_1f",
        "interior": {"layout": "LAYOUT_EVER_GRANDE_CITY_POKEMON_LEAGUE_1F", "ground": [0x240],
                     "pieces": own_shell(league_1f()), "open": LEAGUE_1F_OPEN},
    },
    {
        # the four halls between the Elite Four's rooms
        "name": "league_hall",
        "interior": {"layout": "LAYOUT_EVER_GRANDE_CITY_SHORT_HALL", "ground": [0x2d1],
                     "pieces": own_shell(league_hall(208))},
    },
    {
        "name": "league_hall4",
        "interior": {"layout": "LAYOUT_EVER_GRANDE_CITY_HALL4", "ground": [0x2d1],
                     "pieces": own_shell(league_hall(544, rail=208))},
    },
    {
        "name": "hall_of_fame",
        "interior": {"layout": "LAYOUT_EVER_GRANDE_CITY_HALL_OF_FAME", "ground": [0x2f6],
                     "pieces": own_shell(hall_of_fame())},
    },
    {
        "name": "mauville_gym",
        "interior": {"layout": "LAYOUT_MAUVILLE_CITY_GYM", "ground": [0x20a],
                     "pieces": own_shell(mauville_gym()),
                     # the entrance hall's dark stands as a block, its rims
                     # at its top's north edge (block_hall_w/e, `flat`): the
                     # GBA draws them at the floor's level, a block's height
                     # south of where they stand
                     "redrawn": [((0, 248, 32, 336), "the hall's dark, west"),
                                 ((128, 248, 160, 336), "the hall's dark, east")]},
    },
    {
        # its red beams, read off the room in the barriers' other state
        "name": "mauville_gym_alt",
        "interior": {"layout": "LAYOUT_MAUVILLE_CITY_GYM", "ground": [0x20a],
                     "draw": MAUVILLE_GYM_ALT, "pieces": mauville_gym_alt()},
    },
    {
        "name": "mossdeep_gym",
        "interior": {"layout": "LAYOUT_MOSSDEEP_CITY_GYM", "ground": [0x20a],
                     "pieces": own_shell(mossdeep_gym())},
    },
    {
        "name": "fortree_gym",
        "interior": {"layout": "LAYOUT_FORTREE_CITY_GYM", "ground": [0x202],
                     "pieces": own_shell(fortree_gym())},
    },
    {
        "name": "lavaridge_gym_1f",
        "interior": {"layout": "LAYOUT_LAVARIDGE_TOWN_GYM_1F", "ground": [0x208],
                     "shade": ["624a41"],
                     "pieces": own_shell(lavaridge_gym(LAVARIDGE_GYM_1F_RUNS,
                                                       LAVARIDGE_GYM_1F_CURBS,
                                                       LAVARIDGE_GYM_1F_SIDES))},
    },
    {
        "name": "lavaridge_gym_b1f",
        "interior": {"layout": "LAYOUT_LAVARIDGE_TOWN_GYM_B1F", "ground": [0x208],
                     "shade": ["624a41"],
                     "pieces": own_shell(lavaridge_gym(LAVARIDGE_GYM_B1F_RUNS,
                                                       LAVARIDGE_GYM_B1F_CURBS,
                                                       LAVARIDGE_GYM_B1F_SIDES))},
    },
    {
        "name": "sootopolis_gym_1f",
        "interior": {"layout": "LAYOUT_SOOTOPOLIS_CITY_GYM_1F", "ground": [0x20d],
                     "pieces": own_shell(sootopolis_gym_1f())},
    },
    {
        "name": "sootopolis_gym_b1f",
        "interior": {"layout": "LAYOUT_SOOTOPOLIS_CITY_GYM_B1F", "ground": [0x211],
                     "pieces": own_shell(sootopolis_gym_b1f())},
    },
    {
        "name": "scotts_house",
        "interior": {"layout": "LAYOUT_BATTLE_FRONTIER_SCOTTS_HOUSE", "ground": [0x3c9],
                     "pieces": own_shell(room_shell(96, 128)),
                     "open": front_corners(96, 128)},
    },
    {
        # the lounges: two and seven of them
        "name": "frontier_lounge1",
        "interior": {"layout": "LAYOUT_BATTLE_FRONTIER_LOUNGE1", "ground": [0x3c8],
                     "pieces": own_shell(room_shell(208, 128)),
                     "open": front_corners(208, 128)},
    },
    {
        "name": "frontier_lounge2",
        "interior": {"layout": "LAYOUT_BATTLE_FRONTIER_LOUNGE2", "ground": [0x3c8],
                     "pieces": own_shell(room_shell(144, 160)),
                     "open": front_corners(144, 160)},
    },
    {
        # the Battle Frontier's lobbies
        "name": "frontier_arena_lobby",
        "interior": {"layout": FRONTIER_LOBBIES["arena"][0], "ground": [FRONTIER_LOBBIES["arena"][4]],
                     "pieces": own_shell(frontier_lobby("arena"))},
    },
    {
        "name": "frontier_dome_lobby",
        "interior": {"layout": FRONTIER_LOBBIES["dome"][0], "ground": [FRONTIER_LOBBIES["dome"][4]],
                     "pieces": own_shell(frontier_lobby("dome"))},
    },
    {
        "name": "frontier_factory_lobby",
        "interior": {"layout": FRONTIER_LOBBIES["factory"][0], "ground": [FRONTIER_LOBBIES["factory"][4]],
                     "pieces": own_shell(frontier_lobby("factory"))},
    },
    {
        "name": "frontier_palace_lobby",
        "interior": {"layout": FRONTIER_LOBBIES["palace"][0], "ground": [FRONTIER_LOBBIES["palace"][4]],
                     "pieces": own_shell(frontier_lobby("palace"))},
    },
    {
        "name": "frontier_pike_lobby",
        "interior": {"layout": FRONTIER_LOBBIES["pike"][0], "ground": [FRONTIER_LOBBIES["pike"][4]],
                     "pieces": own_shell(frontier_lobby("pike"))},
    },
    {
        "name": "frontier_pyramid_lobby",
        "interior": {"layout": FRONTIER_LOBBIES["pyramid"][0], "ground": [FRONTIER_LOBBIES["pyramid"][4]],
                     "pieces": own_shell(frontier_lobby("pyramid"))},
    },
    {
        "name": "frontier_tower_lobby",
        "interior": {"layout": FRONTIER_LOBBIES["tower"][0], "ground": [FRONTIER_LOBBIES["tower"][4]],
                     "pieces": own_shell(frontier_lobby("tower"))},
    },
    {
        "name": "treasure_house",
        "interior": {"layout": "LAYOUT_ROUTE124_DIVING_TREASURE_HUNTERS_HOUSE", "ground": [0x223],
                     "pieces": own_shell(room_shell(160, 144))},
    },
] + [
    {
        # the coloured stages: the same room, its stairs and emblem in the
        # contest's colour (walked on, drawn flat). The cushions and the case
        # stand as reused; the back wall, the room's own, is laid again
        "name": "contest_stage_" + kind,
        "interior": {"layout": "LAYOUT_CONTEST_HALL_" + kind.upper(), "ground": [0x271, 0x225],
                     "pieces": own_shell(contest_stage()[-1:])},
    }
    for kind in ("beauty", "cool", "cute", "smart", "tough")
]


# The Trick House's puzzle rooms (Route 110): the back wall, its scroll's
# cell (13,1) left out of it - solving a puzzle lays the stairs down there
# (setmetatile), so it stays drawn, under a lintel - and the maze's
# partitions, a piece to each rectangle of their cells. Each map below is
# the room from row 2 down: '#' a partition's cell, 'o' a cell the room's
# script changes (a barrier, a button, a door, an arrow), which stays drawn
# and stands in no piece's rectangle, nor in a side wall's.
TRICK_SIDE = (16, 0, 32, 32)
TRICK_PUZZLES = {
    # cream partitions on wooden fronts, 12 px
    1: (12, """
############...
..#...#.#..#...
...........#...
..#...#.#..#...
.############.#
....#......#...
.#.............
.#..#..##..#...
###.##....#####
..#..#..#..#...
.....#..#......
..#..#..#..#...
..############.
....#.#....#...
...##..........
....#......#...
#############.#
....#....#.....
...............
...............
"""),
    # gold strips, 6 px
    2: (6, """
.........#.....
...####..#.....
o..#.....#.####
####.###...#..o
.....o.#####...
.#...#.........
.#..#########..
.#..#.......#..
.#..#.###...#..
.##.#.#o#.####.
..#.#.#.#.#o.#o
#o#.#...#.#..#.
..#######.#..##
.......o..#..##
.#.#########...
.#....#..#.##..
.#....#..#.....
.#.#.....#..##.
...#######.....
...............
"""),
    # cream partitions on wooden fronts round the barriers, 12 px
    3: (12, """
#..#..#.o#..#..
#oo#..#..#..#oo
#oo#..o..o..#oo
......o..o..o..
#oo#oo#..#oo#..
#ooooo#..#ooo..
...o.....#..o..
#oo####oo#..#oo
#oo####oo#..#oo
...o...........
#oo#oo#..#oo#..
#ooooo#..#oo#..
#..oo.......#..
####oo####..#oo
###ooo###o..#oo
...o..#..o.....
#..#oo#..#oo#..
#..#oo#..#oo#..
...............
...............
"""),
    # gold strips, 6 px
    4: (6, """
....##.....#...
.....#....##...
..##.##.#.#....
...#....#.#..##
...#....#.##.#.
###.....#......
.....#..#......
.....#...######
.....#.........
..########.....
..#....#....###
.##..#...#..#.#
.#...#####.##..
.#...#......#..
.###.....#.....
.#...#.........
.##############
...............
...............
...............
"""),
    # gold strips, 6 px
    6: (6, """
##...#......#..
#....#..#...#..
#.......#.#.##.
.....#..#.#.#..
...###......#..
...#.....#..#.#
.....###.#..#..
......#....##..
#.#.#.#...#....
###.#.#...#....
....#..##.##...
....##..#..#...
###..#..#.###.#
#....#......#.#
#.#..#..#......
..#..##........
.........##.#..
............#..
....##....###..
....#....##....
"""),
    # gold strips, 6 px
    7: (6, """
#..#..#.......#
#..#..#.....###
#..#o.##o####.#
#..#.##o#...#.#
#.....o.###.#.#
###..#......#.#
....#######.#.#
.####.......#.#
.#...o.###..#.#
.#.#####.##.o.#
##.#..#o..#.###
...#..##.##.o.#
...#######....#
............#.#
###########.o.#
#.....##..#.#o#
###..###..#...#
..####......#.#
....##.#..#.#.#
.......#..#####
"""),
}


def trick_house_puzzle(n):
    height, text = TRICK_PUZZLES[n]
    rows = text.split()
    cells = [(x, y + 2) for y, row in enumerate(rows) for x, c in enumerate(row) if c == "#"]
    return rect_pieces("partition", runs_of(cells), height) + trick_house_shell(
        {(x, y + 2) for y, row in enumerate(rows) for x, c in enumerate(row) if c == "o"})


# the eighth's jars on its ice, one to each cell, to a pixel short of its
# foot (the floor's own line there, grey as the jars are, runs under them):
# the floor round them the ice's and the tatami's (not their dark outline,
# the jars' too)
TRICK_JAR_FLOOR = ("c59439", "d5a44a", "e6c562", "ffe67b", "fff6bd", "6a8b31", "83ac4a", "a4cd6a",
                   "c5ffac")
TRICK_JARS = """
...#..........#
..###.#########
....#.#........
.#.............
.......#......#
.........#.....
.............#.
...##..#####.#.
#.#######.#..#.
.#....##.......
....#.....#....
....#.....#....
....#.....#....
....#.....#....
.#........#....
..........#....
..........#....
..###.#...#....
..###.#...#....
..##..#........
"""


def trick_house_puzzle8():
    return [piece("jar_%d_%d" % (x, y + 2), [(x * 16 + 2, y * 16 + 33, x * 16 + 14, y * 16 + 47)], 9,
                  round="lathe", leave=TRICK_JAR_FLOOR)
            for y, row in enumerate(TRICK_JARS.split()) for x, c in enumerate(row)
            if c == "#"] + trick_house_shell()


def trick_house_corridor():
    """The corridor (240 x 384) runs as an L through the dark: its three
    walls, each facing the stretch of it below, stand; the dark round it is
    beyond the room."""
    side = (64, 0, 80, 32)
    return [
        piece("wall", [(48, 0, 240, 32)], 32, fill=16, foot=32, side=side),
        piece("wall_mid", [(0, 96, 48, 128)], 32, fill=16, foot=128, side=side),
        piece("wall_low", [(48, 320, 96, 352)], 32, fill=16, foot=352, side=side),
    ]


def trick_house_shell(drawn=frozenset()):
    """A puzzle room's walls (240 x 352): the back wall round its scroll's
    cell, a lintel over it, and the side walls in stretches round the cells
    the room's script changes along them (a stretch ends a pixel short of
    the next cell, or its rectangle would take it)."""
    side = TRICK_SIDE
    out = [
        piece("wall_w", [(0, 0, 208, 32)], 32, fill=16, foot=32, side=side),
        piece("wall_e", [(224, 0, 240, 32)], 32, fill=16, foot=32, side=side),
        piece("wall_lintel", [(208, 0, 224, 16)], 32, fill=16, foot=32, side=side),
    ]
    for name, x, col in (("side_w", 0, 0), ("side_e", 240, 14)):
        rows = [y for y in range(2, 22) if (col, y) not in drawn]
        for (_, y0, y1) in runs_of([(col, y) for y in rows]):
            a, b = (x, y0 * 16), (x, (y1 + 1) * 16 - (1 if y1 < 21 else 0))
            out.append(piece("%s_%d" % (name, y0), [], 32, side=side,
                             walls=[(b, a) if col == 0 else (a, b)]))
    return out


# the Battle Frontier's battle rooms and corridors
SPECS += [{"name": room[0], "interior": dict({"layout": room[1], "ground": [room[2]],
                                              "pieces": own_shell(room[3]())},
                                             **(room[4] if len(room) > 4 else {}))}
          for room in FRONTIER_ROOMS]


# the Trick House's puzzle rooms; the fifth's dark is beyond the room, round
# the paths drawn in it: its walls alone
SPECS += [{"name": "trick_house_puzzle%d" % n,
           "interior": {"layout": "LAYOUT_ROUTE110_TRICK_HOUSE_PUZZLE%d" % n, "ground": [0x208],
                        "pieces": own_shell(trick_house_puzzle(n) if n in TRICK_PUZZLES
                                            else trick_house_shell())}}
          for n in (1, 2, 3, 4, 5, 6, 7)] + [
    {"name": "trick_house_puzzle8",
     "interior": {"layout": "LAYOUT_ROUTE110_TRICK_HOUSE_PUZZLE8", "ground": [0x26b],
                  "pieces": own_shell(trick_house_puzzle8())}},
    {"name": "trick_house_corridor",
     "interior": {"layout": "LAYOUT_ROUTE110_TRICK_HOUSE_CORRIDOR", "ground": [0x229],
                  "pieces": own_shell(trick_house_corridor())}},
]


# The link rooms (the Colosseums, the Trade Center, the Record Corner, the
# Union Room): a back wall, and side walls on the inner edge of the rims
# down their sides, the rims behind them; the rim across the front, with
# its doorway, is the front wall the GBA's camera looks over, and stays
# drawn. And the truck the game starts in.

def link_room(w, h, foot, x0, x1, z1, top=0):
    """A link room: its back wall (x0 to x1, standing on `foot`) and its
    side walls along x0 and x1 from it to z1, the front rim's edge."""
    side = (x0 + 16, top, x0 + 32, foot)
    return [
        piece("wall", [(x0 - 16 if x0 else 0, top, x1 + 16 if x1 < w else w, foot)], foot - top,
              fill=16, foot=foot, side=side),
        piece("side_w", [], foot - top, side=side, walls=[((x0, z1), (x0, foot))]),
        piece("side_e", [], foot - top, side=side, walls=[((x1, foot), (x1, z1))]),
    ]


def record_corner():
    # the record mixer: its table and the four seats at its corners
    return [
        piece("seat_nw", [(112, 53, 128, 79)], 14),
        piece("seat_sw", [(112, 85, 128, 111)], 14),
        piece("seat_ne", [(192, 53, 208, 79)], 14),
        piece("seat_se", [(192, 85, 208, 111)], 14),
        piece("table", [(128, 67, 192, 112)], 12),
    ] + link_room(320, 160, 32, 64, 256, 144)


def inside_of_truck():
    # the boxes stacked round it, along its back (hanging half a cell over
    # the floor in front of it), its west side and its front, and the vent
    # over its door; the door (4,1-3), which opens when the game starts,
    # stays drawn, in no piece's rectangle
    return [
        piece("boxes_w", [(0, 0, 16, 80)], 24),
        piece("boxes_n", [(16, 0, 64, 24)], 16),
        piece("vent", [(64, 0, 80, 16)], 12),
        piece("boxes_s", [(16, 48, 48, 80)], 24),
        piece("boxes_se", [(48, 64, 80, 80)], 12),
    ]


LINK_ROOMS = [
    ("battle_colosseum_2p", "LAYOUT_BATTLE_COLOSSEUM_2P", 0x255,
     lambda: link_room(224, 144, 48, 16, 208, 128)),
    ("battle_colosseum_4p", "LAYOUT_BATTLE_COLOSSEUM_4P", 0x255,
     lambda: link_room(224, 144, 48, 16, 208, 128)),
    # the trading machine between its two chairs, which stay drawn
    ("trade_center", "LAYOUT_TRADE_CENTER", 0x208,
     lambda: [piece("machine", [(82, 68, 110, 104)], 16)] + link_room(192, 144, 48, 16, 176, 128)),
    ("record_corner", "LAYOUT_RECORD_CORNER", 0x2c4, record_corner),
    ("union_room", "LAYOUT_UNION_ROOM", 0x213, lambda: link_room(240, 192, 32, 0, 240, 176)),
    ("inside_of_truck", "LAYOUT_INSIDE_OF_TRUCK", 0x20a, inside_of_truck),
]
SPECS += [{"name": n, "interior": {"layout": lid, "ground": [g], "pieces": own_shell(f())}}
          for (n, lid, g, f) in LINK_ROOMS]


# The S.S. Tidal: the corridor round its block of cabins, the cargo on its
# lower deck and the cabins; and Trainer Hill's entrance.

def ss_tidal_corridor():
    side = (64, 0, 80, 32)
    return [
        # the cabins' block: its roof white, its face to the south their
        # doors, 26 px
        piece("block", [(48, 57, 240, 159)], 26, solid=True, side=(48, 132, 64, 158)),
        piece("wall", [(16, 0, 288, 32)], 32, fill=16, foot=32, side=side),
        # the corridor runs on east, past the room's edge; the dark down its
        # west side is beyond it
        piece("side_w", [], 32, side=side, walls=[((16, 192), (16, 32))]),
    ]


def kw_pieces(prefix, cells, height, extra):
    """rect_pieces(prefix, runs_of(cells), height) with each piece's own
    keywords besides: `extra` {name: {keyword: value}}."""
    return [piece(name, [(x0, y0, x1, y1)], min(height, y1 - y0 - 4), **extra.get(name, {}))
            for (x0, y0, x1, y1) in run_rects(runs_of(cells))
            for name in ["%s_%d_%d" % (prefix, x0 // 16, y0 // 16)]]


def crate_stacks(cells):
    """The lower deck's cargo, a piece to each rectangle of its cells: the
    crates' tops drawn a cell above them, their fronts 16 px. The stacks
    against the back wall have their sides dressed with their front's grey
    (projected, they would be their dark outline)."""
    return [piece("crates_%d_%d" % (x0 // 16, y0 // 16), [(x0, y0 - 16, x1, y1)], 16,
                  **({"solid": True, "side": (x0 + 4, y1 - 14, x0 + 10, y1 - 8)} if y0 == 32 else {}))
            for (x0, y0, x1, y1) in run_rects(runs_of(cells))]


SS_TIDAL_CARGO = cell_runs((2, 2, 3), (3, 2, 3), (6, 2, 3), (7, 2, 3), (10, 2, 3), (11, 2, 3),
                           (12, 2, 3), (1, 6, 7), (2, 6, 7), (3, 6, 7), (4, 6, 7), (5, 6, 7),
                           (6, 6, 7), (8, 6, 7), (9, 6, 7), (11, 6, 7), (12, 6, 7), (13, 6, 7),
                           (1, 11, 12), (2, 11, 12), (3, 11, 12), (4, 11, 12), (5, 11, 12),
                           (6, 11, 12), (8, 11, 11), (9, 11, 11), (11, 11, 11), (12, 11, 11),
                           (15, 10, 12), (16, 10, 12))

# the cabins, four above (entered through their back walls) and four below
# (entered from the front), the dark between them beyond the rooms: each
# its back wall, its side walls and its furniture - the beds, the tables and
# chairs - a piece to each rectangle of their cells, 12 px
SS_TIDAL_CABIN_X = (1, 10, 19, 28)
# the beds whose sides would be their dark outline, projected: dressed with a
# plain patch of their blanket instead; and a bin on the carpet, the carpet
# round it left
SS_TIDAL_BLANKET = {"n0_1_2": (29, 41), "n0_6_2": (102, 41), "n1_10_2": (173, 41),
                    "n1_15_2": (246, 41), "n2_19_2": (317, 41), "n3_33_2": (534, 41),
                    "s0_1_11": (29, 185), "s0_6_11": (102, 185), "s2_24_11": (390, 185)}
SS_TIDAL_FURNITURE_KW = dict(
    {"furniture_" + k: {"solid": True, "side": (x, y, x + 6, y + 6)}
     for k, (x, y) in SS_TIDAL_BLANKET.items()},
    furniture_n1_10_5={"leave": ("de7b52", "bd5a41", "bd7341", "dea462")})
SS_TIDAL_FURNITURE = {
    "n0": "1,2 2,2 6,2 7,2 1,3 2,3 6,3 7,3 5,6 6,6 5,7 6,7 7,7",
    "s0": "1,11 2,11 6,11 7,11 1,12 2,12 6,12 7,12",
    "n1": "10,2 11,2 15,2 16,2 10,3 11,3 15,3 16,3 10,5 13,5 14,5 13,6 14,6",
    "s1": "15,11 16,11 11,12 12,12 15,12 16,12 11,13 12,13 16,13",
    "n2": "19,2 20,2 19,3 20,3 22,5 23,5 22,6 23,6 23,7",
    "s2": "19,11 20,11 24,11 25,11 19,12 20,12 24,12 25,12",
    "n3": "28,2 29,2 33,2 34,2 28,3 29,3 33,3 34,3 28,4 33,6 34,6 33,7 34,7",
    "s3": "28,11 29,11 28,12 29,12 31,12 32,12 33,12 34,12 28,13 31,13 32,13 33,13 34,13",
}


def ss_tidal_rooms():
    out = []
    for i, cx in enumerate(SS_TIDAL_CABIN_X):
        x0, x1 = cx * 16, (cx + 7) * 16
        for row, top, foot, end in (("n", 0, 32, 128), ("s", 144, 176, 272)):
            name = "%s%d" % (row, i)
            cells = [tuple(int(v) for v in c.split(",")) for c in SS_TIDAL_FURNITURE[name].split()]
            side = (x0, top, x0 + 16, foot)
            out += kw_pieces("furniture_" + name, cells, 12, SS_TIDAL_FURNITURE_KW)
            out += [
                piece("wall_" + name, [(x0, top, x1, foot)], 32, fill=16, foot=foot, side=side),
                piece("side_%s_w" % name, [], 32, side=side, walls=[((x0, end), (x0, foot))]),
                piece("side_%s_e" % name, [], 32, side=side, walls=[((x1, foot), (x1, end))]),
            ]
    return out


# Trainer Hill's entrance: a passage up to the elevator between the arms of
# two counters, which run along the lobby's short back wall, a back counter
# and a front one, the receptionists between them; the dark either side of
# the passage is beyond the room. The front counter's flap (17,10), which
# its script opens, stays drawn
TRAINER_HILL_COUNTERS = (
    cell_runs((7, 4, 4), (8, 4, 10), (10, 4, 9), (11, 4, 4))
    + [(x, 8) for x in range(0, 19) if x not in (8, 9, 10, 11)] + [(11, 8), (11, 9), (11, 10)]
    + [(x, 9) for x in (0, 1, 2, 3, 6, 7, 15)]
    + [(x, 10) for x in (0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15, 16, 18)])


def trainer_hill_entrance():
    shaft = (128, 0, 144, 32)
    lobby = (48, 112, 64, 128)
    # the west arm's sides dressed with its own teal (projected, its dark
    # outline)
    return kw_pieces("counter", TRAINER_HILL_COUNTERS, 12,
                     {"counter_8_4": {"solid": True, "side": (128, 64, 134, 70)}}) + rect_pieces(
        "benches", runs_of(cell_runs((1, 15, 15), (2, 15, 15), (3, 15, 15), (15, 15, 15),
                                     (16, 15, 15), (17, 15, 15))), 12) + [
        piece("wall", [(112, 0, 192, 32)], 32, fill=16, foot=32, side=shaft),
        piece("side_shaft_w", [], 32, side=shaft, walls=[((112, 112), (112, 32))]),
        piece("side_shaft_e", [], 32, side=shaft, walls=[((192, 32), (192, 112))]),
        piece("wall_w", [(0, 112, 112, 128)], 16, fill=8, foot=128, side=lobby),
        piece("wall_e", [(192, 112, 304, 128)], 16, fill=8, foot=128, side=lobby),
        piece("side_w", [], 16, side=lobby, walls=[((0, 272), (0, 128))]),
        piece("side_e", [], 16, side=lobby, walls=[((304, 128), (304, 272))]),
    ]


SPECS += [{"name": n, "interior": {"layout": lid, "ground": [g], "pieces": own_shell(f())}}
          for (n, lid, g, f) in (
              ("ss_tidal_corridor", "LAYOUT_SS_TIDAL_CORRIDOR", 0x2ba, ss_tidal_corridor),
              ("ss_tidal_lower_deck", "LAYOUT_SS_TIDAL_LOWER_DECK", 0x20d,
               lambda: crate_stacks(SS_TIDAL_CARGO) + room_shell(272, 208, 32)),
              ("ss_tidal_rooms", "LAYOUT_SS_TIDAL_ROOMS", 0x238, ss_tidal_rooms),
              # (named apart from Route 111's building of the same name)
              ("trainer_hill_entrance_room", "LAYOUT_TRAINER_HILL_ENTRANCE", 0x331,
               trainer_hill_entrance))]


# The Sky Pillar (#179). Outside, the foot of the tower (80 x 96; the rest
# of it is above the map's top edge): its middle stands 9 px before its
# corner posts (x 0-6 and 74-80, their feet on row 86, sand drawn under
# them), the door in the middle's foot, its last row 95. As deep as it is wide and as tall as the Battle
# Tower, its sides and roof laid with the brick beside its windows.
SPECS += [{
    "name": "sky_pillar_tower",
    "layout": "LAYOUT_SKY_PILLAR_OUTSIDE",
    "rect": (12, 0, 5, 6),
    "ground": [0x121, 0x2b7, 0x2ba],
    # under the corner posts, beside the middle's foot, the tiles draw sand
    "keep": [(0, 0, 80, 86), (6, 86, 74, 96)],
    "parts_art": flat(lambda: [
        PlanPrism("tower", [(0, 16), (80, 16), (80, 86), (74, 86), (74, 96), (6, 96), (6, 86),
                            (0, 86)], 0, 320, (8, 16, 96), top=Tile(8, 8, 16, 24))]),
    "exact": [(0, 0, 80, 96)],
}]


# Its floors (224 x 224): the back wall, the stairs' doorways painted on it,
# and the rubble, a mound to each cell (round="dome") shaped as the ellipse
# in its cell, a pixel short of its foot (taken in, the mound's base showed
# a row below it) - its rocks are drawn in the floor's own greys, so no
# colour tells the floor round them. The shaft in the middle (looked down into, its
# far wall lit) and the cracked floor of 2F and 4F, which the player falls
# through, stay drawn. Each rubble tile is modelled once, in the first floor
# that has it, and found again in the others (reuse_pieces): `o` the
# boulder (0x20f), `x` the stones (0x21f), `O` the boulder under the back
# wall (0x207). `X`, the stones there (0x217), is the stones' piece found
# again by its pixels: one of its own would be left the floor round them.
# Until Rayquaza has left for Sootopolis (VAR_SKY_PILLAR_STATE below 2) the
# game swaps in the `_CLEAN` layouts: less rubble, and 2F's and 4F's floors
# whole - their walls, and the rubble they keep found again.
SKY_PILLAR_RUBBLE = {
    "1F": """
.......O....O.
...x....ox....
....x.........
..............
..o...........
ox........o..x
o..........ox.
.............x
...o.........o
..x......o....
..............
x............o
""",
    "2F": """
........X.....
.....x.oo.....
......o...x...
..............
xx............
.............x
..............
..............
..............
...x.....x....
..x...........
.............x
""",
    "3F": """
....X.....X...
.....x...o....
.x...o..x.....
x.............
...o......x...
..x...........
..x..........o
..........o...
...........x..
o....x...x....
.o............
.......x....o.
""",
    "4F": """
........O.....
..oxxxoxxo...o
Xx........o...
.o............
..............
..x.........o.
..o..........o
x.o...........
x.............
.....o...o....
..o...........
.......o......
""",
    "5F": """
.....O...X....
..o.....x.....
...........o..
x..........o..
.x...........x
..............
..............
..o.........x.
...........x..
o.............
.........x..o.
.o...x........
""",
}
SKY_PILLAR_FLOORS = ("1F", "2F", "3F", "4F", "5F")


def sky_pillar_floor(floor=None):
    """A floor's walls, and its rubble of the tiles no floor before it has."""
    seen = set()
    for f in SKY_PILLAR_FLOORS[:SKY_PILLAR_FLOORS.index(floor)] if floor else ():
        seen |= set(SKY_PILLAR_RUBBLE[f]) - set(".\n")
    out = []
    for y, row in enumerate(SKY_PILLAR_RUBBLE[floor].split() if floor else ()):
        for x, c in enumerate(row):
            if c in "oxO" and c not in seen:
                out.append(piece("rubble_%d_%d" % (x, y + 2), [("ellipse", x * 16 + 8, y * 16 + 39.5, 8, 7.5)],
                                 9, round="dome"))
    return out + room_shell(224, 224, side=(0, 0, 16, 32))


SKY_PILLAR_GROUND = {"1F": 0x2bd, "2F": 0x234, "3F": 0x2bd, "4F": 0x234, "5F": 0x2bd}
SPECS += [{"name": "sky_pillar_%s%s" % (f.lower(), clean.lower()),
           "interior": {"layout": "LAYOUT_SKY_PILLAR_%s%s" % (f, clean),
                        "ground": [SKY_PILLAR_GROUND[f]],
                        "pieces": own_shell(sky_pillar_floor(None if clean else f))}}
          for f in SKY_PILLAR_FLOORS for clean in ("", "_CLEAN")]


# The Abandoned Ship (#179): the S.S. Tidal's twin, wrecked. Its cabins are
# the Tidal's, cabin for cabin - the beds, tables and bins are the Tidal's
# pieces found again (reuse_pieces); the chairs, not blocked, stay drawn as
# they do there - and so is the block of cabins down its corridors. What the wreck adds: passages broken through between
# cabins, a door burst open, and breaches in the walls and floor (blocked,
# drawn dark), which stay drawn.

def ship_cabin(name, x0, top, foot, end, gap=None, side_gaps=(), cells=7):
    """A cabin `cells` wide from x0: its back wall from `top`, standing on
    `foot` (round a passage broken through it: `gap`, (x0, x1)), and its side
    walls from it to `end`, open where a passage runs through them
    (`side_gaps`, [(side, z0, z1)])."""
    x1 = x0 + 16 * cells
    side = (x0, top, x0 + 16, foot)
    out = []
    for i, (a, b) in enumerate([(x0, gap[0]), (gap[1], x1)] if gap else [(x0, x1)]):
        out.append(piece("wall_%s%s" % (name, "_%d" % i if gap else ""), [(a, top, b, foot)], 32,
                         fill=16, foot=foot, side=side))
    for s, x in (("w", x0), ("e", x1)):
        cuts = sorted((z0, z1) for (t, z0, z1) in side_gaps if t == s)
        runs, z = [], foot
        for z0, z1 in cuts:
            runs.append((z, z0)); z = z1
        runs.append((z, end))
        for i, (a, b) in enumerate(runs):
            out.append(piece("side_%s_%s%s" % (name, s, "_%d" % i if len(runs) > 1 else ""), [], 32, side=side,
                             walls=[((x, b), (x, a)) if s == "w" else ((x, a), (x, b))]))
    return out


def ship_corridors_1f():
    """Two corridors either side of the dark between them, each round its
    block of cabins (the Tidal's: 26 px, its roof white, its doors - one of
    them burst - on its south face, painted on). By the back wall, a door to
    the stairs, walked into (drawn), and a stand of bottles."""
    side = (16, 0, 32, 32)
    return [
        piece("block_w", [(32, 57, 80, 159)], 26, solid=True, side=(32, 132, 48, 158)),
        piece("block_e", [(160, 57, 256, 159)], 26, solid=True, side=(160, 132, 176, 158)),
        piece("wall_w", [(0, 0, 112, 32)], 32, fill=16, foot=32, side=side),
        piece("wall_e", [(128, 0, 288, 32)], 32, fill=16, foot=32, side=side),
        piece("side_div_w", [], 32, side=side, walls=[((112, 32), (112, 192))]),
        piece("side_div_e", [], 32, side=side, walls=[((128, 192), (128, 32))]),
        piece("bottles", [(96, 35, 112, 53)], 12),
        piece("bottles_e", [(272, 35, 288, 53)], 12),
    ]


def ship_rooms_1f():
    """Four cabins: a passage broken between the two below (row 13), and one
    down from the north-east cabin through the south-east one's back wall."""
    return (ship_cabin("n0", 16, 0, 32, 128) + ship_cabin("n1", 160, 0, 32, 128)
            + ship_cabin("s0", 16, 144, 176, 272, side_gaps=[("e", 208, 224)])
            + ship_cabin("s1", 160, 144, 176, 272, gap=(208, 224), side_gaps=[("w", 208, 224)]))


def ship_rooms2_1f():
    """Two cabins, a passage broken down from the north one through the
    south one's back wall."""
    return ship_cabin("n0", 16, 0, 32, 128) + ship_cabin("s0", 16, 144, 176, 272, gap=(64, 80))


def ship_corridors_b1f():
    """Below: two blocks of cabins running off the map's top edge, their
    doors on their south faces - the north-west one's burst open, painted on,
    and the north-east one's (11,4) left drawn, its script changing it, a
    lintel over it - and the back wall beside the stairs, which stay drawn.
    Across the south, a third block's roof, its front past the map's edge:
    its cabins' doors are on its back edge, walked onto, as 1F's are."""
    side = (0, 0, 16, 32)
    return [
        piece("block_nw", [(32, 0, 128, 79)], 26, solid=True, side=(32, 54, 48, 78)),
        piece("block_ne_w", [(160, 0, 176, 79)], 26, solid=True, side=(160, 54, 176, 78)),
        piece("block_ne_lintel", [(176, 0, 192, 64)], 26, solid=True, foot=79, side=(160, 54, 176, 78)),
        piece("block_ne_e", [(192, 0, 208, 79)], 26, solid=True, side=(160, 54, 176, 78)),
        piece("block_s", [(64, 121, 208, 160)], 26, solid=True, side=(64, 121, 80, 147)),
        piece("wall_w", [(0, 0, 32, 32)], 32, fill=16, foot=32, side=side),
        piece("wall_mid", [(128, 0, 160, 32)], 32, fill=16, foot=32, side=side),
    ]


def ship_hidden_corridors():
    """The hidden floor's corridor round its block of cabins (one row higher
    than 1F's: its face rows 7-8). Its script changes the doors' bottoms at
    (3,8) and (6,8) and the indents on its back edge at (3,3) and (9,3):
    those cells are in no piece, a lintel over each door, the columns under
    the indents a row shorter. The flooded corner stays drawn."""
    side = (16, 0, 32, 32)
    face = (32, 117, 48, 142)
    return [piece("block_%d" % (x0 // 16), [(x0, y0, x1, y1)], 26, solid=True, side=face,
                  **({"foot": 143} if y1 < 143 else {}))
            for (x0, y0, x1, y1) in ((32, 57, 48, 143), (48, 64, 64, 128), (64, 57, 96, 143),
                                     (96, 57, 112, 128), (112, 57, 144, 143), (144, 64, 160, 143),
                                     (160, 57, 176, 143))] + [
        piece("wall", [(0, 0, 208, 32)], 32, fill=16, foot=32, side=side),
    ]


def ship_hidden_rooms():
    """Six cabins, wider than the others (12 and 13 cells), the dark between
    them beyond the rooms; a passage broken between the north-middle and
    north-east cabins (row 3). Their writing desks, each with its chair
    tucked in, are the Tidal's: its east column is the Tidal's piece found
    again, its west column (taller there in the Tidal's cabin) their own."""
    out = [piece("desk_%d_%d" % (x, y), [(x * 16, y * 16 + 2, x * 16 + 16, y * 16 + 31)], 10)
           for (x, y) in ((22, 2), (40, 2), (40, 10))]
    for i, (x0, n) in enumerate(((16, 12), (240, 13), (480, 13))):
        gaps_n = [("e", 48, 64)] if i == 1 else [("w", 48, 64)] if i == 2 else []
        out += ship_cabin("n%d" % i, x0, 0, 32, 112, side_gaps=gaps_n, cells=n)
        out += ship_cabin("s%d" % i, x0, 128, 160, 240, cells=n)
    return out


# the deck's own greens and the shadows on it, seen between a railing's bars
SHIP_DECK_SEE = ("acb494", "839473", "dedec5", "6a7b41")


def ship_deck():
    """The deck: its railings (the bars seen through, named as its edges -
    the harbor's and the shipyard's railings share their tiles, and a deck
    rail found there would take their pixels), the post at (7,4) - the
    shipyard's pilaster with another top; the others, and the side rails,
    are the shipyard's and the harbor's, found again - and the tall orange
    stacks (the Aqua Hideout's cargo, drawn alike: two tubes to a cell,
    banded at their feet), boxes 8 px deep with their drawing on them, the
    two in the north-west running off the map's top edge. Its levels - the
    hull, the cabins' fronts and the bridge, the stairs between them - stay
    drawn: what stands on them is terrain."""
    see = SHIP_DECK_SEE
    rails = (("low_w", (0, 192, 208, 208)), ("low_e", (240, 192, 368, 208)),
             ("mid_w", (48, 112, 160, 128)), ("mid_e", (192, 112, 224, 128)),
             ("isle_n", (0, 48, 32, 64)), ("isle_s", (0, 112, 32, 128)),
             ("bridge", (112, 48, 224, 64)), ("top", (144, 16, 192, 32)), ("east", (224, 80, 368, 96)))
    stacks = ((80, 0, 96, 64, 56), (96, 0, 112, 80, 72), (256, 32, 272, 80, 40),
              (272, 16, 288, 80, 56), (320, 32, 336, 80, 40))
    return ([piece("edge_rail_" + n, [r], 12, through=True, leave=see) for n, r in rails]
            + [piece("pilaster_7_4", [(112, 64, 128, 99)], 35, back=96, leave=see)]
            + [piece("stack_%d" % (x0 // 16), [(x0, y0, x1, y1)], h)
               for (x0, y0, x1, y1, h) in stacks])


ABANDONED_SHIP_ROOMS = [
    ("abandoned_ship_corridors_1f", "LAYOUT_ABANDONED_SHIP_CORRIDORS_1F", 0x203, ship_corridors_1f),
    ("abandoned_ship_rooms_1f", "LAYOUT_ABANDONED_SHIP_ROOMS_1F", 0x238, ship_rooms_1f),
    ("abandoned_ship_rooms2_1f", "LAYOUT_ABANDONED_SHIP_ROOMS2_1F", 0x238, ship_rooms2_1f),
    ("abandoned_ship_corridors_b1f", "LAYOUT_ABANDONED_SHIP_CORRIDORS_B1F", 0x202, ship_corridors_b1f),
    # three cabins in a row, the middle one's floor broken through to the
    # water below (drawn)
    ("abandoned_ship_rooms_b1f", "LAYOUT_ABANDONED_SHIP_ROOMS_B1F", 0x238,
     lambda: sum((ship_cabin("c%d" % i, x0, 0, 32, 128) for i, x0 in enumerate((16, 160, 304))), [])),
    # two cabins, a passage broken between them (row 4)
    ("abandoned_ship_rooms2_b1f", "LAYOUT_ABANDONED_SHIP_ROOMS2_B1F", 0x238,
     lambda: ship_cabin("w", 16, 0, 32, 128, side_gaps=[("e", 64, 80)])
     + ship_cabin("e", 160, 0, 32, 128, side_gaps=[("w", 64, 80)])),
    ("abandoned_ship_room_b1f", "LAYOUT_ABANDONED_SHIP_ROOM_B1F", 0x238,
     lambda: ship_cabin("c", 16, 0, 32, 128)),
    ("abandoned_ship_hidden_floor_corridors", "LAYOUT_ABANDONED_SHIP_HIDDEN_FLOOR_CORRIDORS", 0x202,
     ship_hidden_corridors),
    ("abandoned_ship_hidden_floor_rooms", "LAYOUT_ABANDONED_SHIP_HIDDEN_FLOOR_ROOMS", 0x238,
     ship_hidden_rooms),
    # the captain's office, in Stern's Shipyard's tiles: its model ship on
    # its desk and its blueprint table are the shipyard's, found again; its
    # back wall (the machines along it painted on) and the locker by the west
    # wall its own
    ("abandoned_ship_captains_office", "LAYOUT_ABANDONED_SHIP_CAPTAINS_OFFICE", 0x202,
     lambda: [piece("locker", [(0, 47, 16, 80)], 20),
              piece("wall", [(0, 0, 144, 32)], 32, fill=16, foot=32, side=(32, 0, 48, 32))]),
    ("abandoned_ship_deck", "LAYOUT_ABANDONED_SHIP_DECK", 0x228, ship_deck),
    # the flooded rooms the player dives to: their back walls (Underwater 2's
    # hole through to Underwater 1 painted on); the breaches stay drawn
    ("abandoned_ship_underwater1", "LAYOUT_ABANDONED_SHIP_UNDERWATER1", 0x2c9,
     lambda: [piece("wall", [(0, 0, 128, 32)], 32, fill=16, foot=32, side=(16, 0, 32, 32))]),
    ("abandoned_ship_underwater2", "LAYOUT_ABANDONED_SHIP_UNDERWATER2", 0x2cf,
     lambda: [piece("wall", [(0, 0, 336, 32)], 32, fill=16, foot=32, side=(16, 0, 32, 32))]),
]
SPECS += [{"name": n, "interior": {"layout": lid, "ground": [g], "pieces": own_shell(f())}}
          for (n, lid, g, f) in ABANDONED_SHIP_ROOMS]



# The Aqua Hideout (#179), in the Facility tiles of Stern's Shipyard and the
# harbor: its machines, desks, capsules, bins and crates are theirs, found
# again (reuse_pieces). Its own: the partitions between its rooms - their
# white tops and grey faces, 23 px, a piece to each rectangle of them, the
# floor cut away round them (`leave`, as Sootopolis's gym) - the back walls,
# the same partition with its top above the map's edge, and the railings
# round its pools and channels: the long ones a cell deep, the side ones
# half a cell, the floor and the water seen between their bars. The water,
# its rock banks and the steps down into it stay drawn. Each floor's map:
# `#` partition, `-` a long railing, `[` and `]` the west and east side
# railings.

AQUA_TANKS = {"1F": ((2, 16), (24, 19), (2, 23), (24, 24)), "B2F": ((17, 7), (21, 8), (25, 8), (28, 8))}
AQUA_FLOOR = ("acb494", "839473", "dedec5", "6a7b41", "8bb4ac", "a4cdbd", "9c8b94", "628b83", "739c8b")
AQUA_SEE = AQUA_FLOOR + ("526ad5", "29418b", "39529c", "6a83d5", "acc5e6", "8ba4de")
AQUA_MAPS = {
    "1F": """
.....######################.
.....#################.####.
.#####....................#.
.#####....................#.
.#........................#.
.#......------------......#.
.#......[..........]......#.
.#......[..........]......#.
.#......------------......#.
.#........................#.
.#........................#.
.#...--------..--------...#.
.#...[................]...#.
.#...[................]...#.
.#...[................]...#.
.#...[................]...#.
.#...-................-...#.
.#........................#.
.#........................#.
.#...-................-...#.
.#...[................]...#.
.#...[................]...#.
.#...[................]...#.
.#...[................]...#.
.#...-................-...#.
.#........................#.
.#........................#.
.#........................#.
.#........................#.
............................
""",
    "B1F": """
.......###########################......###########
.......#####.#####.##########.####......###########
########......#..........#.......#......#.........#
###.####......#..........#.......#......#.........#
#.............#..........#.......#......#.........#
#.............#..........#.......#......#.........#
#.............####################......#---------#
#.............####################......#[.......]#
#.............#..........#.......#......#---------#
#.............#..........#.......#......#.........#
#.............#..........#.......#......#---------#
#.............#..........#.......#......#[.......]#
##########################.......#......#---------#
##########################.......#......#.........#
#........................#########......#---------#
#........................#########......#[.......]#
#---------------.................#......#---------#
#[.............].................#......#.........#
#[.............].................#......#.........#
#---------------.................#......###########
#................................#......###########
#................................#.................
##################################.................
##################################.................
""",
    "B2F": """
......########################...#
......######.#####.###############
#######.......#..................#
###.###.......#..................#
#.....#.......#..................#
#.....#.......#..................#
#.....#.......#..................#
#.....#.......#..................#
#.....#.......#..................#
#.....#.......#..................#
###############..................#
###############..................#
#.............#..................#
#.............#..................#
#.............#.........##########
#-------------#.........##########
#..............---------.........#
#......................].........#
#......................-.........#
#................................#
#......................-.........#
#......................].........#
#......................-##########
#-----------------------##########
""",
}


def aqua_hideout(floor):
    rows = AQUA_MAPS[floor].strip("\n").split("\n")
    at = lambda ch: [(x, y) for y, r in enumerate(rows) for x, c in enumerate(r) if c == ch]
    out = rect_pieces("partition", runs_of(at("#")), 23, leave=AQUA_FLOOR)
    longs = run_rects(runs_of(at("-")))
    # a side railing's long faces, which the drawing never shows: the bars of
    # the floor's first long railing
    x0, y0 = longs[0][0], longs[0][1]
    bars = (x0 + 2, y0, x0 + 14, y0 + 8)
    out += [piece("edge_rail_%d_%d" % (a // 16, b // 16), [(a, b, c, d)], 12, through=True, leave=AQUA_SEE)
            for (a, b, c, d) in longs]
    # a west side railing two cells long or more is the harbor's or the
    # shipyard's, found again (theirs share its tiles): those single cells
    # long are the floor's own, and the east ones (mirrored) all are
    for ch, dx in (("[", 0), ("]", 8)):
        out += [piece("edge_rail_%s_%d_%d" % ("w" if ch == "[" else "e", a // 16, b // 16),
                      [(a + dx, b, a + dx + 8, d)], 12, through=True, leave=AQUA_SEE, side=bars)
                for (a, b, c, d) in run_rects(runs_of(at(ch))) if ch == "]" or d - b == 16]
    # the tanks with pipes and a control panel along the walls, each floor's
    # own (named as blocks: drawn alike, found from one floor in the other
    # they would stand twice)
    out += [piece("block_tank_%d_%d" % (x, y), [(x * 16, y * 16, x * 16 + 32, y * 16 + 32)], 20,
                  leave=AQUA_FLOOR)
            for (x, y) in AQUA_TANKS.get(floor, ())]
    return out


SPECS += [{"name": "aqua_hideout_%s" % f.lower(),
           "interior": {"layout": "LAYOUT_AQUA_HIDEOUT_%s" % f, "ground": [0x228],
                        "pieces": own_shell(aqua_hideout(f))}}
          for f in ("1F", "B1F", "B2F")]


# The Magma Hideout (#179): a cave, its rock walls and lava the cave's own
# (#179 asks whether they're the mountains' work), and the machines in it -
# seven kinds, each modelled in the room that has fewest of it and found
# again in the others (reuse_pieces): on 3F 2R the consoles with a dish on
# top, the cream tank and the drills boring into the rock, on 3F 3R the
# domed robot, on 2F 1R the tall capsule and the console under a dish (4F's
# small dish boxes are the same, found again), and on 4F, the only floor
# with them, the drills standing on the floor. The cave floor round them
# cut away (`leave`).

MAGMA_FLOOR = ("d59c8b", "bd8373", "9c6252")
MAGMA_MACHINES = {
    "3F_2R": [("console", 3, 6, 2, 3, 24), ("console", 5, 6, 2, 3, 24), ("tank", 20, 3, 2, 3, 30),
              ("drill", 7, 1, 2, 2, 16), ("drill", 9, 1, 2, 2, 16), ("drill", 11, 1, 2, 2, 16)],
    "3F_3R": [("robot", 18, 20, 2, 2, 16)],
    "2F_1R": [("capsule", 16, 16, 2, 3, 32), ("dish_console", 14, 16, 2, 3, 24)],
    "4F": [("drill_floor", 12, 2, 2, 2, 16), ("drill_floor", 14, 2, 2, 2, 16), ("drill_floor", 9, 3, 2, 2, 16),
           ("drill_floor", 47, 14, 2, 2, 16)],
}
SPECS += [{"name": "magma_hideout_%s" % f.lower(),
           "interior": {"layout": "LAYOUT_MAGMA_HIDEOUT_%s" % f, "ground": [0x3a2],
                        "pieces": [piece("%s_%d_%d" % (k, x, y), [(x * 16, y * 16, (x + w) * 16, (y + h) * 16)],
                                         ht, leave=MAGMA_FLOOR)
                                   for (k, x, y, w, h, ht) in machines]}}
          for f, machines in MAGMA_MACHINES.items()]
