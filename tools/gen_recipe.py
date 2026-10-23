#!/usr/bin/env python3
"""Write the recipe that lets the Emerald 3DS Builder rebuild the game data.

Run on the maintainer's machine after a build, with the supported ROM:

    python tools/gen_recipe.py --romfs 3ds_port/romfs --rom baserom.gba \\
        --out dist/emerald3ds.recipe --release v0.1.0

Every game-data file in the staging is described as ROM copies, byte fills and
literals (see builder/emerald3ds_builder/recipe.py). Pointer sites of the
bundled payloads are always literals: they hold addresses of this executable.
Everything else is searched in the ROM; what cannot be found stays literal and
is reported, file by file, so that nothing derived from the game can hide in
the literal pool. The run fails when unexplained literals exceed --max-literal.

Files produced by the builder's own generators (voxel data) are recorded with
their expected size and CRC only.
"""

from __future__ import annotations

import argparse
import bisect
import struct
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder"))
sys.path.insert(0, str(ROOT / "tools" / "port_common"))
from emerald3ds_builder import pak, recipe as rcp  # noqa: E402
from emerald3ds_builder.rom import load_rom  # noqa: E402
import staging  # noqa: E402
import vtree_manifest  # noqa: E402

GENERATED = {
    "voxel/regions.bin": "voxel",
    "voxel/signposts.bin": "voxel",
    "voxel/buildings.bin": "voxel",
    "voxel/relief.bin": "voxel",
    "stage/leaves.bin": "stage",
}
# Payloads whose relocation sites are pointers into the executable.
BUNDLES = {
    "scripts/scripts.bin": "scripts/scripts.rel",
    "sound/songs.bin": "sound/songs.rel",
    "gamedata/gamedata.bin": "gamedata/gamedata.rel",
}
MIN_FIND = 8
FILL_MIN = 16
SEARCH_STRIDE = 4


def pointer_mask(romfs: Path, payload: str, size: int) -> bytearray:
    mask = bytearray(size)
    rel = (romfs / BUNDLES[payload]).read_bytes()
    _, _, _, _, internal, external = struct.unpack_from("<4sIIIII", rel, 0)
    sites = struct.unpack_from("<%dI" % (internal + external), rel, 24)
    for site in sites:
        mask[site:site + 4] = b"\1\1\1\1"
    return mask


def nm(tool: str, elf: Path) -> dict[str, list[int]]:
    import subprocess
    out = subprocess.check_output([tool, "--defined-only", str(elf)], text=True, errors="ignore")
    result: dict[str, list[int]] = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and not parts[2].startswith("$"):
            result.setdefault(parts[2], []).append(int(parts[0], 16))
    return result


def gamedata_hints(nm_tool: str, elf3: Path, elfgba: Path) -> dict[int, int]:
    """Payload offset -> ROM offset for every symbol both builds define once."""
    s3, sg = nm(nm_tool, elf3), nm(nm_tool, elfgba)
    start = s3["__ctr_gamedata"][0]
    end = s3["__ctr_gamedata_end"][0]
    hints = {}
    for name, addrs in s3.items():
        if len(addrs) != 1 or not start <= addrs[0] < end:
            continue
        g = [a for a in sg.get(name, []) if 0x08000000 <= a < 0x09000000]
        if len(g) == 1:
            hints[addrs[0] - start] = g[0] - 0x08000000
    return hints


