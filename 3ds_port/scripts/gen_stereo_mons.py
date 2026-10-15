#!/usr/bin/env python3
"""Depth planes for the stereoscopic Pokémon viewer (3ds_stereo_mon.c).

  python3 scripts/gen_stereo_mons.py --work ../../../sprite-stereo/work \
      --sprites ../../../sprite-stereo/sprites --output romfs/stereo/mons.bin [--planes 4]

The Sprite Stereo Lab (sprite-stereo/) turns an Emerald battle sprite into a
depth map at sprite resolution (work/<national>/depth_hint_s.png, L, 255 =
nearest). This script does what its viewer's column A+ does in "Blur +
posterize" mode - a 3x3 blur of that map, posterised into N planes between
its 2nd and 98th percentile - and carries the result onto the game's own
front picture (graphics/pokemon/<name>/anim_front.png, two 64x64 frames):

* the lab's sprite (an animated GIF on a canvas of its own size) is aligned to
  frame 0 by silhouette overlap, which is exact for every sprite tried;
* frame 1, a different pose, takes each pixel's plane from the nearest pixel
  of frame 0 with the same colour, then every run of same-coloured pixels
  votes (the lab's own propagation, tools/render_gif.py).

When work/<national>/anatomy.json exists, both poses instead use authored
semantic part masks. Schema: {"version":1,"size":[64,64],"parts":[
{"id":1,"name":"head","depth":0.8}],"frames":[{"rows":[...64 rows...]},
{"rows":[...64 rows...]}]}. Each row contains [start, end_exclusive, part_id]
runs. Every opaque pixel must be covered exactly once; background must never
be covered. Part IDs identify the same anatomy in both poses, independent of
colour or position. An optional depth is normalized (0 far, 1 near); otherwise
the median authored key-frame plane of that part is used for both poses.
Eyes, markings, outlines and claws inherit the containing mask's depth.
This deliberately uses one plane per part; finer curvature requires finer
semantic masks. Invalid annotations abort export instead of silently reverting.
An optional source_sha256 locks the masks to the original anim_front.png bytes;
build verifies it before using the annotations.

Nothing here comes from the ROM: the output holds plane numbers only, the
picture itself is the game's at run time.

Output (little endian): b"EM3DSTM1", u16 count, u8 planes, u8 frames, u16
width, u16 height, then count index entries (u16 national, u16 reserved, u32
offset), then per entry frames x (width*height/2) bytes: one nibble per
pixel, rows top to bottom, the even column in the low nibble; 0 is the
farthest plane, planes-1 the nearest. The output holds no transparent
pixel (15) and so no outline: once the planes are made, each transparent
pixel takes the plane of its nearest opaque one (collapse_stereo_planes.
fill_transparent), and the game's own picture tells at run time which are
drawn.
"""
import argparse, hashlib, json, math, os, statistics, struct, sys
from PIL import Image, ImageSequence

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
MAGIC = b"EM3DSTM1"
W = H = 64
NONE = 15
SPIRAL = sorted(((dx, dy) for dy in range(-7, 8) for dx in range(-7, 8)), key=lambda p: p[0] * p[0] + p[1] * p[1])


def dex_names(root):
    """National dex number -> graphics folder, from include/constants/pokedex.h."""
    names, inside = {}, False
    with open(os.path.join(root, "include", "constants", "pokedex.h")) as f:
        for line in f:
            s = line.strip()
            if s.startswith("enum") and "NATIONAL_DEX_NONE" in s or s == "NATIONAL_DEX_NONE,":
                inside = True
                continue
            if inside:
                if s.startswith("}"):
                    break
                if s.startswith("NATIONAL_DEX_") and s.endswith(","):
                    names[len(names) + 1] = s[len("NATIONAL_DEX_"):-1].lower()
    if not names:
        sys.exit("gen_stereo_mons: no NATIONAL_DEX_* names in include/constants/pokedex.h")
    return names


def q5(c):
    return (c[0] >> 3, c[1] >> 3, c[2] >> 3)


