#!/usr/bin/env python3
"""Black 2 / White 2 animated battle sprites for Emerald 3DS.

Downloads the animated sprites (APNG) of Pokemon #001-#386 from Bulbagarden
Archives - front, back, and their shiny versions - and converts them to the
port's BWS1 format (3ds_port/include/3ds_bw_mons.h), all in one pack. Copy
it to the SD card as:

    sdmc:/3ds/emerald3ds/bw.pak

(--loose writes them as one file each instead, under bw/front and bw/back:
slower to open on the card, kept for testing.)

Usage:
    pip install pillow
    python tools/bw_sprites.py --out bw_out
    python tools/bw_sprites.py --out bw_out --only 1-9,25,384   # a test

Already downloaded? --cache keeps the PNGs (default: bw_cache) and they are
not downloaded again; --offline converts only what is in the cache.

The sprites belong to Nintendo / Game Freak; this tool is for your own
console with your own copy of the game, and does not redistribute anything.
"""
import argparse
import io
import json
import re
import struct
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

API = "https://archives.bulbagarden.net/w/api.php"
CATEGORIES = [
    "Black 2 and White 2 sprites",
    "Black 2 and White 2 Shiny sprites",
    "Black 2 and White 2 back sprites",
    "Black 2 and White 2 Shiny back sprites",
]
USER_AGENT = "Emerald3DS-bw-sprites/1.0 (personal use converter)"
# File:Spr [b ]5b[2] NNN[FORM][ m| f][ s].png
NAME = re.compile(r"^File:Spr (b )?5b(2)? (\d{3})([A-Z]{1,2})?(?: ([mf]))?( s)?\.png$")
MAX_DIM = 256


def api_get(params):
    params = dict(params, format="json")
    url = API + "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode("utf-8"))


def category_files(category):
    out, cont = [], None
    while True:
        params = {"action": "query", "list": "categorymembers", "cmtitle": "Category:" + category,
                  "cmtype": "file", "cmlimit": "500"}
        if cont:
            params["cmcontinue"] = cont
        data = api_get(params)
        out += [m["title"] for m in data["query"]["categorymembers"]]
        cont = data.get("continue", {}).get("cmcontinue")
        if not cont:
            return out


def parse_name(title):
    """(side, dex, form, female, shiny, b2w2) or None."""
    m = NAME.match(title)
    if not m:
        return None
    back, b2, dex, form, gender, shiny = m.groups()
    dex = int(dex)
    if not 1 <= dex <= 386:
        return None
    return ("back" if back else "front", dex, form or "", gender == "f", bool(shiny), bool(b2))


def output_name(side, dex, form, female, shiny):
    return "%s/%03d%s%s%s.bin" % (side, dex, form, "_f" if female else "", "_s" if shiny else "")


def choose(titles, only):
    """The file for each output: Black 2 / White 2's own over Black / White's."""
    chosen = {}
    for t in titles:
        p = parse_name(t)
        if not p:
            continue
        side, dex, form, female, shiny, b2 = p
        if only and dex not in only:
            continue
        key = output_name(side, dex, form, female, shiny)
        if key not in chosen or (b2 and not chosen[key][1]):
            chosen[key] = (t, b2)
    return {k: v[0] for k, v in chosen.items()}


def image_urls(titles):
    urls = {}
    titles = list(titles)
    for i in range(0, len(titles), 50):
        data = api_get({"action": "query", "titles": "|".join(titles[i:i + 50]),
                        "prop": "imageinfo", "iiprop": "url"})
        norm = {n["to"]: n["from"] for n in data["query"].get("normalized", [])}
        for page in data["query"]["pages"].values():
            if "imageinfo" in page:
                urls[norm.get(page["title"], page["title"])] = page["imageinfo"][0]["url"]
    return urls


def download(url, dest):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                data = r.read()
            dest.write_bytes(data)
            return True
        except Exception as e:  # network hiccups: try again a little later
            if attempt == 3:
                print("  failed: %s (%s)" % (url, e), file=sys.stderr)
                return False
            time.sleep(2 + attempt * 3)