def divisors_desc(n: int):
    return sorted({d for i in range(1, int(n ** 0.5) + 1) if n % i == 0 for d in (i, n // i)},
                  reverse=True)


def columns(blob: bytes, stride: int, count: int) -> list[int]:
    """One integer per bit position of a record: bit k set if record k has it."""
    cols = [0] * (stride * 8)
    for k in range(count):
        rec = blob[k * stride:(k + 1) * stride]
        for byte_i, byte in enumerate(rec):
            if byte:
                for b in range(8):
                    if byte >> b & 1:
                        cols[byte_i * 8 + b] |= 1 << k
    return cols


def find_remap(dst: bytes, dst_mask: bytes, src: bytes) -> tuple | None:
    """Explain `dst` as `src` records with their bits moved.

    Tries record counts from the largest that divides both sizes down, and
    accepts the first where every destination bit outside pointer sites equals
    some source bit (or a constant) in every record. Pointer sites are patched
    from literals afterwards, so a bit only has to be explained in the records
    where it is not part of a pointer. Correctness never depends on the
    choice: the recipe is verified against the expected bytes."""
    from math import gcd
    g = gcd(len(dst), len(src))
    full_mask = bytes(0xFF if m else 0 for m in dst_mask)
    for count in divisors_desc(g):
        if count < 4:
            break
        dst_stride, src_stride = len(dst) // count, len(src) // count
        if dst_stride > 96 or src_stride > 96:
            continue
        full = (1 << count) - 1
        sc = columns(src, src_stride, count)
        dc = columns(dst, dst_stride, count)
        mc = columns(full_mask, dst_stride, count)
        where = {}
        for idx, col in enumerate(sc):
            where.setdefault(col, idx)
        bitmap = []
        ok = True
        for idx, col in enumerate(dc):
            m = mc[idx]
            if m == full:
                bitmap.append(-1)   # a pointer bit in every record: patched
                continue
            keep = full & ~m
            want = col & keep
            if m == 0 and col in where:
                bitmap.append(where[col])
            elif want == 0:
                bitmap.append(-1)
            elif want == keep:
                bitmap.append(-2)
            else:
                hit = next((s for s, sv in enumerate(sc) if sv & keep == want), None)
                if hit is None:
                    ok = False
                    break
                bitmap.append(hit)
        if ok:
            return src_stride, dst_stride, count, bitmap
    return None


def find_projection(dst: bytes, dst_mask: bytes, src: bytes) -> tuple | None:
    """Explain short arrays whose records only lost or moved padding: each
    destination byte comes from a source byte at the same or a later offset of
    the record, in order. Works with any number of records."""
    from math import gcd
    for count in divisors_desc(gcd(len(dst), len(src))):
        ds, ss = len(dst) // count, len(src) // count
        if ds > ss or ss > 96:
            continue
        recs_d = [dst[k * ds:(k + 1) * ds] for k in range(count)]
        recs_m = [dst_mask[k * ds:(k + 1) * ds] for k in range(count)]
        recs_s = [src[k * ss:(k + 1) * ss] for k in range(count)]
        mapping, s = [], 0
        ok = True
        for j in range(ds):
            if all(m[j] for m in recs_m):
                mapping.append(None)
                continue
            while s < ss and not all(recs_m[k][j] or recs_d[k][j] == recs_s[k][s] for k in range(count)):
                s += 1
            if s >= ss:
                ok = False
                break
            mapping.append(s)
            s += 1
        if not ok:
            continue
        bitmap = []
        for j in range(ds):
            for b in range(8):
                bitmap.append(-1 if mapping[j] is None else mapping[j] * 8 + b)
        return ss, ds, count, bitmap
    return None


def gamedata_remaps(nm_tool: str, elf3: Path, elfgba: Path, rom: bytes, data: bytes,
                    mask: bytearray) -> dict[int, tuple]:
    """Record remaps for game tables whose layout differs between the builds."""
    import subprocess

    def sized(elf):
        out = subprocess.check_output([nm_tool, "-S", "--defined-only", str(elf)], text=True, errors="ignore")
        res = {}
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 4:
                res.setdefault(parts[3], []).append((int(parts[0], 16), int(parts[1], 16)))
        return res

    s3, sg = sized(elf3), sized(elfgba)
    start = s3["__ctr_gamedata"][0][0] if "__ctr_gamedata" in s3 else nm(nm_tool, elf3)["__ctr_gamedata"][0]
    remaps = {}
    small = []
    for name, entries in s3.items():
        if len(entries) != 1 or name not in sg or len(sg[name]) != 1:
            continue
        (a, size), (ga, gsize) = entries[0], sg[name][0]
        off = a - start
        if not (0 <= off < len(data)) or size < 2 or not 0x08000000 <= ga < 0x09000000:
            continue
        if size < 16:
            small.append((off, size, ga, gsize))
            continue
        dst = data[off:off + size]
        src = rom[ga - 0x08000000:ga - 0x08000000 + gsize]
        dmask = bytes(mask[off:off + size])
        if size == gsize:
            same = all(dmask[k] or dst[k] == src[k] for k in range(size))
            if same:
                continue
        found = find_remap(dst, dmask, src)
        if found:
            src_stride, dst_stride, count, bitmap = found
            remaps[off] = (size, ga - 0x08000000, src_stride, dst_stride, count, bitmap)
        else:
            small.append((off, size, ga, gsize))
    # Arrays too short to learn a layout from reuse one learnt from a long
    # array with the same record sizes, when it reproduces them exactly.
    learnt: dict[tuple[int, int], list] = {}
    for _, _, src_stride, dst_stride, count, bitmap in remaps.values():
        if count >= 16:
            learnt.setdefault((src_stride, dst_stride), [])
            if bitmap not in learnt[(src_stride, dst_stride)]:
                learnt[(src_stride, dst_stride)].append(bitmap)
    for off, size, ga, gsize in small:
        dst = data[off:off + size]
        src = rom[ga - 0x08000000:ga - 0x08000000 + gsize]
        dmask = bytes(mask[off:off + size])
        if size == gsize and all(dmask[k] or dst[k] == src[k] for k in range(size)):
            continue
        for (src_stride, dst_stride), bitmaps in learnt.items():
            if size % dst_stride or gsize % src_stride or size // dst_stride != gsize // src_stride:
                continue
            count = size // dst_stride
            for bitmap in bitmaps:
                out = bytearray()
                for k in range(count):
                    record = bytearray(dst_stride)
                    rcp.apply_bitmap(src[k * src_stride:(k + 1) * src_stride], record, bitmap)
                    out += record
                if all(dmask[k] or out[k] == dst[k] for k in range(size)):
                    remaps[off] = (size, ga - 0x08000000, src_stride, dst_stride, count, bitmap)
                    break
            if off in remaps:
                break
        if off not in remaps:
            found = find_projection(dst, dmask, src)
            if found:
                src_stride, dst_stride, count, bitmap = found
                remaps[off] = (size, ga - 0x08000000, src_stride, dst_stride, count, bitmap)
    return remaps


def classify_literals(entry: dict, mask: bytearray, image_map: Path, image_elf: Path,
                      gba_elf: Path, nm_tool: str) -> tuple[dict, list]:
    """Say where every unexplained literal byte of the game data comes from.

    code     .rodata.<function>: tables the compiler built from a function
             (switch lookups); derived from code that the 3DSX already carries
    string   C string pools (format strings, assertion text)
    port     objects the original game does not have
    game     objects the original game has: these should not be literal
    """
    import re
    import subprocess
    lines = image_map.read_text(encoding="utf-8", errors="ignore").splitlines()
    secs, pending = [], None
    for line in lines:
        m = re.match(r"^ (\.rodata\S*)\s*$", line)
        if m:
            pending = m.group(1)
            continue
        m = re.match(r"^ (\.rodata\S*)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)", line)
        if m:
            secs.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1), m.group(4)))
            pending = None
            continue
        m = re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o)\s*$", line)
        if m and pending:
            secs.append((int(m.group(1), 16), int(m.group(2), 16), pending, m.group(3)))
        pending = None
    secs.sort()
    starts = [s[0] for s in secs]

    def symbols(elf):
        out = subprocess.check_output([nm_tool, "--defined-only", str(elf)], text=True, errors="ignore")
        funcs, names = set(), set()
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3:
                names.add(parts[2])
                if parts[1] in "Tt":
                    funcs.add(parts[2])
        return funcs, names

    funcs, _ = symbols(image_elf)
    _, gba_names = symbols(gba_elf)
    base = nm(nm_tool, image_elf)["__ctr_gamedata"][0]
    totals = {"padding": 0, "zero": 0, "code": 0, "string": 0, "port": 0, "game": 0}
    data = None
    game = {}
    pos = 0
    for op in entry["ops"]:
        length = op[2] if op[0] in "CFL" else (op[3] * op[4] if op[0] == "R" else 0)
        if op[0] == "L":
            for k in range(pos, pos + length):
                if mask[k]:
                    continue
                j = bisect.bisect_right(starts, base + k) - 1
                inside = j >= 0 and base + k < secs[j][0] + secs[j][1]
                name = secs[j][2] if inside else ".rodata"
                sym = name[len(".rodata."):] if name.startswith(".rodata.") else ""
                if data is None:
                    data = entry.get("_data", b"")
                if not inside:
                    kind = "padding"
                elif data and data[k] == 0:
                    kind = "zero"
                elif ".str" in name:
                    kind = "string"
                elif sym in funcs:
                    kind = "code"
                elif sym and sym not in gba_names:
                    kind = "port"
                else:
                    kind = "game"
                    game[sym or name + " (" + secs[j][3].split("/")[-1] + ")"] = \
                        game.get(sym or name + " (" + secs[j][3].split("/")[-1] + ")", 0) + 1
                totals[kind] += 1
        pos += length
    return totals, sorted(((n, s) for s, n in game.items()), reverse=True)


