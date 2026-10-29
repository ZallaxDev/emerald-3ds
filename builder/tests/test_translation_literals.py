"""gen_recipe.py credits a fan translation's own texts, and only them: the
texts of a catalog encoded as the game holds them, whole short texts only
where a string starts, and nothing else. Tiny synthetic charmap and catalog,
never game data.

Run: python -m unittest discover -s builder/tests
"""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import gen_recipe  # noqa: E402

LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
CHARMAP = "\n".join(["' ' = 00"] + ["'%s' = %02X" % (c, 0xBB + i) for i, c in enumerate(LETTERS)]
                    + ["'$' = FF", "'\\n' = FE", "'\\l' = FA", "'\\p' = FB", "PLAYER = FD 01", ""])
FONTS = "const u8 gFontNormalLatinGlyphWidths[] = {%s};\n" % ", ".join(["6"] * 256)


def encode(text: str) -> bytes:
    return bytes(0x00 if c == " " else 0xBB + LETTERS.index(c) for c in text)


class TranslationTextsTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        root = Path(self._tmp.name)
        self.tree = root / "tree"
        (self.tree / "src").mkdir(parents=True)
        (self.tree / "charmap.txt").write_text(CHARMAP, encoding="utf-8")
        (self.tree / "src" / "fonts.c").write_text(FONTS, encoding="utf-8")
        self.catalog = root / "pt_br"
        (self.catalog / "data" / "text").mkdir(parents=True)
        (self.catalog / "src").mkdir()
        (self.catalog / "referencia").mkdir()
        (self.catalog / "data" / "text" / "x.inc.txt").write_text(
            "[gText_Hello] 0123456789\nOLA {PLAYER} TUDO BEM\\nCOMO VAI$\n\n", encoding="utf-8")
        (self.catalog / "src" / "strings.c.txt").write_text(
            "[gText_Yes] 0123456789\nSIM\n\n[gText_Ok] 0123456789\nOK\n\n", encoding="utf-8")
        (self.catalog / "referencia" / "notes.txt").write_text("[gText_Not] 0123456789\nNAO CONTA$\n",
                                                               encoding="utf-8")

    def test_texts_are_encoded_and_cut_at_placeholders(self):
        runs, shorts = gen_recipe.translation_texts(self.catalog, self.tree)
        self.assertIn(encode(" TUDO BEM") + b"\xfe" + encode("COMO VAI") + b"\xff", runs)
        self.assertNotIn(encode("OLA "), runs)               # 4 bytes, and not a whole text
        self.assertEqual(shorts, {encode("SIM") + b"\xff"})  # "OK" + terminator is too short
        self.assertFalse(any(encode("NAO CONTA") in r for r in runs))  # referencia/ is not a catalog

    def test_mask_covers_the_texts_only(self):
        runs, shorts = gen_recipe.translation_texts(self.catalog, self.tree)
        text = encode(" TUDO BEM") + b"\xfe" + encode("COMO VAI") + b"\xff"
        data = b"\x12\x34" + text + b"\x56" + b"\xff" + encode("SIM") + b"\xff" + b"\x01" + encode("SIM") + b"\xff"
        mask = gen_recipe.translation_mask(data, runs, shorts)
        self.assertEqual(mask[:2], b"\x00\x00")
        self.assertEqual(mask[2:2 + len(text)], b"\x01" * len(text))
        start = 2 + len(text) + 2
        self.assertEqual(mask[start:start + 4], b"\x01" * 4)        # after a terminator: a string starts
        self.assertEqual(mask[start + 5:start + 9], b"\x00" * 4)    # after another byte: not credited

    def test_literal_positions_follow_the_operations(self):
        ops = [["C", 0, 3], ["L", 0, 2], ["F", 7, 4], ["L", 2, 1]]
        self.assertEqual(list(gen_recipe.literal_positions(ops, b"\x00" * 16)), [3, 4, 9])


if __name__ == "__main__":
    unittest.main()
