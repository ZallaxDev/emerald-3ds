"""ZIP input regressions using synthetic bytes only; no cartridge data."""

import hashlib
import io
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch
import warnings
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from emerald3ds_builder import rom
from emerald3ds_builder.errors import BuilderError


def archive(entries, compression=zipfile.ZIP_STORED):
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", compression=compression) as zf:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            for name, data in entries:
                zf.writestr(name, data)
    return out.getvalue()


def change_headers(data, local_offset, central_offset, value):
    """Change a 16-bit ZIP field in both headers of a single-member fixture."""
    data = bytearray(data)
    central = data.index(b"PK\x01\x02")
    struct.pack_into("<H", data, local_offset, value)
    struct.pack_into("<H", data, central + central_offset, value)
    return bytes(data)


class RomZipTests(unittest.TestCase):
    def assert_error(self, data, code):
        with self.assertRaises(BuilderError) as ctx:
            rom.check_rom_bytes(data, Path("game.zip"))
        self.assertEqual(ctx.exception.code, code)
        return ctx.exception

    def test_valid_trimmed_rom_extensions_and_compression(self):
        trimmed = bytearray(b"\xff" * 0xC0)
        trimmed[0xA0:0xB0] = b"POKEMON EMERBPEE"
        full = bytes(trimmed) + b"\xff" * (rom.ROM_SIZE - len(trimmed))
        sha1 = hashlib.sha1(full).hexdigest()
        with patch.dict(rom.ROM_PROFILES, {"BPEE": sha1}):
            for ext in ("GBA", "agb", "bin"):
                for compression in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                    with self.subTest(ext=ext, compression=compression):
                        data = archive([("folder/", b""), ("readme.txt", b"notes"),
                                        ("folder/game." + ext, trimmed)], compression)
                        loaded = rom.check_rom_bytes(data, Path("game.ZIP"))
                        self.assertEqual((loaded.data, loaded.sha1, loaded.code), (full, sha1, "BPEE"))

    def test_missing_multiple_and_duplicate_roms(self):
        for entries in ([], [("notes.txt", b"notes")],
                        [("a.gba", b"a"), ("b.bin", b"b")],
                        [("game.gba", b"a"), ("game.gba", b"b")]):
            with self.subTest(entries=entries):
                error = self.assert_error(archive(entries), "rom_zip_contents")
                for ext in (".gba", ".agb", ".bin"):
                    self.assertIn(ext, error.message)

    def test_oversized_member_rejected_before_extraction(self):
        data = archive([("game.gba", b"x" * (rom.ROM_SIZE + 1))], zipfile.ZIP_DEFLATED)
        with patch.object(zipfile.ZipFile, "open", side_effect=AssertionError("must not extract")):
            self.assert_error(data, "rom_too_large")

    def test_password_protected_member_rejected_before_extraction(self):
        data = change_headers(archive([("game.gba", b"synthetic")]), 6, 8, 1)
        with patch.object(zipfile.ZipFile, "open", side_effect=AssertionError("must not extract")):
            error = self.assert_error(data, "rom_zip_encrypted")
        self.assertIn("Extract", error.hint)

    def test_unsupported_compression_has_actionable_error(self):
        data = change_headers(archive([("game.gba", b"synthetic")]), 8, 10, 99)
        error = self.assert_error(data, "rom_zip_unsupported")
        self.assertIn("Extract", error.hint)

    def test_unavailable_compression_library_has_actionable_error(self):
        data = archive([("game.gba", b"synthetic")], zipfile.ZIP_DEFLATED)
        with patch.object(zipfile, "zlib", None):
            self.assert_error(data, "rom_zip_unsupported")

    def test_corrupt_archive_and_crc_are_read_errors(self):
        self.assert_error(b"not a ZIP file", "rom_unreadable")
        data = bytearray(archive([("game.gba", b"synthetic")]))
        data[30 + len("game.gba")] ^= 1
        self.assert_error(bytes(data), "rom_unreadable")

    def test_corrupt_deflate_stream_is_a_read_error(self):
        data = bytearray(archive([("game.gba", b"synthetic" * 100)], zipfile.ZIP_DEFLATED))
        # Reserved DEFLATE block type (BTYPE=3).
        data[30 + len("game.gba")] = 7
        self.assert_error(bytes(data), "rom_unreadable")

    def test_modified_rom_still_rejected_after_extraction(self):
        data = bytearray(b"\xff" * 0xC0)
        data[0xA0:0xB0] = b"POKEMON EMERBPEE"
        self.assert_error(archive([("game.gba", data)]), "rom_modified")

    def test_extraction_read_is_bounded(self):
        data = archive([("game.gba", b"synthetic")])
        with patch.object(zipfile.ZipFile, "open") as opened:
            opened.return_value.__enter__.return_value.read.return_value = b"x" * (rom.ROM_SIZE + 1)
            self.assert_error(data, "rom_too_large")
        opened.return_value.__enter__.return_value.read.assert_called_once_with(rom.ROM_SIZE + 1)


if __name__ == "__main__":
    unittest.main()
