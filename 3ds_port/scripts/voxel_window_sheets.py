"""Contact sheets of the window glow, to judge the classifier by eye.

For each building drawing: the day art, the same art on a dark-blue night
tint with the glass mixed to the glow colour as the console does it
(BuildingGlowEnv: texel -> colour by level / 255), and the glass texels
marked on the day art. Not part of the build; it writes PNGs only.

  py -3 scripts/voxel_window_sheets.py OUT_DIR [--models a,b,c] [--per-sheet 4]

Without --models it takes every model of an outdoor map that has glass.
The models come from gen_voxel_buildings.build_models (some minutes).
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from PIL import Image, ImageDraw

GLOW = (255, 196, 84)           # the proposed BuildingGlowEnv constant (0xFF54C4FF)
NIGHT = (0.55, 0.62, 0.86)      # the world grade at full night, a little darker
SCALE = 3


def night_panel(art, mask, colour=GLOW):
    out = Image.new("RGBA", art.size, (20, 26, 48, 255))
    src, dst = art.load(), out.load()
    for y in range(art.height):
        for x in range(art.width):
            r, g, b, a = src[x, y]
            if a < 128:
                continue
            r, g, b = r * NIGHT[0], g * NIGHT[1], b * NIGHT[2]
            k = mask[y * art.width + x] / 255.0
            if k:
                r, g, b = (r + (colour[0] - r) * k, g + (colour[1] - g) * k,
                           b + (colour[2] - b) * k)
            dst[x, y] = (int(r), int(g), int(b), 255)
    return out


def mark_panel(art, mask):
    out = Image.new("RGBA", art.size, (40, 50, 80, 255))
    out.alpha_composite(art)
    px = out.load()
    for y in range(art.height):
        for x in range(art.width):
            if mask[y * art.width + x]:
                px[x, y] = (255, 0, 255, 255)
    return out


def sheet(entries, path):
    """entries: [(name, art, mask)] laid out as day | night | marks, a model a row."""
    rows = [(n, a, m) for n, a, m in entries]
    width = max(a.width for _, a, _ in rows) * SCALE * 3 + 40
    height = sum(a.height * SCALE + 16 for _, a, _ in rows)
    out = Image.new("RGB", (width, height), (30, 30, 45))
    draw = ImageDraw.Draw(out)
    y = 0
    for name, art, mask in rows:
        draw.text((2, y), "%s  %dx%d  glass texels: %d" % (name, art.width, art.height,
                                                       sum(1 for v in mask if v)),
                  fill=(255, 255, 255))
        y += 14
        day = Image.new("RGBA", art.size, (40, 50, 80, 255))
        day.alpha_composite(art)
        for i, panel in enumerate((day, night_panel(art, mask), mark_panel(art, mask))):
            out.paste(panel.convert("RGB").resize((art.width * SCALE, art.height * SCALE),
                                                  Image.NEAREST),
                      (i * (art.width * SCALE + 20), y))
        y += art.height * SCALE + 2
    out.save(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    parser.add_argument("--models", default=None)
    parser.add_argument("--per-sheet", type=int, default=4)
    parser.add_argument("--art-dir", default=None,
                        help="read PNG drawings from here instead of building the models")
    args = parser.parse_args()
    import voxel_night
    os.makedirs(args.out, exist_ok=True)
    entries = []
    if args.art_dir:
        if not args.models:
            parser.error("--art-dir needs --models (the drawings' names)")
        # (a drawing alone: no mesh, so glass no upright face reads shows too)
        for name in args.models.split(","):
            art = Image.open(os.path.join(args.art_dir, name + ".png")).convert("RGBA")
            entries.append((name, art, voxel_night.building_mask(art, name)))
    else:
        import gen_voxel_buildings as generator
        wanted = set(args.models.split(",")) if args.models else None
        for model in generator.build_models(None):
            if wanted is not None and model.name not in wanted:
                continue
            mask = voxel_night.building_mask(model.art, model.name, model.mesh.tris)
            if any(mask) or wanted is not None:
                entries.append((model.name, model.art.convert("RGBA"), mask))
    per = args.per_sheet
    for i in range(0, len(entries), per):
        sheet(entries[i:i + per], os.path.join(args.out, "windows_%03d.png" % (i // per + 1)))
    print("%d model(s), %d sheet(s) in %s" % (len(entries), (len(entries) + per - 1) // per, args.out))


if __name__ == "__main__":
    main()
