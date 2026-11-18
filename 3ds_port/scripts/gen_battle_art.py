#!/usr/bin/env python3
"""The bottom screen's battle art: the port's own, drawn here from geometry.

The battle menus (3ds_bottom_battleplate.inc, 3ds_bottom_battledraw.inc) stand on a teal
backdrop with an engraved Poke Ball, and their buttons are plates in the
style of the DS/3DS touch menus: an ink outline with cut corners, a cream
section on the left with a faint Poke Ball and a diagonal cut into a strong
colour, a light inner line, gloss, a darker lower band, fine scanlines and a
soft drop shadow. Everything is drawn at 4x, smooth, and box-filtered down to
the 320x240 screen. Nothing here comes from the game: the texts, the item and
type icons and FIGHT's Rayquaza are the game's own and are drawn at run time.

A plate's colour is chosen at run time (FIGHT's red, BAG's yellow, each
move's type), so plates are stored in a form that takes any colour. Every
step of the drawing is linear in the colour under the plate (bg), the
plate's colour (base) and the focus ring's colour (F), so each output pixel
is, per channel,

    out = bg * (255 - alpha) / 255 + base * A / 255 + F * R / 255 + C

The script draws each element four times (all black; then bg, base and F
white one at a time) and reads alpha, A, R and C off the differences.

Output (little endian): b"EM3DBTA1", u16 count, then per element:
    u8 id, u8 flags, s16 x, s16 y, u16 w, u16 h
    flags 1: alpha, A (u8 each), C (u16 RGB565), all w*h, rows top to bottom
    flags 2: also R (u8, w*h) after C
    flags 4: only alpha (a clip mask)
    flags 8: only C, w*h u16 in the bottom screen's own layout (columns,
             each from the bottom row up): the backdrop, copied as it is
x, y place the element's box relative to its anchor (a plate's top left
corner; a party ball's centre).
"""

import argparse
import math
import struct
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

MAGIC = b"EM3DBTA1"
SS = 4
W, H = 320, 240
WHITE = (255, 255, 255)

# Element ids, shared with 3ds_bottom_ui.c (BTA_*).
ID_BACKDROP = 0
SHAPES = {  # id base, size, cut, cream width, cream slope, coloured head
    "fight_w": (16, 224, 74, 74 * 0.36, 60, 0.5, 0),
    "fight_f": (19, 284, 74, 74 * 0.36, 60, 0.5, 0),
    "ball": (22, 78, 74, 7, 34, 0.5, 0),
    "bottom": (25, 100, 72, 7, 24, 0.36, 0),
    "move": (28, 150, 80, 7, 0, 0.5, 24),
    "cancel": (31, 152, 26, 7, 0, 0.5, 0),
    "target": (34, 150, 72, 7, 30, 0.5, 0),
}
STATES = ("normal", "focus", "pressed")
ID_CLIP_FIGHT_W, ID_CLIP_FIGHT_F = 40, 41
ICONS = {"bag": 48, "run": 49, "near": 50, "block": 51}
BALLS = {"ok": 56, "status": 57, "faint": 58, "empty": 59}
SINK = 2          # a pressed plate sinks this many pixels
MARGIN = (6, 6, 6, 8)  # room for the ring and the shadow: left, top, right, bottom


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def rgb(c):
    return tuple(max(0, min(255, round(v))) for v in c)


def S(*v):
    return [c * SS for c in v]


# ------------------------------------------------------------------ primitives

class Pic:
    def __init__(self, w, h, fill):
        self.w, self.h = w, h
        self.im = Image.new("RGB", (w * SS, h * SS), rgb(fill))

    def paint(self, colour, mask, alpha=1.0):
        if alpha < 1:
            mask = mask.point(lambda v: round(v * alpha))
        self.im.paste(Image.new("RGB", self.im.size, rgb(colour)), (0, 0), mask)

    def paint_img(self, img, mask):
        self.im.paste(img, (0, 0), mask)

    def blank(self):
        return Image.new("L", self.im.size, 0)

    def down(self):
        return self.im.resize((self.w, self.h), Image.BOX)


def mul(a, b):
    return ImageChops.multiply(a, b)


def poly(pic, pts):
    m = pic.blank()
    ImageDraw.Draw(m).polygon([(x * SS, y * SS) for x, y in pts], fill=255)
    return m


