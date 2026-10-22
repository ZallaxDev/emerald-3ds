"""Pin non-terminated source-array widths without cartridge or game-text fixtures."""
import gzip
import json
import unittest
from pathlib import Path

MANIFEST = Path(__file__).resolve().parents[2] / "tools/locales/german.json.gz"


class GermanRawManifestTests(unittest.TestCase):
    def setUp(self):
        self.files = json.loads(gzip.decompress(MANIFEST.read_bytes()))["files"]

    def test_healthbox_clear_buffers_are_twenty_bytes_not_terminated_strings(self):
        edits = self.files["src/battle_interface.c"]["edits"]
        self.assertEqual([edit[3] for edit in edits], [20, 20])

    def test_hex_digit_lookup_contains_only_sixteen_entries(self):
        edits = self.files["src/string_util.c"]["edits"]
        self.assertEqual([edit[3] for edit in edits], [16])

    def test_keyboard_stages_complete_nine_column_storage_and_positions(self):
        edits = self.files["src/naming_screen.c"]["edits"]
        # Terminated title plus two bounded raw tables. Never scan raw keyboard
        # rows to an EOS, or truncate nine-column retail rows to eight bytes.
        self.assertEqual([edit[4] for edit in edits], ["c", "u8pages4x9", "u8rows9"])
        self.assertEqual([edit[2:4] for edit in edits[1:]],
                         [[5884744, 108], [5884855, 27]])