def domain_of(rel: str) -> str:
    for prefix, name in (("graphics/", "graphics"), ("data/", "graphics"), ("generated/", "graphics"),
                         ("maps/", "maps"), ("scripts/", "scripts"), ("sound/", "songs"),
                         ("gamedata/", "gamedata")):
        if rel.startswith(prefix):
            return name
    return "other"


def layout_breaks(root: Path) -> list[int]:
    """File boundaries inside maps/layouts.bin, in gen_map_data.py's order."""
    import re
    text = (root / "data" / "layouts" / "layouts.inc").read_text()
    breaks, pos = [], 0
    for rel in re.findall(r'\.incbin "(data/layouts/[^"]+)"', text):
        breaks.append(pos)
        pos += (root / rel).stat().st_size
    return breaks


def match_len(a: memoryview, i: int, b: memoryview, j: int, limit: int) -> int:
    limit = min(limit, len(b) - j)
    length = 0
    step = 4096
    while length < limit:
        s = min(step, limit - length)
        if a[i + length:i + length + s] == b[j + length:j + length + s]:
            length += s
            continue
        lo, hi = 0, s
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if a[i + length:i + length + mid] == b[j + length:j + length + mid]:
                lo = mid
            else:
                hi = mid - 1
        return length + lo
    return length


