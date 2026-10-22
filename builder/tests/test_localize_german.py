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

    def assert_untouched(self):
        for name, text in self.sources.items():
            self.assertEqual((self.tree / name).read_bytes(), text.encode(), name)
        self.assertFalse((self.tree / ".emerald3ds-locale").exists())

    def test_raw_graphic_replaces_the_entire_source_asset_without_touching_neighbors(self):
        relative = "graphics/battle_interface/hpbar.4bpp"
        target = self.tree / relative
        target.parent.mkdir(parents=True)
        target.write_bytes(b"old tiles")
        neighbor = target.with_name("expbar.4bpp")
        neighbor.write_bytes(b"unchanged neighbor")
        self.manifest["graphics"] = [[relative, 32, 64]]
        self.stage()
        self.assertEqual(target.read_bytes(), self.data[32:96])
        self.assertEqual(neighbor.read_bytes(), b"unchanged neighbor")

    def prepare_text_edit(self, initializer, size, charmap=True):
        relative = "src/raw_text.c"
        text = "const unsigned char buffer[] = " + initializer + ";\n"
        self.sources[relative] = text
        path = self.tree / relative
        path.write_text(text, encoding="utf-8")
        start = text.index(initializer)
        self.manifest["files"][relative] = {
            "sha256": hashlib.sha256(text.encode()).hexdigest(),
            "edits": [[start, start + len(initializer), 32, size, "c"]],
        }
        if charmap:
            self.sources["charmap.txt"] = (
                "' ' = 00\n'A' = BB\n'é' = 1B\n'あ' = 01\n'$' = FF\n"
                "'\\\'' = B4\n'\"' = B1\n'\\n' = FE\n'\\l' = FA\n"
                "COLOR = FC 01 @ two-byte control prefix\n"
                "HIGHLIGHT = FC 02\nWHITE = 01\nTRANSPARENT = 00\n"
            )
            (self.tree / "charmap.txt").write_text(self.sources["charmap.txt"], encoding="utf-8")
        return path

    def assert_text_staged(self, path, size):
        expected = "const unsigned char buffer[] = {" + localize_german.encoded(self.data[32:32 + size]) + "};\n"
        self.assertEqual(path.read_text(), expected)
        self.assertEqual((self.tree / ".emerald3ds-locale").read_text(), "BPED experimental\n")

    def test_oversized_raw_text_aborts_before_any_write_without_adding_eos(self):
        self.prepare_text_edit('__("Aéあ")', 4)
        with self.assertRaisesRegex(ValueError, r"Raw text.*src/raw_text\.c.*4.*capacity 3"):
            self.stage()
        self.assert_untouched()

    def test_oversized_raw_controls_abort_and_preserve_existing_marker(self):
        self.prepare_text_edit('__("{COLOR WHITE}{HIGHLIGHT TRANSPARENT}              ")', 21)
        marker = self.tree / ".emerald3ds-locale"
        marker.write_bytes(b"existing locale\n")
        before = {p: p.read_bytes() for p in self.tree.rglob("*") if p.is_file()}
        with self.assertRaisesRegex(ValueError, r"Raw text.*21.*capacity 20"):
            self.stage()
        self.assertEqual({p: p.read_bytes() for p in self.tree.rglob("*") if p.is_file()}, before)

    def test_raw_controls_accept_exact_encoded_capacity(self):
        path = self.prepare_text_edit('__("{COLOR WHITE}{HIGHLIGHT TRANSPARENT}              ")', 20)
        self.stage()
        self.assert_text_staged(path, 20)

    def test_raw_unicode_accepts_exact_one_byte_glyph_capacity(self):
        path = self.prepare_text_edit('__("Aéあ")', 3)
        self.stage()
        self.assert_text_staged(path, 3)

    def test_raw_dollar_padding_counts_toward_exact_capacity(self):
        path = self.prepare_text_edit('__("あ$$$$$$$$$$")', 11)
        self.stage()
        self.assert_text_staged(path, 11)

    def test_raw_dollar_padding_does_not_allow_overflow(self):
        self.prepare_text_edit('__("あ$$$$$$$$$$")', 12)
        with self.assertRaisesRegex(ValueError, r"Raw text.*12.*capacity 11"):
            self.stage()
        self.assert_untouched()

    def test_raw_width_comes_from_the_tree_charmap(self):
        path = self.prepare_text_edit('__("é")', 2)
        charmap = self.tree / "charmap.txt"
        charmap.write_text(charmap.read_text().replace("'é' = 1B", "'é' = 1B 02"))
        self.stage()
        self.assert_text_staged(path, 2)

    def test_raw_replacement_may_be_shorter_than_source_capacity(self):
        path = self.prepare_text_edit('__("Aéあ")', 2)
        self.stage()
        self.assert_text_staged(path, 2)

    def test_raw_concatenated_literals_and_escapes_have_encoded_width(self):
        path = self.prepare_text_edit(r'''__("A\"" "\n\l'")''', 5)
        self.stage()
        self.assert_text_staged(path, 5)

    def test_raw_concatenated_literals_and_escapes_reject_one_byte_over(self):
        self.prepare_text_edit(r'''__("A\"" "\n\l'")''', 6)
        with self.assertRaisesRegex(ValueError, r"Raw text.*6.*capacity 5"):
            self.stage()
        self.assert_untouched()

    def test_unknown_raw_glyph_fails_closed_before_any_write(self):
        self.prepare_text_edit('__("Z")', 1)
        with self.assertRaisesRegex(ValueError, "Unknown raw text"):
            self.stage()
        self.assert_untouched()

    def test_unknown_raw_control_fails_closed_before_any_write(self):
        self.prepare_text_edit('__("{COLOR UNKNOWN}")', 1)
        with self.assertRaisesRegex(ValueError, "Unknown raw text"):
            self.stage()
        self.assert_untouched()

    def test_nonliteral_raw_initializer_fails_closed_before_any_write(self):
        self.prepare_text_edit('__("A" + OTHER)', 1)
        with self.assertRaisesRegex(ValueError, "Invalid raw text initializer"):
            self.stage()
        self.assert_untouched()

    def test_terminated_text_may_grow_without_loading_charmap(self):
        path = self.prepare_text_edit('_("A")', 12, charmap=False)
        self.stage()
        self.assert_text_staged(path, 12)
        self.assertFalse((self.tree / "charmap.txt").exists())

    def prepare_id_list(self, ids=(2, 1)):
        relative = "src/data/easy_chat/easy_chat_group_pokemon.h"
        text = "const unsigned short group[] = {\n    SPECIES_ALPHA,\n    SPECIES_BETA,\n};\n"
        constants = "include/constants/species.h"
        self.sources[relative] = text
        self.sources[constants] = "#define SPECIES_ALPHA 1\n#define SPECIES_BETA 2\n"
        for name in (relative, constants):
            path = self.tree / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(self.sources[name])
        self.data = self.data[:96] + b"".join(value.to_bytes(2, "little") for value in ids) + self.data[100:]
        self.sha = hashlib.sha1(self.data).hexdigest()
        self.rom = Rom(self.data, self.sha, "SYNTHETIC", "BPED", self.root / "fixture")
        self.manifest["rom_sha1"] = self.sha
        self.manifest["id_lists"] = {"gEasyChatGroup_Pokemon": [96, 4]}
        return self.tree / relative

    def test_id_lists_follow_rom_order_without_a_reference_symbol_file(self):
        path = self.prepare_id_list()
        (self.locales / "german-symbols.s.gz").unlink()
        self.stage()
        self.assertEqual(path.read_text(),
                         "const unsigned short group[] = {\n    SPECIES_BETA,\n    SPECIES_ALPHA,\n};\n")

    def test_id_list_order_is_unchanged_when_already_correct(self):
        path = self.prepare_id_list((1, 2))
        original = path.read_bytes()
        self.stage()
        self.assertEqual(path.read_bytes(), original)

    def assert_id_list_coordinates_rejected(self, location, error):
        self.prepare_id_list()
        self.manifest["id_lists"]["gEasyChatGroup_Pokemon"] = location
        with self.assertRaisesRegex(ValueError, error):
            self.stage()
        self.assert_untouched()

    def test_missing_id_list_coordinates_abort_before_writes(self):
        self.assert_id_list_coordinates_rejected(None, "coordinates")

    def test_wrong_id_list_size_aborts_before_writes(self):
        self.assert_id_list_coordinates_rejected([96, 3], "does not fit")

    def test_out_of_bounds_id_list_aborts_before_writes(self):
        self.assert_id_list_coordinates_rejected([127, 4], "does not fit")

    def test_non_integer_id_list_coordinates_abort_before_writes(self):
        self.assert_id_list_coordinates_rejected([96, "4"], "coordinates")

    def test_id_list_with_wrong_ids_aborts_before_writes(self):
        self.prepare_id_list((9, 1))
        with self.assertRaisesRegex(ValueError, "other IDs"):
            self.stage()
        self.assert_untouched()

    def test_reference_symbol_entrypoint_keeps_its_existing_contract(self):
        from locale_lists import stage_id_lists
        path = self.prepare_id_list()
        symbols = self.root / "reference.s.gz"
        symbols.write_bytes(gzip.compress(
            (".equ gEasyChatGroup_Pokemon, %d\n.size gEasyChatGroup_Pokemon, 4\n"
             % (0x08000000 + 96)).encode()))
        writes = {}
        stage_id_lists(self.tree, self.data, symbols, writes)
        self.assertEqual(writes[path].decode(),
                         "const unsigned short group[] = {\n    SPECIES_BETA,\n    SPECIES_ALPHA,\n};\n")
        self.assert_untouched()

    def test_numeric_tables_keep_their_c_dimensions(self):
        self.stage()
        self.assertEqual((self.tree / "src/fonts.c").read_text(),
                         "const unsigned char widths[] = {0x08,0x09,0x0A};\n")
        self.assertEqual((self.tree / "src/phrases.c").read_text(),
                         "const unsigned short words[][2] = {{0x1110,0x1312},{0x1514,0x1716}};\n")
        self.assertIn("LANGUAGE_GERMAN", (self.tree / "include/constants/global.h").read_text())

    def test_wrong_rom_hash_is_rejected_before_any_write(self):
        self.manifest["rom_sha1"] = "00" * 20
        with self.assertRaisesRegex(ValueError, "clean German BPED"):
            self.stage()
        self.assert_untouched()

    def prepare_nine_column_keyboard(self, size=108, kind="u8pages4x9"):
        name = "src/keyboard.c"
        initializer = "{{{0}}}"
        text = "const unsigned char keyboard[3][4][9] = " + initializer + ";\n"
        self.sources[name] = text
        (self.tree / name).write_text(text)
        start = text.index(initializer)
        self.manifest["files"][name] = {
            "sha256": hashlib.sha256(text.encode()).hexdigest(),
            "edits": [[start, start + len(initializer), 8, size, kind]],
        }
        return self.tree / name

    def test_whole_nine_column_keyboard_keeps_page_row_column_nesting(self):
        path = self.prepare_nine_column_keyboard()
        self.stage()
        data = self.data[8:116]
        rows = ["{" + localize_german.encoded(data[i:i + 9]) + "}"
                for i in range(0, 108, 9)]
        pages = ["{" + ",".join(rows[i:i + 4]) + "}" for i in range(0, 12, 4)]
        self.assertEqual(path.read_text(),
                         "const unsigned char keyboard[3][4][9] = {" + ",".join(pages) + "};\n")

    def test_wrong_whole_keyboard_dimensions_abort_before_any_write(self):
        for size in (27, 96, 107, 109):
            with self.subTest(size=size):
                self.prepare_nine_column_keyboard(size)
                with self.assertRaisesRegex(ValueError, "naming keyboard table dimensions"):
                    self.stage()
                self.assert_untouched()

    def test_naming_positions_kind_remains_three_nine_byte_rows(self):
        self.prepare_nine_column_keyboard(108, "u8rows9")
        with self.assertRaisesRegex(ValueError, "naming column table dimensions"):
            self.stage()
        self.assert_untouched()

    def test_keyboard_rom_overrun_aborts_before_any_write(self):
        self.prepare_nine_column_keyboard()
        self.manifest["files"]["src/keyboard.c"]["edits"][0][2] = 30
        with self.assertRaisesRegex(ValueError, "out-of-range"):
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