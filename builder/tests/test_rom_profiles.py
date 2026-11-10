"""ROM profile and release metadata checks; synthetic bytes only."""

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "builder"))
sys.path.insert(0, str(ROOT / "tools"))

import build_web_payload
from emerald3ds_builder import rom
from emerald3ds_builder.errors import BuilderError
from emerald3ds_builder.recipe import Recipe


class RomProfileTests(unittest.TestCase):
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
