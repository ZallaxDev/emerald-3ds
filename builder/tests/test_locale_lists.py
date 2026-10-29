"""Easy Chat's ID lists in the order of a translated cartridge, on synthetic bytes."""
import gzip
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import locale_lists

LIST = "src/data/easy_chat/easy_chat_group_move_1.h"


class LocaleListTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.tree = self.root / "tree"
        (self.tree / "src/data/easy_chat").mkdir(parents=True)
        (self.tree / "include/constants").mkdir(parents=True)
        (self.tree / LIST).write_text("const u16 gEasyChatGroup_Move1[] = {\n"
                                      "    MOVE_POUND,\n    MOVE_TACKLE,\n    MOVE_SURF,\n};\n", encoding="utf-8")
        (self.tree / "include/constants/moves.h").write_text(
            "#define MOVE_POUND 1\n#define MOVE_TACKLE 33\n#define MOVE_SURF 57\n", encoding="utf-8")
        self.symbols = self.root / "symbols.s.gz"
        self.symbol(16, 6)

    def symbol(self, offset, size):
        self.symbols.write_bytes(gzip.compress((".equ gEasyChatGroup_Move1, %d\n.size gEasyChatGroup_Move1, %d\n"
                                                % (0x08000000 + offset, size)).encode()))

    def stage(self, ids):
        rom = bytearray(64)
        struct.pack_into("<%dH" % len(ids), rom, 16, *ids)
        writes = {}
        locale_lists.stage_id_lists(self.tree, bytes(rom), self.symbols, writes)
        return writes

    def test_the_list_follows_the_cartridge(self):
        # German: Surfer, Pfund, Tackle
        writes = self.stage([57, 1, 33])
        self.assertEqual(writes[self.tree / LIST].decode(), "const u16 gEasyChatGroup_Move1[] = {\n"
                         "    MOVE_SURF,\n    MOVE_POUND,\n    MOVE_TACKLE,\n};\n")

    def test_the_same_order_writes_nothing(self):
        self.assertEqual(self.stage([1, 33, 57]), {})

    def test_a_list_that_does_not_fit_is_refused(self):
        for offset, size, error in ((16, 8, "does not fit"), (62, 6, "does not fit")):
            with self.subTest(offset=offset, size=size):
                self.symbol(offset, size)
                with self.assertRaisesRegex(ValueError, error):
                    self.stage([57, 1, 33])

    def test_other_ids_are_refused(self):
        with self.assertRaisesRegex(ValueError, "other IDs"):
            self.stage([57, 1, 1])

    def test_a_missing_symbol_is_refused(self):
        self.symbols.write_bytes(gzip.compress(b".syntax unified\n"))
        with self.assertRaisesRegex(ValueError, "lack gEasyChatGroup_Move1"):
            self.stage([57, 1, 33])


if __name__ == "__main__":
    unittest.main()
