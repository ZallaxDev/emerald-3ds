"""Fortree's treehouse: its leafy roof front is a slope with the source
artwork projected at its natural density, its rear continues the same
courses; the deck and bridges stay terrain (preserve_ground, own pixels) and
are never raised as obstacles.
"""
from voxel_building import Band, Prism, Proj, Strip, Tile


def house(layout_id, rect, ground, foot, facade, inset, roof_band):
    from voxel_building import LayoutArt
    layout = LayoutArt(layout_id)
    art = layout.building_art(*rect, layout.ground_tiles(ground))
    width = rect[2] * 16
    parts = []
    # Roof columns follow the drawing's crown instead of giving its round
    # finial or leafy corners a rectangular ridge behind the transparent art.
    crowns = []
    for x in range(width):
        rows = [v for v in range(facade) if art.getpixel((x, v))[3] >= 128]
        crowns.append(min(rows) if rows else None)
    spans = []
    x = 0
    while x < width:
        end = x + 1
        while end < width and crowns[end] == crowns[x] and (end < inset or end >= width - inset) == (x < inset or x >= width - inset):
            end += 1
        if crowns[x] is not None:
            spans.append((x, end, crowns[x]))
        x = end
    for x, end, top in spans:
        wall_height = foot - facade
        back = wall_height + top
        ridge_z = foot - (facade - top) / 2
        ridge_y = wall_height + (facade - top) / 2
        roof_tile = Tile(16, roof_band[0], 20, roof_band[1])
        parts.append(Prism("roof_%d" % x, x, end,
            [(foot, wall_height), (ridge_z, ridge_y), (back, wall_height)],
            edges={0: Proj(top, facade), 1: Strip(roof_band, repeat=roof_band, wrap=(16, 32))},
            skip=(2,), caps=[Band(wall_height, ridge_y + 1, roof_tile, foot)]))
        if inset <= x < width - inset:
            side = Tile(inset, facade, inset + 4, foot)
            parts.append(Prism("wall_%d" % x, x, end,
                [(foot, 0), (foot, wall_height), (back, wall_height), (back, 0)],
                edges={0: Proj(facade, foot), 2: side}, skip=(1, 3),
                caps=[Band(0, wall_height + 1, side, foot)]))
    return parts


def structural_pixels(*zones):
    """Only painted roof/wall pixels; deck, foliage and water stay terrain."""
    def own(art):
        return {(x, y) for x0, y0, x1, y1 in zones
                for x in range(x0, x1) for y in range(y0, y1)
                if art.getpixel((x, y))[3] >= 128}
    return own


SPECS = [
    {
        "name": "fortree_treehouse", "layout": "LAYOUT_FORTREE_CITY",
        "rect": (9, 1, 3, 3), "ground": [0x001, 0x0C6, 0x0C7, 0x22A],
        "parts": lambda: house("LAYOUT_FORTREE_CITY", (9, 1, 3, 3), [0x001, 0x0C6, 0x0C7, 0x22A], 48, 16, 0, (4, 16)),
        "exact": [(0, 0, 48, 48, True), (2, 16, 46, 48)],
        "preserve_ground": True,
        "own": structural_pixels((0, 0, 48, 16), (2, 16, 46, 48)),
    },
]
