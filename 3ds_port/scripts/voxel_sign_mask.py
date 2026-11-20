#!/usr/bin/env python3
"""Which pixels of a signpost cell are the sign.

A sign metatile draws the post over the ground it stands on. The upper layer
is always the sign; on the lower layer, a pixel is ground when its colour is
one the walkable cells around the sign draw on their own lower layer. What
remains, closed off from the cell's border by a flood fill, is the sign's
silhouette, one bit per pixel and row. A sign drawn as a post's
(voxel_cells.Layout.draws_post_sign) is its upper layer alone.
"""

from collections import deque


def ground_colours(pair, layout, x, y):
    """Lower-layer colours of the walkable cells next to (x, y)."""
    colours = set()
    for dx, dy in ((-1, 0), (1, 0), (0, 1), (-1, 1), (1, 1)):
        nx, ny = x + dx, y + dy
        if layout.off_map(nx, ny) or layout.blocked(nx, ny):
            continue
        m = layout.metatile(nx, ny)
        if pair.layer_pixels(m, 1):
            continue  # something is drawn over that ground; not a clean sample
        colours.update(pair.layer_pixels(m, 0).values())
    return colours


def cutout_mask(opaque, width=16, height=16):
    """Rows of bits: pixels not reachable from the border through empty ones."""
    if width <= 0 or height <= 0:
        return [0] * height
    solid = 0
    for x, y in opaque:
        if 0 <= x < width and 0 <= y < height:
            solid |= 1 << (y * width + x)
    row_mask = (1 << width) - 1
    outside = exterior_mask(solid, width, height)
    return [row_mask ^ ((outside >> (y * width)) & row_mask) for y in range(height)]


def exterior_mask(solid, width, height):
    """Find exterior pixels in a nonempty rectangle packed in row order."""
    row_mask = (1 << width) - 1
    full = (1 << (width * height)) - 1
    left = full // row_mask
    right = left << (width - 1)
    border = left | right | row_mask | (row_mask << ((height - 1) * width))
    empty = full ^ solid
    outside = border & empty
    frontier = outside
    while frontier:
        # Horizontal shifts must stop at the edge of each row. Vertical
        # shifts can simply be clipped to the empty pixels of the image.
        neighbors = (((frontier & ~right) << 1) | ((frontier & ~left) >> 1)
                     | (frontier << width) | (frontier >> width))
        frontier = neighbors & empty & ~outside
        outside |= frontier
    return outside


def metatile_mask(pair, layout, x, y):
    metatile = layout.metatile(x, y)
    if layout.draws_post_sign(metatile):
        # all its lower layer is ground, however the cells around draw theirs
        # (Dewford's sand, on both layers) and whatever it stands against
        # (the corner of Lavaridge's gym)
        return cutout_mask(pair.layer_pixels(metatile, 1))
    ground = ground_colours(pair, layout, x, y)
    pixels = {p: c for p, c in pair.layer_pixels(metatile, 0).items() if c not in ground}
    pixels.update(pair.layer_pixels(metatile, 1))
    return cutout_mask(pixels)


# A head as wide as its cell, or bigger than this, is a crown or a roof the
# sign stands in front of, not a part of it. A gym's sign is a pixel short of
# its cell's width, and so is the round top it draws in the cell north.
HEAD_MAX_WIDTH = 15
HEAD_MAX_PIXELS = 160


def head_mask(pair, sign_rows, north):
    """The part of a sign drawn in the cell north of it: a lamp's lantern,
    a gym sign's top.

    Only the upper layer is a candidate - what the GBA draws over the player,
    who walks behind the lamp - and only the pixels 8-connected to the sign's
    own top row. Returns 16 rows (0 for none).
    """
    if bin(sign_rows[0]).count("1") > HEAD_MAX_WIDTH:
        return [0] * 16  # a block that fills its cell has no lantern
    upper = set(pair.layer_pixels(north, 1))
    return connected_head(upper, sign_rows, HEAD_MAX_WIDTH, HEAD_MAX_PIXELS)


def connected_head(upper, sign_rows, max_width, max_pixels):
    seeds = [(x, 15) for x in range(16) if (x, 15) in upper
             and any(sign_rows[0] >> xx & 1 for xx in (x - 1, x, x + 1) if 0 <= xx < 16)]
    head, queue = set(seeds), deque(seeds)
    while queue:
        x, y = queue.popleft()
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                p = (x + dx, y + dy)
                if p in upper and p not in head:
                    head.add(p)
                    queue.append(p)
    if not head:
        return [0] * 16
    xs = [x for x, _ in head]
    if max(xs) - min(xs) + 1 > max_width or len(head) > max_pixels:
        return [0] * 16
    return cutout_mask(head)


def event_board_masks(pair, layout, x, y):
    """Recover a two-cell event board beside a wall over proven clean ground."""
    if (not layout.outdoor or (x, y) not in layout.signs or not layout.blocked(x, y)
            or any(layout.off_map(x, row) or layout.blocked(x, row)
                   for row in (y - 1, y + 1))
            or all(layout.off_map(x + offset, y) or layout.blocked(x + offset, y)
                   for offset in (-1, 1))):
        return None
    ground = layout.metatile(x, y + 1)
    lower = pair.layer_pixels(ground, 0)
    if len(lower) != 256 or any(lower.get(pixel) != colour
                               for pixel, colour in pair.layer_pixels(ground, 1).items()):
        return None
    body, north = layout.metatile(x, y), layout.metatile(x, y - 1)
    if any(pair.layer_pixels(metatile, 0) != lower for metatile in (body, north)):
        return None
    body_pixels, head_pixels = ({pixel for pixel, colour in pair.layer_pixels(metatile, 1).items()
                                if colour != lower.get(pixel)} for metatile in (body, north))
    rows = cutout_mask(body_pixels)
    head = connected_head(head_pixels, rows, 16, 256)
    if not any(rows) or not any(head):
        return None
    return rows, head, ground