def vgrad(pic, y, h, stops):
    """A picture-sized image ramping through colour stops over rows [y, y+h)."""
    n = max(2, round(h * SS))
    line = Image.new("RGB", (1, n))
    px = line.load()
    for i in range(n):
        t = i / (n - 1)
        for k in range(len(stops) - 1):
            t0, c0 = stops[k]
            t1, c1 = stops[k + 1]
            if t0 <= t <= t1:
                px[0, i] = rgb(mix(c0, c1, (t - t0) / max(1e-6, t1 - t0)))
                break
    full = Image.new("RGB", pic.im.size, rgb(stops[0][1]))
    full.paste(line.resize((pic.im.size[0], n), Image.NEAREST), (0, round(y * SS)))
    rest = pic.im.size[1] - round(y * SS) - n
    if rest > 0:
        full.paste(Image.new("RGB", (pic.im.size[0], rest), rgb(stops[-1][1])), (0, round(y * SS) + n))
    return full


def chamfer_pts(x, y, w, h, c, inset=0.0):
    k = inset
    cc = c - k * (math.sqrt(2) - 1)
    x0, y0, x1, y1 = x + k, y + k, x + w - k, y + h - k
    return [(x0 + cc, y0), (x1 - cc, y0), (x1, y0 + cc), (x1, y1 - cc), (x1 - cc, y1), (x0 + cc, y1),
            (x0, y1 - cc), (x0, y0 + cc)]


def scanlines(pic):
    m = pic.blank()
    d = ImageDraw.Draw(m)
    for y in range(0, pic.h, 2):
        d.rectangle([0, y * SS, pic.im.size[0], y * SS + SS - 1], fill=255)
    return m


# ------------------------------------------------------------------ the backdrop

TEAL = {"top": (58, 156, 134), "mid": (70, 174, 148), "low": (44, 128, 112), "ring": (40, 120, 104),
        "ringHi": (104, 196, 170), "ringLo": (30, 96, 84), "bar": (18, 44, 52), "bar2": (30, 66, 74),
        "mint": (156, 226, 206), "dots": (34, 108, 94)}