# ---------------------------------------------------------------- conversion

def frames_of(data):
    """[(RGBA image, milliseconds)] of an APNG (or a still PNG)."""
    from PIL import Image, ImageSequence

    im = Image.open(io.BytesIO(data))
    out = []
    for frame in ImageSequence.Iterator(im):
        ms = frame.info.get("duration", im.info.get("duration", 100)) or 100
        out.append((frame.convert("RGBA"), float(ms)))
    return out


def packbits(data):
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        run = 1
        while i + run < n and run < 129 and data[i + run] == data[i]:
            run += 1
        if run >= 2:
            out += bytes((run + 126, data[i]))
            i += run
            continue
        j = i
        while j < n and j - i < 128 and not (j + 1 < n and data[j + 1] == data[j]):
            j += 1
        if j == i:
            j = i + 1
        out.append(j - i - 1)
        out += data[i:j]
        i = j
    return bytes(out)


def convert(data):
    """BWS1 bytes of an animated sprite."""
    frames = frames_of(data)
    w, h = frames[0][0].size
    if w > MAX_DIM or h > MAX_DIM:
        raise ValueError("%dx%d is larger than %d" % (w, h, MAX_DIM))
    # The palette: every opaque colour of every frame; colour 0 transparent.
    colours, index = [0], {}
    pixels = []
    for img, _ in frames:
        if img.size != (w, h):
            img = img.crop((0, 0, w, h))
        px = []
        for r, g, b, a in img.getdata():
            if a < 128:
                px.append(0)
                continue
            key = (r, g, b)
            k = index.get(key)
            if k is None:
                k = len(colours)
                index[key] = k
                colours.append((r << 24) | (g << 16) | (b << 8) | 0xFF)
            px.append(k)
        pixels.append(px)
    if len(colours) > 256:
        return convert_quantized(frames, w, h)
    return pack(frames, pixels, colours, w, h)


def convert_quantized(frames, w, h):
    """More than 255 colours (not expected of a DS sprite): one shared
    255-colour palette for every frame."""
    from PIL import Image

    sheet = Image.new("RGB", (w, h * len(frames)))
    for i, (img, _) in enumerate(frames):
        sheet.paste(img.convert("RGB"), (0, i * h))
    pal_img = sheet.quantize(255)
    pal = pal_img.getpalette()[:255 * 3]
    colours = [0] + [(pal[i * 3] << 24) | (pal[i * 3 + 1] << 16) | (pal[i * 3 + 2] << 8) | 0xFF
                     for i in range(255)]
    pixels = []
    for img, _ in frames:
        q = img.convert("RGB").quantize(palette=pal_img)
        alpha = img.getchannel("A").getdata()
        pixels.append([0 if a < 128 else i + 1 for i, a in zip(q.getdata(), alpha)])
    return pack(frames, pixels, colours, w, h)


