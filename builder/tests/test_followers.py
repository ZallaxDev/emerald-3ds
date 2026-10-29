"""Follower sprites (emerald3ds_builder/followers.py) on synthetic files only:
no HGSS data and no network."""

import hashlib
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "builder"))
from emerald3ds_builder import followers, pak  # noqa: E402

spec = importlib.util.spec_from_file_location("staging", ROOT / "tools/port_common/staging.py")
staging = importlib.util.module_from_spec(spec)
spec.loader.exec_module(staging)


def nitro_dictionary(names, entries):
    """A Nitro info dictionary as followers._dictionary reads it."""
    count = len(names)
    size = len(entries[0])
    out = struct.pack("<BBH", 0, count, 0) + struct.pack("<HHI", 8, 0, 0) + b"\0" * (4 * count)
    out += struct.pack("<HH", size, 0) + b"".join(entries)
    out += b"".join(n.encode("latin-1").ljust(16, b"\0") for n in names)
    return out


def make_nsbtx(w=32, h=32, frames=8, fmt=3):
    """A BTX0 with `frames` 4bpp textures whose pixel (x, y) of frame f is
    (x + y + f) % 16, and two palettes: 0x0000.. and 0x8000|i (bit 15 set)."""
    tex_bytes = b""
    params = []
    for f in range(frames):
        offset = len(tex_bytes)
        data = bytearray()
        for y in range(h):
            for x in range(0, w, 2):
                data.append(((x + y + f) % 16) | (((x + 1 + y + f) % 16) << 4))
        tex_bytes += bytes(data)
        wl, hl = (w // 8).bit_length() - 1, (h // 8).bit_length() - 1
        params.append(struct.pack("<II", (offset >> 3) | (wl << 20) | (hl << 23) | (fmt << 26) | (1 << 29), 0))
    pal_bytes = struct.pack("<16H", *range(16)) + struct.pack("<16H", *(0x8000 | (i * 3) for i in range(16)))
    tex_dict = nitro_dictionary(["t%d" % i for i in range(frames)], params)
    pal_dict = nitro_dictionary(["p0", "p1"], [struct.pack("<HH", 0, 0), struct.pack("<HH", 4, 0)])
    head_size = 0x3C
    tex_info = head_size
    pal_info = tex_info + len(tex_dict)
    tex_data = pal_info + len(pal_dict)
    pal_data = tex_data + len(tex_bytes)
    tex0 = bytearray(head_size)
    tex0[0:4] = b"TEX0"
    struct.pack_into("<H", tex0, 0x0E, tex_info)
    struct.pack_into("<I", tex0, 0x14, tex_data)
    struct.pack_into("<I", tex0, 0x30, len(pal_bytes) >> 3)
    struct.pack_into("<I", tex0, 0x34, pal_info)
    struct.pack_into("<I", tex0, 0x38, pal_data)
    tex0 += tex_dict + pal_dict + tex_bytes + pal_bytes
    head = bytearray(0x14)
    head[0:4] = b"BTX0"
    struct.pack_into("<HI", head, 0x0E, 1, 0x14)
    return bytes(head + tex0)


def sources_for(files):
    """A sources dict listing `files` {path: (bytes, key or None)}."""
    entries = []
    for i, (path, (data, key)) in enumerate(sorted(files.items())):
        e = {"source": "hg", "path": path, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        if key:
            e["key"], e["follower_index"] = key, i
        entries.append(e)
    return {"sources": {"hg": {"repo": "owner/repo", "commit": "c" * 40}}, "files": entries}


class ConvertTests(unittest.TestCase):
    def test_layout(self):
        out = followers.convert(make_nsbtx(), flags=5)
        self.assertEqual(out[:8], b"FMN1" + bytes((32, 32, 8, 5)))
        self.assertEqual(struct.unpack_from("<16H", out, 8), tuple(range(16)))
        # bit 15 dropped from the shiny palette
        self.assertEqual(struct.unpack_from("<16H", out, 40), tuple(i * 3 for i in range(16)))
        self.assertEqual(len(out), 72 + 8 * 32 * 32 // 2)
        frame = 512
        # frame 2, tile (1, 1) of a 4x4-tile frame, row 3: pixel x = 8..15, y = 11
        tile = 72 + 2 * frame + (1 * 4 + 1) * 32
        row = out[tile + 3 * 4:tile + 3 * 4 + 4]
        pixels = [(row[i // 2] >> ((i & 1) * 4)) & 15 for i in range(8)]
        self.assertEqual(pixels, [(x + 11 + 2) % 16 for x in range(8, 16)])

    def test_big_sprite(self):
        out = followers.convert(make_nsbtx(64, 64))
        self.assertEqual(out[4:6], bytes((64, 64)))
        self.assertEqual(len(out), 72 + 8 * 64 * 64 // 2)

    def test_rejects_odd_files(self):
        with self.assertRaises(followers.FollowersError):
            followers.convert(make_nsbtx(frames=6))
        with self.assertRaises(followers.FollowersError):
            followers.convert(make_nsbtx(fmt=4))
        with self.assertRaises(followers.FollowersError):
            followers.convert(b"NOPE" + b"\0" * 64)

    def test_param_flags(self):
        csv = "index,unk0,too_tall,unk2_0,hover,unk3\nA,0,false,false,false,0\nB,0,true,true,false,0\nC,0,false,false,true,0\n"
        self.assertEqual(followers.param_flags(csv), [0, 3, 4])


def corpus(messages, bank=followers.TEXT_BANK):
    """A poke-corpus *_msg.txt holding `messages` as bank `bank`, between two
    other banks."""
    return ("Text File : 1\n~~~~\nfirst\n~~~~~~~~\nText File : %d\n~~~~~~~~\n%s\n~~~~~~~~\n"
            "Text File : %d\n~~~~\nlast\n" % (bank, "\n".join(messages), bank + 1))


class TextTests(unittest.TestCase):
    def test_characters(self):
        self.assertEqual(followers.encode_message("Az 09!"),
                         bytes((0xBB, 0xD5 + 25, 0x00, 0xA1, 0xA1 + 9, 0xAB, 0xFF)))
        self.assertEqual(followers.encode_message("¿Qué?"), bytes((0x51, 0xBB + 16, 0xD5 + 20, 0x1B, 0xAC, 0xFF)))

    def test_variables_and_breaks(self):
        text = "Huh?\\n[VAR 0101(0000,0000)] has\\r[VAR 0108(0001,0000)]!\\c Take it?[VAR 0200(0000)]"
        out = followers.encode_message(text)
        self.assertIn(b"\xAC\x00\xFD\x02\x00", out)       # a line break is a space: the game wraps
        self.assertIn(b"\x00\xFD\x03\xAB\xFB\xCE", out)  # a page break eats the spaces round it
        self.assertTrue(out.endswith(b"\xAC\xFF"))       # the yes/no marker goes
        with self.assertRaises(followers.FollowersError):
            followers.encode_message("[VAR 0999(0000)]")

    def test_bank(self):
        messages = ["m%d" % i for i in range(followers.TEXT_COUNT)]
        self.assertEqual(followers.bank_messages(corpus(messages)), messages)
        out = followers.convert_texts(corpus(messages))
        self.assertEqual(out[:4], followers.TEXT_MAGIC)
        count, = struct.unpack_from("<H", out, 4)
        self.assertEqual(count, followers.TEXT_COUNT)
        first, second = struct.unpack_from("<2I", out, 8)
        self.assertEqual(out[first:second], followers.encode_message("m0"))
        with self.assertRaises(followers.FollowersError):
            followers.convert_texts(corpus(messages[:-1]))
        with self.assertRaises(followers.FollowersError):
            followers.convert_texts(corpus(messages, bank=1))


class FetchTests(unittest.TestCase):
    def setUp(self):
        self.good = make_nsbtx()
        self.files = {"mon/a.NSBTX": (self.good, "001"), "mon/b.NSBTX": (make_nsbtx(64, 64), "002"),
                      "tp_param.csv": (b"index,a,b,c,d,e\nX,0,true,false,false,0\nY,0,false,false,true,0\n", None)}
        self.sources = sources_for(self.files)

    def network(self, served):
        calls = []

        def fetch(url):
            calls.append(url)
            for path, data in served.items():
                if url.endswith("/" + path):
                    return data
            raise OSError("404")
        return fetch, calls

    def test_hash_mismatch_falls_to_the_next_host_and_is_refused(self):
        fetch, calls = self.network({"mon/a.NSBTX": self.good[:-1] + b"X"})
        fetcher = followers.Fetcher(self.sources, fetch=fetch, workers=1)
        entry = next(e for e in self.sources["files"] if e.get("key") == "001")
        self.assertIsNone(fetcher.get(entry))
        self.assertEqual(len(calls), 2)
        self.assertIn("raw.githubusercontent.com/owner/repo/" + "c" * 40 + "/mon/a.NSBTX", calls[0])
        self.assertIn("cdn.jsdelivr.net/gh/owner/repo@" + "c" * 40, calls[1])

    def test_offline_and_missing_files_build_what_they_can(self):
        served = {p: d for p, (d, _) in self.files.items() if p != "mon/b.NSBTX"}
        fetch, _ = self.network(served)
        with tempfile.TemporaryDirectory() as tmp:
            fetcher = followers.Fetcher(self.sources, cache=Path(tmp), fetch=fetch, workers=2)
            files, info = followers.build_section(fetcher)
            self.assertEqual(sorted(files), ["followers/mon/001.bin", "followers/sources.txt"])
            self.assertEqual(info["missing"], ["002"])
            self.assertEqual(files["followers/mon/001.bin"][7], 1)  # first row's flags: too tall
            # offline, the cache serves what was fetched; nothing else is asked
            offline = followers.Fetcher(self.sources, cache=Path(tmp), network=False, workers=1)
            files2, info2 = followers.build_section(offline)
            self.assertEqual(files2, files)
            self.assertEqual(info2["downloaded"], 0)

    def test_nothing_at_all(self):
        fetcher = followers.Fetcher(self.sources, network=False, workers=1)
        files, info = followers.build_section(fetcher)
        self.assertEqual(files, {})
        self.assertEqual(sorted(info["missing"]), ["001", "002"])

    def test_local_copy_first(self):
        with tempfile.TemporaryDirectory() as tmp:
            for path, (data, _) in self.files.items():
                dst = Path(tmp) / "hg" / path
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes(data)
            fetcher = followers.Fetcher(self.sources, [Path(tmp)], network=False, workers=1)
            files, info = followers.build_section(fetcher)
            self.assertEqual(info["missing"], [])
            self.assertEqual(len(files), 3)

    def test_texts_of_the_languages_asked(self):
        good = corpus(["hi"] * followers.TEXT_COUNT).encode("utf-8")
        files = dict(self.files)
        files["corpus/en_msg.txt"] = (good, None)
        files["corpus/es_msg.txt"] = (good, None)
        files["corpus/fr_msg.txt"] = (corpus(["hi"]).encode("utf-8"), None)   # a bank too short
        sources = sources_for(files)
        for e in sources["files"]:
            if e["path"].startswith("corpus/"):
                e["language"] = e["path"][7:9]
        fetch, calls = self.network({p: d for p, (d, _) in files.items()})
        fetcher = followers.Fetcher(sources, fetch=fetch, workers=1)
        out, info = followers.build_section(fetcher, languages=["es"])
        self.assertIn("followers/text/es.bin", out)
        self.assertNotIn("followers/text/en.bin", out)
        self.assertEqual((info["sprites"], info["texts"]), (2, 1))
        out, info = followers.build_section(fetcher, languages=["en", "fr"])
        self.assertIn("followers/text/en.bin", out)
        self.assertEqual(info["missing"], ["text_fr"])


class PinnedListTests(unittest.TestCase):
    def test_pinned_list(self):
        sources = followers.load_sources()
        sprites = followers.sprite_files(sources)
        self.assertEqual(len(sprites), 419)
        keys = {e["key"] for e in sprites}
        self.assertEqual(len(keys), 419)
        self.assertTrue({"%03d" % d for d in range(1, 387)} <= keys)
        self.assertEqual(len([k for k in keys if k.endswith("_f")]), 6)
        self.assertEqual(len([k for k in keys if k.startswith("201_")]), 27)
        self.assertEqual(sorted(e["language"] for e in followers.text_files(sources)), ["de", "en", "es", "fr", "it"])
        for e in sources["files"]:
            self.assertEqual(len(e["sha256"]), 64)
            self.assertIn(e["source"], sources["sources"])

    def test_workspace_copy_matches(self):
        """The private workspace's copy, where there is one, is what is pinned."""
        root = ROOT / "private_assets" / "hgss_followers"
        if not root.is_dir():
            self.skipTest("no private copy")
        for e in followers.load_sources()["files"]:
            self.assertTrue(followers.check(e, (root / e["source"] / e["path"]).read_bytes()), e["path"])


class StagingTests(unittest.TestCase):
    def test_optional_files_skip_the_abi_and_the_release(self):
        with tempfile.TemporaryDirectory() as tmp:
            romfs = Path(tmp) / "romfs"
            (romfs / "maps").mkdir(parents=True)
            (romfs / "maps" / "a.bin").write_bytes(b"map")
            abi_before, _ = staging.compute_abi(romfs)
            (romfs / "followers" / "mon").mkdir(parents=True)
            (romfs / "followers" / "mon" / "025.bin").write_bytes(b"FMN1....")
            abi_after, items = staging.compute_abi(romfs)
            self.assertEqual(abi_before, abi_after)
            self.assertEqual([i[0] for i in items], ["maps/a.bin"])
            out = Path(tmp) / "x.pak"
            staging.main(["pak", "--romfs", str(romfs), "--out", str(out)])
            with pak.PakReader(out) as reader:
                self.assertEqual(reader.read("followers/mon/025.bin"), b"FMN1....")
                self.assertEqual(reader.abi, abi_before)
            engine = Path(tmp) / "engine"
            staging.main(["engine", "--romfs", str(romfs), "--out", str(engine)])
            self.assertFalse((engine / "followers").exists())


if __name__ == "__main__":
    unittest.main()
