"""Window and door glass found in a building's own drawing, with no hand
rectangles: voxel_night.building_mask uses it for every model.

The game paints its glass with a handful of cool, light colours (pale blue,
cyan, teal) - the same colours as water, blue roof tiles, steel railings,
white-blue walls and signs. Colour alone cannot tell them apart, so a pane is
a shape, judged in this order:

  1. glass colour: opaque, red >= 90, green > red, blue > red + 12, blue not
     far under green (sea green is not glass) and not dark (a dark grey-blue
     wall is not glass);
  2. a connected run of such texels (4-neighbours) is one pane. A window with
     crossbars is several panes, one a bar; the bars stay dark;
  3. size: at least 2 x 2 texels and 4 texels, at most 24 x 24 texels and 400
     (a door of the Pokemon Center is 14 x 16), and under 3 times longer than
     wide (a pillar or a strip of pale wall is not a window);
  4. a pane is a rectangle (or an arch): its texels fill 80 % of its box;
  5. a pane is framed: on at least three of its four sides, 75 % of the
     texels touching it are a darker frame colour (anything opaque and not
     glass whose r + g + b is under 420; the drawing's edge is not a frame:
     a pale wall strip along it is not a window). A white-blue wall panel
     leans on a pale pillar and a gray band, so it fails; a window or a door
     leans on its frame all round;
  6. a pane is glass, not a mosaic: dots (a texel whose four neighbours in the
     pane are all another colour) are under 8 % of it - the pale dotted wall
     panels of Dewford's houses fail here;
  7. the panes of one window (a gap of a bar or two between them) together are
     not a single flat colour of any size: they show two colours of glass or
     one strong blue (blue - red >= 75), or are small (64 texels or less). A
     big flat pale panel is a wall;
  8. a pane is not on the drawing's side edge (3 texels): the pale strip of a
     corner pillar is framed like a window and is not one;
  9. a pane stands on a wall: when the model's faces are known, at least half
     of its texels are read by faces that stand upright (not a roof's slope
     or a flat top). A roof's tiles, solar panels and the ball on a Pokemon
     Center's roof are framed rectangles of the same colours but lie on top.

The level written for a glass texel is voxel_night's: 128 + (r + g + b) / 6 -
the pane's own shading, so its pale glint is brighter than its lower half and
the bars between panes (not glass) are 0. tools/voxel_precompile.py derives
that level from the texel when it compiles the page, so the formula must not
change without it.

Hand data lives in voxel_night.py as overrides (NIGHT_EXCLUDE, NIGHT_WINDOWS,
NIGHT_DARK), never here.
"""

FRAME_SUM = 420          # r + g + b under this, not glass: a frame colour
MIN_SIDE, MAX_SIDE = 2, 24
MIN_AREA, MAX_AREA = 4, 400
FILL = 0.80              # of its box a pane fills (an arch's corners are cut)
SIDE_FRAMED = 0.70       # of the texels along one side that are frame or clear
SIDES_NEEDED = 3
MAX_ASPECT = 3.0         # long side / short side, exclusive: a pillar, a sill or a
                         # strip of pale wall is longer than any window or door
EDGE = 3                 # a pane this near the drawing's side is a pillar's strip
UPRIGHT = 0.35           # |normal y| under this: a face that stands, not a roof
MAX_DOTS = 0.08          # of a pane's texels that stand alone in it
STRONG_BLUE = 75         # blue - red of a glass colour that is glass by itself
FLAT_SMALL = 64          # texels of a one-colour window that may be glass
ON_WALL = 0.5            # of a pane's texels that upright faces read


def is_glass(pixel):
    red, green, blue, alpha = pixel
    return (alpha >= 128 and red >= 90 and green > red and blue > red + 12
            and blue >= green - 24 and red + green + blue >= 330)


