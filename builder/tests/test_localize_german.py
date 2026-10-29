"""Localization staging against tiny synthetic bytes, never ROM fixtures."""
import contextlib
import gzip
import hashlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import localize_german
from emerald3ds_builder.rom import Rom


class GermanStagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.tree = self.root / "tree"
        self.data = bytes(range(128))
        self.sha = hashlib.sha1(self.data).hexdigest()
        self.rom = Rom(self.data, self.sha, "SYNTHETIC", "BPED", self.root / "fixture")
        self.sources = {
            "src/fonts.c": "const unsigned char widths[] = {0};\n",
            "src/phrases.c": "const unsigned short words[][2] = {{0,0}};\n",
            "src/data/credits.h": "/* synthetic credits placeholder */\n",
            "src/data/battle_frontier/trainer_hill.h": "/* no floors */\n",
            "include/constants/global.h": "#define GAME_LANGUAGE (LANGUAGE_ENGLISH)\n",
        }
        for name, text in self.sources.items():
            path = self.tree / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(text.encode())
        files = {}
        for name, needle, offset, size, kind in (
                ("src/fonts.c", "{0}", 8, 3, "u8array"),
                ("src/phrases.c", "{0,0}", 16, 8, "u16rows2")):
            text = self.sources[name]
            start = text.index(needle)
            files[name] = {"sha256": hashlib.sha256(text.encode()).hexdigest(),
                           "edits": [[start, start + len(needle), offset, size, kind]]}
        self.manifest = {"rom_sha1": self.sha, "files": files, "graphics": [],
                         "credits": 0, "credits_entries": 0, "floors": []}
        self.locales = self.root / "tools/locales"
        self.locales.mkdir(parents=True)
        # Avoid any dependency on reference symbols from a real cartridge.
        (self.locales / "german-symbols.s.gz").write_bytes(gzip.compress(b".syntax unified\n"))

    def stage(self):
        (self.locales / "german.json.gz").write_bytes(
            gzip.compress(json.dumps(self.manifest).encode()))
        with patch.object(localize_german, "ROOT", self.root), \
                patch.object(localize_german, "GERMAN_SHA1", self.sha), \
                patch.object(localize_german, "load_rom", return_value=self.rom), \
                contextlib.redirect_stdout(io.StringIO()):
            localize_german.stage(self.tree, self.rom.source)

    def stage_unpatched(self):
        """stage() with the module's own GERMAN_SHA1 (whatever the test set)."""
        (self.locales / "german.json.gz").write_bytes(
            gzip.compress(json.dumps(self.manifest).encode()))
        with patch.object(localize_german, "ROOT", self.root), \
                patch.object(localize_german, "load_rom", return_value=self.rom), \
                contextlib.redirect_stdout(io.StringIO()):
            localize_german.stage(self.tree, self.rom.source)

    def assert_untouched(self):
        for name, text in self.sources.items():
            self.assertEqual((self.tree / name).read_bytes(), text.encode(), name)
        self.assertFalse((self.tree / ".emerald3ds-locale").exists())

    def test_numeric_tables_keep_their_c_dimensions(self):
        self.stage()
        self.assertEqual((self.tree / "src/fonts.c").read_text(),
                         "const unsigned char widths[] = {0x08,0x09,0x0A};\n")
        self.assertEqual((self.tree / "src/phrases.c").read_text(),
                         "const unsigned short words[][2] = {{0x1110,0x1312},{0x1514,0x1716}};\n")
        self.assertIn("LANGUAGE_GERMAN", (self.tree / "include/constants/global.h").read_text())
        self.assertEqual((self.tree / ".emerald3ds-locale").read_text(), "BPED experimental\n")

    def test_another_cartridge_is_rejected_before_any_write(self):
        # a clean BPED dump is the only one this manifest describes: even a
        # ROM the manifest names is refused if it is not the German one
        with patch.object(localize_german, "GERMAN_SHA1", "00" * 20):
            with self.assertRaisesRegex(ValueError, "clean German BPED"):
                self.stage_unpatched()
        self.assert_untouched()

    def test_wrong_rom_hash_is_rejected_before_any_write(self):
        self.manifest["rom_sha1"] = "00" * 20
        with self.assertRaisesRegex(ValueError, "clean German BPED"):
            self.stage()
        self.assert_untouched()

    def test_naming_column_positions_are_read_as_three_nine_byte_rows(self):
        edit = self.manifest["files"]["src/fonts.c"]["edits"][0]
        edit[3:] = [27, "u8rows9"]
        self.stage()
        rows = ["{" + ",".join("0x%02X" % n for n in range(start, start + 9)) + "}"
                for start in (8, 17, 26)]
        expected = "const unsigned char widths[] = {" + ",".join(rows) + "};\n"
        self.assertEqual((self.tree / "src/fonts.c").read_text(), expected)

    def test_source_mismatch_is_rejected_before_any_write(self):
        self.manifest["files"]["src/phrases.c"]["sha256"] = "00" * 32
        with self.assertRaisesRegex(ValueError, "Source differs"):
            self.stage()
        self.assert_untouched()

    def test_bad_edit_or_table_dimensions_leave_all_sources_untouched(self):
        original = self.manifest["files"]["src/phrases.c"]["edits"][0]
        for edit, error in (
                ([original[0], original[1], 127, 8, "u16rows2"], "out-of-range"),
                ([original[0], original[1], 16, 6, "u16rows2"], "dimensions"),
                ([original[0], original[1], 16, 8, "unknown"], "Unknown edit kind")):
            with self.subTest(edit=edit):
                self.manifest["files"]["src/phrases.c"]["edits"] = [edit]
                with self.assertRaisesRegex(ValueError, error):
                    self.stage()
                self.assert_untouched()

    def test_overlapping_edits_are_rejected_before_any_write(self):
        edits = self.manifest["files"]["src/phrases.c"]["edits"]
        edits.append(edits[0][:])
        with self.assertRaisesRegex(ValueError, "Overlapping"):
            self.stage()
        self.assert_untouched()

    def raw_text(self, name, size, initializer='__("a{PAUSE}")'):
        """One __() buffer in `name` (three bytes by default), replaced by `size` ROM bytes."""
        text = 'const u8 sText[] = %s;\n' % initializer
        self.sources[name] = text
        (self.tree / name).parent.mkdir(parents=True, exist_ok=True)
        (self.tree / name).write_bytes(text.encode())
        (self.tree / "charmap.txt").write_text(
            "'a' = BB\n'$' = FF\n'\\n' = FE\n'é' = 1B\nPAUSE = FC 08\nCOLOR = FC 01\n", encoding="utf-8")
        start = text.index("__(")
        self.manifest["files"][name] = {"sha256": hashlib.sha256(text.encode()).hexdigest(),
                                        "edits": [[start, text.index(";"), 40, size, "c"]]}

    def test_raw_text_fills_its_buffer(self):
        self.raw_text("src/battle_interface.c", 3)
        self.stage()
        self.assertEqual((self.tree / "src/battle_interface.c").read_text(),
                         "const u8 sText[] = {0x28,0x29,0x2A};\n")

    def test_raw_text_longer_than_its_buffer_is_rejected_before_any_write(self):
        # a __() buffer has no terminator: one more byte would be cut off by
        # the compiler or run into the next object
        self.raw_text("src/battle_interface.c", 4)
        with self.assertRaisesRegex(ValueError, "longer than its buffer"):
            self.stage()
        self.assert_untouched()

    def test_only_the_keyboard_rows_grow_to_nine_columns(self):
        self.raw_text("src/naming_screen.c", 10)
        with self.assertRaisesRegex(ValueError, "longer than its buffer"):
            self.stage()
        self.assert_untouched()
        self.raw_text("src/naming_screen.c", 9)
        self.stage()
        self.assertIn("{0x28,", (self.tree / "src/naming_screen.c").read_text())

    def test_raw_graphics_are_copied_byte_for_byte(self):
        # the healthbox's "KP" bar is raw 4bpp inside a concatenated table
        self.manifest["graphics"] = [["graphics/battle_interface/hpbar.4bpp", 64, 32]]
        self.stage()
        self.assertEqual((self.tree / "graphics/battle_interface/hpbar.4bpp").read_bytes(),
                         self.data[64:96])

    def test_raw_text_may_be_shorter_than_its_buffer(self):
        self.raw_text("src/battle_interface.c", 2)
        self.stage()
        self.assertEqual((self.tree / "src/battle_interface.c").read_text(),
                         "const u8 sText[] = {0x28,0x29};\n")

    def test_raw_text_width_counts_padding_escapes_and_every_literal(self):
        # "$" padding, "\\n", a one-byte accent and two literals: 1+1+1+1+1+2 = 7
        initializer = '__("a$\\n" "é$" "{PAUSE}")'
        self.raw_text("src/battle_interface.c", 8, initializer)
        with self.assertRaisesRegex(ValueError, "longer than its buffer"):
            self.stage()
        self.assert_untouched()
        self.raw_text("src/battle_interface.c", 7, initializer)
        self.stage()
        self.assertIn("0x2E}", (self.tree / "src/battle_interface.c").read_text())

    def test_unknown_raw_text_fails_before_any_write(self):
        for initializer in ('__("ß")', '__("{NOPE}")', '__(sOther)'):
            with self.subTest(initializer=initializer):
                self.raw_text("src/battle_interface.c", 1, initializer)
                with self.assertRaisesRegex(ValueError, "raw text"):
                    self.stage()
                self.assert_untouched()

    def test_terminated_text_does_not_need_the_charmap(self):
        # _() strings end at their terminator, so a longer German text is fine
        text = 'const u8 sText[] = _("a");\n'
        self.sources["src/strings.c"] = text
        (self.tree / "src/strings.c").write_bytes(text.encode())
        start = text.index("_(")
        self.manifest["files"]["src/strings.c"] = {"sha256": hashlib.sha256(text.encode()).hexdigest(),
                                                   "edits": [[start, text.index(";"), 40, 12, "c"]]}
        self.stage()
        self.assertFalse((self.tree / "charmap.txt").exists())
        self.assertIn("0x33}", (self.tree / "src/strings.c").read_text())

    def test_graphics_outside_the_rom_fail_before_any_write(self):
        self.manifest["graphics"] = [["graphics/battle_interface/hpbar.4bpp", 120, 32]]
        with self.assertRaisesRegex(ValueError, "outside ROM"):
            self.stage()
        self.assert_untouched()
        self.assertFalse((self.tree / "graphics").exists())