class Cover:
    def __init__(self, rom: bytes):
        self.rom = rom
        self.romv = memoryview(rom)
        self.literals = bytearray()
        self.bitmaps: list[list[int]] = []
        self.bitmap_ids: dict[tuple, int] = {}

    def run(self, data: bytes, mask: bytearray | None, hints: dict[int, int] | None = None,
            remaps: dict | None = None, breaks: list[int] | None = None):
        """Cover `data`. `hints` maps payload offsets (symbol starts) to the
        ROM offset of the same symbol in the original game; `remaps` maps
        payload offsets to record remaps (see find_remap)."""
        hints = hints or {}
        remaps = remaps or {}
        remap_starts = sorted(set(remaps) | set(breaks or ()))
        break_set = set(breaks or ())
        n = len(data)
        dv = memoryview(data)
        ops: list[list] = []
        patches: list[list] = []
        stats = {"copy": 0, "fill": 0, "pointer": 0, "literal": 0, "remap": 0}
        prev = None  # ROM offset expected to continue the last copy
        lit_start = None
        i = 0

        def emit(op):
            if ops:
                last = ops[-1]
                if op[0] == "C" and last[0] == "C" and last[1] + last[2] == op[1]:
                    last[2] += op[2]
                    return
                if op[0] == "L" and last[0] == "L" and last[1] + last[2] == op[1]:
                    last[2] += op[2]
                    return
                if op[0] == "F" and last[0] == "F" and last[1] == op[1]:
                    last[2] += op[2]
                    return
            ops.append(op)

        def literal(start: int, end: int, kind: str):
            off = len(self.literals)
            self.literals += data[start:end]
            emit(["L", off, end - start])
            stats[kind] += end - start

        pending = None  # start of an unexplained literal run
        while i < n:
            if i in remaps:
                if pending is not None:
                    literal(pending, i, "literal")
                    pending = None
                dst_size, rom_off, src_stride, dst_stride, count, bitmap = remaps[i]
                index = self.bitmap_index(bitmap)
                emit(["R", rom_off, src_stride, dst_stride, count, index])
                stats["remap"] += dst_size
                if mask is not None:
                    k = i
                    while k < i + dst_size:
                        if mask[k]:
                            j = k
                            while j < i + dst_size and mask[j]:
                                j += 1
                            off = len(self.literals)
                            self.literals += data[k:j]
                            patches.append([k, off, j - k])
                            stats["pointer"] += j - k
                            stats["remap"] -= j - k
                            k = j
                        else:
                            k += 1
                prev = rom_off + src_stride * count
                i += dst_size
                continue
            if i in hints:
                prev = hints[i]
            if mask is not None and mask[i]:
                if pending is not None:
                    literal(pending, i, "literal")
                    pending = None
                j = i
                while j < n and mask[j] and not (j > i and j in remaps):
                    j += 1
                literal(i, j, "pointer")
                if prev is not None:
                    prev += j - i
                i = j
                continue
            seg_end = n
            if mask is not None:
                k = mask.find(1, i)
                seg_end = n if k < 0 else k
            if remap_starts:
                r = bisect.bisect_right(remap_starts, i)
                if r < len(remap_starts):
                    seg_end = min(seg_end, remap_starts[r])
            # continuation of the previous copy
            if prev is not None and prev < len(self.rom):
                length = match_len(dv, i, self.romv, prev, seg_end - i)
                if length >= 4 or (length > 0 and length == seg_end - i):
                    if pending is not None:
                        literal(pending, i, "literal")
                        pending = None
                    emit(["C", prev, length])
                    stats["copy"] += length
                    prev += length
                    i += length
                    continue
            # a run of one byte
            byte = data[i]
            j = i
            while j < seg_end and data[j] == byte:
                j += 1
            if j - i >= FILL_MIN:
                if pending is not None:
                    literal(pending, i, "literal")
                    pending = None
                emit(["F", byte, j - i])
                stats["fill"] += j - i
                if prev is not None:
                    prev += j - i
                i = j
                continue
            # search the ROM; inside a literal run only every SEARCH_STRIDE bytes,
            # so a long run of the port's own data costs a few scans, not one
            # per byte
            found = -1
            boundary = i > 0 and data[i - 1] in (0x00, 0xFF) and data[i] not in (0x00, 0xFF)
            if pending is None or (i - pending) % SEARCH_STRIDE == 0 or i in break_set or boundary:
                # Longer windows first: a short one often matches a coincidental
                # copy elsewhere in the ROM that then stops early. A game string
                # is also tried whole, up to and including its 0xFF terminator.
                widths = (4096, 256, 32, 16, MIN_FIND) if seg_end - i >= MIN_FIND else (seg_end - i,)
                eos = data.find(bytes([0xFF]), i, min(seg_end, i + 256))
                if eos >= 0 and eos + 1 - i >= 4:
                    widths = (eos + 1 - i,) + widths
                for width in widths:
                    width = min(width, seg_end - i) if width > 32 else width
                    if width >= 2 and seg_end - i >= width:
                        found = self.rom.find(bytes(dv[i:i + width]))
                        if found >= 0:
                            break
            if found >= 0:
                if pending is not None:
                    literal(pending, i, "literal")
                    pending = None
                length = match_len(dv, i, self.romv, found, seg_end - i)
                emit(["C", found, length])
                stats["copy"] += length
                prev = found + length
                i += length
                continue
            if pending is None:
                pending = i
            if prev is not None:
                prev += 1
            i += 1
        if pending is not None:
            literal(pending, n, "literal")
        return ops, patches, stats

    def bitmap_index(self, bitmap: list[int]) -> int:
        key = tuple(bitmap)
        if key not in self.bitmap_ids:
            self.bitmap_ids[key] = len(self.bitmaps)
            self.bitmaps.append(list(bitmap))
        return self.bitmap_ids[key]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--romfs", type=Path, required=True)
    ap.add_argument("--rom", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--release", default="dev")
    ap.add_argument("--max-literal", type=int, default=65536,
                    help="fail above this many unexplained literal bytes")
    ap.add_argument("--report", type=Path, default=None, help="per-file literal report")
    ap.add_argument("--elf", type=Path, default=None, help="the 3DS executable (symbol hints)")
    ap.add_argument("--gba-elf", type=Path, default=None, help="the original game's ELF (symbol hints)")
    ap.add_argument("--nm", default="arm-none-eabi-nm")
    ap.add_argument("--image-map", type=Path, default=None, help="build/gamedata_image.map")
    ap.add_argument("--image-elf", type=Path, default=None, help="build/gamedata_image.elf")
    ap.add_argument("--decomp", type=Path, default=ROOT, help="decomp tree for the voxel inputs")
    ap.add_argument("--max-game-literal", type=int, default=4096,
                    help="fail above this many literal bytes of the original game's own objects")
    args = ap.parse_args()

    source_rom = load_rom(args.rom)
    rom, sha1 = source_rom.data, source_rom.sha1
    if source_rom.code != "BPEE" and not (args.gba_elf and args.elf and args.image_elf):
        raise SystemExit("gen_recipe: %s requires the game's own GBA ELF and matching 3DS/image ELFs; "
                         "English offsets and game data cannot be used." % source_rom.code)
    abi, items = staging.compute_abi(args.romfs)
    cover = Cover(rom)
    hints = {}
    remaps = {}
    if args.elf and args.gba_elf:
        hints = gamedata_hints(args.nm, args.elf, args.gba_elf)
        gd = (args.romfs / "gamedata" / "gamedata.bin").read_bytes()
        remaps = gamedata_remaps(args.nm, args.elf, args.gba_elf, rom, gd,
                                 pointer_mask(args.romfs, "gamedata/gamedata.bin", len(gd)))
        print("gen_recipe: %d symbol hints, %d record remaps for the game data" % (len(hints), len(remaps)))
    out = rcp.Recipe(engine_abi=abi, rom_sha1=sha1, release=args.release)
    totals = {"copy": 0, "fill": 0, "pointer": 0, "literal": 0, "remap": 0}
    domains: dict[str, dict] = {}
    per_file = []
    start = time.time()
    for rel, path in staging.data_files(args.romfs):
        data = path.read_bytes()
        crc = zlib.crc32(data) & 0xFFFFFFFF
        if rel in GENERATED:
            out.generated.append({"path": rel, "size": len(data), "crc": crc, "generator": GENERATED[rel]})
            continue
        mask = pointer_mask(args.romfs, rel, len(data)) if rel in BUNDLES else None
        is_gd = rel == "gamedata/gamedata.bin"
        breaks = layout_breaks(args.decomp) if rel == "maps/layouts.bin" else None
        ops, patches, stats = cover.run(data, mask, hints if is_gd else None, remaps if is_gd else None,
                                        breaks)
        entry = {"path": rel, "size": len(data), "crc": crc, "ops": ops}
        if patches:
            entry["patches"] = patches
        out.entries.append(entry)
        for k in totals:
            totals[k] += stats[k]
        dom = domains.setdefault(domain_of(rel), {"files": 0, "bytes": 0, "rom": 0, "literal": 0})
        dom["files"] += 1
        dom["bytes"] += len(data)
        dom["rom"] += stats["copy"] + stats["fill"] + stats["remap"]
        dom["literal"] += stats["literal"]
        if stats["literal"]:
            per_file.append((stats["literal"], rel))
    if args.gba_elf:
        syms = vtree_manifest.gba_symbols(args.nm, args.gba_elf)
        out.vtree = vtree_manifest.metadata(args.decomp, syms)
        missing = [m["folder"] for m in out.vtree["maps"] if m["rom"] is None]
        missing += [l["id"] for l in out.vtree["layouts"] if "name" in l and l["rom"] is None]
        if missing:
            raise SystemExit("gen_recipe: no ROM symbol for %s" % missing[:5])
        lit_before = len(cover.literals)
        for rel in vtree_manifest.binary_inputs(args.decomp):
            data = (args.decomp / rel).read_bytes()
            crc = zlib.crc32(data) & 0xFFFFFFFF
            ops, stats = None, {"literal": 0}
            whole = rom.find(data) if data else -1
            lz = args.decomp / (rel + ".lz")
            if whole >= 0:
                ops = [["C", whole, len(data)]]
            elif lz.exists():
                off = rom.find(lz.read_bytes())
                if off >= 0 and rcp.lz77_decompress(rom, off) == data:
                    ops = [["Z", off]]
            if ops is None:
                ops, patches, stats = cover.run(data, None)
            if stats["literal"]:
                raise SystemExit("gen_recipe: voxel input %s is not in the ROM (%d literal bytes)"
                                 % (rel, stats["literal"]))
            out.inputs.append({"path": rel, "size": len(data), "crc": crc, "ops": ops})
        if len(cover.literals) != lit_before:
            raise SystemExit("gen_recipe: voxel inputs must not need literals")
        print("gen_recipe: %d voxel generator inputs, %d maps, %d layouts"
              % (len(out.inputs), len(out.vtree["maps"]), len(out.vtree["layouts"])))
    out.literals = bytes(cover.literals)
    out.bitmaps = cover.bitmaps
    # Self-check: the recipe must rebuild every file exactly.
    for entry in out.entries + out.inputs:
        rcp.build_entry(entry, rom, out.literals, out.bitmaps)
    per_file.sort(reverse=True)
    elapsed = time.time() - start
    print("gen_recipe: %d files from the ROM, %d generated, %.1fs" % (len(out.entries), len(out.generated), elapsed))
    print("gen_recipe: parity by domain (bytes rebuilt from the ROM; the rest are executable pointers "
          "or unexplained literals):")
    for name, d in sorted(domains.items()):
        print("   %-10s %5d files %9d bytes  %6.2f%% from the ROM  %6d unexplained"
              % (name, d["files"], d["bytes"], 100.0 * d["rom"] / max(d["bytes"], 1), d["literal"]))
    for g in out.generated:
        print("   %-10s %s: regenerated by the builder, checked by CRC" % ("voxel", g["path"]))
    print("gen_recipe: copy %d  fill %d  remap %d  pointer %d  unexplained literal %d bytes"
          % (totals["copy"], totals["fill"], totals["remap"], totals["pointer"], totals["literal"]))
    for n, rel in per_file[:15]:
        print("   %8d  %s" % (n, rel))
    if args.report:
        args.report.write_text("".join("%d %s\n" % x for x in per_file), encoding="utf-8")
    if args.image_map and args.image_elf and args.gba_elf:
        gd_entry = dict(next(e for e in out.entries if e["path"] == "gamedata/gamedata.bin"))
        gd_entry["_data"] = (args.romfs / "gamedata" / "gamedata.bin").read_bytes()
        gd_mask = pointer_mask(args.romfs, "gamedata/gamedata.bin", gd_entry["size"])
        kinds, game = classify_literals(gd_entry, gd_mask, args.image_map, args.image_elf,
                                        args.gba_elf, args.nm)
        print("gen_recipe: game-data literal by origin: padding %(padding)d  zero %(zero)d  "
              "code %(code)d  string %(string)d  port %(port)d  game %(game)d" % kinds)
        for n, name in game[:25]:
            print("   %6d  %s" % (n, name))
        if kinds["game"] > args.max_game_literal:
            raise SystemExit("gen_recipe: %d literal bytes of the game's own objects exceed "
                             "--max-game-literal %d" % (kinds["game"], args.max_game_literal))
    if totals["literal"] > args.max_literal:
        raise SystemExit("gen_recipe: %d unexplained literal bytes exceed --max-literal %d"
                         % (totals["literal"], args.max_literal))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    out.save(args.out)
    print("gen_recipe: %s (%d KiB, ABI %08x)" % (args.out, args.out.stat().st_size >> 10, abi))


if __name__ == "__main__":
    main()
