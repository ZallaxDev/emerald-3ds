"""emerald3ds.pak: the single data pack the console reads game data from.

Layout (all integers little endian):

    header, 64 bytes
        0   magic         8   b"EM3DPAK\\0"
        8   schema        u32 1
        12  engine_abi    u32 must equal the executable's romfs:/engine/abi.bin
        16  rom_sha1      20  SHA-1 of the ROM the pack was built from
        36  entry_count   u32
        40  index_offset  u64
        48  data_offset   u64
        56  index_crc32   u32 CRC-32 of the whole index
        60  header_crc32  u32 CRC-32 of bytes 0..59

    index, entry_count * 40 bytes, sorted by id
        0   id            u64 FNV-1a 64 of the path ("maps/layouts.bin")
        8   type          u32 informational category, see TYPES
        12  flags         u32 bit 0: compressed (not used by schema 1)
        16  offset        u64 absolute offset of the payload
        24  stored_size   u32
        28  raw_size      u32
        32  crc32         u32 CRC-32 of the payload
        36  reserved      u32 0

    payloads, each aligned to 32 bytes, in path order (bytewise, as
    "graphics/a/x" < "graphics/a/y" < "graphics/b"): the files under any one
    prefix of paths - a directory, a tileset, a source file's generated
    tables - lie together, so the console's asset warm-up, which reads them by
    prefix, reads each group in a few long runs (3ds_port/src/3ds_assets.c).
    Readers find a payload through its entry's offset only; the order is no
    part of the schema.

The console-side reader is 3ds_port/src/3ds_data.c.
"""

from __future__ import annotations

import hashlib
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterable

MAGIC = b"EM3DPAK\0"
SCHEMA = 1
HEADER = struct.Struct("<8sII20sIQQII")
ENTRY = struct.Struct("<QIIQIIII")
ALIGN = 32
TYPES = {"graphics/": 1, "data/": 1, "generated/": 1, "maps/": 2, "scripts/": 3,
         "sound/": 4, "gamedata/": 5, "voxel/": 6}

assert HEADER.size == 64 and ENTRY.size == 40


class PakError(Exception):
    pass


def path_id(path: str) -> int:
    value = 0xcbf29ce484222325
    for byte in path.encode("utf-8"):
        value ^= byte
        value = (value * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return value


def entry_type(path: str) -> int:
    for prefix, code in TYPES.items():
        if path.startswith(prefix):
            return code
    return 0


def engine_abi(items: Iterable[tuple[str, int, int]]) -> int:
    """The ABI both sides agree on: a digest of every data file's path, size
    and CRC-32. Any change to any payload the executable expects changes it."""
    text = "".join("%s\n%d\n%08x\n" % (p, s, c) for p, s, c in sorted(items))
    value = zlib.crc32(text.encode("utf-8")) & 0xFFFFFFFF
    return value or 1


def normalise(path: str) -> str:
    path = path.replace("\\", "/")
    if path.startswith("/") or ".." in path.split("/") or ":" in path or not path:
        raise PakError("invalid pack path: %r" % path)
    return path


@dataclass
class Entry:
    path: str | None
    id: int
    type: int
    flags: int
    offset: int
    stored_size: int
    raw_size: int
    crc32: int


def write_pak(out: Path, files: Iterable[tuple[str, bytes]], abi: int, rom_sha1: bytes) -> dict:
    """Write a pack. `files` yields (path, payload). Returns a summary."""
    if len(rom_sha1) != 20:
        raise PakError("rom_sha1 must be 20 bytes")
    items = []
    seen: dict[int, str] = {}
    for path, data in files:
        path = normalise(path)
        pid = path_id(path)
        if pid in seen:
            raise PakError("id collision between %s and %s" % (seen[pid], path))
        seen[pid] = path
        items.append((pid, path, data))
    # The index by id (the console's binary search), the payloads by path.
    items.sort()
    by_path = sorted(items, key=lambda item: item[1].encode("utf-8"))
    count = len(items)
    index_offset = HEADER.size
    data_offset = (index_offset + count * ENTRY.size + ALIGN - 1) // ALIGN * ALIGN
    offsets: dict[int, int] = {}
    cursor = data_offset
    for pid, _, data in by_path:
        offsets[pid] = cursor
        cursor = (cursor + len(data) + ALIGN - 1) // ALIGN * ALIGN
    entries = [ENTRY.pack(pid, entry_type(path), 0, offsets[pid], len(data), len(data),
                          zlib.crc32(data) & 0xFFFFFFFF, 0) for pid, path, data in items]
    index = b"".join(entries)
    head = HEADER.pack(MAGIC, SCHEMA, abi, rom_sha1, count, index_offset, data_offset,
                       zlib.crc32(index) & 0xFFFFFFFF, 0)
    head = head[:60] + struct.pack("<I", zlib.crc32(head[:60]) & 0xFFFFFFFF)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".tmp")
    with tmp.open("wb") as f:
        f.write(head)
        f.write(index)
        f.write(b"\0" * (data_offset - f.tell()))
        for pid, path, data in by_path:
            if f.tell() != offsets[pid]:
                raise PakError("payload of %s written at %d, indexed at %d" % (path, f.tell(), offsets[pid]))
            f.write(data)
            pad = (-f.tell()) % ALIGN
            f.write(b"\0" * pad)
    tmp.replace(out)
    return {"entries": count, "bytes": out.stat().st_size, "abi": abi}