def gif_frame0(path):
    im = Image.open(path)
    fr = next(ImageSequence.Iterator(im)).convert("RGBA")
    px = list(fr.getdata())
    return fr.size, [(q5(p) if p[3] else None) for p in px]


def key_planes(depth_path, gw, gh, gpx, n):
    """Blur + posterise of the Claude-guided depth map (the lab's maths)."""
    dep = list(Image.open(depth_path).convert("L").getdata())
    if len(dep) != gw * gh:
        return None
    blur = {}
    for y in range(gh):
        for x in range(gw):
            i = y * gw + x
            if gpx[i] is None:
                continue
            s = w = 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    xx, yy = x + dx, y + dy
                    if 0 <= xx < gw and 0 <= yy < gh and gpx[yy * gw + xx] is not None:
                        k = 4 if not (dx or dy) else (1 if dx and dy else 2)
                        s += dep[yy * gw + xx] * k
                        w += k
            blur[i] = s / w / 255
    v = sorted(blur.values())
    lo, hi = v[int(len(v) * 0.02)], v[int(len(v) * 0.98)]
    out = [-1] * (gw * gh)
    for i, b in blur.items():
        out[i] = max(0, min(n - 1, int((b - lo) / max(1e-6, hi - lo) * (n - 1) + 0.5)))
    return out


def decomp_frames(path):
    """The two frames of anim_front.png as 64x64 lists of quantised colours (None = transparent)."""
    im = Image.open(path)
    if im.size != (W, 2 * H):
        return None
    idx = list(im.getdata())
    rgb = list(im.convert("RGB").getdata())
    frames = []
    for f in range(2):
        frames.append([(q5(rgb[(y + f * H) * W + x]) if idx[(y + f * H) * W + x] else None)
                       for y in range(H) for x in range(W)])
    return frames


