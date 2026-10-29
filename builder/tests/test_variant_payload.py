"""Language variants must bring their own executables to a desktop build.

A payload/<lang>/ subfolder is picked from the player's ROM; the desktop build
and install copy that variant's executables next to the pack. These tests make
sure a variant without them fails early, by name, before extraction and before
the output folder or SD card is touched, while the web builder (recipe only)
keeps working. Synthetic data only: no ROM is needed and none may ever be
committed.

Run: python -m unittest discover -s builder/tests
"""

import hashlib
import io
import json
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

HERE = Path(__file__).resolve()
sys.path.insert(0, str(HERE.parents[1]))
sys.path.insert(0, str(HERE.parents[2] / "tools"))

import build_web_payload  # noqa: E402
from emerald3ds_builder import cli, pak, rom as romlib, web  # noqa: E402
from emerald3ds_builder.build import EXECUTABLE_NAMES, Payload, build_pack, select_payload  # noqa: E402
from emerald3ds_builder.errors import BuilderError  # noqa: E402

REAL_BUILD_PACK = build_pack


def checked_rom(path: Path, title: str = "SYNTHETIC") -> romlib.Rom:
    """The synthetic ROM as load_rom would return it (the real one rejects it)."""
    data = path.read_bytes() + b"\xff" * (romlib.ROM_SIZE - path.stat().st_size)
    return romlib.Rom(data=data, sha1=hashlib.sha1(data).hexdigest(), title=title, code="TEST", source=path)


def snapshot(root: Path) -> dict:
    """Every file and folder under root, with the files' bytes."""
    if not root.exists():
        return {}
    return {p.relative_to(root).as_posix(): (p.read_bytes() if p.is_file() else None)
            for p in sorted(root.rglob("*"))}


def quick_build_pack(*args, **kwargs):
    """The real build_pack, with in-process generators and no follower download."""
    kwargs.update(runner="inprocess", follower_fetcher=False)
    return REAL_BUILD_PACK(*args, **kwargs)


class VariantPayloadMixin:
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        with redirect_stdout(io.StringIO()):
            self.synthetic = build_web_payload.make_synthetic(self.tmp / "synthetic")
        self.root = Path(self.synthetic["payload"])
        self.payload = Payload(self.root)
        self.es_dir = self.root / "es"
        self.rom_en = checked_rom(self.synthetic["rom"])
        self.rom_es = checked_rom(self.synthetic["rom_es"])
        # The fixture is complete to begin with; each test takes away what it needs.
        for folder in (self.root, self.es_dir):
            for name in EXECUTABLE_NAMES:
                self.assertTrue((folder / name).is_file(), folder / name)

    def tearDown(self):
        self._tmp.cleanup()

    def remove_es(self, *names) -> None:
        for name in names or EXECUTABLE_NAMES:
            (self.es_dir / name).unlink()