def backdrop():
    pic = Pic(W, H, (0, 0, 0))
    sz = pic.im.size
    full = Image.new("L", sz, 255)
    pic.paint_img(vgrad(pic, 0, H, [(0, TEAL["top"]), (0.45, TEAL["mid"]), (1, TEAL["low"])]), full)
    rg = Image.radial_gradient("L").resize((sz[0] * 13 // 10, sz[1] * 15 // 10))
    vig = Image.new("L", sz, 255)
    vig.paste(rg, ((sz[0] - rg.width) // 2, (sz[1] - rg.height) // 2))
    vig = vig.point(lambda v: max(0, min(255, (v - 120) * 2)))
    pic.paint((22, 70, 64), vig, 0.55)
    # halftone dots toward the sides
    dots = pic.blank()
    d = ImageDraw.Draw(dots)
    for gy in range(14, H - 14, 4):
        for gx in range(0, W, 4):
            e = min(gx, W - gx) / 46
            if e >= 1:
                continue
            r = (1 - e) * 1.5
            cx = gx + (2 if (gy // 4) % 2 else 0)
            d.ellipse([(cx - r) * SS, (gy - r) * SS, (cx + r) * SS, (gy + r) * SS], fill=255)
    pic.paint(TEAL["dots"], dots, 0.55)
    # the Poke Ball, engraved: its outline, the band running out to the sides
    # with a step (a circuit), the centre button
    cx, cy, R = 160, 124, 104
    shapes = pic.blank()
    sd = ImageDraw.Draw(shapes)

    def ring(r_out, r_in):
        sd.ellipse(S(cx - r_out, cy - r_out, cx + r_out, cy + r_out), fill=255)
        sd.ellipse(S(cx - r_in, cy - r_in, cx + r_in, cy + r_in), fill=0)
    ring(R + 9, R)
    band = 7
    for side in (-1, 1):
        def X(dx):
            return cx + side * dx
        pts = [(X(R + 4), cy - band), (X(R + 26), cy - band), (X(R + 34), cy - band + 8),
               (0 if side < 0 else W, cy - band + 8), (0 if side < 0 else W, cy + band + 8),
               (X(R + 38), cy + band + 8), (X(R + 30), cy + band), (X(R + 4), cy + band)]
        sd.polygon([(x * SS, y * SS) for x, y in pts], fill=255)
    sd.rectangle(S(cx - R, cy - band, cx - 34, cy + band), fill=255)
    sd.rectangle(S(cx + 34, cy - band, cx + R, cy + band), fill=255)
    ring(36, 26)
    sd.ellipse(S(cx - 15, cy - 15, cx + 15, cy + 15), fill=255)
    # the halves as a real ball has them: the top (red) one darker, the
    # bottom (white) one lighter
    upper = pic.blank()
    ImageDraw.Draw(upper).pieslice(S(cx - R, cy - R, cx + R, cy + R), 180, 360, fill=255)
    pic.paint((30, 100, 88), upper, 0.30)
    lower = pic.blank()
    ImageDraw.Draw(lower).pieslice(S(cx - R, cy - R, cx + R, cy + R), 0, 180, fill=255)
    pic.paint((130, 220, 192), lower, 0.16)
    lip_lo = ImageChops.subtract(ImageChops.offset(shapes, 0, SS), shapes)
    lip_hi = ImageChops.subtract(ImageChops.offset(shapes, 0, -SS), shapes)
    pic.paint_img(vgrad(pic, 0, H, [(0, (44, 130, 112)), (1, (34, 108, 94))]), shapes)
    pic.paint(TEAL["ringHi"], lip_lo, 0.8)
    pic.paint(TEAL["ringLo"], lip_hi, 0.7)
    face = pic.blank()
    ImageDraw.Draw(face).ellipse(S(cx - 12, cy - 12, cx + 12, cy + 12), fill=255)
    pic.paint((92, 186, 160), face, 0.9)
    hl = pic.blank()
    ImageDraw.Draw(hl).ellipse(S(cx - 8, cy - 10, cx + 4, cy - 2), fill=255)
    pic.paint(WHITE, hl.filter(ImageFilter.GaussianBlur(2 * SS)), 0.35)
    inner = pic.blank()
    ImageDraw.Draw(inner).ellipse(S(cx - 74, cy - 74, cx + 74, cy + 74), outline=255, width=2 * SS)
    pic.paint(TEAL["ring"], inner, 0.45)
    pic.paint((0, 0, 0), scanlines(pic), 0.05)
    # the dark bars, notched, edged with a mint line
    for top in (True, False):
        def P(x, y):
            return (x, y if top else H - y)
        m = poly(pic, [P(0, 0), P(W, 0), P(W, 11), P(W - 48, 11), P(W - 56, 7), P(56, 7), P(48, 11), P(0, 11)])
        pic.paint_img(vgrad(pic, 0 if top else H - 12, 12,
                            [(0, TEAL["bar"]), (1, TEAL["bar2"])] if top else [(0, TEAL["bar2"]), (1, TEAL["bar"])]), m)
        edge = [P(0, 11), P(48, 11), P(56, 7), P(W - 56, 7), P(W - 48, 11), P(W, 11)]
        line = pic.blank()
        ImageDraw.Draw(line).line([(x * SS, y * SS) for x, y in edge], fill=255, width=round(1.5 * SS), joint="curve")
        pic.paint(TEAL["mint"], line.filter(ImageFilter.GaussianBlur(1.5 * SS)), 0.35)
        pic.paint(TEAL["mint"], line)
        fine = [P(0, 3), P(52, 3), P(58, 1), P(W - 58, 1), P(W - 52, 3), P(W, 3)]
        l2 = pic.blank()
        ImageDraw.Draw(l2).line([(x * SS, y * SS) for x, y in fine], fill=255, width=SS // 2)
        pic.paint(TEAL["mint"], l2, 0.35)
    return pic.im.resize((W, H), Image.LANCZOS)


# ------------------------------------------------------------------ plates

INK = (22, 28, 38)
CREAM = {"top": (252, 245, 234), "low": (236, 224, 208), "ball": (222, 204, 188)}


def tones(base):
    return {"base": base, "top": mix(base, WHITE, 0.22), "low": mix(base, (0, 0, 0), 0.16),
            "deep": mix(base, (0, 0, 0), 0.30), "dark": mix(base, (0, 0, 0), 0.62),
            "light": mix(base, WHITE, 0.45)}


def faint_ball(pic, cx, cy, r, clip, alpha):
    m = pic.blank()
    d = ImageDraw.Draw(m)
    d.ellipse(S(cx - r, cy - r, cx + r, cy + r), outline=255, width=round(r * 0.2 * SS))
    d.rectangle(S(cx - r, cy - r * 0.11, cx + r, cy + r * 0.11), fill=255)
    d.ellipse(S(cx - r * 0.42, cy - r * 0.42, cx + r * 0.42, cy + r * 0.42), fill=255)
    d.ellipse(S(cx - r * 0.24, cy - r * 0.24, cx + r * 0.24, cy + r * 0.24), fill=0)
    pic.paint(CREAM["ball"], mul(m, clip), alpha)


def plate(pic, x, y, shape, state, base, F, paint=True):
    """Draws a plate at (x, y); returns its masks (colour area, cream, inside)."""
    _, w, h, c, cream_w, slope, head = SHAPES[shape]
    T = tones(base)
    pressed = state == "pressed"
    oy = y + (SINK if pressed else 0)
    hh = h - (SINK if pressed else 0)
    outer = poly(pic, chamfer_pts(x, oy, w, hh, c))
    inner = poly(pic, chamfer_pts(x, oy, w, hh, c, inset=1.2))
    sl = lambda yy: x + cream_w + (oy + hh / 2 - yy) * slope
    cream = mul(poly(pic, [(x, oy), (sl(oy), oy), (sl(oy + hh), oy + hh), (x, oy + hh)]), inner) if cream_w else None
    colour = inner if cream is None else ImageChops.subtract(inner, cream)
    if not paint:
        return colour, cream, inner
    sh = poly(pic, chamfer_pts(x, y + (SINK - 0.5 if pressed else 3.5), w, h - (SINK if pressed else 0), c))
    pic.paint((10, 50, 44), sh.filter(ImageFilter.GaussianBlur(1.2 * SS)), 0.55)
    if state == "focus":
        ring = poly(pic, chamfer_pts(x - 2.5, oy - 2.5, w + 5, hh + 5, c + 1))
        pic.paint(F, ring.filter(ImageFilter.GaussianBlur(1.6 * SS)), 0.9)
        pic.paint(F, ring)
    pic.paint(INK, outer)
    face_base = base
    if state == "focus":
        T = dict(T, top=mix(T["top"], WHITE, 0.15), base=mix(base, WHITE, 0.12))
    if pressed:
        T = dict(T, top=T["base"], base=T["low"], low=T["deep"])
    pic.paint_img(vgrad(pic, oy, hh, [(0, T["top"]), (0.45, T["base"]), (1, T["low"])]), inner)
    band = poly(pic, [(x, oy + hh * 0.64), (x + w, oy + hh * 0.58), (x + w, oy + hh), (x, oy + hh)])
    pic.paint(T["deep"], mul(band, inner), 0.45)
    if cream is not None:
        pic.paint_img(vgrad(pic, oy, hh, [(0, CREAM["top"]), (0.6, CREAM["top"]), (1, CREAM["low"])]), cream)
        for off, wd, al in ((1.0, 2.0, 0.85), (5, 9, 0.22), (17, 4, 0.14)):
            st = poly(pic, [(sl(oy) + off, oy), (sl(oy) + off + wd, oy), (sl(oy + hh) + off + wd, oy + hh),
                            (sl(oy + hh) + off, oy + hh)])
            pic.paint(WHITE, mul(st, inner), al)
        r = 21 if shape.startswith("fight") else (13 if shape == "ball" else 12)
        cx = x + (31 if shape.startswith("fight") else (15 if shape == "ball" else 13))
        cy = oy + (hh / 2 - 1 if shape.startswith("fight") else (hh / 2 if shape == "ball" else 16))
        faint_ball(pic, cx, cy, r, cream, 0.9 if shape.startswith("fight") else (0.8 if shape == "ball" else 0.85))
    if head:
        face = mul(poly(pic, [(x, oy + head), (x + w, oy + head), (x + w, oy + hh), (x, oy + hh)]), inner)
        pic.paint_img(vgrad(pic, oy + head, hh - head, [(0, mix(face_base, WHITE, 0.88)), (0.6, mix(face_base, WHITE, 0.84)),
                                                        (1, mix(face_base, WHITE, 0.72))]), face)
        sep = poly(pic, [(x, oy + head - 0.6), (x + w, oy + head - 0.6), (x + w, oy + head + 0.6), (x, oy + head + 0.6)])
        pic.paint(INK, mul(sep, inner), 0.9)
        for off, wd, al in ((0, 10, 0.20), (14, 3, 0.16)):
            st = poly(pic, [(x + w - 46 + off, oy), (x + w - 46 + off + wd, oy),
                            (x + w - 46 + off + wd - head * 0.5, oy + head), (x + w - 46 + off - head * 0.5, oy + head)])
            pic.paint(WHITE, mul(st, inner), al)
    gl = vgrad(pic, oy, hh * 0.5, [(0, WHITE), (1, (0, 0, 0))]).convert("L")
    pic.paint(WHITE, mul(gl, inner), 0.16)
    rim = pic.blank()
    pts = chamfer_pts(x, oy, w, hh, c, inset=1.9)
    ImageDraw.Draw(rim).line([(px * SS, py * SS) for px, py in pts + pts[:1]], fill=255, width=round(0.75 * SS), joint="curve")
    fade = vgrad(pic, oy, hh, [(0, WHITE), (0.55, (90, 90, 90)), (0.8, (40, 40, 40)), (1, (170, 170, 170))]).convert("L")
    pic.paint(WHITE, mul(rim, fade), 0.75)
    pic.paint((0, 0, 0), mul(scanlines(pic), inner), 0.045)
    return colour, cream, inner


# ------------------------------------------------------------------ icons

def layer(pic, fn):
    m = pic.blank()
    fn(ImageDraw.Draw(m))
    return m


def bag_icon(pic, x, y, s, T, clip):
    k = s / 40

    def P(*v):
        return [(x + c * k) * SS if i % 2 == 0 else (y + c * k) * SS for i, c in enumerate(v)]
    body = layer(pic, lambda d: d.rounded_rectangle(P(3, 9, 37, 40), radius=10 * k * SS, fill=255))
    handle = layer(pic, lambda d: d.rounded_rectangle(P(12, 0, 28, 14), radius=6 * k * SS, outline=255,
                                                      width=round(3.2 * k * SS)))
    grown = ImageChops.lighter(body, handle).filter(ImageFilter.MaxFilter(2 * round(1.3 * SS) + 1))
    pic.paint(T["dark"], mul(grown, clip), 0.85)
    pic.paint(mix(T["base"], (0, 0, 0), 0.06), mul(body, clip))
    pic.paint(mix(T["base"], WHITE, 0.25), mul(handle, clip))
    lines = layer(pic, lambda d: (d.arc(P(4, 2, 36, 26), 20, 160, fill=255, width=round(1.4 * k * SS)),
                                  d.line(P(20, 25, 20, 38), fill=255, width=round(1.2 * k * SS))))
    pic.paint(T["dark"], mul(lines, clip), 0.7)
    pic.paint(T["dark"], mul(layer(pic, lambda d: d.ellipse(P(14.5, 18, 25.5, 29), fill=255)), clip), 0.85)
    pic.paint(mix(T["base"], WHITE, 0.35), mul(layer(pic, lambda d: d.ellipse(P(16.5, 20, 23.5, 27), fill=255)), clip))
    gloss = layer(pic, lambda d: d.rounded_rectangle(P(7, 12, 16, 18), radius=3 * k * SS, fill=255))
    pic.paint(WHITE, mul(gloss.filter(ImageFilter.GaussianBlur(SS)), clip), 0.25)


def runner_icon(pic, x, y, s, T, clip):
    k = s / 44

    def dr(d):
        def L(pts, w):
            d.line([((x + a * k) * SS, (y + b * k) * SS) for a, b in pts], fill=255, width=round(w * k * SS), joint="curve")
        d.ellipse([(x + 28 * k) * SS, y * SS, (x + 37 * k) * SS, (y + 9 * k) * SS], fill=255)
        L([(30, 12), (23, 25)], 7)
        L([(29, 14), (36, 19), (41, 15)], 4.2)
        L([(28, 14), (21, 16), (16, 22)], 4.2)
        L([(23, 25), (31, 31), (30, 41)], 5)
        L([(23, 25), (17, 33), (9, 35)], 5)
        for yy, x0, ln in ((15, 0, 11), (21, 3, 9), (27, 0, 11)):
            d.rounded_rectangle([(x + x0 * k) * SS, (y + (yy - 1) * k) * SS, (x + (x0 + ln) * k) * SS,
                                 (y + (yy + 1) * k) * SS], radius=SS, fill=255)
    pic.paint(T["light"], mul(layer(pic, dr), clip), 0.9)


def steps_icon(pic, x, y, s, T, clip):
    k = s / 40

    def dr(d):
        for ox, oy in ((2, 15), (20, 1)):
            d.ellipse([(x + (ox + 2) * k) * SS, (y + (oy + 9) * k) * SS, (x + (ox + 16) * k) * SS,
                       (y + (oy + 23) * k) * SS], fill=255)
            for tx, ty in ((0, 5), (5, 0.5), (11, 0.5), (16, 5)):
                d.ellipse([(x + (ox + tx - 1) * k) * SS, (y + (oy + ty) * k) * SS, (x + (ox + tx + 4) * k) * SS,
                           (y + (oy + ty + 6) * k) * SS], fill=255)
    pic.paint(T["light"], mul(layer(pic, dr), clip), 0.9)


def block_icon(pic, x, y, s, T, clip):
    k = s / 40

    def pts(*p):
        return [((x + a * k) * SS, (y + b * k) * SS) for a, b in p]
    m = layer(pic, lambda d: d.polygon(pts((6, 13), (14, 5), (38, 5), (38, 29), (30, 37), (6, 37)), fill=255))
    pic.paint(T["dark"], mul(m.filter(ImageFilter.MaxFilter(2 * round(1.2 * SS) + 1)), clip), 0.85)
    pic.paint(mix(T["base"], WHITE, 0.3), mul(m, clip))
    pic.paint(mix(T["base"], WHITE, 0.55), mul(layer(pic, lambda d: d.polygon(pts((6, 13), (14, 5), (38, 5), (30, 13)), fill=255)), clip))
    pic.paint(mix(T["base"], (0, 0, 0), 0.05), mul(layer(pic, lambda d: d.polygon(pts((30, 13), (38, 5), (38, 29), (30, 37)), fill=255)), clip))


ICON_AT = {"bag": (bag_icon, -47, 7, 38), "run": (runner_icon, -52, 7, 44), "near": (steps_icon, -46, 6, 38),
           "block": (block_icon, -46, 8, 36)}


def party_ball(pic, cx, cy, state, T, r=5.5):
    box = S(cx - r, cy - r, cx + r, cy + r)
    if state == "empty":
        pic.paint(T["dark"], layer(pic, lambda d: d.ellipse(box, outline=255, width=round(1.3 * SS))), 0.8)
        return
    full = layer(pic, lambda d: d.ellipse(box, fill=255))
    pic.paint((0, 0, 0), ImageChops.offset(full, 0, SS).filter(ImageFilter.GaussianBlur(0.6 * SS)), 0.35)
    pic.paint((24, 24, 32), full)
    top_c = {"ok": (236, 48, 40), "status": (248, 176, 24), "faint": (150, 150, 160)}[state]
    bot_c = (240, 240, 244) if state != "faint" else (196, 196, 202)
    ins = S(cx - r + 1, cy - r + 1, cx + r - 1, cy + r - 1)
    pic.paint(top_c, layer(pic, lambda d: d.pieslice(ins, 180, 360, fill=255)))
    pic.paint(bot_c, layer(pic, lambda d: d.pieslice(ins, 0, 180, fill=255)))
    pic.paint((24, 24, 32), mul(layer(pic, lambda d: d.rectangle(S(cx - r, cy - 0.6, cx + r, cy + 0.6), fill=255)), full))
    pic.paint((24, 24, 32), layer(pic, lambda d: d.ellipse(S(cx - 2.2, cy - 2.2, cx + 2.2, cy + 2.2), fill=255)))
    pic.paint(WHITE, layer(pic, lambda d: d.ellipse(S(cx - 1.2, cy - 1.2, cx + 1.2, cy + 1.2), fill=255)))
    if state != "faint":
        pic.paint(WHITE, layer(pic, lambda d: d.ellipse(S(cx - r * 0.7, cy - r * 0.8, cx - r * 0.1, cy - r * 0.35), fill=255)), 0.7)


# ------------------------------------------------------------------ decomposition

def decompose(draw, size):
    """draw(pic, base, F) on a picture of `size`; returns per-pixel alpha, A, R, C."""
    w, h = size

    def run(bg, base, F):
        pic = Pic(w, h, bg)
        draw(pic, base, F)
        return pic.down()
    zero = (0, 0, 0)
    r0 = run(zero, zero, zero)
    rbg = run(WHITE, zero, zero)
    rb = run(zero, WHITE, zero)
    rf = run(zero, zero, WHITE)
    p0, pbg, pb, pf = r0.load(), rbg.load(), rb.load(), rf.load()
    out = []
    for j in range(h):
        for i in range(w):
            c = p0[i, j]
            inv = sum(pbg[i, j][k] - c[k] for k in range(3)) / 3
            a = sum(pb[i, j][k] - c[k] for k in range(3)) / 3
            f = sum(pf[i, j][k] - c[k] for k in range(3)) / 3
            out.append((max(0, min(255, round(255 - inv))), max(0, min(255, round(a))), max(0, min(255, round(f))), c))
    return out


def rgb565(c):
    r, g, b = c
    return ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)


def trim(px, w, h, keep=lambda p: p[0] or p[1] or p[2] or any(p[3])):
    xs = [i for j in range(h) for i in range(w) if keep(px[j * w + i])]
    ys = [j for j in range(h) for i in range(w) if keep(px[j * w + i])]
    if not xs:
        return 0, 0, 1, 1
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def element(eid, px, w, h, ox, oy, with_r=False, alpha_only=False):
    x0, y0, x1, y1 = trim(px, w, h)
    bw, bh = x1 - x0, y1 - y0
    rows = [px[j * w + i] for j in range(y0, y1) for i in range(x0, x1)]
    flags = 4 if alpha_only else (1 | (2 if with_r else 0))
    out = bytearray(struct.pack("<BBhhHH", eid, flags, x0 - ox, y0 - oy, bw, bh))
    out += bytes(p[0] for p in rows)
    if not alpha_only:
        out += bytes(p[1] for p in rows)
        out += b"".join(struct.pack("<H", rgb565(p[3])) for p in rows)
        if with_r:
            out += bytes(p[2] for p in rows)
    return bytes(out)


def build():
    parts = []
    # the backdrop, in the canvas' own layout
    bd = backdrop().load()
    raw = bytearray(struct.pack("<BBhhHH", ID_BACKDROP, 8, 0, 0, W, H))
    for x in range(W):
        for y in range(H - 1, -1, -1):
            raw += struct.pack("<H", rgb565(bd[x, y]))
    parts.append(bytes(raw))
    ml, mt, mr, mb = MARGIN
    for name, (base_id, w, h, *_rest) in SHAPES.items():
        size = (w + ml + mr, h + mt + mb)
        for k, state in enumerate(STATES):
            px = decompose(lambda pic, base, F: plate(pic, ml, mt, name, state, base, F), size)
            parts.append(element(base_id + k, px, size[0], size[1], ml, mt, with_r=state == "focus"))
    # FIGHT's colour area, where its watermark may go
    for name, eid in (("fight_w", ID_CLIP_FIGHT_W), ("fight_f", ID_CLIP_FIGHT_F)):
        _, w, h, *_ = SHAPES[name]
        pic = Pic(w + ml + mr, h + mt + mb, (0, 0, 0))
        colour, _, _ = plate(pic, ml, mt, name, "normal", (0, 0, 0), (0, 0, 0), paint=False)
        a = colour.resize((pic.w, pic.h), Image.BOX).load()
        px = [(a[i, j], 0, 0, (0, 0, 0)) for j in range(pic.h) for i in range(pic.w)]
        parts.append(element(eid, px, pic.w, pic.h, ml, mt, alpha_only=True))
    # the bottom row's icons, over the plate's colour area
    _, bw, bh, *_ = SHAPES["bottom"]
    for name, eid in ICONS.items():
        fn, dx, dy, s = ICON_AT[name]

        def draw(pic, base, F, fn=fn, dx=dx, dy=dy, s=s):
            colour, _, _ = plate(pic, ml, mt, "bottom", "normal", base, F, paint=False)
            fn(pic, ml + bw + dx, mt + dy, s, tones(base), colour)
        parts.append(element(eid, decompose(draw, (bw + ml + mr, bh + mt + mb)), bw + ml + mr, bh + mt + mb, ml, mt))
    # party balls, anchored at their centre
    for name, eid in BALLS.items():
        def draw(pic, base, F, name=name):
            party_ball(pic, 8, 8, name, tones(base))
        parts.append(element(eid, decompose(draw, (16, 16)), 16, 16, 8, 8))
    return MAGIC + struct.pack("<H", len(parts)) + b"".join(parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--output", required=True)
    args = ap.parse_args()
    data = build()
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    Path(args.output).write_bytes(data)
    print("battle art: %d bytes" % len(data))


if __name__ == "__main__":
    main()
