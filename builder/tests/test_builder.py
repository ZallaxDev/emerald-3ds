"""Host tests for the builder. They use synthetic data only: no ROM is needed
and none may ever be committed. Run: python -m unittest discover builder/tests
"""

import hashlib
import json
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from emerald3ds_builder import pak, recipe as rcp, rom as romlib, vtree  # noqa: E402
from emerald3ds_builder.build import Payload, build_pack
from emerald3ds_builder.errors import BuilderError  # noqa: E402
from emerald3ds_builder.install import install  # noqa: E402


def lz77_literal(data: bytes) -> bytes:
    """A valid GBA LZ77 stream that stores everything as literals."""
    out = bytearray([0x10, len(data) & 0xFF, (len(data) >> 8) & 0xFF, (len(data) >> 16) & 0xFF])
    for i in range(0, len(data), 8):
        out.append(0)
        out += data[i:i + 8]
    while len(out) % 4:
        out.append(0)
    return bytes(out)


class PakTests(unittest.TestCase):
    def test_roundtrip_and_verify(self):
        files = [("maps/layouts.bin", b"\x01\x02" * 100), ("graphics/a.4bpp", bytes(range(256))),
                 ("gamedata/gamedata.bin", b"")]
        abi = pak.engine_abi([(p, len(d), zlib.crc32(d) & 0xFFFFFFFF) for p, d in files])
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "x.pak"
            info = pak.write_pak(out, files, abi, b"\x11" * 20)
            self.assertEqual(info["entries"], 3)
            with pak.PakReader(out) as reader:
                self.assertEqual(reader.abi, abi)
                self.assertEqual(reader.verify(), 3)
                self.assertEqual(reader.read("graphics/a.4bpp"), bytes(range(256)))
                offsets = [e.offset for e in reader.entries.values()]
                self.assertTrue(all(o % pak.ALIGN == 0 for o in offsets))

    def test_corruption_is_detected(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "x.pak"
            pak.write_pak(out, [("a/b", b"hello world" * 10)], 7, b"\0" * 20)
            raw = bytearray(out.read_bytes())
            raw[-20] ^= 0xFF
            out.write_bytes(raw)
            with pak.PakReader(out) as reader:
                with self.assertRaises(pak.PakError):
                    reader.verify()
            raw[20] ^= 0xFF   # header
            out.write_bytes(raw)
            with self.assertRaises(pak.PakError):
                pak.PakReader(out)

    def test_rejects_bad_paths(self):
        for bad in ("/abs", "../up", "c:/x", ""):
            with self.assertRaises(pak.PakError):
                pak.normalise(bad)

    def test_path_id_is_fnv1a64(self):
        # Reference value: FNV-1a 64 of "a".
        self.assertEqual(pak.path_id("a"), 0xaf63dc4c8601ec8c)


class RecipeTests(unittest.TestCase):
    def test_all_operations(self):
        rom = bytearray(range(256)) * 16
        packed = lz77_literal(b"ABCDEFGHIJ")
        rom[1024:1024 + len(packed)] = packed
        # records of 4 bytes {a, b, pad, pad} become 2 bytes {b, a}
        records = bytes([1, 2, 0, 0, 3, 4, 0, 0])
        rom[2048:2056] = records
        bitmap = [8 + i for i in range(8)] + [i for i in range(8)]
        literals = b"LITPTR"
        entry = {"path": "x", "ops": [["C", 16, 4], ["F", 0xAA, 3], ["L", 0, 3], ["Z", 1024],
                                      ["R", 2048, 4, 2, 2, 0]],
                 "patches": [[4, 3, 3]]}
        expected = bytes(rom[16:20]) + b"PTR" + b"LIT" + b"ABCDEFGHIJ" + bytes([2, 1, 4, 3])
        entry["size"] = len(expected)
        entry["crc"] = zlib.crc32(expected) & 0xFFFFFFFF
        self.assertEqual(rcp.build_entry(entry, bytes(rom), literals, [bitmap]), expected)

    def test_mismatch_raises(self):
        entry = {"path": "x", "ops": [["F", 1, 4]], "size": 4, "crc": 0}
        with self.assertRaises(rcp.RecipeError):
            rcp.build_entry(entry, b"", b"", [])

    def test_container_roundtrip(self):
        r = rcp.Recipe(engine_abi=5, rom_sha1="00" * 20, release="t",
                       entries=[{"path": "a", "size": 1, "crc": 2, "ops": [["F", 0, 1]]}],
                       literals=b"xyz")
        back = rcp.Recipe.from_bytes(r.to_bytes())
        self.assertEqual((back.engine_abi, back.entries, back.literals), (5, r.entries, b"xyz"))


class RomTests(unittest.TestCase):
    def _write(self, tmp, data, name="game.gba"):
        path = Path(tmp) / name
        path.write_bytes(data)
        return path

    def test_wrong_rom_is_rejected_with_its_name(self):
        data = bytearray(b"\xff" * romlib.ROM_SIZE)
        data[0xA0:0xB0] = b"POKEMON FIREBPRE"
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(BuilderError) as ctx:
                romlib.load_rom(self._write(tmp, bytes(data)))
            self.assertIn("FireRed", ctx.exception.message)

    def test_modified_emerald_is_rejected(self):
        data = bytearray(b"\xff" * romlib.ROM_SIZE)
        data[0xA0:0xB0] = b"POKEMON EMERBPEE"
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(BuilderError) as ctx:
                romlib.load_rom(self._write(tmp, bytes(data)))
            self.assertIn("not an unmodified", ctx.exception.message)

    def test_modified_spanish_emerald_is_rejected(self):
        data = bytearray(b"\xff" * romlib.ROM_SIZE)
        data[0xA0:0xB0] = b"POKEMON EMERBPES"
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(BuilderError) as ctx:
                romlib.load_rom(self._write(tmp, bytes(data)))
            self.assertIn(romlib.SPANISH_SHA1, str(ctx.exception))

    def test_modified_french_emerald_is_rejected(self):
        data = bytearray(b"\xff" * romlib.ROM_SIZE)
        data[0xA0:0xB0] = b"POKEMON EMERBPEF"
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(BuilderError) as ctx:
                romlib.load_rom(self._write(tmp, bytes(data)))
            self.assertIn(romlib.FRENCH_SHA1, str(ctx.exception))

    def test_modified_japanese_emerald_is_rejected(self):
        data = bytearray(b"\xff" * romlib.ROM_SIZE)
        data[0xA0:0xB0] = b"POKEMON EMERBPEJ"
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(BuilderError) as ctx:
                romlib.load_rom(self._write(tmp, bytes(data)))
            self.assertIn(romlib.JAPANESE_SHA1, str(ctx.exception))

    def test_each_region_requires_its_exact_hash_even_when_trimmed_or_zipped(self):
        # Synthetic fixtures substitute the known clean hash, not the validation.
        for code in ("BPEE", "BPES", "BPEF", "BPEJ"):
            data = bytearray(b"\xff" * romlib.ROM_SIZE)
            data[0xA0:0xB0] = b"POKEMON EMER" + code.encode()
            sha = hashlib.sha1(data).hexdigest()
            with tempfile.TemporaryDirectory() as tmp, patch.dict(romlib.ROM_PROFILES, {code: sha}):
                full = self._write(tmp, data)
                trimmed = self._write(tmp, data[:0xC0], "trimmed.gba")
                zipped = Path(tmp) / "game.zip"
                with zipfile.ZipFile(zipped, "w") as zf:
                    zf.writestr("game.gba", data[:0xC0])
                for path in (full, trimmed, zipped):
                    loaded = romlib.load_rom(path)
                    self.assertEqual((loaded.code, loaded.sha1, loaded.data), (code, sha, data))

    def test_other_regional_gen3_games_are_named(self):
        # A Spanish FireRed or a German Ruby is named, not reported as an unknown file.
        cases = {b"POKEMON FIREBPRS": "Pokemon FireRed (Spain)",
                 b"POKEMON RUBYAXVD": "Pokemon Ruby (Germany)",
                 b"POKEMON LEAFBPGJ": "Pokemon LeafGreen (Japan)"}
        for head, name in cases.items():
            data = bytearray(b"\xff" * romlib.ROM_SIZE)
            data[0xA0:0xB0] = head
            with tempfile.TemporaryDirectory() as tmp:
                with self.assertRaises(BuilderError) as ctx:
                    romlib.load_rom(self._write(tmp, bytes(data)))
                self.assertEqual(ctx.exception.code, "rom_wrong_game")
                self.assertIn(name, ctx.exception.message)

    def test_zip_must_hold_one_rom(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "r.zip"
            with zipfile.ZipFile(path, "w") as zf:
                zf.writestr("a.gba", b"1")
                zf.writestr("b.gba", b"2")
            with self.assertRaises(BuilderError):
                romlib.load_rom(path)


class RegionalBuildTests(unittest.TestCase):
    def test_matching_spanish_recipe_builds_and_english_recipe_is_rejected(self):
        self._matching_recipe_builds("BPES")

    def test_matching_japanese_recipe_builds_and_english_recipe_is_rejected(self):
        self._matching_recipe_builds("BPEJ")

    def _matching_recipe_builds(self, code):
        data = bytearray(b"\xff" * romlib.ROM_SIZE)
        data[0xA0:0xB0] = b"POKEMON EMER" + code.encode()
        data[0x200:0x204] = b"TEST"
        sha = hashlib.sha1(data).hexdigest()
        crc = zlib.crc32(b"TEST") & 0xFFFFFFFF
        abi = pak.engine_abi([("game/test.bin", 4, crc)])
        with tempfile.TemporaryDirectory() as tmp, patch.dict(romlib.ROM_PROFILES, {code: sha}):
            root = Path(tmp)
            source = root / "regional.gba"
            source.write_bytes(data)
            recipe = rcp.Recipe(engine_abi=abi, rom_sha1=romlib.SUPPORTED_SHA1, release="test",
                                entries=[{"path": "game/test.bin", "size": 4, "crc": crc,
                                          "ops": [["C", 0x200, 4]]}])
            recipe.save(root / "emerald3ds.recipe")
            output = root / "output.pak"
            with self.assertRaises(BuilderError) as ctx:
                build_pack(source, Payload(root), output)
            self.assertIn(code, str(ctx.exception))
            self.assertFalse(output.exists())
            recipe.rom_sha1 = sha
            recipe.save(root / "emerald3ds.recipe")
            build_pack(source, Payload(root), output)
            with pak.PakReader(output) as reader:
                self.assertEqual(reader.read("game/test.bin"), b"TEST")
                self.assertEqual(reader.rom_sha1, bytes.fromhex(sha))


class VariantPayloadTests(unittest.TestCase):
    def test_build_picks_the_variant_made_from_the_rom(self):
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
        import build_web_payload
        from contextlib import redirect_stdout
        import io
        with tempfile.TemporaryDirectory() as tmp:
            with redirect_stdout(io.StringIO()):
                synthetic = build_web_payload.make_synthetic(Path(tmp) / "s")
            payload = Payload(synthetic["payload"])
            self.assertEqual([v.root.name for v in payload.variants()], ["payload", "es"])
            for rom, name in ((synthetic["rom"], "payload"), (synthetic["rom_es"], "es")):
                data = rom.read_bytes() + b"\xff" * (romlib.ROM_SIZE - rom.stat().st_size)
                sha = hashlib.sha1(data).hexdigest()
                checked = romlib.Rom(data=data, sha1=sha, title="SYNTHETIC", code="TEST", source=rom)
                info = build_pack(rom, payload, Path(tmp) / ("%s.pak" % name), runner="inprocess",
                                  rom=checked)
                self.assertEqual(info["payload"].root.name, name)
                self.assertEqual(info["payload"].voxelgen, synthetic["payload"] / "voxelgen")
                with pak.PakReader(Path(tmp) / ("%s.pak" % name)) as reader:
                    self.assertEqual(reader.rom_sha1.hex(), sha)


class InstallTests(unittest.TestCase):
    def test_install_writes_the_app_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "src.bin"
            src.write_bytes(b"x" * 1000)
            sd = Path(tmp) / "sd"
            sd.mkdir()
            dest = install(sd, {"Emerald3DS.3dsx": src, "emerald3ds.pak": src})
            self.assertEqual(sorted(p.name for p in dest.iterdir()), ["Emerald3DS.3dsx", "emerald3ds.pak"])
            self.assertEqual(dest, sd / "3ds" / "emerald3ds")


class VtreeTests(unittest.TestCase):
    def test_structures_are_read_back_from_the_rom(self):
        rom = bytearray(0x1000)
        base = 0x08000000
        # Tileset structs at 0x100 and 0x120; a layout at 0x200.
        struct.pack_into("<iiIIII", rom, 0x200, 20, 30, 0, 0, base + 0x100, base + 0x120)
        # Warps at 0x300: one warp (5, 6) to group 0 num 1.
        struct.pack_into("<hhBBBB", rom, 0x300, 5, 6, 0, 2, 1, 0)
        # Background events at 0x320: a sign at (7, 8) and a hidden item.
        struct.pack_into("<HHBB2xI", rom, 0x320, 7, 8, 0, 0, 0)
        struct.pack_into("<HHBB2xI", rom, 0x32C, 9, 9, 0, 7, 0)
        struct.pack_into("<BBBBIIII", rom, 0x280, 0, 1, 0, 2, 0, base + 0x300, 0, base + 0x320)
        # Connections at 0x340 -> table at 0x350.
        struct.pack_into("<iI", rom, 0x340, 1, base + 0x350)
        struct.pack_into("<B3xiBB2x", rom, 0x350, 2, -4, 0, 1)
        # Map header at 0x400: events 0x280, connections 0x340, layout id 1, type 3.
        struct.pack_into("<IIIIHH", rom, 0x400, base + 0x200, base + 0x280, 0, base + 0x340, 0, 1)
        rom[0x417] = 3
        meta = {
            "tilesets": {"gTileset_A": 0x100, "gTileset_B": 0x120},
            "layouts_table_label": "gMapLayouts",
            "layouts": [{"id": "LAYOUT_X", "name": "X_Layout", "blockdata_filepath": "data/layouts/X/map.bin",
                         "border_filepath": "data/layouts/X/border.bin", "rom": 0x200}],
            "maps": [{"folder": "Here", "id": "MAP_HERE", "name": "Here", "group": 0, "num": 0, "rom": 0x400},
                     {"folder": "There", "id": "MAP_THERE", "name": "There", "group": 0, "num": 1, "rom": 0x400}],
            "map_types": {"3": "MAP_TYPE_ROUTE"},
            "connection_directions": {"2": "up"},
            "metatiles_h": [["gMetatiles_A", "data/tilesets/primary/a/metatiles.bin"]],
            "headers_h": [["gTileset_A", "gMetatiles_A"]],
            "metatile_behaviors": [["MB_NORMAL", 0], ["MB_JUMP_EAST", 56]],
        }
        payload = b"\x01\x02"
        rom[0x600:0x602] = payload
        r = rcp.Recipe(engine_abi=0, rom_sha1="", release="t", vtree=meta,
                       inputs=[{"path": "data/layouts/X/map.bin", "size": 2,
                                "crc": zlib.crc32(payload) & 0xFFFFFFFF, "ops": [["C", 0x600, 2]]}])
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            vtree.build_tree(bytes(rom), r, root)
            layouts = json.loads((root / "data/layouts/layouts.json").read_text())["layouts"]
            self.assertEqual((layouts[0]["width"], layouts[0]["height"]), (20, 30))
            self.assertEqual((layouts[0]["primary_tileset"], layouts[0]["secondary_tileset"]),
                             ("gTileset_A", "gTileset_B"))
            m = json.loads((root / "data/maps/Here/map.json").read_text())
            self.assertEqual(m["layout"], "LAYOUT_X")
            self.assertEqual(m["map_type"], "MAP_TYPE_ROUTE")
            self.assertEqual((m["warp_events"][0]["x"], m["warp_events"][0]["y"]), (5, 6))
            self.assertEqual(m["warp_events"][0]["dest_map"], "MAP_THERE")
            self.assertEqual(m["connections"][0], {"map": "MAP_THERE", "offset": -4, "direction": "up"})
            self.assertEqual((root / "data/layouts/X/map.bin").read_bytes(), payload)
            self.assertIn("gMetatiles_A", (root / "src/data/tilesets/metatiles.h").read_text())
            self.assertEqual(m["bg_events"], [{"type": "sign", "x": 7, "y": 8, "elevation": 0}])
            self.assertIn("MB_JUMP_EAST = 56,",
                          (root / "include/constants/metatile_behaviors.h").read_text())


if __name__ == "__main__":
    unittest.main()
