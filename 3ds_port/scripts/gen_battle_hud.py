#!/usr/bin/env python3
"""The 2D battle's info boxes and text box at screen resolution: the port's own art.

On the top screen the battle scene is magnified 1.4 times (CTR_BATTLE_ZOOM),
which no pixel art survives: the info boxes are drawn instead at the
screen's own resolution (3ds_battle_hud.c), smooth, in the shapes and colours
of the GBA's - the frame with its shadow, the HP and EXP bars, the status
pills - with a light gradient and a gloss on the panel, and the battle text
box becomes a floating panel with rounded corners.

Nothing here comes from the game. The shapes are geometry measured off the
GBA's boxes (in GBA pixels, relative to the box sprite's top-left corner) and
drawn here as rounded rectangles and polygons; the colours are its palette.
What does come from the game is drawn at run time from the game's own data:
the texts in the game's font, and the letters of the "HP" and "EXP" labels
and of the status pills (localized in the Spanish game), traced smooth.

Everything is drawn at 5x the screen (7 canvas pixels to a GBA pixel) and
box-filtered down, an element's origin on a whole screen pixel.

Output (little endian): b"EM3DHUD1", u16 count, then per element:
    u8 id, u8 pad, s16 x, s16 y, u16 w, u16 h, then w*h RGBA8 (straight
    alpha), rows top to bottom.
x, y place the element relative to its anchor, in screen pixels: a box's
frame and EXP fill to the box sprite's top-left corner, the HP bar's parts to
the bar's origin, a pill or the ball to the status icon's origin, the text
box's parts to the screen's top-left corner.
"""

import argparse
import math
import struct
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

MAGIC = b"EM3DHUD1"
SS = 5                 # canvas pixels per screen pixel
ZOOM = 1.4             # screen pixels per GBA pixel (CTR_BATTLE_ZOOM)
K = SS * ZOOM          # canvas pixels per GBA pixel

# Element ids, shared with 3ds_battle_hud.c (HUD_*).
FRAMES = {"single_opponent": 0, "single_player": 1, "double_opponent": 2,
          "double_player": 3, "safari": 4}
ID_HP_TRACK = 8
ID_HP_TRACK_CAPPED = 13
ID_HP_FILL = {"green": 9, "yellow": 10, "red": 11}
ID_EXP_FILL = 12
ID_PILL = {"psn": 16, "par": 17, "slp": 18, "frz": 19, "brn": 20}
ID_BALL = 21
ID_BOX_CAP = 24
ID_BOX_MIDDLE = 25

# The GBA's box geometry (healthbox_*.png): the outline's rectangle and the
# shadow's polygons beyond the one-pixel shadow ring round it.
SHAPES = {
    "single_opponent": ((2, 3, 92, 26), [[(92, 21), (101, 30), (12, 30), (9, 27), (2, 27), (2, 25)]]),
    "single_player": ((10, 3, 100, 34), [[(100, 20), (104, 20), (104, 35), (102, 37), (100, 38), (1, 38), (9, 30)]]),
    "double_opponent": ((2, 3, 92, 26), [[(92, 16), (101, 25), (92, 25)]]),
    "double_player": ((10, 3, 100, 26), [[(10, 16), (1, 25), (10, 25)]]),
    "safari": ((10, 3, 100, 34), [[(100, 20), (104, 20), (104, 35), (102, 37), (100, 38), (1, 38), (9, 30)]]),
}
# The player's EXP bar, under the box: x0, y0, x1, y1 of its inside.
EXP_RECT = (32, 34.6, 96, 37.0)
# The HP bar, from its origin: the whole bar, the white track, the inside.
HP_BAR = (0, 0, 66, 6)
HP_TRACK = (15, 0.8, 65.2, 5.2)
HP_INSIDE = (15.8, 1.5, 64.4, 4.5)
# Where a status pill takes the "HP" label's place (opponents', doubles'),
# the GBA swaps the bar's first two tiles for nothing and the frame's round
# end (HEALTHBOX_GFX_0, _65, UpdateStatusIconInHealthbox): no dark label end,
# the box's panel up to a round cap just left of the track.
HP_CAP_X = 12.8
# A status pill (24x8 on the GBA, its letters in the first 20 columns).
PILL = (0, 0, 20, 8)
STATUS_COLOURS = {  # sStatusIconColors (battle_interface.c), RGB5 to 8
    "psn": (192, 96, 192), "par": (184, 184, 24), "slp": (160, 160, 136),
    "frz": (136, 176, 224), "brn": (224, 112, 80),
}
FILL_GRADIENTS = {
    "green": [(0, (176, 255, 204)), (0.45, (98, 226, 142)), (1, (44, 168, 96))],
    "yellow": [(0, (255, 246, 150)), (0.45, (246, 208, 44)), (1, (196, 146, 8))],
    "red": [(0, (255, 176, 156)), (0.45, (240, 92, 64)), (1, (172, 46, 50))],
}
# The text box: screen pixels, the band from line 192 down.
BOX_BAND_TOP = 192
BOX_TOP = 195          # CTR_BATTLE_BOX_TOP
BOX_MARGIN = 8         # each side
BOX_BOTTOM_MARGIN = 3  # under it
BOX_CAP = 20           # the left cap's width; the right one is it mirrored


