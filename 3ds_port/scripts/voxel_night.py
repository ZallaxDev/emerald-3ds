"""Exterior glass: the panes found in a building's own drawing
(voxel_windows), plus the few hand corrections kept here as data.

building_mask is the glow mask of a drawing. The classifier finds the glass;
NIGHT_WINDOWS (rectangles of pixels where every glass-coloured texel glows),
NIGHT_DARK (rectangles of unlit dark panes) and NIGHT_EXCLUDE (rectangles
where nothing glows) are per-model corrections, never rules for a map.
Recolouring frames or roof art is never allowed: only opaque texels glow.
"""

import voxel_windows

# Where the classifier is wrong: rectangles (left, top, right, bottom) of a
# model's drawing where nothing glows. Filled in from the contact sheets
# (voxel_window_sheets.py).
# "all" for a whole drawing.
NIGHT_EXCLUDE = {
    # pillar capitals under the roof
    "contest_hall": "all",
    # a blue door and its corner strips; the little window is not glass-coloured
    "verdanturf_house_4": "all",
    "verdanturf_house_5": "all",
    # wooden doors with pale frames
    "lilycove_house_4": ((17, 45, 29, 63), (3, 56, 10, 63)),
    "lilycove_house_5": ((31, 45, 46, 63),),
    # the balustrade's blue squares and the pillars below the windows
    "lilycove_museum": ((0, 70, 160, 288),),
    # the roof's red and pale stripes
    "mossdeep_house_wide": "all",
    # window sills and the doorway's side lights
    "frontier_hall_porch": "all",
    # white scraps on a door's frame and an arch's capital
    "battle_arena": "all",
    "frontier_arch": "all",
    "frontier_house": ((7, 40, 17, 60), (32, 40, 42, 60), (7, 170, 17, 190)),
}

# Drawings whose name starts so have no windows (the Cycling Road's steel
# deck is glass-coloured and framed, and no glass).
NIGHT_NO_GLASS = ("cycling_road_", "parked_bikes")

# Unlit dark panes (not a glass colour) that are windows: every opaque texel
# of the rectangle glows at the dark level.
NIGHT_DARK = {}

NIGHT_WINDOWS = {
    "dewford_house": ((34, 46, 39, 52), (41, 46, 46, 52)),
    "dewford_hall": ((50, 46, 55, 52), (57, 46, 62, 52)),
    "dewford_gym_exterior": ((11, 50, 21, 53), (27, 50, 37, 53), (75, 50, 85, 53),
                             (49, 62, 55, 77), (57, 62, 63, 77)),
    "littleroot_house_w": ((17, 62, 23, 68), (25, 62, 31, 68)),
    "littleroot_house_e": ((49, 62, 55, 68), (57, 62, 63, 68)),
    "littleroot_lab": tuple((xpos, top, xpos + 5, bottom)
                           for xpos in (18, 25, 34, 41) for top, bottom in ((61, 63), (64, 68))),
    "oldale_house": tuple((xpos, top, xpos + 5, bottom)
                          for xpos in (34, 41) for top, bottom in ((43, 49), (50, 53)))
                    + ((20, 48, 28, 52),),
    "kit_house_4": tuple((xpos, top, xpos + 5, bottom)
                         for xpos in (34, 41) for top, bottom in ((43, 45), (46, 49), (50, 53)))
                   + ((20, 48, 28, 52),),
    "kit_house_5": tuple((xpos, top, xpos + 5, bottom)
                         for xpos in (18, 25, 50, 57) for top, bottom in ((43, 45), (46, 49), (50, 53)))
                   + ((36, 48, 44, 52),),
    "flower_shop": tuple((xpos, top, xpos + 6, bottom)
                         for xpos in (9, 65, 81) for top, bottom in ((42, 48), (49, 52))),
    "pokemon_center": ((17, 45, 31, 61),),
    "poke_mart": ((17, 45, 31, 61),),
    "gym": ((11, 50, 21, 53), (27, 50, 37, 53), (75, 50, 85, 53),
            (49, 62, 55, 77), (57, 62, 63, 77)),
    "gym_rustboro": ((11, 50, 21, 53), (27, 50, 37, 53), (75, 50, 85, 53),
                     (49, 62, 55, 77), (57, 62, 63, 77)),
    "devon_corporation": tuple((xpos, top, xpos + 4, top + 12)
                               for xpos in (18, 42, 122, 138) for top in (42, 74))
                         + tuple((xpos, top, xpos + 2, top + 5)
                                 for xpos in (55, 71, 87, 103) for top in (58, 66, 90, 98))
                         + tuple((xpos, 98, xpos + 4, 102)
                                 for xpos in (10, 26, 42, 114, 130, 146))
                         + tuple((xpos, 105, xpos + 4, 110)
                                 for xpos in (10, 18, 26, 34, 42, 114, 122, 130, 138, 146)),
}


