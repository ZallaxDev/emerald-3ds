"""The data pack's layout: the index sorted by id (the console's binary search,
3ds_port/src/3ds_pak.c) and the payloads in path order, so that the files under
one prefix of paths lie together and the console's asset warm-up reads them in
long runs (3ds_port/src/3ds_assets.c). Synthetic data only.
Run: python -m unittest discover builder/tests
"""

import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from emerald3ds_builder import pak  # noqa: E402

# Out of order on purpose, with directories whose names sort around each
# other ("a.bin" < "a-x/" < "a/" bytewise), empty and unaligned payloads.
FILES = [
    ("graphics/b/one.4bpp", b"\x01" * 100),
    ("maps/layouts.bin", b"\x02" * 33),
    ("graphics/a/two.gbapal", b"\x03" * 32),
    ("data/tilesets/primary/general/tiles.4bpp", b"\x04" * 4097),
    ("graphics/a.bin", b""),
    ("graphics/a/one.4bpp", b"\x05" * 7),
    ("graphics/a-x/one.bin", b"\x06" * 64),
    ("data/tilesets/primary/general/palettes/00.gbapal", b"\x07" * 32),
    ("graphics/a/sub/deep.bin", b"\x08" * 3),
    ("voxel/relief.bin", b"\x09" * 1000),
    ("data/tilesets/secondary/petalburg/tiles.4bpp", b"\x0a" * 300),
]


def raw_entries(out: Path):
    raw = out.read_bytes()
    head = pak.HEADER.unpack_from(raw, 0)
    count, index_offset, data_offset = head[4], head[5], head[6]
    entries = [pak.ENTRY.unpack_from(raw, index_offset + i * pak.ENTRY.size) for i in range(count)]
    return raw, data_offset, entries


class PakLayoutTests(unittest.TestCase):
    def write(self, tmp: str, files=FILES) -> Path:
        out = Path(tmp) / "x.pak"
        abi = pak.engine_abi([(p, len(d), zlib.crc32(d) & 0xFFFFFFFF) for p, d in files])
        pak.write_pak(out, files, abi, b"\x22" * 20)
        return out

    def test_index_in_id_order_data_in_path_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = self.write(tmp)
            raw, data_offset, entries = raw_entries(out)
            ids = [e[0] for e in entries]
            self.assertEqual(ids, sorted(ids))
            self.assertEqual(len(set(ids)), len(ids))
            by_id = {e[0]: e for e in entries}
            paths = sorted((p for p, _ in FILES), key=lambda p: p.encode("utf-8"))
            offsets = [by_id[pak.path_id(p)][3] for p in paths]
            # Ascending (an empty payload shares its offset with the next).
            self.assertEqual(offsets, sorted(offsets))
            self.assertEqual(offsets[0], data_offset)
            # Each payload starts at the next aligned byte after the one before.
            for path, nxt in zip(paths, paths[1:]):
                entry = by_id[pak.path_id(path)]
                end = entry[3] + entry[5]
                self.assertEqual(by_id[pak.path_id(nxt)][3], (end + pak.ALIGN - 1) // pak.ALIGN * pak.ALIGN)
            # The payloads are where the index says, whole and with their CRCs.
            data = dict(FILES)
            for path in paths:
                _, _, _, offset, stored, size, crc, _ = by_id[pak.path_id(path)]
                self.assertEqual(stored, size)
                self.assertEqual(raw[offset:offset + size], data[path])
                self.assertEqual(zlib.crc32(data[path]) & 0xFFFFFFFF, crc)
                self.assertEqual(offset % pak.ALIGN, 0)

    def test_every_prefix_is_one_stretch(self):
        # What the console reads in runs: every directory's files (and their
        # subdirectories') with no other payload between them.
        with tempfile.TemporaryDirectory() as tmp:
            out = self.write(tmp)
            _, _, entries = raw_entries(out)
            offset_of = {e[0]: e[3] for e in entries}
            order = sorted(FILES, key=lambda f: offset_of[pak.path_id(f[0])])
            prefixes = {p[:i + 1] for p, _ in FILES for i, c in enumerate(p) if c == "/"}
            for prefix in prefixes:
                inside = [i for i, (p, _) in enumerate(order) if p.startswith(prefix)]
                self.assertEqual(inside, list(range(inside[0], inside[-1] + 1)), prefix)

    def test_readers_accept_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = self.write(tmp)
            with pak.PakReader(out) as reader:
                self.assertEqual(reader.verify(), len(FILES))
                for path, data in FILES:
                    self.assertEqual(reader.read(path), data)

    def test_order_does_not_depend_on_input_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = self.write(tmp).read_bytes()
        with tempfile.TemporaryDirectory() as tmp:
            second = self.write(tmp, list(reversed(FILES))).read_bytes()
        self.assertEqual(first, second)

    def test_header_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            raw = self.write(tmp).read_bytes()
            self.assertEqual(raw[:8], pak.MAGIC)
            self.assertEqual(struct.unpack_from("<I", raw, 8)[0], pak.SCHEMA)
            self.assertEqual(pak.SCHEMA, 1)


if __name__ == "__main__":
    unittest.main()
