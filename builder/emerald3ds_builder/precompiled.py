"""Precompiled generator outputs: the voxel data rebuilt without the generators.

The voxel generators (3ds_port/scripts/gen_voxel_*.py) spend minutes reading
the drawing: which cells are cliffs, how high each lattice point stands, what
shape every building has. None of that depends on who runs the builder, only
on the ROM, and the release supports a fixed set of ROMs. So the release runs
the generators once per supported ROM (tools/voxel_precompile.py) and stores,
for each output file, a *program* that writes the very same bytes. The
builder runs the program instead of the generators.

A program never holds the game's pictures. Every texel of a building's
texture is written as a reference into the player's own ROM: the tileset's
tile, the row and column in it, the palette - the tiles and palettes being the
recipe's voxel `inputs` (tiles.4bpp, palettes/NN.gbapal), rebuilt from the ROM
as before. What the program holds literally is the port's own data
(geometry, heights, placements, shapes, masks) and the texels no palette of
the ROM has (none, for the files of this release; the compiler reports them
and the release audit fails on them). A building's window glass, which the
generator makes from the colour of each window texel, is written the same
way: from the texel the program already wrote.

A program is a byte string (it lives in the recipe's literal pool, op "V":
recipe.py) of operations run left to right, appending to the output:

    0x01 LIT   n:varint, n bytes             own bytes
    0x02 ZERO  n:varint                      n zero bytes
    0x03 COPY  src:varint, n:varint          n bytes already written at src
    0x04 PATHS n:varint, n x (len:varint, utf-8 path)
                                             the input files sources name
    0x05 SRCS  n:varint, n x (tiles:varint, tile:varint, palette:varint)
                                             tile sources: path ids of a 4bpp
                                             tiles file and of a 16-colour
                                             palette, and the tile's number in
                                             that file
    0x06 TEX   w:varint, h:varint, order:u8, then runs filling w*h texels
                                             a block of RGBA5551 texels (u16
                                             LE), raster order (0) or the
                                             PICA200's 8x8 Morton tiles (1)
    0x07 COLOR palette:varint, index:u8      one RGBA5551 palette colour, opaque
    0x08 PIXEL tiles:varint, tile:varint, pixel:u8, high:u8
                                             one u8: high | a tile's colour
                                             index at pixel (row * 8 + column)
    0x09 GLOW  then runs filling one byte per texel of the last TEX block
                                             a window-glass mask (L8, in the
                                             block's order)

TEX runs (byte k, then its fields), in raster order:

    k & 3 == 0  SKIP  n:varint               n transparent texels (0)
    k & 3 == 1  LIT   n:varint, n x u16      n texels of the port's own
    k & 3 == 2  TILE  len = (k >> 2 & 7) + 1, source:varint, b:u8
                      `len` texels of row b & 7 of a tile source, from
                      column b >> 3 & 7, leftwards when b & 0x40, with the
                      alpha bit clear when b & 0x80
    k & 3 == 3  PAL   palette:varint, b:u8, n:varint
                      n texels of colour b & 15 of a palette (path id),
                      alpha clear when b & 0x80

GLOW runs (byte k, then n:varint):

    k == 0  SKIP   n zero bytes
    k == 1  LIT    n bytes of the port's own (a fixed glass level), inline
    k == 2  TEXEL  n bytes from the block's texels at the same places:
                   min(255, 128 + (r + g + b) // 6) with r, g, b the texel's
                   colour as the generator reads it (c5 * 255 // 31) -
                   voxel_night.window_mask's glass

A palette colour c (GBA BGR555) becomes the texel
(c & 31) << 11 | (c >> 5 & 31) << 6 | (c >> 10 & 31) << 1 | alpha, exactly
the generators' conversion of the same colour (dump_region_art.py reads it
as c * 255 // 31, and RGBA5551 keeps its top five bits: c again).

`run` refuses (ProgramError) a program that is damaged or would write more
than `limit` bytes, before allocating them.
"""

from __future__ import annotations

from array import array

LIT, ZERO, COPY, PATHS, SRCS, TEX, COLOR, PIXEL, GLOW = range(1, 10)
SKIP_RUN, LIT_RUN, TILE_RUN, PAL_RUN = range(4)
GLOW_SKIP, GLOW_LIT, GLOW_TEXEL = range(3)
RASTER, PICA = 0, 1
LITTLE = array("H", [1]).tobytes() == b"\x01\x00"


