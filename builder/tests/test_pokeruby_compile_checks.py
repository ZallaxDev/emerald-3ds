"""Guard the experimental compile check against host tools and wrong sources."""
import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("pokeruby_compile_check", ROOT / "tools/check_pokeruby_3ds.py")
checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checks)


class CompilerSelectionTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("gcc"), "requires a host GCC")
    def test_host_compiler_cannot_count_as_3ds_verification(self):
        # Actual compiler target query, not a mocked successful compile.
        with self.assertRaisesRegex(ValueError, "arm-none-eabi"):
            checks.compiler_path("gcc")


@unittest.skipUnless(shutil.which("git"), "requires Git")
class SourceSelectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.tree = Path(self.temp.name).resolve()
        self.git("init", "-q")
        self.repo = "https://example.invalid/source.git"
        self.git("remote", "add", "origin", self.repo)
        (self.tree / "source.txt").write_text("original\n")
        self.git("add", ".")
        self.git("commit", "-qm", "pinned")
        self.commit = self.git("rev-parse", "HEAD")

    def git(self, *args):
        return subprocess.check_output(
            ["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
             "-c", "commit.gpgSign=false", *args], cwd=self.tree, text=True).strip()

    def test_clean_pinned_source_accepts_url_rewrites(self):
        self.git("config", "url.https://example.invalid/mirror.git.insteadOf", self.repo)
        checks.check_source(self.tree, self.repo, self.commit)
        self.assertEqual(self.git("status", "--porcelain"), "")

    def test_reject_changed_commit_and_origin_without_resetting(self):
        self.git("commit", "--allow-empty", "-qm", "my work")
        changed = self.git("rev-parse", "HEAD")
        with self.assertRaisesRegex(ValueError, "commit"):
            checks.check_source(self.tree, self.repo, self.commit)
        self.assertEqual(self.git("rev-parse", "HEAD"), changed)
        self.git("remote", "set-url", "origin", "https://example.invalid/other.git")
        with self.assertRaisesRegex(ValueError, "origin"):
            checks.check_source(self.tree, self.repo, changed)

    def test_reject_dirty_and_nested_sources_without_modification(self):
        (self.tree / "source.txt").write_text("my edits\n")
        (self.tree / "notes.txt").write_text("my notes\n")
        with self.assertRaisesRegex(ValueError, "clean"):
            checks.check_source(self.tree, self.repo, self.commit)
        nested = self.tree / "nested"
        nested.mkdir()
        with self.assertRaisesRegex(ValueError, "separate"):
            checks.check_source(nested, self.repo, self.commit)
        self.assertEqual((self.tree / "source.txt").read_text(), "my edits\n")
        self.assertEqual((self.tree / "notes.txt").read_text(), "my notes\n")


if __name__ == "__main__":
    unittest.main()
