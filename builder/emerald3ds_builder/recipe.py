"""Recipes: how to rebuild each game-data file from the player's ROM.

A recipe lists, for every file of the data pack, its path, size, CRC-32 and a
sequence of operations that write it from left to right:

    ["C", rom_offset, length]         copy bytes from the ROM
    ["F", byte, length]               repeat one byte
    ["L", literal_offset, length]     bytes from the recipe's literal pool
    ["Z", rom_offset]                 LZ77-decompress (GBA BIOS format) data
                                      that starts at rom_offset
    ["R", rom_offset, src_stride, dst_stride, count, bitmap_index]
                                      copy `count` records, remapping bits:
                                      bitmap[i] is the source bit of
                                      destination bit i, or -1/-2 for a
                                      constant 0/1
    ["V", literal_offset, length]     run the program at that place of the
                                      literal pool (precompiled.py): a
                                      generated file, its texels read from
                                      the voxel `inputs`

`generated` entries (the voxel generators' outputs) carry "ops" when the
release precompiled them (tools/voxel_precompile.py); without, the builder
runs the generators.

An entry may also carry "patches": [dst_offset, literal_offset, length]
triples written over the result afterwards (pointers inside remapped records).

The literal pool holds only bytes that are not in the ROM: pointers into the
executable (they depend on its layout, not on the game) and data the port
itself adds. tools/gen_recipe.py writes recipes and audits the pool;
tools/release_audit.py re-checks it against the ROM.

Container: b"EM3DRCP1", u32 json_length, the JSON (LZMA), then the literal
pool (LZMA).
"""

from __future__ import annotations

import json
import lzma
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

MAGIC = b"EM3DRCP1"
SCHEMA = 1
HEADER_SIZE = len(MAGIC) + 4

# Metadata fields the builder reads: (key, type, required). Optional fields
# default to empty when absent but must have the right type when present.
_META_FIELDS = (("engine_abi", int, True), ("rom_sha1", str, True), ("release", str, True),
                ("entries", list, True), ("bitmaps", list, False), ("generated", list, False),
                ("inputs", list, False), ("vtree", dict, False))
_TYPE_NAMES = {int: "an integer", str: "a string", list: "a list", dict: "an object"}


class RecipeError(Exception):
    pass


def _decode_header(head: bytes) -> int:
    """The length of the compressed metadata, from the container's first bytes."""
    # A short prefix of the magic is a cut-off recipe, anything else is not one.
    if not head or head[:len(MAGIC)] != MAGIC[:len(head)]:
        raise RecipeError("not a Emerald 3DS recipe")
    if len(head) < HEADER_SIZE:
        raise RecipeError("damaged recipe: header truncated (%d of %d bytes)" % (len(head), HEADER_SIZE))
    (size,) = struct.unpack_from("<I", head, len(MAGIC))
    return size


def _decompress(blob: bytes, what: str) -> bytes:
    try:
        return lzma.decompress(blob)
    except lzma.LZMAError as exc:
        raise RecipeError("damaged recipe: %s cannot be decompressed (%s)" % (what, exc)) from exc


def _decode_meta(blob: bytes, size: int) -> dict:
    """The metadata object from its compressed blob, which should be `size` bytes."""
    if len(blob) != size:
        raise RecipeError("damaged recipe: metadata truncated (%d of %d bytes)" % (len(blob), size))
    raw = _decompress(blob, "metadata")
    try:
        meta = json.loads(raw)
    except ValueError as exc:   # JSONDecodeError and UnicodeDecodeError
        raise RecipeError("damaged recipe: metadata is not valid JSON (%s)" % exc) from exc
    if not isinstance(meta, dict):
        raise RecipeError("damaged recipe: metadata is not an object")
    return meta


def _check_field(meta: dict, key: str, kind: type, required: bool) -> None:
    if key not in meta:
        if required:
            raise RecipeError("damaged recipe: metadata has no %r" % key)
        return
    value = meta[key]
    if not isinstance(value, kind) or (kind is int and isinstance(value, bool)):
        raise RecipeError("damaged recipe: metadata %r is not %s" % (key, _TYPE_NAMES[kind]))
    # build_pack indexes every entry by name, so a non-object would escape as TypeError.
    if key in ("entries", "generated") and not all(isinstance(item, dict) for item in value):
        raise RecipeError("damaged recipe: metadata %r holds a non-object item" % key)


