"""The checked-in German manifest and the port's German texts, without a ROM."""
import ast
import gzip
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import localize_german
from emerald3ds_builder import rom

KINDS = {"c", "u8array", "asm", "u32", "u16s", "u16array", "u16rows2", "u8rows9",
         "u8rows22", "u16rows10", "bard"}
ROM_SIZE = 16 * 1024 * 1024


def manifest():
    return json.loads(gzip.decompress((ROOT / "tools/locales/german.json.gz").read_bytes()))


class GermanManifestTests(unittest.TestCase):
    def test_one_cartridge_everywhere(self):
        self.assertEqual(manifest()["rom_sha1"], localize_german.GERMAN_SHA1)
        self.assertEqual(rom.GERMAN_SHA1, localize_german.GERMAN_SHA1)

    def test_edits_are_positions_only(self):
        # offsets, sizes and kinds: never a byte of the game's text
        for name, entry in manifest()["files"].items():
            self.assertRegex(entry["sha256"], r"^[0-9a-f]{64}$", name)
            previous_end = 0
            for edit in sorted(entry["edits"]):
                start, end, offset, size, kind = edit
                with self.subTest(file=name, edit=edit):
                    self.assertTrue(all(isinstance(n, int) for n in (start, end, offset, size)))
                    self.assertIn(kind, KINDS)
                    self.assertTrue(previous_end <= start < end)
                    self.assertTrue(0 <= offset < offset + size <= ROM_SIZE)
                previous_end = end

    def test_graphics_and_tables_are_rom_ranges(self):
        data = manifest()
        for path, offset, size in data["graphics"]:
            self.assertTrue(path.startswith("graphics/") or path.startswith("data/"), path)
            self.assertTrue(0 <= offset < offset + size <= ROM_SIZE, path)
        for name, offset, size in data["floors"]:
            self.assertTrue(0 <= offset < offset + size <= ROM_SIZE, name)
        self.assertEqual(len(data["floors"]), 4)

    def test_no_more_texts_left_in_english(self):
        # each one is listed with its reason; a regenerated manifest that
        # leaves more of them unmatched is a regression, not a detail
        unmapped = manifest()["unmapped_source_texts"]
        self.assertLessEqual(len(unmapped), 25)
        for text in unmapped:
            self.assertTrue(text["reason"], text)

    def test_battle_healthbox_is_german(self):
        # "KP" instead of "HP", and the German status icons of all four
        # battlers, as whole raw tiles of the cartridge's own table
        graphics = {path: (offset, size) for path, offset, size in manifest()["graphics"]}
        expected = {"hpbar": 384, "status": 480, "status2": 480, "status3": 480, "status4": 480}
        for name, size in expected.items():
            self.assertEqual(graphics["graphics/battle_interface/%s.4bpp" % name][1], size, name)
        hpbar = graphics["graphics/battle_interface/hpbar.4bpp"][0]
        self.assertEqual(graphics["graphics/battle_interface/status.4bpp"][0], hpbar + 384 + 288)

    def test_naming_keyboard_has_nine_columns(self):
        edits = manifest()["files"]["src/naming_screen.c"]["edits"]
        rows = [edit for edit in edits if edit[4] == "c" and edit[3] == 9]
        self.assertEqual(len(rows), 12)  # 3 pages of 4 rows
        self.assertEqual([edit[3] for edit in edits if edit[4] == "u8rows9"], [27])

    def test_easy_chat_lists_have_their_german_order(self):
        # the four move and Pokemon lists are read from the cartridge in the
        # German alphabetical order; the symbol table must point at all four
        symbols = gzip.decompress((ROOT / "tools/locales/german-symbols.s.gz").read_bytes()).decode()
        for name in ("gEasyChatGroup_Move1", "gEasyChatGroup_Move2",
                     "gEasyChatGroup_Pokemon", "gEasyChatGroup_Pokemon2"):
            address = int(re.search(r"\.equ %s, (\d+)" % name, symbols)[1])
            size = int(re.search(r"\.size %s, (\d+)" % name, symbols)[1])
            self.assertTrue(0x08000000 <= address < address + size <= 0x08000000 + ROM_SIZE, name)
            self.assertTrue(size > 0 and size % 2 == 0, name)


