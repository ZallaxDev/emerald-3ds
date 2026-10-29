#!/usr/bin/env python3
"""Precompile the voxel generators' outputs into the recipe.

    python tools/voxel_precompile.py --recipe dist/emerald3ds.recipe --rom baserom.gba

The voxel generators (3ds_port/scripts/gen_voxel_*.py, gen_intro_margins.py)
take minutes; their result depends only on the ROM. This runs them once, here,
the way the player's builder runs them (builder/emerald3ds_builder: the tree
rebuilt from the ROM by the recipe's `inputs`, the payload's scripts), checks
that every output is the one the release was built with (the recipe's CRC:
the game's executable was built against those files), and stores in the
recipe, for each output, a program that writes it again
(builder/emerald3ds_builder/precompiled.py). The builder then runs the
programs instead of the generators.

What a program holds:

* the port's own data, literally: geometry, heights, placements, shapes,
  masks, tables - everything that is not a colour;
* every texel of a building's texture as a reference into the ROM: the tile
  source (tileset tiles, tile, palette), row, column and direction it reads -
  found by matching the texture against every tile row the metatiles of the
  tilesets draw, preferring the tilesets of the page's own map and the
  source the texel beside or above came from;
* the animation table's palette colours and background colours as
  references to a palette and an index;
* the texels nothing of the ROM matches, literally, counted in the report
  (`--max-own-texels` fails the run above that count).

Nothing is written unless every program reproduces its file byte for byte.
`--outputs DIR` compiles already generated files instead of running the
generators (they must still match the recipe's CRCs).
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import sys
import tempfile
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder"))
from emerald3ds_builder import precompiled as pc  # noqa: E402
from emerald3ds_builder.recipe import Recipe  # noqa: E402

NUM_PRIMARY = 512


# ── encoding ────────────────────────────────────────────────────────────────

def varint(n: int) -> bytes:
    if n < 0:
        raise ValueError("varint of %d" % n)
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


class Program:
    """A program under construction: ops appended in output order."""

    def __init__(self):
        self.body = bytearray()
        self.paths: list[str] = []
        self.path_id: dict[str, int] = {}
        self.srcs: list[tuple[int, int, int]] = []
        self.src_id: dict[tuple, int] = {}
        self.pending = bytearray()      # literal bytes not yet emitted
        self.size = 0                   # bytes the program writes so far

    def path(self, p: str) -> int:
        if p not in self.path_id:
            self.path_id[p] = len(self.paths)
            self.paths.append(p)
        return self.path_id[p]

    def source(self, tiles: str, tile: int, palette: str) -> int:
        key = (self.path(tiles), tile, self.path(palette))
        if key not in self.src_id:
            self.src_id[key] = len(self.srcs)
            self.srcs.append(key)
        return self.src_id[key]

    def _flush(self):
        data = bytes(self.pending)
        self.pending.clear()
        i = 0
        while i < len(data):
            # long zero runs as ZERO, the rest as LIT
            j = data.find(bytes(64), i)
            if j < 0:
                j = len(data)
            if j > i:
                self.body += bytes([pc.LIT]) + varint(j - i) + data[i:j]
            if j >= len(data):
                break
            k = j
            while k < len(data) and data[k] == 0:
                k += 1
            self.body += bytes([pc.ZERO]) + varint(k - j)
            i = k

    def lit(self, data: bytes):
        self.pending += data
        self.size += len(data)

    def raw(self, op: bytes, written: int):
        self._flush()
        self.body += op
        self.size += written

    def copy(self, src: int, n: int):
        self.raw(bytes([pc.COPY]) + varint(src) + varint(n), n)

    def colour(self, palette: str, index: int):
        self.raw(bytes([pc.COLOR]) + varint(self.path(palette)) + bytes([index]), 2)

    def bytes(self) -> bytes:
        self._flush()
        head = bytearray()
        if self.paths:
            head += bytes([pc.PATHS]) + varint(len(self.paths))
            for p in self.paths:
                b = p.encode("utf-8")
                head += varint(len(b)) + b
        if self.srcs:
            head += bytes([pc.SRCS]) + varint(len(self.srcs))
            for t, n, p in self.srcs:
                head += varint(t) + varint(n) + varint(p)
        return bytes(head + self.body)


# ── the ROM's tile rows ─────────────────────────────────────────────────────

class TilePool:
    """Every tile row the tilesets' metatiles draw: each (tiles file, tile,
    palette file) a metatile entry of some layout's tileset pair names, read
    left to right and right to left, as texels."""

    def __init__(self, tree: Path, input_paths: set[str]):
        self.tree = tree
        scripts = tree / "3ds_port" / "scripts"
        sys.path.insert(0, str(scripts))
        import dump_region_art as dra      # the generators' own reading of the tree
        self.dra = dra
        mts = dra.incbin_map(r"gMetatiles_(\w+)\[\]\s*=\s*INCBIN_U16\(\"([^\"]+)\"\)")
        layouts = json.loads((tree / "data/layouts/layouts.json").read_text(encoding="utf-8"))["layouts"]
        self.layout_pair: dict[int, tuple[str, str]] = {}
        files: dict[str, bytes] = {}

        def rel(path: str) -> str:
            return Path(path).resolve().relative_to(tree.resolve()).as_posix()

        def tiles_of(ts):
            d = dra.tileset_dir(ts)
            return None if d is None else rel(os.path.join(d, "tiles.4bpp"))

        def pal_of(ts, p):
            d = dra.tileset_dir(ts)
            return None if d is None else rel(os.path.join(d, "palettes", "%02d.gbapal" % p))

        self.sources: list[tuple[str, int, str]] = []
        self.source_id: dict[tuple, int] = {}
        self.pair_sources: dict[tuple[str, str], set[int]] = {}
        self.pair_palettes: dict[tuple[str, str], list[str]] = {}
        for lid, e in enumerate(layouts, start=1):
            if "primary_tileset" not in e:
                continue
            self.layout_pair[lid] = (e["primary_tileset"], e["secondary_tileset"])
        for pair in sorted(set(self.layout_pair.values())):
            p, s = pair
            mine = set()
            self.pair_palettes[pair] = [x for x in (pal_of(p if i < 6 else s, i) for i in range(13))
                                        if x in input_paths]
            for ts in (p, s):
                if ts not in mts or not os.path.exists(mts[ts]):
                    continue
                for v in dra.read_u16(mts[ts]):
                    t, pal = v & 0x3FF, v >> 12
                    tiles = tiles_of(p if t < NUM_PRIMARY else s)
                    palette = pal_of(p if pal < 6 else s, pal)
                    if tiles not in input_paths or palette not in input_paths:
                        continue
                    if tiles not in files:
                        files[tiles] = (tree / tiles).read_bytes()
                    local = t % NUM_PRIMARY
                    if (local + 1) * 32 > len(files[tiles]):
                        continue
                    key = (tiles, local, palette)
                    if key not in self.source_id:
                        self.source_id[key] = len(self.sources)
                        self.sources.append(key)
                    mine.add(self.source_id[key])
            self.pair_sources[pair] = mine
        # the animations' frames a model's texels follow, over every palette of
        # the pairs whose secondary tileset they belong to
        for path in sorted(input_paths):
            if "/anim/" not in path or not path.endswith(".4bpp"):
                continue
            folder = path.split("/anim/")[0]
            data = (tree / path).read_bytes()
            files[path] = data
            for pair, pals in sorted(self.pair_palettes.items()):
                if not any(x and x.startswith(folder + "/") for x in pals):
                    continue
                for k in range(len(data) // 32):
                    for palette in pals:
                        key = (path, k, palette)
                        if key not in self.source_id:
                            self.source_id[key] = len(self.sources)
                            self.sources.append(key)
                        self.pair_sources[pair].add(self.source_id[key])
        for path in sorted(input_paths):
            if path.endswith(".gbapal"):
                files[path] = (tree / path).read_bytes()
        self._index(files)

    @classmethod
    def of(cls, files: dict[str, bytes], sources: list[tuple[str, int, str]],
           layout_pair: dict | None = None, pair_sources: dict | None = None) -> "TilePool":
        """A pool over the given files and sources (tests)."""
        self = cls.__new__(cls)
        self.sources = list(sources)
        self.source_id = {k: i for i, k in enumerate(self.sources)}
        self.layout_pair = layout_pair or {}
        self.pair_sources = pair_sources or {}
        self.pair_palettes = {}
        self._index(files)
        return self

    def _index(self, files: dict[str, bytes]) -> None:
        self.palettes = {}
        for path, raw in sorted(files.items()):
            if path.endswith(".gbapal"):
                self.palettes[path] = [raw[2 * i] | raw[2 * i + 1] << 8 for i in range(16)]
        # rows: content (8 opaque texels) -> id; id -> its first (source, row, leftwards)
        self.content_id: dict[tuple, int] = {}
        self.content: list[tuple] = []
        self.where: list[list[tuple[int, int, int]]] = []
        self.row_content: dict[tuple[int, int, int], int] = {}
        for s, (tiles, local, palette) in enumerate(self.sources):
            data, pal = files[tiles], self.palettes[palette]
            for r in range(8):
                idx = []
                for b in data[local * 32 + r * 4:local * 32 + r * 4 + 4]:
                    idx += (b & 15, b >> 4)
                for left in (0, 1):
                    vals = tuple(pc.texel(pal[i]) for i in (idx[::-1] if left else idx))
                    cid = self.content_id.get(vals)
                    if cid is None:
                        cid = self.content_id[vals] = len(self.content)
                        self.content.append(vals)
                        self.where.append([])
                    self.where[cid].append((s, r, left))
                    self.row_content[(s, r, left)] = cid
        self.idx4: dict[tuple, list[tuple[int, int]]] = {}
        self.idx1: dict[int, list[tuple[int, int]]] = {}
        for cid, vals in enumerate(self.content):
            for col in range(5):
                self.idx4.setdefault(vals[col:col + 4], []).append((cid, col))
            for col in range(8):
                self.idx1.setdefault(vals[col], []).append((cid, col))
        # colour -> (palette file, index): the pair's palettes first, then all
        self.colour_at: dict[int, list[tuple[str, int]]] = {}
        for path, pal in self.palettes.items():
            for i, c in enumerate(pal):
                self.colour_at.setdefault(pc.texel(c), []).append((path, i))

    def pair_of(self, layout: int | None) -> tuple[str, str] | None:
        return None if layout is None else self.layout_pair.get(layout)

    def colour_ref(self, value: int, pair) -> tuple[str, int] | None:
        found = self.colour_at.get(value | 1)
        if not found:
            return None
        if pair is not None:
            mine = self.pair_palettes.get(pair, ())
            for path, i in found:
                if path in mine:
                    return path, i
        return found[0]


class Matcher:
    """Texels -> TEX runs over a TilePool."""

    MAX_CANDIDATES = 64

    def __init__(self, pool: TilePool):
        self.pool = pool
        # tile: texels from a tile row ("tile in map": of the tilesets of the
        # page's own map); palette: a palette colour; own: written literally
        self.stats = {"tile": 0, "tile in map": 0, "palette": 0, "own": 0, "skip": 0, "runs": 0}

    def _best_place(self, cid: int, mine: set[int]):
        where = self.pool.where[cid]
        for w in where:
            if w[0] in mine:
                return w, True
        return where[0], False

    def encode(self, prog: Program, w: int, h: int, order: int, values: list[int], layout: int | None):
        pool = self.pool
        pair = pool.pair_of(layout)
        mine = pool.pair_sources.get(pair, set()) if pair else set()
        out = bytearray([pc.TEX]) + varint(w) + varint(h) + bytes([order])
        prev_row: list = [None] * w      # x -> (s, r, left, col) the texel came from
        content, row_content = pool.content, pool.row_content
        idx4, idx1 = pool.idx4, pool.idx1
        pending_lit: list[int] = []

        def flush_lit():
            if pending_lit:
                out.extend(bytes([pc.LIT_RUN]) + varint(len(pending_lit))
                           + struct.pack("<%dH" % len(pending_lit), *pending_lit))
                self.stats["own"] += len(pending_lit)
                pending_lit.clear()

        for y in range(h):
            row = values[y * w:(y + 1) * w]
            here: list = [None] * w
            x = 0
            last = None          # (s, r, left, col_at_x0, x0) of the last tile run
            while x < w:
                v = row[x]
                if v == 0:
                    n = 1
                    while x + n < w and row[x + n] == 0:
                        n += 1
                    flush_lit()
                    out.extend(bytes([pc.SKIP_RUN]) + varint(n))
                    self.stats["skip"] += n
                    x += n
                    continue
                alpha = v & 1
                L = 1
                while L < 8 and x + L < w and row[x + L] != 0 and (row[x + L] & 1) == alpha:
                    L += 1
                want = [t | 1 for t in row[x:x + L]]

                def length(vals, col):
                    k = 0
                    m = min(L, 8 - col)
                    while k < m and vals[col + k] == want[k]:
                        k += 1
                    return k

                best = None      # (length, in pair, rank, (s, r, left, col))
                hints = []
                if last is not None:
                    s, r, left, c0, x0 = last
                    c = c0 + (x - x0)
                    if c < 8:
                        hints.append((s, r, left, c))
                up = prev_row[x]
                if up is not None:
                    s, r, left, c = up
                    for rr in (r + 1, r - 1):
                        if 0 <= rr < 8:
                            hints.append((s, rr, left, c))
                for rank, (s, r, left, c) in enumerate(hints):
                    vals = content[row_content[(s, r, left)]]
                    k = length(vals, c)
                    if k and (best is None or (k, s in mine, -rank) > best[:3]):
                        best = (k, s in mine, -rank, (s, r, left, c))
                if best is None or best[0] < L:
                    cands = idx4.get(tuple(want[:4])) if L >= 4 else None
                    if not cands:
                        cands = idx1.get(want[0], ())
                    for rank, (cid, col) in enumerate(cands[:self.MAX_CANDIDATES]):
                        k = length(content[cid], col)
                        if not k:
                            continue
                        (s, r, left), inside = self._best_place(cid, mine)
                        key = (k, inside, -100 - rank)
                        if best is None or key > best[:3]:
                            best = key + ((s, r, left, col),)
                if best is not None:
                    k, _, _, (s, r, left, c) = best
                    tiles, local, palette = pool.sources[s]
                    sid = prog.source(tiles, local, palette)
                    flush_lit()
                    out.extend(bytes([pc.TILE_RUN | (k - 1) << 2]) + varint(sid)
                               + bytes([r | c << 3 | left << 6 | (0 if alpha else 0x80)]))
                    self.stats["tile"] += k
                    self.stats["tile in map"] += k if s in mine else 0
                    self.stats["runs"] += 1
                    for i in range(k):
                        here[x + i] = (s, r, left, c + i)
                    last = (s, r, left, c, x)
                    x += k
                    continue
                last = None
                ref = pool.colour_ref(v, pair)
                if ref is not None:
                    n = 1
                    while x + n < w and row[x + n] == v:
                        n += 1
                    flush_lit()
                    out.extend(bytes([pc.PAL_RUN]) + varint(prog.path(ref[0]))
                               + bytes([ref[1] | (0 if alpha else 0x80)]) + varint(n))
                    self.stats["palette"] += n
                    x += n
                    continue
                pending_lit.append(v)
                x += 1
            prev_row = here
        flush_lit()
        prog.raw(bytes(out), 2 * w * h)


# ── the files ───────────────────────────────────────────────────────────────

def compile_plain(data: bytes) -> Program:
    """A file that holds no colour of the game: all of it the port's own."""
    prog = Program()
    prog.lit(data)
    return prog


