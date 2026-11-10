"""Source-only Ruby/Sapphire setup, using a local synthetic Git upstream."""
import contextlib
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
spec = importlib.util.spec_from_file_location("bootstrap_pokeruby_test", ROOT / "tools/bootstrap.py")
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


@unittest.skipUnless(shutil.which("git"), "requires Git")
class PokerubyBootstrapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "port"
        self.root.mkdir()
        self.upstream = Path(self.temp.name) / "upstream"
        self.upstream.mkdir()
        self.git(self.upstream, "init", "-q")
        (self.upstream / "source.txt").write_text("pinned source\n")
        self.git(self.upstream, "add", ".")
        self.git(self.upstream, "commit", "-qm", "pinned")
        self.commit = self.git(self.upstream, "rev-parse", "HEAD")
        (self.upstream / "source.txt").write_text("newer source\n")
        self.git(self.upstream, "commit", "-qam", "newer")
        # An Emerald overlay and an invalid patch must never reach this path.
        for name in ("3ds_port", "builder", "tools", "patches/pokeemerald"):
            folder = self.root / name
            folder.mkdir(parents=True)
            (folder / "sentinel").write_text("Emerald only")
        (self.root / "patches/pokeemerald/0001.patch").write_text("invalid patch")
        repo = json.dumps(str(self.upstream))
        (self.root / "upstream.lock").write_text(
            '[pokeruby]\nrepository = %s\ncommit = "%s"\n' % (repo, self.commit))

    def git(self, tree, *args):
        return subprocess.check_output(
            ["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
             "-c", "commit.gpgSign=false", *args], cwd=tree, text=True).strip()

    def invoke(self, *args):
        output = io.StringIO()
        with patch.object(bootstrap, "ROOT", self.root), \
                patch.object(sys, "argv", ["bootstrap.py", *map(str, args)]), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            result = bootstrap.main()
        return result, output.getvalue()

    def prepare(self, game="ruby"):
        result, output = self.invoke("--game", game)
        self.assertEqual(result, 0)
        self.assertIn("Emerald remains the priority", output)
        return self.root / "build" / ("upstream-" + game)

    def test_both_games_get_separate_pinned_source_only_trees(self):
        for game in ("ruby", "sapphire"):
            tree = self.prepare(game)
            self.assertEqual(self.git(tree, "rev-parse", "HEAD"), self.commit)
            self.assertEqual((tree / "source.txt").read_text(), "pinned source\n")
            self.assertEqual(self.git(tree, "status", "--porcelain"), "")
            for name in ("3ds_port", "builder", "tools", ".emerald3ds-patches", "upstream.lock"):
                self.assertFalse((tree / name).exists(), name)
            self.assertFalse((self.root / "build/upstream").exists())

    def test_custom_destination_and_repeat_need_no_fetch_or_checkout(self):
        tree = self.root / "build/custom-ruby"
        self.invoke("--game", "ruby", "--dir", tree)
        with patch.object(bootstrap, "run") as run:
            self.assertEqual(self.invoke("--game", "ruby", "--dir", tree)[0], 0)
            run.assert_not_called()

    def test_build_cleanup_and_locale_options_fail_before_preparation(self):
        options = (("--make",), ("--clean",), ("--port-lang", "pt_br")) + tuple(
            ("--" + language + "-rom", "unused.gba")
            for language in ("spanish", "french", "italian", "german", "japanese"))
        for game in ("ruby", "sapphire"):
            for option in options:
                with self.subTest(game=game, option=option), \
                        patch.object(bootstrap, "prepare_pokeruby") as prepare:
                    with self.assertRaises(SystemExit) as ctx:
                        self.invoke("--game", game, *option)
                    self.assertEqual(ctx.exception.code, 2)
                    prepare.assert_not_called()
        self.assertFalse((self.root / "build").exists())

    def test_local_changes_and_untracked_work_are_preserved(self):
        tree = self.prepare()
        (tree / "source.txt").write_text("my changes\n")
        (tree / "notes.txt").write_text("my notes\n")
        with self.assertRaises(SystemExit):
            self.invoke("--game", "ruby")
        self.assertEqual((tree / "source.txt").read_text(), "my changes\n")
        self.assertEqual((tree / "notes.txt").read_text(), "my notes\n")

    def test_changed_commit_or_remote_is_not_reset(self):
        tree = self.prepare()
        self.git(tree, "remote", "set-url", "origin", "https://example.invalid/other.git")
        with self.assertRaises(SystemExit):
            self.invoke("--game", "ruby")
        self.assertEqual(self.git(tree, "remote", "get-url", "origin"), "https://example.invalid/other.git")
        self.git(tree, "remote", "set-url", "origin", str(self.upstream))
        self.git(tree, "commit", "--allow-empty", "-qm", "my work")
        changed = self.git(tree, "rev-parse", "HEAD")
        with self.assertRaises(SystemExit):
            self.invoke("--game", "ruby")
        self.assertEqual(self.git(tree, "rev-parse", "HEAD"), changed)

    def test_existing_directory_and_emerald_destinations_are_untouched(self):
        emerald = self.root / "build/upstream"
        emerald.mkdir(parents=True)
        (emerald / "work.txt").write_text("keep")
        for tree in (emerald, emerald / "nested", self.root, self.root.parent):
            with self.subTest(tree=tree), self.assertRaises(SystemExit):
                self.invoke("--game", "ruby", "--dir", tree)
        self.assertEqual((emerald / "work.txt").read_text(), "keep")
        self.assertFalse((emerald / "nested").exists())

    def test_emerald_and_other_game_cannot_reuse_source_checkout(self):
        tree = self.prepare()
        for destination in (tree, tree / "nested"):
            for args in (("--game", "sapphire"), ("--game", "emerald"), ()):
                with self.subTest(args=args, destination=destination), self.assertRaises(SystemExit):
                    self.invoke(*args, "--dir", destination)
        self.assertFalse((tree / "nested").exists())
        self.assertEqual(self.git(tree, "rev-parse", "HEAD"), self.commit)
        self.assertFalse((tree / "3ds_port").exists())

    def test_failed_download_does_not_leave_partial_checkout(self):
        real_run = bootstrap.run
        def fail_fetch(command, cwd=None):
            if command[:2] == ["git", "fetch"]:
                raise subprocess.CalledProcessError(1, command)
            real_run(command, cwd)
        with patch.object(bootstrap, "run", side_effect=fail_fetch):
            with self.assertRaises(subprocess.CalledProcessError):
                self.invoke("--game", "ruby")
        self.assertEqual(list((self.root / "build").iterdir()), [])


if __name__ == "__main__":
    unittest.main()
