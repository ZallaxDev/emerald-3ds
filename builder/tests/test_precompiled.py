"""Precompiled generator outputs (precompiled.py, tools/voxel_precompile.py).
Synthetic data only: no ROM is needed and none may ever be committed."""

import contextlib
import hashlib
import io
import random
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve()
sys.path.insert(0, str(HERE.parents[1]))
sys.path.insert(0, str(HERE.parents[2] / "tools"))

from emerald3ds_builder import pak, precompiled as pc, recipe as rcp, rom as romlib  # noqa: E402
from emerald3ds_builder.build import Payload, build_generated, build_pack  # noqa: E402
from emerald3ds_builder.errors import BuilderError  # noqa: E402
import voxel_precompile as vp  # noqa: E402

TILES = "data/tilesets/primary/t/tiles.4bpp"
PAL = "data/tilesets/primary/t/palettes/00.gbapal"


def texel_offset(x, y, width):
    """gen_voxel_buildings.texel_offset, as the generator writes a page."""
    morton = 0
    for bit in range(3):
        morton |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1)
    return ((y // 8) * (width // 8) + x // 8) * 64 + morton


def synthetic_graphics(seed=1):
    rnd = random.Random(seed)
    tiles = bytes(rnd.randrange(256) for _ in range(32 * 8))
    palette = struct.pack("<16H", *[rnd.randrange(0x8000) for _ in range(16)])
    return tiles, palette


def tile_texel(tiles, palette, tile, row, col, alpha=1):
    b = tiles[tile * 32 + row * 4 + col // 2]
    i = b >> 4 if col & 1 else b & 15
    c = struct.unpack_from("<H", palette, 2 * i)[0]
    return (c & 31) << 11 | (c >> 5 & 31) << 6 | (c >> 10 & 31) << 1 | alpha


class ProgramTests(unittest.TestCase):
    def test_every_operation(self):
        tiles, palette = synthetic_graphics()
        files = {TILES: tiles, PAL: palette}
        prog = vp.Program()
        prog.lit(b"HEAD" + bytes(100) + b"T")
        s = prog.source(TILES, 3, PAL)
        # an 8x2 raster block: a tile row, read leftwards with alpha clear from column 2
        runs = bytes([pc.TEX]) + vp.varint(8) + vp.varint(2) + bytes([pc.RASTER])
        runs += bytes([pc.TILE_RUN | 7 << 2]) + vp.varint(s) + bytes([5])
        runs += bytes([pc.SKIP_RUN]) + vp.varint(1)
        runs += bytes([pc.TILE_RUN | 3 << 2]) + vp.varint(s) + bytes([1 | 2 << 3 | 0x40 | 0x80])
        runs += bytes([pc.LIT_RUN]) + vp.varint(1) + struct.pack("<H", 0xBEEF)
        runs += bytes([pc.PAL_RUN]) + vp.varint(prog.path(PAL)) + bytes([7]) + vp.varint(2)
        prog.raw(runs, 32)
        prog.colour(PAL, 9)
        prog.raw(bytes([pc.PIXEL]) + vp.varint(prog.path(TILES)) + vp.varint(2) + bytes([13, 0x30]), 1)
        prog.copy(0, 4)
        program = prog.bytes()

        want = bytearray(b"HEAD" + bytes(100) + b"T")
        block = [tile_texel(tiles, palette, 3, 5, c) for c in range(8)] + [0]
        block += [tile_texel(tiles, palette, 3, 1, 7 - c, 0) for c in range(2, 6)]
        c7 = struct.unpack_from("<H", palette, 14)[0]
        block += [0xBEEF] + [pc.texel(c7)] * 2
        want += struct.pack("<16H", *block)
        want += struct.pack("<H", pc.texel(struct.unpack_from("<H", palette, 18)[0]))
        b = tiles[2 * 32 + 6]
        want.append(0x30 | b >> 4)
        want += b"HEAD"
        stats = {}
        self.assertEqual(pc.run(program, pc.Sources(files.get), stats), bytes(want))
        info = pc.summary(program)
        self.assertEqual((info["tile texels"], info["own texels"], info["palette texels"], info["skip texels"],
                          info["colours"], info["pixels"], info["copy"]), (12, 1, 2, 1, 1, 1, 4))
        self.assertEqual(sorted(info["paths"]), sorted([TILES, PAL]))

    def test_pica_order_is_the_generators(self):
        w, h = 16, 16
        values = [(i * 2654435761) & 0xFFFF for i in range(w * h)]
        page = bytearray(2 * w * h)
        for y in range(h):
            for x in range(w):
                struct.pack_into("<H", page, 2 * texel_offset(x, y, w), values[y * w + x])
        self.assertEqual(vp.inverse_pica(bytes(page), w, h), values)
        prog = vp.Program()
        prog.raw(bytes([pc.TEX]) + vp.varint(w) + vp.varint(h) + bytes([pc.PICA, pc.LIT_RUN])
                 + vp.varint(w * h) + struct.pack("<%dH" % (w * h), *values), 2 * w * h)
        self.assertEqual(pc.run(prog.bytes(), pc.Sources(lambda p: None)), bytes(page))

    def test_glass_is_read_off_the_texels(self):
        tiles, palette = synthetic_graphics(5)
        files = {TILES: tiles, PAL: palette}
        prog = vp.Program()
        s = prog.source(TILES, 0, PAL)
        prog.raw(bytes([pc.TEX]) + vp.varint(8) + vp.varint(1) + bytes([pc.RASTER, pc.TILE_RUN | 7 << 2])
                 + vp.varint(s) + bytes([0]), 16)
        prog.raw(bytes([pc.GLOW, pc.GLOW_SKIP]) + vp.varint(2) + bytes([pc.GLOW_TEXEL]) + vp.varint(5)
                 + bytes([pc.GLOW_LIT]) + vp.varint(1) + bytes([180]), 8)
        out = pc.run(prog.bytes(), pc.Sources(files.get))
        texels = struct.unpack_from("<8H", out)
        want = [0, 0]
        for v in texels[2:7]:
            r, g, b = ((v >> sh & 31) * 255 // 31 for sh in (11, 6, 1))
            want.append(min(255, 128 + (r + g + b) // 6))     # voxel_night.window_mask
        self.assertEqual(list(out[16:]), want + [180])
        info = pc.summary(prog.bytes())
        self.assertEqual((info["glass texels"], info["own glass"], info["own glass values"]), (5, 1, {180}))

    def test_damaged_programs_fail_cleanly(self):
        huge = vp.varint(1 << 33)
        for program in (bytes([99]), bytes([pc.COPY]) + vp.varint(5) + vp.varint(1),
                        bytes([pc.LIT]) + vp.varint(9) + b"abc",
                        bytes([pc.ZERO]) + huge,                       # must not allocate it
                        bytes([pc.TEX]) + huge + vp.varint(1) + bytes([0]),
                        bytes([pc.PATHS, 1, 2, 0xFF, 0xFE]),           # not UTF-8
                        bytes([pc.ZERO]) + b"\xff" * 20,               # endless number
                        bytes([pc.GLOW, 0, 1])):                       # glass before any texel
            with self.subTest(program=program[:12]), self.assertRaises(pc.ProgramError):
                pc.run(program, pc.Sources(lambda p: None), limit=1 << 20)
        rnd = random.Random(7)
        tiles, palette = synthetic_graphics()
        good = vp.Program()
        good.lit(b"x" * 40)
        good.raw(bytes([pc.TEX]) + vp.varint(8) + vp.varint(1) + bytes([pc.RASTER, pc.TILE_RUN | 7 << 2])
                 + vp.varint(good.source(TILES, 1, PAL)) + bytes([3]), 16)
        good = bytearray(good.bytes())
        for _ in range(300):
            bad = bytearray(good)
            bad[rnd.randrange(len(bad))] = rnd.randrange(256)
            try:
                pc.run(bytes(bad), pc.Sources({TILES: tiles, PAL: palette}.get), limit=4096)
            except pc.ProgramError:
                pass
        missing = vp.Program()
        missing.colour(PAL, 0)
        with self.assertRaises(pc.ProgramError):
            pc.run(missing.bytes(), pc.Sources(lambda p: None))


class MatcherTests(unittest.TestCase):
    def test_textures_of_tile_rows_are_written_as_references(self):
        tiles, palette = synthetic_graphics(2)
        files = {TILES: tiles, PAL: palette}
        pool = vp.TilePool.of(files, [(TILES, t, PAL) for t in range(8)])
        w, h = 32, 16
        values = [0] * (w * h)
        rnd = random.Random(3)
        for y in range(h):
            x = 0
            while x < w:
                n = rnd.randrange(1, 9)
                t, r, c0, left = rnd.randrange(8), rnd.randrange(8), rnd.randrange(8), rnd.randrange(2)
                for k in range(min(n, w - x, 8 - c0)):
                    c = 7 - (c0 + k) if left else c0 + k
                    values[y * w + x + k] = tile_texel(tiles, palette, t, r, c)
                x += n + rnd.randrange(3)
        values[5] = 0x1234 | 1      # a colour no palette has: written literally
        page = bytearray(2 * w * h)
        for y in range(h):
            for x in range(w):
                struct.pack_into("<H", page, 2 * texel_offset(x, y, w), values[y * w + x])
        matcher = vp.Matcher(pool)
        prog = vp.Program()
        matcher.encode(prog, w, h, pc.PICA, vp.inverse_pica(bytes(page), w, h), None)
        self.assertEqual(pc.run(prog.bytes(), pc.Sources(files.get)), bytes(page))
        self.assertEqual(matcher.stats["own"], 1)
        self.assertEqual(matcher.stats["tile"] + matcher.stats["palette"] + matcher.stats["own"]
                         + matcher.stats["skip"], w * h)
        self.assertGreater(matcher.stats["tile"], 0)


class AuditTests(unittest.TestCase):
    def test_a_literal_texel_fails_the_release_audit(self):
        import release_audit
        prog = vp.Program()
        prog.raw(bytes([pc.TEX]) + vp.varint(2) + vp.varint(1) + bytes([pc.RASTER, pc.LIT_RUN])
                 + vp.varint(2) + struct.pack("<2H", 0x7FFF, 0x0001), 4)
        program = prog.bytes()
        recipe = rcp.Recipe(engine_abi=0, rom_sha1="0" * 40, release="t", literals=program,
                            generated=[{"path": "voxel/buildings.bin", "size": 4, "crc": 0,
                                        "ops": [["V", 0, len(program)]]}])
        with contextlib.redirect_stdout(io.StringIO()):
            found = release_audit.audit_programs("payload/emerald3ds.recipe", recipe.to_bytes())
        self.assertEqual([f.rule for f in found], ["precompiled-texels"])

    def test_glass_written_literally_fails_the_release_audit(self):
        import release_audit
        for level, rules in ((180, []), (201, ["precompiled-texels"])):
            prog = vp.Program()
            prog.raw(bytes([pc.TEX]) + vp.varint(1) + vp.varint(1) + bytes([pc.RASTER, pc.SKIP_RUN])
                     + vp.varint(1), 2)
            prog.raw(bytes([pc.GLOW, pc.GLOW_LIT]) + vp.varint(1) + bytes([level]), 1)
            program = prog.bytes()
            recipe = rcp.Recipe(engine_abi=0, rom_sha1="0" * 40, release="t", literals=program,
                                generated=[{"path": "voxel/buildings.bin", "size": 3, "crc": 0,
                                            "ops": [["V", 0, len(program)]]}])
            with contextlib.redirect_stdout(io.StringIO()):
                found = release_audit.audit_programs("payload/emerald3ds.recipe", recipe.to_bytes())
            self.assertEqual([f.rule for f in found], rules)


class BuildTests(unittest.TestCase):
    """build_pack with precompiled files: no generator, or only the rest."""

    RELIEF = ("import sys\nfrom pathlib import Path\n"
              "out = Path(sys.argv[sys.argv.index('--output') + 1])\n"
              "data = (out.parent / 'buildings.bin').read_bytes()\n"
              "out.write_bytes(bytes(reversed(data)))\n")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        tiles, palette = synthetic_graphics(4)
        rom = bytearray(b"\xff" * romlib.ROM_SIZE)
        rom[0x1000:0x1000 + len(tiles)] = tiles
        rom[0x2000:0x2020] = palette
        self.rom = bytes(rom)
        (self.root / "rom.gba").write_bytes(self.rom)
        self.sha = hashlib.sha1(self.rom).hexdigest()
        self.inputs = [{"path": TILES, "size": len(tiles), "crc": zlib.crc32(tiles) & 0xFFFFFFFF,
                        "ops": [["C", 0x1000, len(tiles)]]},
                       {"path": PAL, "size": 32, "crc": zlib.crc32(palette) & 0xFFFFFFFF,
                        "ops": [["C", 0x2000, 32]]}]
        prog = vp.Program()
        prog.lit(b"VXB9-synthetic")
        prog.colour(PAL, 3)
        prog.raw(bytes([pc.TEX]) + vp.varint(8) + vp.varint(8) + bytes([pc.PICA])
                 + b"".join(bytes([pc.TILE_RUN | 7 << 2]) + vp.varint(prog.source(TILES, 1, PAL)) + bytes([r])
                            for r in range(8)), 128)
        self.program = prog.bytes()
        self.buildings = pc.run(self.program, pc.Sources({TILES: tiles, PAL: palette}.get))
        self.relief = bytes(reversed(self.buildings))
        self.rom_obj = romlib.Rom(data=self.rom, sha1=self.sha, title="SYNTHETIC", code="TEST",
                                  source=self.root / "rom.gba")

    def tearDown(self):
        self.tmp.cleanup()

    def recipe(self, precompile_relief: bool) -> rcp.Recipe:
        def gen(path, data):
            return {"path": path, "size": len(data), "crc": zlib.crc32(data) & 0xFFFFFFFF,
                    "generator": "voxel"}
        generated = [gen("voxel/buildings.bin", self.buildings), gen("voxel/relief.bin", self.relief)]
        literals = self.program
        generated[0]["ops"] = [["V", 0, len(self.program)]]
        if precompile_relief:
            relief = vp.Program()
            relief.lit(self.relief)
            literals += relief.bytes()
            generated[1]["ops"] = [["V", len(self.program), len(literals) - len(self.program)]]
        items = [(g["path"], g["size"], g["crc"]) for g in generated]
        vtree = {"tilesets": {}, "layouts_table_label": "gMapLayouts", "layouts": [], "maps": [],
                 "map_types": {}, "connection_directions": {}, "metatiles_h": [], "headers_h": [],
                 "metatile_behaviors": [["MB_NORMAL", 0]]}
        return rcp.Recipe(engine_abi=pak.engine_abi(items), rom_sha1=self.sha, release="t", generated=generated,
                          inputs=self.inputs, vtree=vtree, literals=literals)

    def build(self, recipe, **kw):
        payload = self.root / "payload"
        payload.mkdir(exist_ok=True)
        recipe.save(payload / "emerald3ds.recipe")
        out = self.root / "out.pak"
        messages = []
        info = build_pack(self.root / "rom.gba", Payload(payload), out, lambda f, m: messages.append((f, m)),
                          rom=self.rom_obj, follower_fetcher=False, **kw)
        with pak.PakReader(out) as reader:
            files = {name: reader.read(name) for name in ("voxel/buildings.bin", "voxel/relief.bin")}
        return info, files, messages

    def test_all_precompiled_runs_no_generator(self):
        # no voxelgen at all: a generator run would fail with payload_incomplete
        for runner in ("subprocess", "inprocess"):
            info, files, messages = self.build(self.recipe(True), runner=runner)
            self.assertEqual(files, {"voxel/buildings.bin": self.buildings, "voxel/relief.bin": self.relief})
            self.assertNotIn("tree", [name for name, _ in info["timings"]])
            self.assertIn("Generating the 3D scenery: buildings (1/2), done in", " ".join(m for _, m in messages))
        self.assertEqual(build_generated(self.recipe(True), self.rom),
                         {"voxel/buildings.bin": self.buildings, "voxel/relief.bin": self.relief})

    def test_two_generators_after_a_program_run_side_by_side(self):
        # two steps left: the parallel branch of run_generators, numbered after the program
        scripts = self.root / "payload" / "voxelgen" / "scripts"
        scripts.mkdir(parents=True)
        (scripts / "gen_voxel_relief.py").write_text(self.RELIEF, encoding="utf-8")
        leaves = b"leaves of the intro"
        (scripts / "gen_intro_margins.py").write_text(
            "import sys\nfrom pathlib import Path\n"
            "out = Path(sys.argv[sys.argv.index('--output') + 1])\n"
            "out.parent.mkdir(parents=True, exist_ok=True)\nout.write_bytes(%r)\n" % leaves, encoding="utf-8")
        recipe = self.recipe(False)
        recipe.generated.append({"path": "stage/leaves.bin", "size": len(leaves),
                                 "crc": zlib.crc32(leaves) & 0xFFFFFFFF, "generator": "stage"})
        recipe.engine_abi = pak.engine_abi([(g["path"], g["size"], g["crc"]) for g in recipe.generated])
        info, files, messages = self.build(recipe, runner="subprocess")
        self.assertEqual(files["voxel/relief.bin"], self.relief)
        text = " ".join(m for _, m in messages)
        for label in ("buildings (1/3)", "terrain relief (2/3)", "intro scene (3/3)"):
            self.assertIn(label, text)
        fractions = [f for f, _ in messages]
        self.assertEqual(fractions, sorted(fractions))

    def test_the_rest_is_generated_after_the_precompiled_files(self):
        scripts = self.root / "payload" / "voxelgen" / "scripts"
        scripts.mkdir(parents=True)
        (scripts / "gen_voxel_relief.py").write_text(self.RELIEF, encoding="utf-8")
        for runner in ("subprocess", "inprocess"):
            info, files, messages = self.build(self.recipe(False), runner=runner)
            self.assertEqual(files["voxel/relief.bin"], self.relief)
            fractions = [f for f, _ in messages]
            self.assertEqual(fractions, sorted(fractions))
            text = " ".join(m for _, m in messages)
            self.assertIn("buildings (1/2)", text)
            self.assertIn("terrain relief (2/2)", text)
            self.assertEqual([n for n, _ in info["timings"] if n.startswith("gen_")],
                             ["gen_voxel_buildings.py", "gen_voxel_relief.py"])

    def test_a_program_that_does_not_match_fails(self):
        recipe = self.recipe(True)
        recipe.generated[0]["crc"] ^= 1
        with self.assertRaises(BuilderError) as ctx:
            with contextlib.redirect_stdout(io.StringIO()):
                self.build(recipe)
        self.assertEqual(ctx.exception.code, "generator_output_mismatch")


if __name__ == "__main__":
    unittest.main()