class ProgramError(Exception):
    pass


def texel(colour: int, alpha: int = 1) -> int:
    """A GBA BGR555 colour as an RGBA5551 texel."""
    return (colour & 31) << 11 | (colour >> 5 & 31) << 6 | (colour >> 10 & 31) << 1 | alpha


def glass(value: int) -> int:
    """voxel_night.window_mask's glass level over an RGBA5551 texel."""
    r, g, b = (value >> 11 & 31) * 255 // 31, (value >> 6 & 31) * 255 // 31, (value >> 1 & 31) * 255 // 31
    return min(255, 128 + (r + g + b) // 6)


# The glass levels voxel_night writes of its own (the Devon building's
# windows), not read off a texel: the only glass a program may carry literally.
OWN_GLASS = frozenset({180})

_GLASS = bytes(glass(v << 1) for v in range(1 << 15))   # by texel >> 1

_PERMUTATIONS: dict[tuple[int, int], list[int]] = {}


def pica_pairs(w: int, h: int) -> list[int]:
    """For each pair of texels of a w x h texture in the PICA200's order (8x8
    tiles in rows, Morton order inside: gen_voxel_buildings.texel_offset),
    the raster index of that pair. A pair is two texels side by side (x, x+1,
    x even): the Morton order keeps them together, so a page is reordered 32
    bits at a time."""
    key = (w, h)
    if key not in _PERMUTATIONS:
        if w <= 0 or h <= 0 or w % 8 or h % 8:
            raise ProgramError("a PICA texture is whole 8x8 tiles, not %dx%d" % (w, h))
        half = w // 2
        tile = []           # the 32 pairs of a tile, in order: (dx/2, dy)
        for m in range(32):
            # pair bit 0 is y bit 0, bit 1 x bit 1, bit 2 y bit 1, bit 3 x bit 2, bit 4 y bit 2
            dx = (m >> 1 & 1) | (m >> 3 & 1) << 1
            dy = (m & 1) | (m >> 2 & 1) << 1 | (m >> 4 & 1) << 2
            tile.append(dy * half + dx)
        perm = []
        for ty in range(0, h, 8):
            for tx in range(0, half, 4):
                base = ty * half + tx
                perm.extend(base + t for t in tile)
        _PERMUTATIONS[key] = perm
    return _PERMUTATIONS[key]


class Sources:
    """The input files a program reads, by path: `read(path) -> bytes`."""

    def __init__(self, read):
        self.read = read
        self.files: dict[str, bytes] = {}
        self.palettes: dict[str, list[int]] = {}

    def file(self, path: str) -> bytes:
        if path not in self.files:
            data = self.read(path)
            if data is None:
                raise ProgramError("missing input %s" % path)
            self.files[path] = data
        return self.files[path]

    def palette(self, path: str) -> list[int]:
        if path not in self.palettes:
            raw = self.file(path)
            if len(raw) < 32:
                raise ProgramError("palette %s is %d bytes" % (path, len(raw)))
            self.palettes[path] = [raw[2 * i] | raw[2 * i + 1] << 8 for i in range(16)]
        return self.palettes[path]


class _Reader:
    __slots__ = ("data", "pos")

    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def u8(self) -> int:
        if self.pos >= len(self.data):
            raise ProgramError("program truncated")
        b = self.data[self.pos]
        self.pos += 1
        return b

    def varint(self) -> int:
        data, pos = self.data, self.pos
        value = shift = 0
        while True:
            if pos >= len(data) or shift > 56:
                raise ProgramError("damaged number in the program")
            b = data[pos]
            pos += 1
            value |= (b & 0x7F) << shift
            if b < 0x80:
                self.pos = pos
                return value
            shift += 7

    def take(self, n: int) -> bytes:
        out = self.data[self.pos:self.pos + n]
        if len(out) != n:
            raise ProgramError("program truncated")
        self.pos += n
        return out

    def path(self) -> str:
        try:
            return self.take(self.varint()).decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ProgramError("damaged path in the program") from exc


def run(program: bytes, sources: Sources, stats: dict | None = None, limit: int = 1 << 30) -> bytes:
    """The bytes a program writes, at most `limit`. `stats`, when given,
    counts what came from where: bytes of each operation, texels of each run
    kind."""
    try:
        return _run(program, sources, stats, limit)
    except (IndexError, ValueError, OverflowError, KeyError) as exc:
        raise ProgramError("damaged program (%s: %s)" % (type(exc).__name__, exc)) from exc


def _run(program, sources, stats, limit):
    r = _Reader(program)
    out = bytearray()
    paths: list[str] = []
    srcs: list[tuple[int, int, int]] = []
    rows: dict[tuple, bytes] = {}       # (source, row, leftwards, alpha) -> its 8 texels, LE
    last_tex = None                     # (offset, texels) of the last TEX block
    count = stats.setdefault if stats is not None else None

    def add(key, n):
        if count is not None:
            stats[key] = count(key, 0) + n

    def room(n):
        if n < 0 or len(out) + n > limit:
            raise ProgramError("the program writes past the file's size")

    def tile_row(s: int, row: int, left: bool, alpha: int) -> bytes:
        key = (s, row, left, alpha)
        got = rows.get(key)
        if got is None:
            tiles_id, tile, pal_id = srcs[s]
            data = sources.file(paths[tiles_id])
            pal = sources.palette(paths[pal_id])
            base = tile * 32 + row * 4
            if base + 4 > len(data):
                raise ProgramError("tile %d past the end of %s" % (tile, paths[tiles_id]))
            idx = []
            for b in data[base:base + 4]:
                idx += (b & 15, b >> 4)
            if left:
                idx.reverse()
            got = array("H", [texel(pal[i], alpha) for i in idx])
            if not LITTLE:
                got.byteswap()
            got = got.tobytes()
            rows[key] = got
        return got

    end = len(program)
    while r.pos < end:
        op = r.u8()
        if op == LIT:
            n = r.varint()
            room(n)
            out += r.take(n)
            add("literal", n)
        elif op == ZERO:
            n = r.varint()
            room(n)
            out += bytes(n)
            add("zero", n)
        elif op == COPY:
            src, n = r.varint(), r.varint()
            room(n)
            if src + n > len(out):
                raise ProgramError("copy past what is written")
            out += out[src:src + n]
            add("copy", n)
        elif op == PATHS:
            for _ in range(r.varint()):
                paths.append(r.path())
        elif op == SRCS:
            for _ in range(r.varint()):
                srcs.append((r.varint(), r.varint(), r.varint()))
        elif op == TEX:
            w, h, order = r.varint(), r.varint(), r.u8()
            total = w * h
            room(2 * total)
            block = bytearray()
            while len(block) < 2 * total:
                k = r.u8()
                kind = k & 3
                if kind == SKIP_RUN:
                    n = r.varint()
                    if len(block) + 2 * n > 2 * total:
                        break
                    block += bytes(2 * n)
                    add("texel skip", n)
                elif kind == LIT_RUN:
                    n = r.varint()
                    if len(block) + 2 * n > 2 * total:
                        break
                    block += r.take(2 * n)
                    add("texel literal", n)
                elif kind == TILE_RUN:
                    n = (k >> 2 & 7) + 1
                    s = r.varint()
                    b = r.u8()
                    col = b >> 3 & 7
                    if col + n > 8:
                        raise ProgramError("tile run past its row")
                    block += tile_row(s, b & 7, bool(b & 0x40), 0 if b & 0x80 else 1)[2 * col:2 * (col + n)]
                    add("texel tile", n)
                else:
                    pal_id = r.varint()
                    b = r.u8()
                    n = r.varint()
                    if len(block) + 2 * n > 2 * total:
                        break
                    value = texel(sources.palette(paths[pal_id])[b & 15], 0 if b & 0x80 else 1)
                    block += bytes((value & 0xFF, value >> 8)) * n
                    add("texel palette", n)
            if len(block) != 2 * total:
                raise ProgramError("texel runs overrun their block")
            if order == PICA:
                pairs = array("I", bytes(block))
                if pairs.itemsize != 4:
                    raise ProgramError("no 32-bit array type")
                block = array("I", [pairs[i] for i in pica_pairs(w, h)]).tobytes()
            elif order != RASTER:
                raise ProgramError("unknown texel order %d" % order)
            last_tex = (len(out), total)
            out += block
        elif op == GLOW:
            if last_tex is None:
                raise ProgramError("glass with no texels before it")
            at, total = last_tex
            room(total)
            glow = bytearray()
            while len(glow) < total:
                k, n = r.u8(), r.varint()
                if len(glow) + n > total:
                    raise ProgramError("glass runs overrun their block")
                if k == GLOW_SKIP:
                    glow += bytes(n)
                    add("glass skip", n)
                elif k == GLOW_LIT:
                    glow += r.take(n)
                    add("glass literal", n)
                elif k == GLOW_TEXEL:
                    i = at + 2 * len(glow)
                    glow += bytes(_GLASS[(out[i + 2 * j] | out[i + 2 * j + 1] << 8) >> 1] for j in range(n))
                    add("glass texel", n)
                else:
                    raise ProgramError("unknown glass run %d" % k)
            out += glow
        elif op == COLOR:
            pal_id, index = r.varint(), r.u8()
            room(2)
            value = texel(sources.palette(paths[pal_id])[index & 15])
            out += bytes((value & 0xFF, value >> 8))
            add("colour", 2)
        elif op == PIXEL:
            tiles_id, tile, pixel, high = r.varint(), r.varint(), r.u8(), r.u8()
            room(1)
            data = sources.file(paths[tiles_id])
            if tile * 32 + 32 > len(data) or pixel > 63:
                raise ProgramError("pixel %d of tile %d past %s" % (pixel, tile, paths[tiles_id]))
            b = data[tile * 32 + pixel // 2]
            out.append(high | (b >> 4 if pixel & 1 else b & 15))
            add("pixel", 1)
        else:
            raise ProgramError("unknown operation %d at %d" % (op, r.pos - 1))
    return bytes(out)


def summary(program: bytes) -> dict:
    """What a program writes, by origin, without running it: "paths" (the
    inputs it reads) and counts - "literal", "zero", "copy" (bytes of the
    port's own, written as they are or repeated), "tile texels", "palette
    texels", "colours", "pixels", "glass texels" (read from the ROM's inputs
    or derived from what was), "own texels" (texels written literally),
    "own glass" (glass bytes written literally), "skip texels" (clear)."""
    r = _Reader(program)
    out = {"paths": [], "literal": 0, "zero": 0, "copy": 0, "tile texels": 0, "palette texels": 0,
           "own texels": 0, "skip texels": 0, "colours": 0, "pixels": 0, "glass texels": 0,
           "own glass": 0, "own glass values": set()}
    last = None
    end = len(program)
    try:
        while r.pos < end:
            op = r.u8()
            if op == LIT:
                out["literal"] += len(r.take(r.varint()))
            elif op == ZERO:
                out["zero"] += r.varint()
            elif op == COPY:
                r.varint()
                out["copy"] += r.varint()
            elif op == PATHS:
                for _ in range(r.varint()):
                    out["paths"].append(r.path())
            elif op == SRCS:
                for _ in range(r.varint()):
                    r.varint(), r.varint(), r.varint()
            elif op == TEX:
                w, h, _ = r.varint(), r.varint(), r.u8()
                last = left = w * h
                while left > 0:
                    k = r.u8()
                    kind = k & 3
                    if kind == SKIP_RUN:
                        n = r.varint()
                        out["skip texels"] += n
                    elif kind == LIT_RUN:
                        n = r.varint()
                        r.take(2 * n)
                        out["own texels"] += n
                    elif kind == TILE_RUN:
                        r.varint(), r.u8()
                        n = (k >> 2 & 7) + 1
                        out["tile texels"] += n
                    else:
                        r.varint(), r.u8()
                        n = r.varint()
                        out["palette texels"] += n
                    left -= n
                if left:
                    raise ProgramError("texel runs overrun their block")
            elif op == GLOW:
                if last is None:
                    raise ProgramError("glass with no texels before it")
                left = last
                while left > 0:
                    k, n = r.u8(), r.varint()
                    if k == GLOW_LIT:
                        out["own glass values"].update(r.take(n))
                        out["own glass"] += n
                    elif k == GLOW_TEXEL:
                        out["glass texels"] += n
                    elif k != GLOW_SKIP:
                        raise ProgramError("unknown glass run %d" % k)
                    left -= n
                if left:
                    raise ProgramError("glass runs overrun their block")
            elif op == COLOR:
                r.varint(), r.u8()
                out["colours"] += 1
            elif op == PIXEL:
                r.varint(), r.varint(), r.u8(), r.u8()
                out["pixels"] += 1
            else:
                raise ProgramError("unknown operation %d at %d" % (op, r.pos - 1))
    except (IndexError, ValueError, OverflowError) as exc:
        raise ProgramError("damaged program (%s)" % exc) from exc
    return out