def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(len(a)))


def gradient_rows(stops, n):
    """n colours down a gradient of (t, rgb) stops."""
    out = []
    for i in range(n):
        t = i / max(1, n - 1)
        for k in range(len(stops) - 1):
            t0, c0 = stops[k]
            t1, c1 = stops[k + 1]
            if t <= t1 or k == len(stops) - 2:
                u = 0 if t1 == t0 else min(1, max(0, (t - t0) / (t1 - t0)))
                out.append(lerp(c0, c1, u))
                break
    return out


class Canvas:
    """RGBA float canvas at SS x the screen; GBA coordinates map through K."""

    def __init__(self, w, h, origin=(0.0, 0.0)):
        self.w, self.h = w, h
        self.ox, self.oy = origin           # canvas position of GBA (0, 0)
        self.img = Image.new("RGBA", (w, h), (0, 0, 0, 0))

    def gx(self, x):
        return self.ox + x * K

    def gy(self, y):
        return self.oy + y * K

    def mask(self):
        return Image.new("L", (self.w, self.h), 0)

    def paint(self, mask, stops, alpha=1.0):
        """Paints a vertical gradient over the mask's own extent."""
        box = mask.getbbox()
        if not box:
            return
        y0, y1 = box[1], box[3]
        rows = gradient_rows(stops, y1 - y0)
        column = Image.new("RGBA", (1, self.h))
        column.putdata([tuple(int(round(v)) for v in rows[min(max(y - y0, 0), len(rows) - 1)]) + (255,)
                        for y in range(self.h)])
        grad = column.resize((self.w, self.h), Image.NEAREST)
        if alpha < 1:
            mask = mask.point(lambda v: int(v * alpha))
        self.img.paste(grad, (0, 0), mask)

    def tint(self, mask, colour, alpha_rows):
        """Paints one colour with a per-row strength (0..1) under a mask."""
        column = Image.new("L", (1, self.h))
        column.putdata([int(255 * min(1.0, max(0.0, alpha_rows(y)))) for y in range(self.h)])
        strength = ImageChops.multiply(column.resize((self.w, self.h), Image.NEAREST), mask)
        self.img.paste(Image.new("RGBA", (self.w, self.h), colour + (255,)), (0, 0), strength)

    def rounded(self, x0, y0, x1, y1, r):
        m = self.mask()
        ImageDraw.Draw(m).rounded_rectangle((self.gx(x0), self.gy(y0), self.gx(x1) - 1, self.gy(y1) - 1),
                                            radius=r * K, fill=255)
        return m

    def capsule(self, x0, y0, x1, y1):
        return self.rounded(x0, y0, x1, y1, (y1 - y0) / 2)

    def corners(self, x0, y0, x1, y1, radii):
        """A rectangle with its own radius at each corner (top-left, top-right,
        bottom-right, bottom-left): each corner's square cut away and a
        quarter circle of its radius put back."""
        X0, Y0, X1, Y1 = self.gx(x0), self.gy(y0), self.gx(x1), self.gy(y1)
        m = self.mask()
        d = ImageDraw.Draw(m)
        d.rectangle((X0, Y0, X1 - 1, Y1 - 1), fill=255)
        for (cx, cy, sx, sy), r in zip(((X0, Y0, 1, 1), (X1, Y0, -1, 1), (X1, Y1, -1, -1), (X0, Y1, 1, -1)), radii):
            R = r * K
            if R <= 0:
                continue
            square = (min(cx, cx + sx * R), min(cy, cy + sy * R), max(cx, cx + sx * R) - 1, max(cy, cy + sy * R) - 1)
            d.rectangle(square, fill=0)
            q = self.mask()
            ImageDraw.Draw(q).ellipse((cx + sx * R - R, cy + sy * R - R, cx + sx * R + R - 1, cy + sy * R + R - 1), fill=255)
            clip = self.mask()
            ImageDraw.Draw(clip).rectangle(square, fill=255)
            m.paste(255, (0, 0), ImageChops.darker(q, clip))
        return m

    def bar(self, x0, y0, x1, y1):
        """A bar as the GBA's: its left end only softened, its right end round."""
        r = (y1 - y0) / 2
        return self.corners(x0, y0, x1, y1, (min(1.2, r), r, r, min(1.2, r)))

    def polygon(self, points):
        m = self.mask()
        ImageDraw.Draw(m).polygon([(self.gx(x), self.gy(y)) for x, y in points], fill=255)
        return m

    def out(self):
        return self.img.resize((self.w // SS, self.h // SS), Image.BOX)


def union(*masks):
    out = masks[0].copy()
    for m in masks[1:]:
        out.paste(255, (0, 0), m)
    return out


# ------------------------------------------------------------------ the boxes

def draw_frame(name):
    (x0, y0, x1, y1), polygons = SHAPES[name]
    # The canvas covers GBA (0, 0) .. (106, 40) of the box sprite.
    cv = Canvas(int(106 * K) // SS * SS + SS * 2, int(40 * K) // SS * SS + SS * 2)
    # The GBA's box is rounder at its top-left and bottom-right corners.
    big, small = 3.4, 1.6

    def radii(inset):
        return (max(big - inset, 0.6), max(small - inset, 0.4), max(big - inset, 0.6), max(small - inset, 0.4))

    shadow = union(cv.corners(x0 - 1, y0 - 1, x1 + 1, y1 + 1, radii(-1)), *[cv.polygon(p) for p in polygons])
    cv.paint(shadow, [(0, (92, 116, 106)), (1, (70, 92, 84))])
    outline = cv.corners(x0, y0, x1, y1, radii(0))
    cv.paint(outline, [(0, (40, 66, 12)), (1, (26, 48, 0))])
    bevel = cv.corners(x0 + 1, y0 + 1, x1 - 1, y1 - 1, radii(1))
    cv.paint(bevel, [(0, (230, 222, 190)), (1, (206, 196, 150))])
    inside = cv.corners(x0 + 2, y0 + 2, x1 - 2, y1 - 2, radii(2))
    # The panel: a warm cream gradient, an inner shadow along its edges and
    # a glassy light over its upper part with a fine bright line under the top.
    cv.paint(inside, [(0, (255, 255, 240)), (0.5, (255, 250, 222)), (1, (240, 230, 190))])
    edge = Image.eval(inside, lambda v: 255 - v).filter(ImageFilter.GaussianBlur(5))
    edge = Image.composite(edge, Image.new("L", edge.size, 0), inside)
    cv.img.paste(Image.new("RGBA", edge.size, (170, 150, 100, 255)), (0, 0), edge.point(lambda v: int(v * 0.55)))
    box = inside.getbbox()
    top, height = box[1], box[3] - box[1]
    cv.tint(inside, (255, 255, 255), lambda y: (max(0.0, 1 - (y - top) / (0.42 * height)) ** 1.6) * 0.7)
    cv.tint(inside, (255, 255, 255), lambda y: math.exp(-(((y - top) / height - 0.06) / 0.035) ** 2) * 0.8)
    if name == "single_player":
        ex0, ey0, ex1, ey1 = EXP_RECT
        cv.paint(cv.capsule(ex0 - 0.5, ey0 - 0.5, ex1 + 0.5, ey1 + 0.5), [(0, (52, 72, 62)), (1, (52, 72, 62))])
        cv.paint(cv.capsule(ex0, ey0, ex1, ey1), [(0, (70, 92, 84)), (1, (108, 128, 118))])
    return cv.out()


def draw_hp_track(capped=False):
    """The HP bar's frame and track, from its origin; capped, without the
    "HP" label's dark end (HP_CAP_X): its image then starts at the screen
    pixel the cap's left falls in (returned)."""
    x0, y0, x1, y1 = HP_BAR
    cv = Canvas(int((x1 + 1) * K) // SS * SS + SS, int((y1 + 1) * K) // SS * SS + SS)
    if capped:
        r = (y1 - y0) / 2
        cv.paint(cv.corners(HP_CAP_X, y0, x1, y1, (r, r, r, r)), [(0, (96, 122, 104)), (1, (60, 84, 70))])
    else:
        cv.paint(cv.bar(x0, y0, x1, y1), [(0, (96, 122, 104)), (1, (60, 84, 70))])
    cv.paint(cv.bar(*HP_TRACK), [(0, (255, 255, 255)), (1, (226, 232, 226))])
    cv.paint(cv.bar(*HP_INSIDE), [(0, (58, 58, 72)), (1, (96, 108, 102))])
    image = cv.out()
    if not capped:
        return 0, image
    px = math.floor(HP_CAP_X * ZOOM)
    return px, image.crop((px, 0, image.width, image.height))


def draw_fill(rect, stops, gloss):
    """A bar's fill at full length. Its image starts at the screen pixel its
    corner falls in (returned), the corner where it is on the bar."""
    x0, y0, x1, y1 = rect
    px, py = math.floor(x0 * ZOOM), math.floor(y0 * ZOOM)
    cv = Canvas(int((x1 - x0 + 2) * K) // SS * SS + SS, int((y1 - y0 + 2) * K) // SS * SS + SS,
                origin=(-px * SS, -py * SS))
    cv.paint(cv.capsule(x0, y0, x1, y1), stops)
    if gloss:
        cv.paint(cv.capsule(x0 + 0.8, y0 + 0.4, x1 - 0.8, y0 + 1.0), [(0, (255, 255, 255)), (1, (255, 255, 255))])
    return px, py, cv.out()


def draw_pill(colour):
    hi = tuple(min(255, int(c * 1.25 + 30)) for c in colour)
    lo = tuple(int(c * 0.72) for c in colour)
    x0, y0, x1, y1 = PILL
    cv = Canvas(int((x1 + 1) * K) // SS * SS + SS, int((y1 + 1) * K) // SS * SS + SS)
    cv.paint(cv.capsule(x0, y0, x1, y1), [(0, lo), (1, lo)])
    cv.paint(cv.capsule(x0 + 0.4, y0 + 0.4, x1 - 0.4, y1 - 0.4), [(0, hi), (0.5, colour), (1, lo)])
    return cv.out()


def draw_ball():
    cv = Canvas(int(8 * K) // SS * SS + SS, int(8 * K) // SS * SS + SS)
    cx, cy, r = cv.gx(3.5), cv.gy(3.5), 3.6 * K
    m = cv.mask()
    ImageDraw.Draw(m).ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
    cv.paint(m, [(0, (60, 54, 74)), (1, (40, 36, 52))])
    r2 = r - 0.7 * K
    top = cv.mask()
    ImageDraw.Draw(top).pieslice((cx - r2, cy - r2, cx + r2, cy + r2), 180, 360, fill=255)
    cv.paint(top, [(0, (255, 140, 120)), (1, (214, 60, 56))])
    bottom = cv.mask()
    ImageDraw.Draw(bottom).pieslice((cx - r2, cy - r2, cx + r2, cy + r2), 0, 180, fill=255)
    cv.paint(bottom, [(0, (255, 255, 255)), (1, (196, 196, 210))])
    band = cv.mask()
    ImageDraw.Draw(band).rectangle((cx - r, cy - 0.45 * K, cx + r, cy + 0.45 * K), fill=255)
    cv.paint(band, [(0, (50, 46, 62)), (1, (50, 46, 62))])
    for rr, col in ((1.5, (50, 46, 62)), (0.9, (255, 255, 255))):
        b = cv.mask()
        ImageDraw.Draw(b).ellipse((cx - rr * K, cy - rr * K, cx + rr * K, cy + rr * K), fill=255)
        cv.paint(b, [(0, col), (1, col)])
    return cv.out()


# ------------------------------------------------------------------ the text box

def draw_text_box():
    """The text box band (400 x 48 from line 192): the left cap and one middle column."""
    width, height = BOX_CAP + 4, 48
    cv = Canvas(width * SS, height * SS)

    def rr(x0, y0, x1, y1, r):
        m = cv.mask()
        ImageDraw.Draw(m).rounded_rectangle((x0 * SS, y0 * SS, x1 * SS - 1, y1 * SS - 1), radius=r * SS, fill=255)
        return m

    top, bottom = BOX_TOP - BOX_BAND_TOP, height - BOX_BOTTOM_MARGIN
    left, right = BOX_MARGIN, 400 - BOX_MARGIN
    # A soft shadow under the panel, in place of the GBA's dark border.
    sh = cv.mask()
    ImageDraw.Draw(sh).rounded_rectangle(((left + 1) * SS, (top + 1.5) * SS, (right - 1) * SS, (bottom + 2.5) * SS),
                                         radius=7 * SS, fill=120)
    sh = sh.filter(ImageFilter.GaussianBlur(1.6 * SS))
    cv.img.paste(Image.new("RGBA", sh.size, (30, 40, 34, 255)), (0, 0), sh)
    # The red frame (the GBA's), with a light rim along its top.
    frame = rr(left, top, right, bottom, 6.5)
    cv.paint(frame, [(0, (236, 104, 84)), (0.18, (222, 80, 62)), (1, (176, 52, 44))])
    rim = cv.mask()
    ImageDraw.Draw(rim).rounded_rectangle((left * SS, top * SS, right * SS - 1, bottom * SS - 1),
                                          radius=6.5 * SS, outline=255, width=int(0.8 * SS))
    cv.tint(rim, (255, 190, 170), lambda y: (max(0.0, 1 - y / (height * SS))) ** 2.5)
    # The grey edge and the teal panel, a soft gradient and a shadow under its top.
    cv.paint(rr(left + 6, top + 1, right - 6, bottom - 1, 5), [(0, (150, 158, 158)), (1, (118, 126, 128))])
    panel = rr(left + 7, top + 2, right - 7, bottom - 2, 4.5)
    cv.paint(panel, [(0, (128, 186, 186)), (0.5, (107, 165, 165)), (1, (88, 146, 148))])
    cv.tint(panel, (40, 70, 72), lambda y: max(0.0, 1 - (y - (top + 2) * SS) / (2.2 * SS)) * 0.27)
    img = cv.out()
    cap = img.crop((0, 0, BOX_CAP, height))
    middle = img.crop((BOX_CAP + 2, 0, BOX_CAP + 3, height))
    return cap, middle


# ------------------------------------------------------------------ output

def element(eid, x, y, image):
    image = image.convert("RGBA")
    w, h = image.size
    return struct.pack("<BBhhHH", eid, 0, x, y, w, h) + image.tobytes()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--preview", type=Path, help="also write a PNG sheet of every element")
    args = ap.parse_args()

    items = []
    for name, eid in FRAMES.items():
        items.append((eid, 0, 0, draw_frame(name)))
    x, track = draw_hp_track()
    items.append((ID_HP_TRACK, x, 0, track))
    x, track = draw_hp_track(capped=True)
    items.append((ID_HP_TRACK_CAPPED, x, 0, track))
    for colour, eid in ID_HP_FILL.items():
        items.append((eid, *draw_fill(HP_INSIDE, FILL_GRADIENTS[colour], True)))
    items.append((ID_EXP_FILL, *draw_fill(EXP_RECT, [(0, (170, 236, 255)), (0.5, (66, 196, 255)), (1, (30, 128, 214))],
                                          False)))
    for status, eid in ID_PILL.items():
        items.append((eid, 0, 0, draw_pill(STATUS_COLOURS[status])))
    items.append((ID_BALL, 0, 0, draw_ball()))
    cap, middle = draw_text_box()
    items.append((ID_BOX_CAP, 0, BOX_BAND_TOP, cap))
    items.append((ID_BOX_MIDDLE, BOX_CAP, BOX_BAND_TOP, middle))

    data = bytearray(MAGIC + struct.pack("<H", len(items)))
    for item in items:
        data += element(*item)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(bytes(data))

    if args.preview:
        sheet = Image.new("RGBA", (420, 40 + sum(i[3].height + 4 for i in items)), (60, 60, 60, 255))
        y = 4
        for _, _, _, im in items:
            sheet.alpha_composite(im.convert("RGBA"), (4, y))
            y += im.height + 4
        sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST).save(args.preview)


if __name__ == "__main__":
    main()
