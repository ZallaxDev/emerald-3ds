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
MAX_METADATA_BYTES = 64 << 20
MAX_LITERAL_BYTES = 128 << 20
MAX_ENTRY_BYTES = 128 << 20
MAX_PACK_BYTES = 512 << 20
MAX_RECIPE_BYTES = HEADER_SIZE + MAX_METADATA_BYTES + MAX_LITERAL_BYTES
_OPERATION_LENGTH = {"C": 3, "F": 3, "L": 3, "Z": 2, "R": 6, "V": 3}

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
    if size > MAX_METADATA_BYTES:
        raise RecipeError("damaged recipe: metadata exceeds its size limit")
    return size


def _decompress(blob: bytes, what: str, limit: int) -> bytes:
    try:
        decoder = lzma.LZMADecompressor(memlimit=MAX_METADATA_BYTES)
        data = decoder.decompress(blob, max_length=limit + 1)
        if len(data) <= limit and (not decoder.eof or decoder.unused_data):
            raise lzma.LZMAError("expected one complete stream")
    except lzma.LZMAError as exc:
        raise RecipeError("damaged recipe: %s cannot be decompressed (%s)" % (what, exc)) from exc

    if len(data) > limit:
        raise RecipeError("damaged recipe: %s exceeds its size limit" % what)
    return data


def _decode_meta(blob: bytes, size: int) -> dict:
    """The metadata object from its compressed blob, which should be `size` bytes."""
    if len(blob) != size:
        raise RecipeError("damaged recipe: metadata truncated (%d of %d bytes)" % (len(blob), size))
    raw = _decompress(blob, "metadata", MAX_METADATA_BYTES)
    try:
        meta = json.loads(raw)
    except (ValueError, RecursionError) as exc:
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


def _uint(value, limit=0xFFFFFFFF) -> bool:
    return type(value) is int and 0 <= value <= limit


def valid_path(value) -> bool:
    return (isinstance(value, str) and "\\" not in value and ":" not in value
            and "\0" not in value and all(part not in ("", ".", "..") for part in value.split("/")))


def _check_entry(entry, *, generated=False) -> None:
    if not isinstance(entry, dict) or not valid_path(entry.get("path")):
        raise RecipeError("damaged recipe: entry has an invalid path")
    path = entry["path"]
    if not _uint(entry.get("size"), MAX_ENTRY_BYTES) or not _uint(entry.get("crc")):
        raise RecipeError("%s: invalid entry size or CRC" % path)
    ops = entry.get("ops", [] if generated else None)
    if not isinstance(ops, list):
        raise RecipeError("%s: invalid or missing operations" % path)
    for op in ops:
        if (not isinstance(op, (tuple, list)) or not op or not isinstance(op[0], str)
                or _OPERATION_LENGTH.get(op[0]) != len(op) or not all(_uint(v) for v in op[1:])):
            raise RecipeError("%s: invalid operation" % path)
    patches = entry.get("patches", [])
    if not isinstance(patches, list) or any(
            not isinstance(patch, (tuple, list)) or len(patch) != 3
            or not all(_uint(v) for v in patch) for patch in patches):
        raise RecipeError("%s: invalid patches" % path)


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
        if len(data) > MAX_RECIPE_BYTES:
            raise RecipeError("recipe exceeds its file size limit")
        size = _decode_header(data[:HEADER_SIZE])
        meta = _decode_meta(data[HEADER_SIZE:HEADER_SIZE + size], size)
        if meta.get("schema") != SCHEMA:
            raise RecipeError("unsupported recipe schema %r" % meta.get("schema"))
        for key, kind, required in _META_FIELDS:
            _check_field(meta, key, kind, required)
        pack_paths = set()
        pack_bytes = 0
        for key in ("entries", "generated", "inputs"):
            entries = meta.get(key, [])
            seen = set() if key == "inputs" else pack_paths
            for entry in entries:
                _check_entry(entry, generated=key == "generated")
                if entry["path"] in seen:
                    raise RecipeError("duplicate %s path %s" % (key, entry["path"]))
                seen.add(entry["path"])
            total = sum(entry["size"] for entry in entries)
            if key != "inputs":
                pack_bytes += total
                total = pack_bytes
            if total > MAX_PACK_BYTES:
                raise RecipeError("recipe %s exceeds its total output limit" % key)
        literals = _decompress(data[HEADER_SIZE + size:], "literal pool", MAX_LITERAL_BYTES)
        return cls(engine_abi=meta["engine_abi"], rom_sha1=meta["rom_sha1"], release=meta["release"],
                   entries=meta["entries"], bitmaps=meta.get("bitmaps", []),
                   generated=meta.get("generated", []), inputs=meta.get("inputs", []),
                   vtree=meta.get("vtree", {}), literals=literals)

    def save(self, path: Path) -> None:
        Path(path).write_bytes(self.to_bytes())

    @classmethod
    def load(cls, path: Path) -> "Recipe":
        with Path(path).open("rb") as stream:
            return cls.from_bytes(stream.read(MAX_RECIPE_BYTES + 1))