def ctr_texts():
    """Every CTR_TEXT(...) of the port whose seven arguments are string literals."""
    literal = r'"(?:[^"\\]|\\.)*"'
    call = re.compile(r"CTR_TEXT\(\s*(" + r"(?:" + literal + r"\s*)+" +
                      r"(?:,\s*(?:" + literal + r"\s*)+){6})\)")
    for folder in ("src", "include"):
        for path in sorted((ROOT / "3ds_port" / folder).glob("*")):
            if path.suffix not in (".c", ".h", ".inc"):
                continue
            for match in call.finditer(path.read_text(encoding="utf-8")):
                args = []
                for arg in re.split(r",\s*(?=\")", match.group(1)):
                    args.append("".join(ast.literal_eval(part)
                                        for part in re.findall(literal, arg)))
                if len(args) == 7:
                    yield path.name, args


def glyphs():
    """What CtrLocale_Glyph draws: the rest becomes '?' on the touch screen."""
    header = (ROOT / "3ds_port/include/3ds_locale_text.h").read_text(encoding="utf-8")
    body = header[header.index("CtrLocale_Glyph"):header.index("default:")]
    chars = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")
    for case in re.findall(r"case\s+(0x[0-9a-fA-F]+|'(?:\\.|[^'\\])'):", body):
        chars.add(chr(int(case, 16)) if case.startswith("0x") else ast.literal_eval(case))
    return chars


class GermanPortTextTests(unittest.TestCase):
    def test_the_port_has_german_texts(self):
        texts = list(ctr_texts())
        self.assertGreater(len(texts), 150)
        for name, args in texts:
            self.assertTrue(args[5], "%s: empty German text for %r" % (name, args[0]))

    def test_german_texts_show_on_the_touch_screen(self):
        # an umlaut is fine, a typographic quote or an accent the font lacks
        # would be drawn as '?'
        drawn = glyphs()
        for ch in "ÄÖÜäöüß":
            self.assertIn(ch, drawn)
        for name, args in ctr_texts():
            missing = sorted(set(args[5]) - drawn - {"\n"})
            self.assertFalse(missing, "%s: %r has %r" % (name, args[5], missing))

    def test_no_port_message_falls_back_to_english(self):
        # CTR_TEXT_PT gives German the English text; only the English ROM's
        # own hint may use it, the rest says it in German too
        literal = r'"(?:[^"\\]|\\.)*"'
        german_hints = 0
        for folder in ("src", "include"):
            for path in sorted((ROOT / "3ds_port" / folder).glob("*")):
                if path.suffix not in (".c", ".h", ".inc"):
                    continue
                source = path.read_text(encoding="utf-8")
                for match in re.finditer(r"\bCTR_TEXT_PT\(\s*(" + literal + ")", source):
                    self.assertTrue(match.group(1).startswith('"Only Pokemon Emerald (USA'),
                                    "%s: %s" % (path.name, match.group(1)))
                for macro, arity, german_index in (("CTR_TEXT_PT_DE", 3, 2),
                                                   ("CTR_TEXT_FR_IT_PT_DE", 5, 4)):
                    pattern = r"\b" + macro + r"\(((?:\s*" + literal + r"\s*,?)+)\)"
                    for match in re.finditer(pattern, source):
                        parts = re.split(r",\s*(?=\")", match.group(1).strip())
                        self.assertEqual(len(parts), arity, path.name)
                        texts = ["".join(ast.literal_eval(p) for p in re.findall(literal, part))
                                 for part in parts]
                        for text in texts:
                            self.assertTrue(text.strip(), path.name)
                            self.assertTrue(text.isascii(), "%s: %r" % (path.name, text))
                        german = texts[german_index]
                        german_hints += 1
        self.assertGreaterEqual(german_hints, 5)


if __name__ == "__main__":
    unittest.main()
