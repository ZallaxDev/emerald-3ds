#!/usr/bin/env python3
"""Depth planes for the battle's other pictures (3ds_stereo_battle.c).

  python3 scripts/gen_stereo_backs.py --work OUTPUT/stereo-back-trainers --root ROOT --output romfs/stereo/backs.bin

The authored collection (work/<asset>/depth-frames.bin: one nibble per
pixel per 64x64 frame, rows top to bottom, even column in the low nibble,
0 the farthest plane .. 9 the nearest, 15 transparent; extraction/
manifest.json names each asset) packed into the viewer's container
(EM3DSTM1, see gen_stereo_mons.py) with one difference: an entry has its
own number of frames in the index's reserved word, and its planes are
frames x 2048 bytes, and like it holds no 15: the transparent pixels take
the plane of their nearest opaque one once the planes are grouped. Ids: a Pokemon's back is 0x8000 | national (canonical
form only), a trainer's front 0x4000 | TRAINER_PIC_<name>, a player's back
0xC000 | TRAINER_BACK_PIC_<name> (the constants of include/constants/
trainers.h under --root). Nothing here comes from the ROM: plane numbers
only. Without --work the output is an empty container.
"""
import argparse, json, os, re, struct, sys

from collapse_stereo_planes import collapse_pixels, fill_transparent
from stereo_depth_groups import smart_pixels

FRAME_BYTES = 64 * 64 // 2

def constants(root):
    path = os.path.join(root, "include", "constants", "trainers.h")
    front, back = {}, {}
    if os.path.exists(path):
        for line in open(path):
            m = re.match(r"#define TRAINER_BACK_PIC_(\w+)\s+(\d+)", line)
            if m: back[m.group(1)] = int(m.group(2)); continue
            m = re.match(r"#define TRAINER_PIC_(\w+)\s+(\d+)", line)
            if m: front[m.group(1)] = int(m.group(2))
    return front, back

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default="")
    ap.add_argument("--root", default="")
    ap.add_argument("--output", required=True)
    ap.add_argument("--planes", type=int, choices=range(2, 11), default=4)
    ap.add_argument('--cleanup', action='store_true', help='clean small fragments when anatomy is missing')
    ap.add_argument('--smart', action='store_true', help='merge whole annotated parts by depth')
    ap.add_argument('--report', help='write grouping audit JSON')
    args = ap.parse_args()
    if args.cleanup and not args.smart:
        ap.error('--cleanup requires --smart')
    reports = []
    entries = []
    skipped = 0
    if args.work:
        manifest = json.load(open(os.path.join(args.work, "extraction", "manifest.json")))
        front, back = constants(args.root)
        for a in sorted(manifest["assets"], key=lambda a: a["name"]):
            cat, inv = a["category"], a.get("inventory", {})
            if cat == "pokemon_back":
                if not a.get("canonical"): skipped += 1; continue
                ident = 0x8000 | int(a["dex"])
            elif cat == "trainer_front":
                if inv.get("table_id") not in front: skipped += 1; continue
                ident = 0x4000 | front[inv["table_id"]]
            elif cat == "player_back":
                if inv.get("table_id") not in back: skipped += 1; continue
                ident = 0xC000 | back[inv["table_id"]]
            else:
                skipped += 1; continue
            path = os.path.join(args.work, "work", a["name"], "depth-frames.bin")
            if not os.path.exists(path): skipped += 1; continue
            data = open(path, "rb").read()
            frames = a["frame_count"]
            if len(data) != frames * FRAME_BYTES or frames < 1 or frames > 15:
                print(f"gen_stereo_backs: {a['name']}: {len(data)} bytes for {frames} frames, skipped", file=sys.stderr)
                skipped += 1; continue
            if args.smart:
                anatomy_path = os.path.join(args.work, 'work', a['name'], 'anatomy.json')
                anatomy = json.load(open(anatomy_path)) if os.path.exists(anatomy_path) else None
                data, report = smart_pixels(data, 10, args.planes, anatomy, args.cleanup)
                reports.append(dict(id=ident, name=a['name'], **report))
            else:
                data = collapse_pixels(data, 10, args.planes)
            # No outline in the file: the game's picture is the silhouette.
            data = fill_transparent(data)
            entries.append((ident, frames, data))
    if args.report:
        os.makedirs(os.path.dirname(args.report) or '.', exist_ok=True)
        with open(args.report, 'w') as f:
            json.dump(reports, f, indent=2)
            f.write('\n')
    entries.sort()
    maxFrames = max([f for _, f, _ in entries], default=1)
    out = bytearray(b"EM3DSTM1")
    out += struct.pack("<HBBHH", len(entries), args.planes, maxFrames, 64, 64)
    offset = 16 + 8 * len(entries)
    for ident, frames, data in entries:
        out += struct.pack("<HHI", ident, frames, offset)
        offset += len(data)
    for _, _, data in entries:
        out += data
    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    open(args.output, "wb").write(out)
    print(f"gen_stereo_backs: {len(entries)} entries ({skipped} skipped), up to {maxFrames} frames, {len(out)} bytes -> {args.output}")

if __name__ == "__main__":
    main()
