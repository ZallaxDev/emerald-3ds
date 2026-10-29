import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("staging", ROOT / "tools/port_common/staging.py")
staging = importlib.util.module_from_spec(spec)
spec.loader.exec_module(staging)


class StagingRomProfileTests(unittest.TestCase):
    def test_pack_fingerprint_matches_compiled_language(self):
        for language, code in (("ENGLISH", "BPEE"), ("SPANISH", "BPES"),
                               ("FRENCH", "BPEF"), ("ITALIAN", "BPEI"), ("GERMAN", "BPED")):
            with self.subTest(language=language), tempfile.TemporaryDirectory() as temporary:
                folder = Path(temporary)
                romfs = folder / "romfs"
                romfs.mkdir()
                (romfs / "fixture.txt").write_bytes(b"synthetic data")
                configuration = folder / "global.h"
                configuration.write_text("#define GAME_LANGUAGE (LANGUAGE_" + language + ")\n")
                destination = folder / "fixture.pak"
                staging.main(["pak", "--romfs", str(romfs), "--out", str(destination),
                              "--game-config", str(configuration)])
                with staging.pak.PakReader(destination) as reader:
                    self.assertEqual(reader.rom_sha1.hex(), staging.ROM_PROFILES[code])
                    self.assertEqual(reader.verify(), 1)

    def test_mismatched_rom_and_unknown_language_do_not_write_pack(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            romfs = folder / "romfs"
            romfs.mkdir()
            configuration = folder / "global.h"
            configuration.write_text("#define GAME_LANGUAGE (LANGUAGE_SPANISH)\n")
            destination = folder / "fixture.pak"
            arguments = ["pak", "--romfs", str(romfs), "--out", str(destination),
                         "--game-config", str(configuration)]
            with patch.object(staging.pak, "sha1_of", return_value=staging.SUPPORTED_ROM_SHA1):
                with self.assertRaisesRegex(ValueError, "does not match"):
                    staging.main(arguments + ["--rom", str(folder / "wrong.gba")])
            self.assertFalse(destination.exists())
            configuration.write_text("#define GAME_LANGUAGE (LANGUAGE_KOREAN)\n")
            with self.assertRaisesRegex(ValueError, "Cannot determine"):
                staging.main(arguments)
            self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main()