class PakReader:
    def __init__(self, path: Path):
        self.path = Path(path)
        self._file: BinaryIO = self.path.open("rb")
        try:
            self._load()
        except Exception:
            self._file.close()
            raise

    def _load(self) -> None:
        head = self._file.read(HEADER.size)
        if len(head) != HEADER.size or head[:8] != MAGIC:
            raise PakError("not a Emerald 3DS data pack")
        if zlib.crc32(head[:60]) & 0xFFFFFFFF != struct.unpack_from("<I", head, 60)[0]:
            raise PakError("header CRC mismatch")
        (_, self.schema, self.abi, self.rom_sha1, count, index_offset, self.data_offset,
         index_crc, _) = HEADER.unpack(head)
        if self.schema != SCHEMA:
            raise PakError("unsupported schema %d" % self.schema)
        self._file.seek(index_offset)
        index = self._file.read(count * ENTRY.size)
        if len(index) != count * ENTRY.size or zlib.crc32(index) & 0xFFFFFFFF != index_crc:
            raise PakError("index CRC mismatch")
        self.entries: dict[int, Entry] = {}
        last = -1
        for i in range(count):
            pid, typ, flags, off, stored, raw, crc, _ = ENTRY.unpack_from(index, i * ENTRY.size)
            if pid <= last:
                raise PakError("index not sorted")
            last = pid
            self.entries[pid] = Entry(None, pid, typ, flags, off, stored, raw, crc)

    def close(self) -> None:
        self._file.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def read(self, path: str) -> bytes:
        entry = self.entries.get(path_id(normalise(path)))
        if entry is None:
            raise KeyError(path)
        self._file.seek(entry.offset)
        data = self._file.read(entry.raw_size)
        if zlib.crc32(data) & 0xFFFFFFFF != entry.crc32:
            raise PakError("CRC mismatch in %s" % path)
        return data

    def verify(self) -> int:
        for entry in self.entries.values():
            self._file.seek(entry.offset)
            data = self._file.read(entry.raw_size)
            if len(data) != entry.raw_size or zlib.crc32(data) & 0xFFFFFFFF != entry.crc32:
                raise PakError("payload CRC mismatch at offset %d" % entry.offset)
        return len(self.entries)


def sha1_of(path: Path) -> bytes:
    digest = hashlib.sha1()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.digest()