def pack(frames, pixels, colours, w, h):
    # The first frame's opaque box: where the Pokemon stands.
    first = pixels[0]
    xs = [i % w for i, p in enumerate(first) if p]
    ys = [i // w for i, p in enumerate(first) if p]
    if xs:
        anchor_x2 = min(xs) + max(xs) + 1
        feet = max(ys)
    else:
        anchor_x2, feet = w, h - 1
    # Frames that repeat the one before are one, shown for longer.
    merged = []
    for px, (_, ms) in zip(pixels, frames):
        ticks = max(1, round(ms * 60 / 1000))
        if merged and merged[-1][0] == px:
            merged[-1][1] += ticks
        else:
            merged.append([px, ticks])
    blobs = [packbits(bytes(px)) for px, _ in merged]
    offsets, at = [], 0
    for b in blobs:
        offsets.append(at)
        at += len(b)
    offsets.append(at)
    out = bytearray(b"BWS1")
    out += struct.pack("<6H", w, h, len(merged), anchor_x2, feet, len(colours))
    out += b"".join(struct.pack("<I", c) for c in colours)
    out += b"".join(struct.pack("<H", min(t, 65535)) for _, t in merged)
    out += b"".join(struct.pack("<I", o) for o in offsets)
    out += b"".join(blobs)
    return bytes(out)


def write_pak(pictures):
    """The pack the port reads (3ds_bw_mons.c): "BWPK", the count, then
    { name[20], offset, size } sorted by name, then the pictures."""
    names = sorted(pictures)
    head = 8 + 28 * len(names)
    index, at = bytearray(), head
    for name in names:
        raw = name.encode("ascii")
        if len(raw) >= 20:
            raise ValueError("name too long for the pack: " + name)
        index += raw.ljust(20, b"\0") + struct.pack("<II", at, len(pictures[name]))
        at += len(pictures[name])
    return b"BWPK" + struct.pack("<I", len(names)) + bytes(index) + b"".join(pictures[n] for n in names)


def parse_only(text):
    if not text:
        return None
    out = set()
    for part in text.split(","):
        if "-" in part:
            a, b = part.split("-")
            out.update(range(int(a), int(b) + 1))
        elif part.strip():
            out.add(int(part))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="bw_out", help="output folder (its contents go to sdmc:/3ds/emerald3ds/bw/)")
    ap.add_argument("--cache", default="bw_cache", help="where the downloaded PNGs are kept")
    ap.add_argument("--only", help="national numbers, e.g. 1-9,25,384")
    ap.add_argument("--offline", action="store_true", help="convert what is in the cache, download nothing")
    ap.add_argument("--delay", type=float, default=0.2, help="seconds between downloads")
    ap.add_argument("--loose", action="store_true", help="one file a picture instead of bw.pak")
    args = ap.parse_args()
    try:
        import PIL  # noqa: F401
    except ImportError:
        sys.exit("Pillow is missing: pip install pillow")

    only = parse_only(args.only)
    cache, out = Path(args.cache), Path(args.out)
    cache.mkdir(parents=True, exist_ok=True)

    if args.offline:
        titles = ["File:" + p.name.replace("_", " ") for p in cache.glob("*.png")]
    else:
        titles = []
        for cat in CATEGORIES:
            print("listing:", cat)
            titles += category_files(cat)
    chosen = choose(titles, only)
    print("%d sprites to convert" % len(chosen))

    def cached(title):
        return cache / title[len("File:"):].replace(" ", "_")

    if not args.offline:
        missing = [t for t in chosen.values() if not cached(t).exists()]
        if missing:
            print("downloading %d files..." % len(missing))
            urls = image_urls(missing)
            for n, t in enumerate(missing, 1):
                if t in urls:
                    download(urls[t], cached(t))
                    time.sleep(args.delay)
                if n % 50 == 0:
                    print("  %d/%d" % (n, len(missing)))

    done, failed, total = 0, [], 0
    packed = {}
    for key, title in sorted(chosen.items()):
        src = cached(title)
        if not src.exists():
            failed.append((key, "not downloaded"))
            continue
        try:
            blob = convert(src.read_bytes())
        except Exception as e:
            failed.append((key, str(e)))
            continue
        if args.loose:
            dest = out / key
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(blob)
        else:
            packed[key[:-len(".bin")]] = blob
        done += 1
        total += len(blob)
    for key, why in failed:
        print("  skipped %s: %s" % (key, why))
    if args.loose:
        print("converted %d sprites (%.1f MB) into %s" % (done, total / 1e6, out))
        print("Copy the 'front' and 'back' folders of %s to sdmc:/3ds/emerald3ds/bw/" % out)
        return
    out.mkdir(parents=True, exist_ok=True)
    pak = out / "bw.pak"
    pak.write_bytes(write_pak(packed))
    print("converted %d sprites into %s (%.1f MB)" % (done, pak, pak.stat().st_size / 1e6))
    print("Copy it to sdmc:/3ds/emerald3ds/bw.pak")


if __name__ == "__main__":
    main()