def outdoor_layouts():
    """The layouts of the outdoor maps (towns, cities, routes, ocean routes):
    the only ones whose glass is lit. From the game's own map files."""
    import glob
    import json
    import os
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    out = set()
    for path in glob.glob(os.path.join(root, "data", "maps", "*", "map.json")):
        with open(path, encoding="utf-8") as f:
            info = json.load(f)
        if info.get("map_type") in ("MAP_TYPE_TOWN", "MAP_TYPE_CITY", "MAP_TYPE_ROUTE",
                                    "MAP_TYPE_OCEAN_ROUTE") and "layout" in info:
            out.add(info["layout"])
    return out


def window_mask(art, rectangles):
    width, height = art.size
    mask = bytearray(width * height)
    pixels = art.load()
    for left, top, right, bottom in rectangles:
        if not (0 <= left < right <= width and 0 <= top < bottom <= height):
            raise ValueError("window rectangle is outside the building art")
        for ypos in range(top, bottom):
            for xpos in range(left, right):
                red, green, blue, alpha = pixels[xpos, ypos]
                if alpha >= 128 and red >= 90 and green > red and blue > red + 12:
                    mask[ypos * width + xpos] = min(255, 128 + (red + green + blue) // 6)
    return mask


def component_rectangles(art, predicate, top):
    pixels = art.load()
    remaining = {(xpos, ypos) for ypos in range(top, art.height)
                 for xpos in range(art.width) if predicate(pixels[xpos, ypos])}
    rectangles = []
    while remaining:
        pending = [remaining.pop()]
        component = []
        while pending:
            xpos, ypos = pending.pop()
            component.append((xpos, ypos))
            for neighbor in ((xpos - 1, ypos), (xpos + 1, ypos),
                             (xpos, ypos - 1), (xpos, ypos + 1)):
                if neighbor in remaining:
                    remaining.remove(neighbor)
                    pending.append(neighbor)
        rectangles.append((min(xpos for xpos, ypos in component),
                           min(ypos for xpos, ypos in component),
                           max(xpos for xpos, ypos in component) + 1,
                           max(ypos for xpos, ypos in component) + 1))
    return sorted(rectangles)


def building_mask(art, name, mesh=None, wall=None):
    """The glow mask of the drawing `art` of model `name` (a bytearray, one
    level a texel, 0 for none). `mesh`: the model's triangles, when known -
    the classifier then keeps to the texels upright faces read (no roofs);
    `wall` is that set when the caller already has it."""
    name = name.removesuffix("_bare")
    if name.startswith(NIGHT_NO_GLASS) and name not in NIGHT_WINDOWS:
        return bytearray(art.width * art.height)
    if wall is None and mesh:
        wall = voxel_windows.upright_texels(mesh, art.size)
    found = voxel_windows.mask_of(art, wall, NIGHT_DARK.get(name, ()))
    exclude = NIGHT_EXCLUDE.get(name, ())
    if exclude == "all":
        exclude = ((0, 0, art.width, art.height),)
    for left, top, right, bottom in exclude:
        for ypos in range(top, bottom):
            for xpos in range(left, right):
                found[ypos * art.width + xpos] = 0
    rectangles = NIGHT_WINDOWS.get(name, ())
    if name.startswith("rustboro_stone_"):
        frames = component_rectangles(art, lambda pixel: pixel == (65, 74, 106, 255), 48)
        rectangles = tuple(rect for rect in frames
                           if rect[2] - rect[0] == 10 and rect[3] - rect[1] == 10)
    elif name.startswith("rustboro_olive_"):
        panes = component_rectangles(art, lambda pixel: pixel[3] >= 128 and pixel[0] >= 90
                                     and pixel[1] > pixel[0] and pixel[2] > pixel[0] + 12, 48)
        rectangles = tuple(rect for rect in panes
                           if 2 <= rect[2] - rect[0] <= 12 and 2 <= rect[3] - rect[1] <= 12)
    mask = window_mask(art, rectangles)
    for index, value in enumerate(found):
        if value > mask[index]:
            mask[index] = value
    if name == "devon_corporation":
        for left, top, right, bottom in rectangles:
            for ypos in range(top, bottom):
                for xpos in range(left, right):
                    red, green, blue, alpha = art.getpixel((xpos, ypos))
                    if alpha >= 128 and red >= 32 and green > red + 2 and blue > red + 12:
                        mask[ypos * art.width + xpos] = 180
    if rectangles and not any(mask):
        raise ValueError("%s has no glass pixels in its authored windows" % name)
    return mask
