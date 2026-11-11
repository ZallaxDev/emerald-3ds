"""ROM profile and release metadata checks; synthetic bytes only."""

import hashlib
import io
import sys
import tempfile
import zipfile
from dataclasses import replace
from unittest.mock import patch
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "builder"))
sys.path.insert(0, str(ROOT / "tools"))

import build_web_payload
from emerald3ds_builder import rom
from emerald3ds_builder.build import Payload, build_pack
from emerald3ds_builder.errors import BuilderError
from emerald3ds_builder.recipe import Recipe


class RomProfileTests(unittest.TestCase):
    def status(self, profile):
        return "not supported"

    def error_code(self, profile):
        return "rom_not_supported"

    def files(self, sha1, folder=None):
        prefix = "payload/" + (folder + "/" if folder else "")
        return {prefix + "emerald3ds.recipe": Recipe(
                    engine_abi=1, rom_sha1=sha1, release="test", entries=[]).to_bytes(),
                prefix + "Emerald3DS.3dsx": b"synthetic executable",
                prefix + "Emerald3DS.smdh": b"synthetic icon"}

    def test_all_supported_roms_keep_their_release_identity(self):
        for profile in rom.SUPPORTED_PROFILES:
            with self.subTest(code=profile.code):
                manifest = build_web_payload.make_manifest(self.files(profile.sha1), "1.0.0", "v1.0.0")
                self.assertEqual(manifest["supportedRoms"], [{
                    "sha1": profile.sha1, "name": rom.KNOWN_CODES[profile.code],
                    "code": profile.code, "region": profile.region, "language": profile.language}])

    def test_translation_uses_input_rom_identity_and_output_language(self):
        files = self.files(rom.SUPPORTED_SHA1)
        files.update(self.files(rom.SUPPORTED_SHA1, "pt"))
        manifest = build_web_payload.make_manifest(files, "1.0.0", "v1.0.0")
        translated = manifest["variants"][0]["rom"]
        self.assertEqual(translated, {**manifest["supportedRoms"][0], "language": "pt"})

    def test_unknown_recipe_is_rejected_in_default_and_variant(self):
        for folder in (None, "es"):
            with self.subTest(folder=folder):
                files = self.files(rom.SUPPORTED_SHA1) if folder else {}
                files.update(self.files("00" * 20, folder))
                with self.assertRaisesRegex(ValueError, "No supported ROM profile"):
                    build_web_payload.make_manifest(files, "1.0.0", "v1.0.0")

    def test_known_unsupported_recipes_cannot_be_published(self):
        for profile in rom.UNSUPPORTED_PROFILES:
            self.assertNotIn(profile.sha1, rom.ROM_PROFILES.values())
            for folder in (None, "es"):
                with self.subTest(rom=profile.name, folder=folder):
                    files = self.files(rom.SUPPORTED_SHA1) if folder else {}
                    files.update(self.files(profile.sha1, folder))
                    with self.assertRaisesRegex(ValueError, "recognised but " + self.status(profile)):
                        build_web_payload.make_manifest(files, "1.0.0", "v1.0.0")

    def fixture(self, profile):
        # Substitute fingerprints, never real game data or the validation logic.
        data = bytearray(b"\xff" * rom.ROM_SIZE)
        data[0xAC:0xB0] = profile.code.encode("ascii")
        return bytes(data), replace(profile, sha1=hashlib.sha1(data).hexdigest())

    def test_known_unsupported_roms_are_blocked_even_when_explicitly_allowed(self):
        for profile in rom.UNSUPPORTED_PROFILES:
            data, fixture = self.fixture(profile)
            archive = io.BytesIO()
            with zipfile.ZipFile(archive, "w") as zf:
                zf.writestr("game.gba", data[:0xC0])
            with patch.object(rom, "UNSUPPORTED_PROFILES", (fixture,)):
                for raw, name in ((data, "full.gba"), (data[:0xC0], "trimmed.gba"),
                                  (archive.getvalue(), "game.zip")):
                    for allowed in (None, (fixture.sha1,)):
                        with self.subTest(rom=profile.name, source=name, allowed=allowed):
                            with self.assertRaises(BuilderError) as ctx:
                                rom.check_rom_bytes(raw, Path(name), supported=allowed)
                            self.assertEqual(ctx.exception.code, self.error_code(profile))
                            self.assertIn(profile.name, ctx.exception.message)
                            self.assertIn("recognised but " + self.status(profile) + ".", ctx.exception.message)

    def test_matching_unsupported_recipe_does_not_create_a_pack(self):
        for profile in rom.UNSUPPORTED_PROFILES:
            data, fixture = self.fixture(profile)
            with tempfile.TemporaryDirectory() as tmp, \
                    patch.object(rom, "UNSUPPORTED_PROFILES", (fixture,)):
                root = Path(tmp)
                source = root / "game.gba"
                source.write_bytes(data)
                Recipe(engine_abi=1, rom_sha1=fixture.sha1, release="test", entries=[]).save(
                    root / "emerald3ds.recipe")
                output = root / "output.pak"
                checked = rom.Rom(data, fixture.sha1, "SYNTHETIC", fixture.code, source)
                for supplied in (None, checked):
                    with self.subTest(rom=profile.name, supplied=supplied is not None):
                        with self.assertRaises(BuilderError) as ctx:
                            build_pack(source, Payload(root), output, rom=supplied, follower_fetcher=False)
                        self.assertEqual(ctx.exception.code, self.error_code(profile))
                        self.assertFalse(output.exists())

    def test_firered_leafgreen_headers_do_not_suggest_future_support(self):
        for code in ("BPRE", "BPRJ", "BPRS", "BPRD", "BPRF", "BPRI",
                     "BPGE", "BPGJ", "BPGS", "BPGD", "BPGF", "BPGI"):
            with self.subTest(code=code):
                data = bytearray(0xC0)
                data[0xAC:0xB0] = code.encode("ascii")
                with self.assertRaises(BuilderError) as ctx:
                    rom.check_rom_bytes(bytes(data), Path("game.gba"))
                self.assertIn(rom.KNOWN_CODES[code], ctx.exception.message)
                self.assertIn("not supported.", ctx.exception.message)
                self.assertNotIn("not yet", str(ctx.exception))

    def test_ruby_sapphire_headers_do_not_enable_building(self):
        for code in ("AXVE", "AXVJ", "AXVS", "AXVD", "AXVF", "AXVI",
                     "AXPE", "AXPJ", "AXPS", "AXPD", "AXPF", "AXPI"):
            with self.subTest(code=code):
                data = bytearray(0xC0)
                data[0xAC:0xB0] = code.encode("ascii")
                with self.assertRaises(BuilderError) as ctx:
                    rom.check_rom_bytes(bytes(data), Path("game.gba"))
                self.assertEqual(ctx.exception.code, "rom_wrong_game")
                self.assertIn(rom.KNOWN_CODES[code], ctx.exception.message)


if __name__ == "__main__":
    unittest.main()
