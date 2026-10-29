"""Going back to English after a localized build leaves no localized art behind."""
import contextlib
import gzip
import importlib.util
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("bootstrap_under_test", ROOT / "tools/bootstrap.py")
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


@unittest.skipUnless(shutil.which("git"), "requires Git")
class BootstrapLocaleSwitchTests(unittest.TestCase):
    def switch_back(self, marker, stem):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            tree = root / "tree"
            (tree / "graphics").mkdir(parents=True)

            def git(*args):
                return subprocess.check_output(["git", "-c", "user.name=t", "-c", "user.email=t@t.invalid",
                                                "-c", "commit.gpgSign=false", *args],
                                               cwd=tree, text=True).strip()
            git("init", "-q")
            tracked = tree / "graphics/tracked.4bpp"
            tracked.write_bytes(b"source art")
            git("add", ".")
            git("commit", "-qm", "pinned")
            (root / "upstream.lock").write_text(
                '[pokeemerald]\nrepository = "unused"\ncommit = "%s"\n' % git("rev-parse", "HEAD"))
            (root / "patches/pokeemerald").mkdir(parents=True)
            (root / "tools/locales").mkdir(parents=True)
            graphics = [["graphics/tracked.4bpp", 0, 1], ["graphics/raw.4bpp", 0, 1],
                        ["graphics/packed.4bpp.lz", 0, 1]]
            (root / "tools/locales" / (stem + ".json.gz")).write_bytes(
                gzip.compress(json.dumps({"graphics": graphics}).encode()))
            # what the localizer left: tracked art changed, generated art added
            tracked.write_bytes(b"localized art")
            stale = [tree / "graphics/raw.4bpp", tree / "graphics/packed.4bpp.lz", tree / "graphics/packed.4bpp"]
            for path in stale:
                path.write_bytes(b"localized art")
            kept = tree / "graphics/unrelated.4bpp"
            kept.write_bytes(b"other generated art")
            (tree / ".emerald3ds-locale").write_text(marker + "\n")
            real_run = bootstrap.run

            def run(command, cwd=None):
                if command[0] == "git":  # reset and apply only; nothing is built
                    with contextlib.redirect_stdout(io.StringIO()):
                        real_run(command, cwd)
            with patch.object(bootstrap, "ROOT", root), patch.object(bootstrap, "OVERLAY", []), \
                    patch.object(bootstrap, "run", run), \
                    patch.object(sys, "argv", ["bootstrap.py", "--dir", str(tree)]), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(bootstrap.main(), 0)
            self.assertEqual(tracked.read_bytes(), b"source art")
            for path in stale:
                self.assertFalse(path.exists(), path.name)
            self.assertTrue(kept.exists())
            self.assertFalse((tree / ".emerald3ds-locale").exists())

    def test_every_language_cleans_its_art(self):
        for marker, stem in (("BPES experimental", "spanish"), ("BPEF experimental", "french"),
                             ("BPEI experimental", "italian"), ("BPED experimental", "german")):
            with self.subTest(marker=marker):
                self.switch_back(marker, stem)


if __name__ == "__main__":
    unittest.main()