@dataclass
class Recipe:
    engine_abi: int
    rom_sha1: str
    release: str
    entries: list = field(default_factory=list)   # dicts: path, size, crc, ops
    bitmaps: list = field(default_factory=list)
    generated: list = field(default_factory=list)  # dicts: path, size, crc, generator
    inputs: list = field(default_factory=list)     # entries written into the generators' tree
    vtree: dict = field(default_factory=dict)      # names and ROM offsets for that tree
    literals: bytes = b""

    def to_bytes(self) -> bytes:
        meta = {"schema": SCHEMA, "engine_abi": self.engine_abi, "rom_sha1": self.rom_sha1,
                "release": self.release, "entries": self.entries, "bitmaps": self.bitmaps,
                "generated": self.generated, "inputs": self.inputs, "vtree": self.vtree}
        blob = lzma.compress(json.dumps(meta, separators=(",", ":")).encode("utf-8"))
        return MAGIC + struct.pack("<I", len(blob)) + blob + lzma.compress(self.literals)

    @classmethod
    def from_bytes(cls, data: bytes) -> "Recipe":
        size = _decode_header(data[:HEADER_SIZE])
        meta = _decode_meta(data[HEADER_SIZE:HEADER_SIZE + size], size)
        if meta.get("schema") != SCHEMA:
            raise RecipeError("unsupported recipe schema %r" % meta.get("schema"))
        for key, kind, required in _META_FIELDS:
            _check_field(meta, key, kind, required)
        literals = _decompress(data[HEADER_SIZE + size:], "literal pool")
        return cls(engine_abi=meta["engine_abi"], rom_sha1=meta["rom_sha1"], release=meta["release"],
                   entries=meta["entries"], bitmaps=meta.get("bitmaps", []),
                   generated=meta.get("generated", []), inputs=meta.get("inputs", []),
                   vtree=meta.get("vtree", {}), literals=literals)

    def save(self, path: Path) -> None:
        Path(path).write_bytes(self.to_bytes())

    @classmethod
    def load(cls, path: Path) -> "Recipe":
        return cls.from_bytes(Path(path).read_bytes())


def recipe_rom_sha1(path: Path) -> str:
    """The SHA-1 of the ROM a recipe is made from, reading only its header."""
    with open(path, "rb") as f:
        size = _decode_header(f.read(HEADER_SIZE))
        meta = _decode_meta(f.read(size), size)
    _check_field(meta, "rom_sha1", str, True)
    return meta["rom_sha1"]


def lz77_decompress(src: bytes, offset: int) -> bytes:
    """GBA BIOS LZ77 (type 0x10)."""
    if src[offset] != 0x10:
        raise RecipeError("no LZ77 header at %#x" % offset)
    size = src[offset + 1] | (src[offset + 2] << 8) | (src[offset + 3] << 16)
    out = bytearray()
    pos = offset + 4
    while len(out) < size:
        flags = src[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                a, b = src[pos], src[pos + 1]
                pos += 2
                length = (a >> 4) + 3
                back = ((a & 0xF) << 8 | b) + 1
                if length <= back <= len(out):      # no overlap: one slice
                    start = len(out) - back
                    out += out[start:start + length]
                else:
                    for _ in range(length):
                        out.append(out[-back])
            else:
                out.append(src[pos])
                pos += 1
    return bytes(out[:size])


def apply_bitmap(src: bytes, dst: bytearray, bitmap: list[int]) -> None:
    for i, s in enumerate(bitmap):
        if s == -1:
            bit = 0
        elif s == -2:
            bit = 1
        else:
            bit = (src[s >> 3] >> (s & 7)) & 1
        if bit:
            dst[i >> 3] |= 1 << (i & 7)
        else:
            dst[i >> 3] &= ~(1 << (i & 7))


def build_entry(entry: dict, rom: bytes, literals: bytes, bitmaps: list, sources=None) -> bytes:
    """`sources` (precompiled.Sources) is what "V" programs read."""
    out = bytearray()
    for op in entry["ops"]:
        kind = op[0]
        if kind == "C":
            _, off, length = op
            if off < 0 or off + length > len(rom):
                raise RecipeError("%s: ROM range out of bounds" % entry["path"])
            out += rom[off:off + length]
        elif kind == "F":
            out += bytes([op[1]]) * op[2]
        elif kind == "L":
            _, off, length = op
            out += literals[off:off + length]
        elif kind == "Z":
            out += lz77_decompress(rom, op[1])
        elif kind == "R":
            _, off, src_stride, dst_stride, count, index = op
            bitmap = bitmaps[index]
            for k in range(count):
                record = bytearray(dst_stride)
                apply_bitmap(rom[off + k * src_stride: off + (k + 1) * src_stride], record, bitmap)
                out += record
        elif kind == "V":
            from . import precompiled
            if (len(op) != 3 or not all(isinstance(v, int) and not isinstance(v, bool) and v >= 0
                                        for v in op[1:]) or op[1] + op[2] > len(literals)):
                raise RecipeError("%s: damaged program reference %r" % (entry["path"], op))
            if sources is None:
                raise RecipeError("%s: a program with nothing to read" % entry["path"])
            _, off, length = op
            try:
                out += precompiled.run(literals[off:off + length], sources,
                                       limit=max(0, entry["size"] - len(out)))
            except precompiled.ProgramError as exc:
                raise RecipeError("%s: %s" % (entry["path"], exc)) from exc
        else:
            raise RecipeError("%s: unknown operation %r" % (entry["path"], kind))
    for dst, off, length in entry.get("patches", ()):
        out[dst:dst + length] = literals[off:off + length]
    data = bytes(out)
    if len(data) != entry["size"] or (zlib.crc32(data) & 0xFFFFFFFF) != entry["crc"]:
        raise RecipeError("%s does not match the expected result (size %d/%d, CRC %08x/%08x)"
                          % (entry["path"], len(data), entry["size"], zlib.crc32(data) & 0xFFFFFFFF,
                             entry["crc"]))
    return data
