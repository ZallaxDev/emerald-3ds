"""Locale switching with a real temporary Git tree and synthetic graphics."""
import contextlib
import gzip
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("bootstrap_locale_switch", ROOT / "tools/bootstrap.py")
assert spec is not None and spec.loader is not None
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


@unittest.skipUnless(shutil.which("git"), "requires Git")
class BootstrapLocaleSwitchTests(unittest.TestCase):
    def assert_switch_cleans_locale_graphics(self, code, language, portuguese=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tree = root / "tree"
            tree.mkdir()
            def git(*args):
                return subprocess.check_output(["git", *args], cwd=tree, stderr=subprocess.STDOUT, text=True).strip()
            git("init", "-q")
            tracked = tree / "graphics/tracked.bin"
            tracked.parent.mkdir()
            tracked.write_bytes(b"synthetic original art")
            git("add", "graphics/tracked.bin")
            git("-c", "core.hooksPath=/dev/null", "-c", "commit.gpgSign=false",
                "-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-qm", "fixture")
            commit = git("rev-parse", "HEAD")
            (root / "upstream.lock").write_text(
                '[pokeemerald]\nrepository = "unused"\ncommit = "' + commit + '"\n')
            (root / "patches/pokeemerald").mkdir(parents=True)
            locales = root / "tools/locales"
            locales.mkdir(parents=True)
            generated = [tree / name for name in ("graphics/generated.bin.lz",
                         "graphics/generated.bin", "graphics/raw.bin")]
            for path in generated:
                path.write_bytes(b"synthetic stale locale art")
            tracked.write_bytes(b"synthetic staged tracked art")
            unrelated = tree / "graphics/unrelated.bin"
            unrelated.write_bytes(b"unrelated generated data")
            manifest = {"graphics": [["graphics/tracked.bin", 0, 1],
                        ["graphics/generated.bin.lz", 0, 1], ["graphics/raw.bin", 0, 1]]}
            (locales / (language + ".json.gz")).write_bytes(gzip.compress(json.dumps(manifest).encode()))
            marker = tree / ".emerald3ds-locale"
            marker.write_text(code + " experimental\n")
            (tree / "3ds_port/build").mkdir(parents=True)
            (tree / "3ds_port/romfs").mkdir()
            commands = []
            real_run = bootstrap.run
            def run(command, cwd=None):
                if "localize_portuguese.py" in str(command[1] if len(command) > 1 else ""):
                    for path in generated:
                        self.assertFalse(path.exists(), "stale art before Portuguese staging")
                    commands.append(command)
                    return
                return real_run(command, cwd=cwd)
            argv = ["bootstrap.py", "--dir", str(tree)]
            if portuguese:
                argv += ["--port-lang", "pt_br"]
            with patch.object(bootstrap, "ROOT", root), patch.object(bootstrap, "OVERLAY", []), \
                    patch.object(bootstrap, "run", side_effect=run), \
                    patch.object(bootstrap, "fetch", side_effect=AssertionError("unexpected fetch")), \
                    patch.object(sys, "argv", argv), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(bootstrap.main(), 0)
            for path in generated:
                self.assertFalse(path.exists(), str(path.relative_to(tree)))
            self.assertEqual(tracked.read_bytes(), b"synthetic original art")
            self.assertEqual(unrelated.read_bytes(), b"unrelated generated data")
            self.assertFalse(marker.exists())
            self.assertFalse((tree / "3ds_port/build").exists())
            self.assertFalse((tree / "3ds_port/romfs").exists())
            self.assertEqual(len(commands), int(portuguese))

    def test_german_to_english_removes_generated_locale_art(self):
        self.assert_switch_cleans_locale_graphics("BPED", "german")

    def test_german_to_portuguese_cleans_before_running_the_port_stager(self):
        self.assert_switch_cleans_locale_graphics("BPED", "german", portuguese=True)

    def test_existing_cartridge_languages_also_remove_generated_locale_art(self):
        for code, language in (("BPES", "spanish"), ("BPEF", "french"), ("BPEI", "italian")):
            with self.subTest(code=code):
                self.assert_switch_cleans_locale_graphics(code, language)