class BuildPackVariantTests(VariantPayloadMixin, unittest.TestCase):
    def build(self, rom: romlib.Rom, out: Path, progress=None, **kwargs) -> dict:
        return build_pack(rom.source, self.payload, out, progress, runner="inprocess", rom=rom,
                          follower_fetcher=False, **kwargs)

    def test_missing_variant_executable_fails_before_extraction(self):
        for name in EXECUTABLE_NAMES:
            with self.subTest(name):
                saved = (self.es_dir / name).read_bytes()
                self.remove_es(name)
                try:
                    # The default variant's copy is there and must not hide the gap.
                    self.assertTrue((self.root / name).is_file())
                    out = self.tmp / ("%s.pak" % name)
                    seen = []
                    with self.assertRaises(BuilderError) as ctx:
                        self.build(self.rom_es, out, lambda f, m: seen.append(m), need_executables=True)
                    exc = ctx.exception
                    self.assertEqual(exc.code, "payload_incomplete")
                    self.assertIn("'es'", exc.message)
                    self.assertIn("es/%s" % name, exc.message)
                    self.assertIn("%s/es" % self.root.name, exc.hint)
                    self.assertIn("Extract the whole ZIP again", exc.hint)
                    self.assertFalse(out.exists())
                    self.assertFalse([m for m in seen if m.startswith("Extracting game data")], seen)
                finally:
                    (self.es_dir / name).write_bytes(saved)

    def test_every_missing_executable_is_named(self):
        self.remove_es()
        with self.assertRaises(BuilderError) as ctx:
            self.build(self.rom_es, self.tmp / "x.pak", need_executables=True)
        for name in EXECUTABLE_NAMES:
            self.assertIn("es/%s" % name, ctx.exception.message)

    def test_select_payload_checks_only_when_asked(self):
        self.remove_es()
        self.assertEqual(select_payload(self.payload, self.rom_es.sha1).root, self.es_dir)
        with self.assertRaises(BuilderError) as ctx:
            select_payload(self.payload, self.rom_es.sha1, need_executables=True)
        self.assertEqual(ctx.exception.code, "payload_incomplete")
        self.assertEqual(select_payload(self.payload, self.rom_en.sha1, need_executables=True).root, self.root)

    def test_default_variant_still_builds_when_another_language_is_incomplete(self):
        self.remove_es()
        out = self.tmp / "en.pak"
        info = self.build(self.rom_en, out, need_executables=True)
        self.assertEqual(info["payload"].root, self.root)
        with pak.PakReader(out) as reader:
            self.assertEqual(reader.rom_sha1.hex(), self.rom_en.sha1)

    def test_complete_variant_builds_with_executables_required(self):
        out = self.tmp / "es.pak"
        info = self.build(self.rom_es, out, need_executables=True)
        self.assertEqual(info["payload"].root, self.es_dir)
        self.assertEqual(info["payload"].language, "es")
        self.assertEqual(info["payload"].executables(), [self.es_dir / n for n in EXECUTABLE_NAMES])
        with pak.PakReader(out) as reader:
            self.assertEqual(reader.rom_sha1.hex(), self.rom_es.sha1)

    def test_recipe_only_build_is_the_default(self):
        # Callers that do not ask for executables (the web builder) are unchanged.
        self.remove_es()
        out = self.tmp / "es.pak"
        info = self.build(self.rom_es, out)
        self.assertEqual(info["payload"].root, self.es_dir)
        with pak.PakReader(out) as reader:
            self.assertEqual(reader.rom_sha1.hex(), self.rom_es.sha1)

    def test_unknown_rom_is_still_a_rom_mismatch(self):
        # No variant was made from this ROM: report that, not missing executables.
        for name in EXECUTABLE_NAMES:
            (self.root / name).unlink()
        other = self.tmp / "other.gba"
        other.write_bytes(b"\x5a" * 4096)
        with self.assertRaises(BuilderError) as ctx:
            self.build(checked_rom(other), self.tmp / "other.pak", need_executables=True)
        self.assertEqual(ctx.exception.code, "rom_mismatch")
        self.assertFalse((self.tmp / "other.pak").exists())

    def test_default_variant_message_is_unchanged(self):
        (self.root / EXECUTABLE_NAMES[0]).unlink()
        with self.assertRaises(BuilderError) as ctx:
            self.payload.check()
        self.assertEqual(ctx.exception.code, "payload_incomplete")
        self.assertEqual(ctx.exception.message, "This release is incomplete (%s missing next to the builder)."
                         % EXECUTABLE_NAMES[0])