def recipe_rom_sha1(path: Path) -> str:
    """The SHA-1 of the ROM a recipe is made from, reading only its header."""
    with open(path, "rb") as f:
        size = _decode_header(f.read(HEADER_SIZE))
        meta = _decode_meta(f.read(size), size)
    _check_field(meta, "rom_sha1", str, True)
    return meta["rom_sha1"]


def lz77_decompress(src: bytes, offset: int, limit: int = MAX_ENTRY_BYTES) -> bytes:
    """GBA BIOS LZ77 (type 0x10)."""
    if not 0 <= offset <= len(src) - 4 or src[offset] != 0x10:
        raise RecipeError("no LZ77 header at %#x" % offset)
    size = src[offset + 1] | (src[offset + 2] << 8) | (src[offset + 3] << 16)
    if size > limit:
        raise RecipeError("LZ77 output exceeds the entry size")
    out = bytearray()
    pos = offset + 4
    while len(out) < size:
        if pos >= len(src):
            raise RecipeError("truncated LZ77 flags")
        flags = src[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                if pos + 2 > len(src):
                    raise RecipeError("truncated LZ77 back reference")
                a, b = src[pos], src[pos + 1]
                pos += 2
                length = min((a >> 4) + 3, size - len(out))
                back = ((a & 0xF) << 8 | b) + 1
                if back > len(out):
                    raise RecipeError("invalid LZ77 back reference")
                if length <= back:      # no overlap: one slice
                    start = len(out) - back
                    out += out[start:start + length]
                else:
                    for _ in range(length):
                        out.append(out[-back])
            else:
                if pos >= len(src):
                    raise RecipeError("truncated LZ77 literal")
                out.append(src[pos])
                pos += 1
    return bytes(out)


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
    _check_entry(entry)
    out = bytearray()
    path = entry["path"]
    for op in entry["ops"]:
        kind = op[0]
        remaining = entry["size"] - len(out)
        if kind in ("C", "L", "V"):
            _, off, length = op
            data = rom if kind == "C" else literals
            if off + length > len(data) or (kind != "V" and length > remaining):
                raise RecipeError("%s: %s range out of bounds" % (path, "ROM" if kind == "C" else "literal"))
            if kind == "V":
                from . import precompiled

                if sources is None:
                    raise RecipeError("%s: a program with nothing to read" % path)
                try:
                    out += precompiled.run(data[off:off + length], sources, limit=remaining)
                except precompiled.ProgramError as exc:
                    raise RecipeError("%s: %s" % (path, exc)) from exc
            else:
                out += data[off:off + length]
        elif kind == "F":
            _, byte, count = op
            if byte > 255 or count > remaining:
                raise RecipeError("%s: invalid fill" % path)
            out += bytes([byte]) * count
        elif kind == "Z":
            out += lz77_decompress(rom, op[1], remaining)
        elif kind == "R":
            _, off, src_stride, dst_stride, count, index = op
            if (index >= len(bitmaps) or src_stride == 0 or dst_stride == 0
                    or off + src_stride * count > len(rom) or dst_stride * count > remaining):
                raise RecipeError("%s: invalid remap range" % path)
            bitmap = bitmaps[index]
            if (not isinstance(bitmap, (tuple, list)) or len(bitmap) > dst_stride * 8
                    or any(type(bit) is not int or not -2 <= bit < src_stride * 8 for bit in bitmap)):
                raise RecipeError("%s: invalid remap bitmap" % path)
            for k in range(count):
                record = bytearray(dst_stride)
                start = off + k * src_stride
                apply_bitmap(rom[start:start + src_stride], record, bitmap)
                out += record
    for dst, off, length in entry.get("patches", ()):
        if dst + length > len(out) or off + length > len(literals):
            raise RecipeError("%s: invalid patch range" % path)
        out[dst:dst + length] = literals[off:off + length]
    data = bytes(out)
    if len(data) != entry["size"] or (zlib.crc32(data) & 0xFFFFFFFF) != entry["crc"]:
        raise RecipeError("%s does not match the expected result (size %d/%d, CRC %08x/%08x)"
                          % (entry["path"], len(data), entry["size"], zlib.crc32(data) & 0xFFFFFFFF,
                             entry["crc"]))
    return data