def align(frame0, gw, gh, gpx):
    """The offset of the game's frame 0 inside the lab's canvas with the most silhouette overlap."""
    def bbox(mask, w, h):
        xs = [i % w for i, m in enumerate(mask) if m]
        ys = [i // w for i, m in enumerate(mask) if m]
        return (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    dcx, dcy = bbox([p is not None for p in frame0], W, H)
    gcx, gcy = bbox([p is not None for p in gpx], gw, gh)
    cx, cy = round(gcx - dcx), round(gcy - dcy)
    best = (-1, 0, 0, 0)
    for oy in range(cy - 8, cy + 9):
        for ox in range(cx - 8, cx + 9):
            m = u = 0
            for y in range(H):
                gy = y + oy
                if not 0 <= gy < gh:
                    u += sum(1 for x in range(W) if frame0[y * W + x] is not None)
                    continue
                for x in range(W):
                    gx = x + ox
                    gm = 0 <= gx < gw and gpx[gy * gw + gx] is not None
                    dm = frame0[y * W + x] is not None
                    u += gm or dm
                    m += gm and dm
            if m > best[0]:
                best = (m, u, ox, oy)
    m, u, ox, oy = best
    return ox, oy, (m / u if u else 0.0)


def centre(px):
    xs = [i % W for i, p in enumerate(px) if p is not None]
    ys = [i // W for i, p in enumerate(px) if p is not None]
    return (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2


def propagate(kf, key, f, n):
    """Key frame planes -> another frame: nearest same-colour key pixel, then same-colour runs vote."""
    kc, fc = centre(kf), centre(f)
    ox, oy = round(kc[0] - fc[0]), round(kc[1] - fc[1])
    ml = [-1] * (W * H)
    for i, p in enumerate(f):
        if p is None:
            continue
        x, y = i % W, i // W
        anyv = -1
        for dx, dy in SPIRAL:
            xx, yy = x + ox + dx, y + oy + dy
            if not (0 <= xx < W and 0 <= yy < H):
                continue
            j = yy * W + xx
            if kf[j] is None or key[j] < 0:
                continue
            if anyv < 0:
                anyv = key[j]
            if kf[j] == p:
                anyv = key[j]
                break
        ml[i] = anyv
    lay = [-1] * (W * H)
    seen = [False] * (W * H)
    for s in range(W * H):
        if f[s] is None or seen[s]:
            continue
        st, comp = [s], []
        seen[s] = True
        while st:
            j = st.pop()
            comp.append(j)
            x, y = j % W, j // W
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < W and 0 <= yy < H:
                    q = yy * W + xx
                    if not seen[q] and f[q] is not None and f[q] == f[s]:
                        seen[q] = True
                        st.append(q)
        votes = {}
        for j in comp:
            if ml[j] >= 0:
                votes[ml[j]] = votes.get(ml[j], 0) + 1
        best = max(votes, key=votes.get) if votes else -1
        for j in comp:
            lay[j] = best if (len(comp) >= 4 and best >= 0) else ml[j]
    return [l if l >= 0 else ((n - 1) // 2 if f[i] is not None else -1) for i, l in enumerate(lay)]


def pack(planes):
    out = bytearray(W * H // 2)
    for i, p in enumerate(planes):
        v = NONE if p < 0 else p
        if i & 1:
            out[i >> 1] |= v << 4
        else:
            out[i >> 1] = v
    return bytes(out)


def boundary_fragments(labels, depth_by_id):
    """Audit 64x64 pose labels without changing art, masks, or depths.

    Flag tiny detached pieces of a larger semantic part only when they touch
    transparency (or the canvas edge) and another depth. Eight-connected
    components preserve diagonal pixel-art links; whole small parts are valid.
    This conservative check is not a substitute for reviewing anatomy masks.
    """
    findings = []
    for fi, mask in enumerate(labels):
        if len(mask) != W * H:
            raise ValueError("boundary audit requires 64x64 labels")
        unseen = {i for i, pid in enumerate(mask) if pid is not None}
        components = {}
        while unseen:
            first = min(unseen)
            unseen.remove(first)
            pid = mask[first]
            pending, component = [first], []
            while pending:
                i = pending.pop()
                component.append(i)
                x, y = i % W, i // W
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        xx, yy = x + dx, y + dy
                        j = yy * W + xx
                        if 0 <= xx < W and 0 <= yy < H and j in unseen and mask[j] == pid:
                            unseen.remove(j)
                            pending.append(j)
            components.setdefault(pid, []).append(component)
        for pid, pieces in sorted(components.items()):
            largest = max(map(len, pieces))
            for piece in pieces:
                if len(piece) > 8 or len(piece) >= largest:
                    continue
                boundary, neighbors = False, set()
                for i in piece:
                    x, y = i % W, i // W
                    for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                        xx, yy = x + dx, y + dy
                        if not (0 <= xx < W and 0 <= yy < H):
                            boundary = True
                            continue
                        other = mask[yy * W + xx]
                        if other is None:
                            boundary = True
                        elif depth_by_id[other] != depth_by_id[pid]:
                            neighbors.add(other)
                if boundary and neighbors:
                    findings.append({'frame': fi, 'part_id': pid,
                                     'pixels': [[i % W, i // W] for i in sorted(piece)],
                                     'neighbor_ids': sorted(neighbors)})
    return findings


def validate_boundary_fragments(labels, depth_by_id, exceptions, fail):
    """Require exact, reasoned review exceptions; reject stale exemptions."""
    findings = boundary_fragments(labels, depth_by_id)
    def key(item):
        return (item['frame'], item['part_id'], tuple(sorted(map(tuple, item['pixels']))))
    # Review is attached to geometry, not a particular export plane count.
    # Distinct sentinel depths retain candidates even if quantization merges
    # the reviewed part's depth with its neighbor in a later export.
    candidates = boundary_fragments(labels, {pid: pid for pid in depth_by_id})
    available = {key(item): item for item in candidates}
    if not isinstance(exceptions, list):
        fail("boundary_exceptions must be a list")
    reviewed = set()
    for item in exceptions:
        if not isinstance(item, dict) or set(item) != {'frame', 'part_id', 'pixels', 'reason'}:
            fail("boundary exception requires frame, part_id, pixels, and reason")
        if (type(item['frame']) is not int or not 0 <= item['frame'] < len(labels)
                or type(item['part_id']) is not int or item['part_id'] not in depth_by_id
                or not isinstance(item['reason'], str) or not item['reason'].strip()):
            fail("boundary exception has invalid frame, part_id, or reason")
        pixels = item['pixels']
        if (not isinstance(pixels, list) or not pixels
                or any(not isinstance(p, list) or len(p) != 2
                       or any(type(v) is not int for v in p)
                       or not (0 <= p[0] < W and 0 <= p[1] < H) for p in pixels)):
            fail("boundary exception requires exact integer pixel coordinates")
        if len(set(map(tuple, pixels))) != len(pixels):
            fail("boundary exception has duplicate pixels")
        identity = key(item)
        if identity in reviewed:
            fail("duplicate boundary exception")
        if identity not in available:
            fail("stale boundary exception does not match an exact flagged component")
        reviewed.add(identity)
    unreviewed = [item for item in findings if key(item) not in reviewed]
    if unreviewed:
        fail("unreviewed transparent-boundary anatomy fragments: " + json.dumps(unreviewed))


def anatomy_planes(path, frames, key_frame_planes, n, source_path=None):
    """Validate per-pose semantic masks and assign a consistent plane per part.

    Raises ValueError (including the sidecar path) on malformed annotations.
    Neither source pixels nor the supplied key-frame planes are modified.
    """
    def fail(message):
        raise ValueError(f"{path}: {message}")

    if type(n) is not int or not 2 <= n <= 15:
        fail("plane count must be 2..15")
    try:
        with open(path) as f:
            doc = json.load(f)
    except (OSError, ValueError) as exc:
        fail(f"cannot read anatomy annotations: {exc}")
    if not isinstance(doc, dict) or type(doc.get("version")) is not int or doc["version"] != 1:
        fail("expected anatomy schema version 1")
    if doc.get("size") != [W, H]:
        fail("size must be [64, 64]")
    if "source_sha256" in doc:
        digest = doc["source_sha256"]
        if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            fail("source_sha256 must be a lowercase SHA-256 digest")
        if source_path is not None:
            with open(source_path, "rb") as source:
                actual = hashlib.sha256(source.read()).hexdigest()
            if actual != digest:
                fail("source_sha256 differs from original sprite; review stale anatomy annotations")
    if len(frames) != 2 or any(len(f) != W * H for f in frames) or len(key_frame_planes) != W * H:
        fail("expected two 64x64 sprite frames and one key plane map")
    parts = doc.get("parts")
    if not isinstance(parts, list) or not parts:
        fail("parts must be a nonempty list")
    by_id, names = {}, set()
    for part in parts:
        if not isinstance(part, dict):
            fail("each part must be an object")
        pid, name = part.get("id"), part.get("name")
        if type(pid) is not int or pid <= 0 or pid in by_id:
            fail("part IDs must be unique positive integers")
        if not isinstance(name, str) or not name.strip() or name.strip() in names:
            fail("part names must be nonempty and unique")
        if "depth" in part:
            depth = part["depth"]
            if type(depth) not in (int, float) or not math.isfinite(depth) or not 0 <= depth <= 1:
                fail(f"part {pid}: depth must be finite and within [0, 1]")
        by_id[pid] = part
        names.add(name.strip())
    annotation_frames = doc.get("frames")
    if not isinstance(annotation_frames, list) or len(annotation_frames) != len(frames):
        fail("frames must contain exactly two pose masks")
    labels, used = [], set()
    for fi, annotation in enumerate(annotation_frames):
        rows = annotation.get("rows") if isinstance(annotation, dict) else None
        if not isinstance(rows, list) or len(rows) != H:
            fail(f"frame {fi}: expected 64 rows")
        mask = [None] * (W * H)
        for y, runs in enumerate(rows):
            if not isinstance(runs, list):
                fail(f"frame {fi}, row {y}: runs must be a list")
            for run in runs:
                if not isinstance(run, list) or len(run) != 3 or any(type(v) is not int for v in run):
                    fail(f"frame {fi}, row {y}: expected integer [start, end, part_id]")
                start, end, pid = run
                if not 0 <= start < end <= W or pid not in by_id:
                    fail(f"frame {fi}, row {y}: invalid bounds or unknown part ID")
                for x in range(start, end):
                    i = y * W + x
                    if mask[i] is not None:
                        fail(f"frame {fi}, pixel ({x}, {y}): overlapping runs")
                    if frames[fi][i] is None:
                        fail(f"frame {fi}, pixel ({x}, {y}): annotated background")
                    mask[i] = pid
                    used.add(pid)
        for i, pixel in enumerate(frames[fi]):
            if pixel is not None and mask[i] is None:
                fail(f"frame {fi}, pixel ({i % W}, {i // W}): uncovered opaque pixel")
        labels.append(mask)
    if used != set(by_id):
        fail("every declared part must occur in at least one pose")
    depth_by_id = {}
    for pid, part in by_id.items():
        if "depth" in part:
            value = part["depth"] * (n - 1)
        else:
            values = [key_frame_planes[i] for i, label in enumerate(labels[0])
                      if label == pid and 0 <= key_frame_planes[i] < n]
            if not values:
                fail(f"part {pid}: no authored key-frame depth; specify explicit depth")
            value = statistics.median(values)
        depth_by_id[pid] = int(value + 0.5)
    validate_boundary_fragments(labels, depth_by_id, doc.get('boundary_exceptions', []), fail)
    return [[-1 if pid is None else depth_by_id[pid] for pid in mask] for mask in labels]


def build(national, name, args):
    anatomy = os.path.join(args.work, str(national), "anatomy.json")
    has_anatomy = os.path.exists(anatomy)

    def unavailable(message):
        if has_anatomy:
            raise ValueError(f"{anatomy}: {message}")
        return None, message

    depth = os.path.join(args.work, str(national), "depth_hint_s.png")
    gif = os.path.join(args.sprites, f"{national}.gif")
    anim = os.path.join(args.root, "graphics", "pokemon", name, "anim_front.png")
    for p in (depth, gif, anim):
        if not os.path.exists(p):
            return unavailable(f"missing {p}")
    (gw, gh), gpx = gif_frame0(gif)
    smart = getattr(args, 'smart', False)
    source_planes = max(10, args.planes) if smart else args.planes
    key = key_planes(depth, gw, gh, gpx, source_planes)
    if key is None:
        return unavailable("depth map is not at sprite size")
    frames = decomp_frames(anim)
    if frames is None:
        return unavailable("anim_front.png is not 64x128")
    ox, oy, overlap = align(frames[0], gw, gh, gpx)
    # The key frame on the game's grid: the lab's planes where its sprite and the game's overlap.
    kf = [None] * (W * H)
    kp = [-1] * (W * H)
    for y in range(H):
        for x in range(W):
            gx, gy = x + ox, y + oy
            if 0 <= gx < gw and 0 <= gy < gh and gpx[gy * gw + gx] is not None and frames[0][y * W + x] is not None:
                kf[y * W + x] = gpx[gy * gw + gx]
                kp[y * W + x] = key[gy * gw + gx]
    if has_anatomy:
        planes = anatomy_planes(anatomy, frames, kp, source_planes, source_path=anim)
    else:
        planes = [propagate(kf, kp, f, source_planes) for f in frames]
    if smart:
        from stereo_depth_groups import smart_pixels, unpack
        doc = None
        if has_anatomy:
            with open(anatomy) as f:
                doc = json.load(f)
        converted, _ = smart_pixels(b''.join(pack(p) for p in planes), source_planes, args.planes, doc, getattr(args, 'cleanup', False))
        pixels = unpack(converted)
        planes = [[-1 if p == 15 else p for p in pixels[f*4096:(f+1)*4096]] for f in range(len(frames))]
    if args.preview:
        preview(os.path.join(args.preview, f"{national}.png"), frames, planes, args.planes)
    from collapse_stereo_planes import fill_transparent
    data = fill_transparent(b"".join(pack(p) for p in planes))
    method = "anatomy masks" if has_anatomy else "legacy colour transfer"
    return data, f"overlap {overlap:.3f} at ({ox:+d},{oy:+d}); {method}"


def preview(path, frames, planes, n):
    """Each frame beside its planes (far = dark blue, near = white), 4x."""
    S = 4
    im = Image.new("RGB", (W * S * 4 + 3 * S, H * S), (30, 30, 36))
    for f, (px, pl) in enumerate(zip(frames, planes)):
        for i in range(W * H):
            x, y = i % W, i // W
            if px[i] is not None:
                c = tuple(v * 255 // 31 for v in px[i])
                im.paste(c, ((f * 2 * W + x) * S + f * S, y * S, (f * 2 * W + x + 1) * S + f * S, (y + 1) * S))
            if pl[i] >= 0:
                t = pl[i] / (n - 1)
                c = (int(40 + 215 * t), int(60 + 195 * t), int(140 + 115 * t))
                im.paste(c, (((f * 2 + 1) * W + x) * S + f * S, y * S, ((f * 2 + 1) * W + x + 1) * S + f * S, (y + 1) * S))
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    im.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--work", help="sprite-stereo/work (per-sprite depth maps); none = an empty file")
    ap.add_argument("--sprites", help="sprite-stereo/sprites (the lab's GIFs); default beside --work")
    ap.add_argument("--root", default=ROOT, help="the decompilation root (graphics/pokemon/*)")
    ap.add_argument("--planes", type=int, default=4)
    ap.add_argument('--cleanup', action='store_true', help='clean small fragments in unannotated smart drafts')
    ap.add_argument('--smart', action='store_true', help='group whole annotated parts by depth before export')
    ap.add_argument("--output", required=True)
    ap.add_argument("--only", type=int, nargs="*", help="national dex numbers to include")
    ap.add_argument("--preview", help="also write <dir>/<national>.png: each frame beside its planes")
    args = ap.parse_args()
    if args.cleanup and not args.smart:
        ap.error('--cleanup requires --smart')
    if not 2 <= args.planes <= 15:
        sys.exit("gen_stereo_mons: --planes must be 2..15")
    entries = []
    if args.work and os.path.isdir(args.work):
        args.sprites = args.sprites or os.path.join(os.path.dirname(os.path.abspath(args.work)), "sprites")
        names = dex_names(args.root)
        ids = sorted(int(d) for d in os.listdir(args.work) if d.isdigit())
        for national in ids:
            if args.only and national not in args.only:
                continue
            if not any(os.path.exists(os.path.join(args.work, str(national), filename))
                       for filename in ("depth_hint_s.png", "anatomy.json")):
                continue
            name = names.get(national)
            if name is None:
                print(f"  {national}: not a national dex number, skipped")
                continue
            data, note = build(national, name, args)
            print(f"  {national:3d} {name:<12s} {note}")
            if data is not None:
                entries.append((national, data))
    else:
        print("gen_stereo_mons: no --work directory, writing an empty file")
    header_size = 16 + 8 * len(entries)
    out = bytearray(struct.pack("<8sHBBHH", MAGIC, len(entries), args.planes, 2, W, H))
    offset = header_size
    for national, data in entries:
        out += struct.pack("<HHI", national, 0, offset)
        offset += len(data)
    for _, data in entries:
        out += data
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(out)
    print(f"gen_stereo_mons: {len(entries)} Pokemon, {args.planes} planes, {len(out)} bytes -> {args.output}")


if __name__ == "__main__":
    main()