def inverse_pica(block: bytes, w: int, h: int) -> list[int]:
    """Texels of a PICA-ordered block, in raster order."""
    pairs = struct.unpack("<%dI" % (w * h // 2), block)
    raster = [0] * (w * h // 2)
    for dst, src in enumerate(pc.pica_pairs(w, h)):
        raster[src] = pairs[dst]
    out = []
    for p in raster:
        out += (p & 0xFFFF, p >> 16)
    return out


def encode_glass(prog: Program, block: bytes, glow: bytes, matcher: Matcher) -> None:
    """A page's glass bytes (same order as its texels `block`) as GLOW runs."""
    runs = bytearray([pc.GLOW])
    i = 0
    while i < len(glow):
        def kind(j):
            v = glow[j]
            if v == 0:
                return pc.GLOW_SKIP
            if v == pc.glass(block[2 * j] | block[2 * j + 1] << 8):
                return pc.GLOW_TEXEL
            return pc.GLOW_LIT
        k = kind(i)
        n = 1
        while i + n < len(glow) and kind(i + n) == k:
            n += 1
        runs += bytes([k]) + varint(n)
        if k == pc.GLOW_LIT:
            runs += glow[i:i + n]
            own = sum(1 for v in glow[i:i + n] if v not in pc.OWN_GLASS)
            matcher.stats["own"] += own
            matcher.stats["glass own"] = matcher.stats.get("glass own", 0) + n
        else:
            key = "glass texel" if k == pc.GLOW_TEXEL else "glass skip"
            matcher.stats[key] = matcher.stats.get(key, 0) + n
        i += n
    prog.raw(bytes(runs), len(glow))


def own_texel(prog: Program, value: int, matcher: Matcher) -> None:
    """One u16 no palette of the ROM gives: a 1x1 block of the port's own,
    where the release audit counts it (0, a clear texel, is not counted)."""
    if value == 0:
        prog.lit(bytes(2))
        return
    prog.raw(bytes([pc.TEX]) + varint(1) + varint(1) + bytes([pc.RASTER, pc.LIT_RUN]) + varint(1)
             + struct.pack("<H", value), 2)
    matcher.stats["own"] += 1


def compile_buildings(data: bytes, matcher: Matcher) -> Program:
    """buildings.bin (VXB9, gen_voxel_buildings.export): the pages' texels and
    the animation table's colours from the ROM, the rest as it is."""
    if data[:4] != b"VXB9":
        raise SystemExit("voxel_precompile: buildings.bin is not VXB9 (the compiler must learn the new format)")
    (npages, nmodels, npm, nplace, nheights, nmasks, nverts, nvariants, ndecks, nconds, _) = \
        struct.unpack_from("<HHHHHHIHHHH", data, 4)
    head = 4 + struct.calcsize("<HHHHHHIHHHH")
    pages = [struct.unpack_from("<HHI", data, head + 8 * i) for i in range(npages)]
    # the body: model records, then page models, then placements (their layout)
    pm_at = head + 8 * npages + 16 * nmodels
    page_models = [struct.unpack_from("<HHhh", data, pm_at + 8 * i) for i in range(npm)]
    pl_at = pm_at + 8 * npm
    page_layout: dict[int, int] = {}
    for i in range(nplace):
        lay, pm = struct.unpack_from("<HH", data, pl_at + 16 * i)
        page_layout.setdefault(page_models[pm][1], lay)
    anim_at = None
    if data[-4:] == b"ANIM":
        (anim_at,) = struct.unpack_from("<I", data, len(data) - 8)
    prog = Program()
    pos = 0
    page_at = []
    for i, (tw, rows, offset) in enumerate(pages):
        glow = rows & 0x8000
        rows &= 0x7FFF
        if offset < pos:
            raise SystemExit("voxel_precompile: buildings.bin pages out of order")
        prog.lit(data[pos:offset])
        block = data[offset:offset + 2 * tw * rows]
        matcher.encode(prog, tw, rows, pc.PICA, inverse_pica(block, tw, rows), page_layout.get(i))
        page_at.append(offset)
        pos = offset + 2 * tw * rows
        if glow:
            # the window glass: from the texel under it (voxel_night.window_mask),
            # or a level of the port's own (180, the Devon building's)
            encode_glass(prog, block, data[pos:pos + tw * rows], matcher)
            pos += tw * rows
    end = len(data) if anim_at is None else anim_at
    prog.lit(data[pos:end])
    if anim_at is not None:
        nruns, npal, nlive, nstatic = struct.unpack_from("<HHII", data, anim_at)
        at = anim_at + 12
        prog.lit(data[anim_at:at + 20 * nruns])
        runs = [struct.unpack_from("<HHIHHII", data, at + 20 * i) for i in range(nruns)]
        at += 20 * nruns
        for i in range(npal):
            for c in range(16):
                (value,) = struct.unpack_from("<H", data, at)
                ref = matcher.pool.colour_ref(value, None) if value & 1 else None
                if ref is not None and pc.texel(matcher.pool.palettes[ref[0]][ref[1]]) == value:
                    prog.colour(*ref)
                    matcher.stats["palette"] += 1
                else:
                    own_texel(prog, value, matcher)
                at += 2
        for i in range(nlive):
            prog.lit(data[at:at + 6])
            (value,) = struct.unpack_from("<H", data, at + 6)
            ref = matcher.pool.colour_ref(value, None) if value & 1 else None
            if ref is not None:
                prog.colour(*ref)
                matcher.stats["palette"] += 1
            else:
                own_texel(prog, value, matcher)
            at += 8
        for (page, layout, first, blocks, lives, first_live, first_static) in runs:
            n = 2 * 64 * blocks
            src = page_at[page] + first
            if data[at:at + n] != data[src:src + n]:
                raise SystemExit("voxel_precompile: an animation run's texels are not its page's")
            prog.copy(src, n)
            at += n
        prog.lit(data[at:])
    return prog


def compile_leaves(data: bytes, matcher: Matcher) -> Program:
    """stage/leaves.bin (gen_intro_margins.py): the intro's margins. Its new
    art is the port's own; the pixels it carries on from the picture's edge
    are written as the place in the scene's tiles they were read from
    (gen_intro_margins.PROVENANCE)."""
    import gen_intro_margins as gim         # the tree's, from TilePool's path
    gim.PROVENANCE = True
    gim.PICS.clear()
    try:
        parts = gim.generate()
    finally:
        gim.PROVENANCE = False
        gim.PICS.clear()
    tiles = gim.SCENE.relative_to(gim.ROOT).as_posix() + "/bg.4bpp"
    prog = Program()
    prog.lit(gim.MAGIC)

    def put(c):
        if isinstance(c, gim.Pixel):
            tile, pixel = c.tile_pixel
            prog.raw(bytes([pc.PIXEL]) + varint(prog.path(tiles)) + varint(tile)
                     + bytes([pixel, c[0] << 4]), 1)
            matcher.stats["tile"] += 1
        else:
            prog.lit(bytes([0xFF if c is None else (c[0] << 4) | c[1]]))

    for L in range(4):
        put(parts[(L, "sky")])
        left, right, patch = parts[(L, "left")], parts[(L, "right")], parts[(L, "patch")]
        for y in range(gim.H):
            for x in range(-gim.W, gim.PATCH):
                put(left.get(-1 - x, y) if x < 0 else patch.get((x, y)))
        for y in range(gim.H):
            for x in range(gim.W):
                put(right.get(x, y))
    if prog.size != len(data):
        raise SystemExit("voxel_precompile: leaves.bin is not gen_intro_margins.encode's layout")
    return prog


COMPILERS = {"voxel/buildings.bin": compile_buildings, "stage/leaves.bin": compile_leaves}
# The generated files that hold no colour, pixel or picture of the game (their
# formats: gen_voxel_regions.py, gen_voxel_sign_masks.write_records,
# gen_voxel_relief.export): stored as they are. A generated file in neither
# list stops the release: its format must be looked at first.
PLAIN = {"voxel/regions.bin", "voxel/signposts.bin", "voxel/relief.bin"}


# ── the run ─────────────────────────────────────────────────────────────────

def check_python() -> None:
    """The release's Python (upstream.lock [python]): another one may make
    other voxel data than the one the release is meant to carry."""
    import tomllib
    want = tomllib.loads((ROOT / "upstream.lock").read_text(encoding="utf-8")).get("python", {}).get("version")
    have = "%d.%d" % sys.version_info[:2]
    if want and have != want:
        raise SystemExit("voxel_precompile: this is Python %s; a release's voxel data is made with Python "
                         "%s (upstream.lock [python])" % (have, want))


def generate(recipe: Recipe, rom: bytes, voxelgen: Path, work: Path) -> dict[str, bytes]:
    """The generators' outputs, made as the player's builder makes them."""
    from emerald3ds_builder.voxel import run_generators
    from emerald3ds_builder.vtree import build_tree
    build_tree(rom, recipe, work)
    from emerald3ds_builder.errors import BuilderError
    timings: list = []
    try:
        out = run_generators(work, voxelgen, recipe.generated, runner="subprocess", timings=timings,
                             progress=lambda f, d=None: print("voxel_precompile: %s" % (d or ""), flush=True))
    except BuilderError as exc:
        raise SystemExit("voxel_precompile: %s %s\nThe RomFS files the recipe was written from are "
                         "older or newer than the generators, models, heights or hand corrections of "
                         "this tree: build again (make release) and rewrite the recipe."
                         % (exc.message, exc.hint or "")) from exc
    for name, sec in timings:
        print("voxel_precompile: %-28s %.1f s" % (name, sec))
    return out


def stage_voxelgen(port: Path, dst: Path) -> Path:
    """payload/voxelgen as build_release.py assembles it."""
    sys.path.insert(0, str(ROOT / "tools"))
    import build_release
    scripts, _ = build_release.local_closure(port / "scripts", build_release.GENERATORS)
    (dst / "scripts").mkdir(parents=True, exist_ok=True)
    for name in scripts:
        shutil.copy2(port / "scripts" / name, dst / "scripts" / name)
    for rel in build_release.VOXELGEN_FILES:
        (dst / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(port / rel, dst / rel)
    return dst


def precompile(recipe: Recipe, rom: bytes, outputs: dict[str, bytes], tree: Path,
               max_own_texels: int = 0, leave: tuple = ()) -> dict:
    """Programs for every generated file into `recipe` (in place), but those
    in `leave`, which the builder keeps generating. Returns a report."""
    paths = {e["path"] for e in recipe.inputs}
    # A recipe precompiled before (a reused one): its programs are the end of
    # the literal pool; they are dropped and made again.
    old = [op for e in recipe.generated for op in e.get("ops", ())]
    if old:
        cut = min(op[1] for op in old)
        used = max([op[1] + op[2] for e in recipe.entries + recipe.inputs for op in e["ops"] if op[0] == "L"]
                   + [p[1] + p[2] for e in recipe.entries for p in e.get("patches", ())] + [0])
        if any(op[0] != "V" for op in old) or used > cut:
            raise SystemExit("voxel_precompile: the recipe's generated files carry unexpected operations")
        recipe.literals = recipe.literals[:cut]
        for e in recipe.generated:
            e.pop("ops", None)
        print("voxel_precompile: the recipe's previous programs dropped")
    for entry in recipe.generated:
        data = outputs[entry["path"]]
        crc = zlib.crc32(data) & 0xFFFFFFFF
        if len(data) != entry["size"] or crc != entry["crc"]:
            raise SystemExit("voxel_precompile: the generators make another %s than the one the "
                             "release was built with (size %d, CRC %08x; recipe %d, %08x): its RomFS "
                             "file is older or newer than the generators, models, heights or hand "
                             "corrections of this tree - build again (make release) and rewrite the recipe"
                             % (entry["path"], len(data), crc, entry["size"], entry["crc"]))
    start = time.perf_counter()
    pool = TilePool(tree, paths)
    print("voxel_precompile: %d tile sources, %d distinct tile rows (%.1f s)"
          % (len(pool.sources), len(pool.content), time.perf_counter() - start))
    literals = bytearray(recipe.literals)
    report = {}
    for entry in recipe.generated:
        rel = entry["path"]
        data = outputs[rel]
        if rel in leave:
            print("voxel_precompile: %-22s left to the builder's generator" % rel)
            continue
        t = time.perf_counter()
        matcher = Matcher(pool)
        if rel in COMPILERS:
            prog = COMPILERS[rel](data, matcher)
        elif rel in PLAIN:
            prog = compile_plain(data)
        else:
            raise SystemExit("voxel_precompile: no compiler knows %s: say whether it holds the game's "
                             "colours (COMPILERS) or not (PLAIN), or leave it to the builder (--generate)"
                             % rel)
        program = prog.bytes()
        offset = len(literals)
        literals += program
        entry["ops"] = [["V", offset, len(program)]]
        report[rel] = {"bytes": len(data), "program": len(program), "seconds": round(time.perf_counter() - t, 1),
                       "texels": dict(matcher.stats)}
        print("voxel_precompile: %-22s %8d bytes -> program %8d bytes, %.1f s, texels %s"
              % (rel, len(data), len(program), time.perf_counter() - t, matcher.stats))
        if matcher.stats["own"] > max_own_texels:
            raise SystemExit("voxel_precompile: %s keeps %d texels no ROM colour matches (max %d)"
                             % (rel, matcher.stats["own"], max_own_texels))
    recipe.literals = bytes(literals)
    # the proof: what the builder will run gives every file exactly
    from emerald3ds_builder.build import build_generated
    t = time.perf_counter()
    rebuilt = build_generated(recipe, rom)
    for entry in recipe.generated:
        if entry["path"] in leave:
            continue
        if rebuilt[entry["path"]] != outputs[entry["path"]]:
            raise SystemExit("voxel_precompile: the program of %s does not rebuild it" % entry["path"])
    print("voxel_precompile: every program rebuilds its file exactly (%.1f s)" % (time.perf_counter() - t))
    return report


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--recipe", type=Path, required=True, help="recipe to complete (rewritten in place)")
    ap.add_argument("--rom", type=Path, required=True, help="the ROM the recipe is made from")
    ap.add_argument("--port", type=Path, default=ROOT / "3ds_port",
                    help="3ds_port whose generators made the release's files")
    ap.add_argument("--outputs", type=Path, default=None,
                    help="already generated files (romfs layout: voxel/*.bin, stage/leaves.bin)")
    ap.add_argument("--out", type=Path, default=None, help="write the recipe here instead")
    ap.add_argument("--report", type=Path, default=None, help="JSON report")
    ap.add_argument("--max-own-texels", type=int, default=0)
    ap.add_argument("--generate", action="append", default=[], metavar="PATH",
                    help="leave this file (e.g. voxel/signposts.bin) to the builder's generator: "
                         "no program, the builder runs its generator as before (repeatable)")
    ap.add_argument("--keep", action="store_true", help="keep the work directory")
    args = ap.parse_args()

    check_python()
    from emerald3ds_builder.rom import load_rom
    recipe = Recipe.load(args.recipe)
    rom = load_rom(args.rom)
    if rom.sha1 != recipe.rom_sha1:
        raise SystemExit("voxel_precompile: %s is not the ROM of this recipe" % args.rom)
    if not recipe.generated:
        print("voxel_precompile: the recipe has no generated files")
        return
    work = Path(tempfile.mkdtemp(prefix="emerald3ds-precompile-"))
    try:
        start = time.perf_counter()
        if args.outputs:
            from emerald3ds_builder.vtree import build_tree
            build_tree(rom.data, recipe, work)
            # the scripts as the builder places them: the pool reads the tree through them
            shutil.copytree(stage_voxelgen(args.port, work / "_voxelgen"), work / "3ds_port", dirs_exist_ok=True)
            outputs = {e["path"]: (args.outputs / e["path"]).read_bytes() for e in recipe.generated}
        else:
            voxelgen = stage_voxelgen(args.port, work / "voxelgen")
            outputs = generate(recipe, rom.data, voxelgen, work / "tree")
            work = work / "tree"
        print("voxel_precompile: outputs ready (%.1f s)" % (time.perf_counter() - start))
        tree = work
        unknown = set(args.generate) - {e["path"] for e in recipe.generated}
        if unknown:
            raise SystemExit("voxel_precompile: no generated file %s" % ", ".join(sorted(unknown)))
        report = precompile(recipe, rom.data, outputs, tree, args.max_own_texels, tuple(args.generate))
    finally:
        if not args.keep:
            shutil.rmtree(work.parent if work.name == "tree" else work, ignore_errors=True)
    out = args.out or args.recipe
    recipe.save(out)
    print("voxel_precompile: %s (%d KiB)" % (out, out.stat().st_size >> 10))
    if args.report:
        args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
