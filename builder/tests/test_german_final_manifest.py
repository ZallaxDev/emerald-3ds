"""German final-gap metadata checks without retail data fixtures."""
import gzip
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class GermanFinalManifestTests(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads(gzip.decompress(
            (ROOT / "tools/locales/german.json.gz").read_bytes()))

    def test_all_thirty_seven_original_gaps_have_a_final_decision(self):
        decisions = self.manifest.get("source_text_decisions", [])
        self.assertEqual(len(decisions), 37)
        self.assertEqual(len({(d["file"], d["edition"]) for d in decisions}), 37)
        counts = {}
        for decision in decisions:
            key = decision["disposition"]
            counts[key] = counts.get(key, 0) + 1
            self.assertTrue(decision["reason"])
        self.assertEqual(counts, {"placement_accepted": 28,
                                  "retain_source_invariant": 1,
                                  "retail_keyboard_table": 8})
        self.assertEqual(self.manifest["unmapped_source_texts"], [])

    def test_each_accepted_scalar_placement_is_present_in_staging_edits(self):
        decisions = [d for d in self.manifest.get("source_text_decisions", [])
                     if d["disposition"] == "placement_accepted"]
        self.assertEqual(len(decisions), 28)
        offsets = []
        for decision in decisions:
            edits = self.manifest["files"][decision["file"]]["edits"]
            self.assertIn([decision["rom_offset"], decision["size"], "c"],
                          [e[2:] for e in edits])
            offsets.append(decision["rom_offset"])
        self.assertEqual(len(set(offsets)), 28)

    def test_uppercase_display_rows_are_the_nine_column_naming_rows(self):
        edits = self.manifest["files"]["src/text_input_strings.c"]["edits"]
        # Naming and Easy Chat keep separate render rows. A plausible eight-key
        # Easy Chat row cannot be reused for the nine-key naming screen.
        coordinates = [edit[2:] for edit in edits]
        self.assertIn([6539757, 37, "c"], coordinates)
        self.assertIn([6539794, 37, "c"], coordinates)

    def test_keyboard_reference_symbols_agree_with_retail_staging(self):
        import re
        symbols = gzip.decompress(
            (ROOT / "tools/locales/german-symbols.s.gz").read_bytes()).decode()
        addresses = {n: int(v) for n, v in re.findall(r"^\.equ (\S+), (\d+)$", symbols, re.M)}
        sizes = {n: int(v) for n, v in re.findall(r"^\.size (\S+), (\d+)$", symbols, re.M)}
        expected = {"sKeyboardChars": (5884744, 108),
                    "sPageColumnCounts": (5884852, 3),
                    "sPageColumnXPos": (5884855, 27),
                    "gText_NamingScreenKeyboard_MNOPQRS": (6539757, 37),
                    "gText_NamingScreenKeyboard_TUVWXYZ": (6539794, 37)}
        for name, (offset, size) in expected.items():
            with self.subTest(symbol=name):
                self.assertEqual(addresses[name], 0x08000000 + offset)
                self.assertEqual(sizes[name], size)

    def test_easy_chat_reference_symbols_agree_with_retail_staging(self):
        import re
        symbols = gzip.decompress(
            (ROOT / "tools/locales/german-symbols.s.gz").read_bytes()).decode()
        addresses = {n: int(v) for n, v in re.findall(r"^\.equ (\S+), (\d+)$", symbols, re.M)}
        sizes = {n: int(v) for n, v in re.findall(r"^\.size (\S+), (\d+)$", symbols, re.M)}
        self.assertEqual(len(self.manifest["id_lists"]), 4)
        for name, (offset, size) in self.manifest["id_lists"].items():
            with self.subTest(symbol=name):
                self.assertEqual(addresses[name], 0x08000000 + offset)
                self.assertEqual(sizes[name], size)

    def test_healthbox_uses_complete_retail_tiles_not_a_hand_edited_letter(self):
        entries = [entry for entry in self.manifest["graphics"]
                   if entry[0] == "graphics/battle_interface/hpbar.4bpp"]
        self.assertEqual(entries, [["graphics/battle_interface/hpbar.4bpp", 0xC11BDC, 384]])