class CliVariantTests(VariantPayloadMixin, unittest.TestCase):
    def run_cli(self, *argv, rom: romlib.Rom | None = None) -> tuple[int, str]:
        rom = rom or self.rom_es
        out, err = io.StringIO(), io.StringIO()
        with patch("emerald3ds_builder.cli.load_rom", return_value=rom), \
                patch("emerald3ds_builder.build.load_rom", return_value=rom), \
                patch("emerald3ds_builder.cli.build_pack", side_effect=quick_build_pack), \
                redirect_stdout(out), redirect_stderr(err):
            code = cli.main(["--payload", str(self.root)] + list(argv))
        return code, err.getvalue()

    def test_build_leaves_an_earlier_output_untouched(self):
        self.remove_es(EXECUTABLE_NAMES[0])
        output = self.tmp / "output"
        app = output / "3ds" / "emerald3ds"
        app.mkdir(parents=True)
        (app / EXECUTABLE_NAMES[0]).write_bytes(b"stale English executable")
        (app / "emerald3ds.pak").write_bytes(b"stale English pack")
        before = snapshot(output)
        code, err = self.run_cli("build", "--rom", str(self.synthetic["rom_es"]), "--output", str(output))
        self.assertEqual(code, 1)
        self.assertIn("Error:", err)
        self.assertIn("es/%s" % EXECUTABLE_NAMES[0], err)
        self.assertEqual(snapshot(output), before)

    def test_build_does_not_create_the_output_folder(self):
        self.remove_es()
        output = self.tmp / "fresh"
        code, err = self.run_cli("build", "--rom", str(self.synthetic["rom_es"]), "--output", str(output))
        self.assertEqual(code, 1)
        self.assertIn("'es'", err)
        self.assertFalse(output.exists())

    def test_build_copies_the_variant_executables(self):
        output = self.tmp / "output"
        app = output / "3ds" / "emerald3ds"
        app.mkdir(parents=True)
        (app / EXECUTABLE_NAMES[0]).write_bytes(b"stale English executable")
        code, err = self.run_cli("build", "--rom", str(self.synthetic["rom_es"]), "--output", str(output))
        self.assertEqual(code, 0, err)
        for name in EXECUTABLE_NAMES:
            self.assertEqual((app / name).read_bytes(), (self.es_dir / name).read_bytes(), name)
        with pak.PakReader(app / "emerald3ds.pak") as reader:
            self.assertEqual(reader.rom_sha1.hex(), self.rom_es.sha1)

    def test_english_recipe_only_build_still_succeeds(self):
        # Developers build the default variant from a recipe-only payload: the
        # missing executables are skipped, as they always were.
        for name in EXECUTABLE_NAMES:
            (self.root / name).unlink()
        self.remove_es()
        output = self.tmp / "output"
        code, err = self.run_cli("build", "--rom", str(self.synthetic["rom"]), "--output", str(output),
                                 rom=self.rom_en)
        self.assertEqual(code, 0, err)
        app = output / "3ds" / "emerald3ds"
        self.assertEqual(sorted(p.name for p in app.iterdir()), ["emerald3ds.pak"])
        with pak.PakReader(app / "emerald3ds.pak") as reader:
            reader.verify()
            self.assertEqual(reader.rom_sha1.hex(), self.rom_en.sha1)

    def test_english_build_copies_the_default_executables(self):
        self.remove_es()   # another language being incomplete does not matter here
        output = self.tmp / "output"
        code, err = self.run_cli("build", "--rom", str(self.synthetic["rom"]), "--output", str(output),
                                 rom=self.rom_en)
        self.assertEqual(code, 0, err)
        app = output / "3ds" / "emerald3ds"
        for name in EXECUTABLE_NAMES:
            self.assertEqual((app / name).read_bytes(), (self.root / name).read_bytes(), name)

    def test_spanish_build_is_strict_even_when_the_default_is_recipe_only(self):
        for name in EXECUTABLE_NAMES:
            (self.root / name).unlink()
        self.remove_es(EXECUTABLE_NAMES[1])
        output = self.tmp / "fresh"
        code, err = self.run_cli("build", "--rom", str(self.synthetic["rom_es"]), "--output", str(output))
        self.assertEqual(code, 1)
        self.assertIn("es/%s" % EXECUTABLE_NAMES[1], err)
        self.assertFalse(output.exists())

    def test_install_leaves_the_sd_card_untouched(self):
        self.remove_es(EXECUTABLE_NAMES[0])
        sd = self.tmp / "sd"
        (sd / "Nintendo 3DS").mkdir(parents=True)
        app = sd / "3ds" / "emerald3ds"
        app.mkdir(parents=True)
        for name in EXECUTABLE_NAMES:
            (app / name).write_bytes(b"stale English " + name.encode())
        (app / "emerald3ds.pak").write_bytes(b"stale English pack")
        before = snapshot(sd)
        with patch("emerald3ds_builder.cli.install") as installer:
            code, err = self.run_cli("install", "--rom", str(self.synthetic["rom_es"]), "--sd", str(sd))
        self.assertEqual(code, 1)
        self.assertIn("Error:", err)
        self.assertIn("es/%s" % EXECUTABLE_NAMES[0], err)
        self.assertNotIn("Traceback", err)
        installer.assert_not_called()
        self.assertEqual(snapshot(sd), before)


class WebRecipeOnlyTests(VariantPayloadMixin, unittest.TestCase):
    def test_web_build_of_a_variant_needs_no_executables(self):
        unpacked = self.tmp / "unpacked"
        with zipfile.ZipFile(self.synthetic["zip"]) as zf:
            zf.extractall(unpacked)
        for folder in (unpacked / "payload", unpacked / "payload" / "es"):
            for name in EXECUTABLE_NAMES:
                (folder / name).unlink(missing_ok=True)
        out = self.tmp / "web" / "emerald3ds.pak"
        result = json.loads(web.run_web_build(str(self.synthetic["rom_es"]), str(unpacked / "payload"),
                                              str(out), None, str(unpacked / "web-manifest.json")))
        self.assertTrue(result["ok"], result)
        with pak.PakReader(out) as reader:
            self.assertEqual(reader.rom_sha1.hex(), self.rom_es.sha1)


if __name__ == "__main__":
    unittest.main()