def level(pixel):
    """The glow level of a glass texel (voxel_night.window_mask's)."""
    return min(255, 128 + (pixel[0] + pixel[1] + pixel[2]) // 6)


def _frame_like(pixel):
    red, green, blue, alpha = pixel
    return alpha >= 128 and not is_glass(pixel) and red + green + blue < FRAME_SUM


def _alone(pixels, inside, xpos, ypos):
    here = pixels[xpos, ypos][:3]
    around = [(xpos - 1, ypos), (xpos + 1, ypos), (xpos, ypos - 1), (xpos, ypos + 1)]
    return all(n in inside and pixels[n[0], n[1]][:3] != here for n in around)


def _windows(found, pixels, gap=2):
    """The panes kept: those of one window together (boxes within `gap`
    texels) must not be one flat pale colour, unless small."""
    group = list(range(len(found)))

    def root(i):
        while group[i] != i:
            group[i] = group[group[i]]
            i = group[i]
        return i

    for i, a in enumerate(found):
        for j in range(i):
            b = found[j]
            if (a[0] - gap <= b[2] and b[0] - gap <= a[2]
                    and a[1] - gap <= b[3] and b[1] - gap <= a[3]):
                group[root(i)] = root(j)
    members = {}
    for i in range(len(found)):
        members.setdefault(root(i), []).append(i)
    keep = []
    for ids in members.values():
        colours = {pixels[x, y][:3] for i in ids for x, y in found[i][4]}
        area = sum(len(found[i][4]) for i in ids)
        if (len(colours) >= 2 or any(c[2] - c[0] >= STRONG_BLUE for c in colours)
                or area <= FLAT_SMALL):
            keep += [found[i] for i in ids]
    return keep


def panes(art, wall=None, dark=()):
    """The panes of a drawing: [(left, top, right, bottom, texels)] (box
    exclusive at right and bottom), `texels` a list of (x, y). `wall`: the
    set of (x, y) texels upright faces read, or None when unknown. `dark`:
    rectangles where an unlit pane is not a glass colour but a dark one (see
    voxel_night.NIGHT_DARK) and every texel there is a pane."""
    width, height = art.size
    pixels = art.load()
    box = art.getchannel("A").point(lambda a: 255 if a >= 128 else 0).getbbox() or (0, 0, width, height)
    glass = bytearray(width * height)
    for ypos in range(height):
        for xpos in range(width):
            if is_glass(pixels[xpos, ypos]):
                glass[ypos * width + xpos] = 1
    seen = bytearray(width * height)
    found = []
    for start in range(width * height):
        if not glass[start] or seen[start]:
            continue
        seen[start] = 1
        pending, texels = [start], []
        while pending:
            index = pending.pop()
            xpos, ypos = index % width, index // width
            texels.append((xpos, ypos))
            for nx, ny in ((xpos - 1, ypos), (xpos + 1, ypos), (xpos, ypos - 1), (xpos, ypos + 1)):
                if 0 <= nx < width and 0 <= ny < height:
                    other = ny * width + nx
                    if glass[other] and not seen[other]:
                        seen[other] = 1
                        pending.append(other)
        left = min(x for x, _ in texels)
        right = max(x for x, _ in texels) + 1
        top = min(y for _, y in texels)
        bottom = max(y for _, y in texels) + 1
        w, h = right - left, bottom - top
        if left < box[0] + EDGE or right > box[2] - EDGE:
            continue
        if (w < MIN_SIDE or h < MIN_SIDE or w > MAX_SIDE or h > MAX_SIDE
                or not MIN_AREA <= len(texels) <= MAX_AREA
                or max(w, h) >= MAX_ASPECT * min(w, h)
                or len(texels) < FILL * w * h):
            continue
        # the frame: what touches the pane, side by side
        inside = set(texels)
        sides = [[0, 0], [0, 0], [0, 0], [0, 0]]      # [framed, all] west east north south
        for xpos, ypos in texels:
            for side, (nx, ny) in enumerate(((xpos - 1, ypos), (xpos + 1, ypos),
                                             (xpos, ypos - 1), (xpos, ypos + 1))):
                if (nx, ny) in inside:
                    continue
                sides[side][1] += 1
                if 0 <= nx < width and 0 <= ny < height and _frame_like(pixels[nx, ny]):
                    sides[side][0] += 1
        framed = sum(1 for ok, total in sides if total and ok >= SIDE_FRAMED * total)
        if framed < SIDES_NEEDED:
            continue
        if sum(1 for xpos, ypos in texels if _alone(pixels, inside, xpos, ypos)) > MAX_DOTS * len(texels):
            continue
        if wall is not None and sum(1 for t in texels if t in wall) < ON_WALL * len(texels):
            continue
        found.append((left, top, right, bottom, texels))
    found = _windows(found, pixels)
    for left, top, right, bottom in dark:
        found.append((left, top, right, bottom,
                      [(x, y) for y in range(top, bottom) for x in range(left, right)
                       if pixels[x, y][3] >= 128]))
    return found


def mask_of(art, wall=None, dark=()):
    """The glow mask of a drawing: a bytearray, width * height, 0 off the glass."""
    width, height = art.size
    pixels = art.load()
    mask = bytearray(width * height)
    for _, _, _, _, texels in panes(art, wall, dark):
        for xpos, ypos in texels:
            mask[ypos * width + xpos] = level(pixels[xpos, ypos])
    return mask


def upright_texels(tris, size):
    """The texels of a drawing that upright faces read: {(x, y)}. `tris`:
    the model's mesh, ((x, y, z, u, v) * 3, shade, tag) a triangle (y up,
    uv in texels of the drawing). A texel is read when its centre is inside
    the triangle's uv; a face edge-on in uv reads along its longest edge."""
    width, height = size
    seen = set()
    for tri, _, _ in tris:
        a, b, c = tri
        ux, uy, uz = (b[i] - a[i] for i in range(3))
        vx, vy, vz = (c[i] - a[i] for i in range(3))
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        length = (nx * nx + ny * ny + nz * nz) ** 0.5
        if length == 0 or abs(ny) / length >= UPRIGHT:
            continue
        uv = [(p[3], p[4]) for p in tri]
        (x0, y0), (x1, y1), (x2, y2) = uv
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        lo_x, hi_x = max(0, int(min(x0, x1, x2) + 1e-4)), min(width, int(-(-max(x0, x1, x2) // 1)))
        lo_y, hi_y = max(0, int(min(y0, y1, y2) + 1e-4)), min(height, int(-(-max(y0, y1, y2) // 1)))
        for ypos in range(lo_y, hi_y):
            for xpos in range(lo_x, hi_x):
                if abs(area) < 1e-6:
                    seen.add((xpos, ypos))
                    continue
                px, py = xpos + 0.5, ypos + 0.5
                d = ((x1 - x0) * (py - y0) - (y1 - y0) * (px - x0),
                     (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1),
                     (x0 - x2) * (py - y2) - (y0 - y2) * (px - x2))
                if all(v >= -1e-6 for v in d) or all(v <= 1e-6 for v in d):
                    seen.add((xpos, ypos))
    return seen


SPILL_STOREY = 40        # a pane whose lowest texel is higher (px) casts no spill
SPILL_GAP = 3            # panes this near along the wall (px) are one span
SPILL_MAX = 6            # spans a model keeps, widest first


def facade_spans(mask, tris, size):
    """Where the lit glass of a drawing stands on the ground floor, for the
    light it throws on the ground in front: [(x0, x1, z, y)] in pixels of
    the model (x east, z south, y the span's lowest glass above the ground),
    one a window or door of the south face. `mask`: the glow mask
    (bytearray, size[0] * size[1]); `tris`: the model's mesh. Panes of one
    wall that are a gap of SPILL_GAP or less apart are one span; a pane
    above SPILL_STOREY does not count (a window upstairs lights the
    window's own wall, not the street, and a tall building would fill the
    reserve)."""
    width, height = size
    best = {}                       # texel -> (x, y, z) of the south-most face
    for tri, _, _ in tris:
        a, b, c = tri
        ux, uy, uz = (b[i] - a[i] for i in range(3))
        vx, vy, vz = (c[i] - a[i] for i in range(3))
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        length = (nx * nx + ny * ny + nz * nz) ** 0.5
        if length == 0 or abs(nz) / length < 0.8:
            continue
        (u0, v0), (u1, v1), (u2, v2) = [(p[3], p[4]) for p in tri]
        det = (u1 - u0) * (v2 - v0) - (u2 - u0) * (v1 - v0)
        if abs(det) < 1e-6:
            continue
        for ypos in range(max(0, int(min(v0, v1, v2) + 1e-4)), min(height, int(-(-max(v0, v1, v2) // 1)))):
            for xpos in range(max(0, int(min(u0, u1, u2) + 1e-4)), min(width, int(-(-max(u0, u1, u2) // 1)))):
                if not mask[ypos * width + xpos]:
                    continue
                pu, pv = xpos + 0.5, ypos + 0.5
                l1 = ((pu - u0) * (v2 - v0) - (u2 - u0) * (pv - v0)) / det
                l2 = ((u1 - u0) * (pv - v0) - (pu - u0) * (v1 - v0)) / det
                l0 = 1.0 - l1 - l2
                if min(l0, l1, l2) < -1e-6:
                    continue
                point = tuple(l0 * a[i] + l1 * b[i] + l2 * c[i] for i in range(3))
                if (xpos, ypos) not in best or point[2] > best[(xpos, ypos)][2]:
                    best[(xpos, ypos)] = point
    walls = {}                      # z plane (px, rounded) -> [(x, y)]
    for x, y, z in best.values():
        walls.setdefault(round(z), []).append((x, y))
    spans = []
    for z, points in walls.items():
        points.sort()
        run = [points[0]]
        for point in points[1:] + [None]:
            if point is not None and point[0] - run[-1][0] <= SPILL_GAP:
                run.append(point)
                continue
            low = min(y for _, y in run)
            if low <= SPILL_STOREY:
                spans.append((min(x for x, _ in run) - 0.5, max(x for x, _ in run) + 0.5, z, low))
            if point is not None:
                run = [point]
    spans.sort(key=lambda s: s[0] - s[1])
    return sorted(spans[:SPILL_MAX])
