"""Host tests for install() failure handling. Synthetic bytes only: no ROM,
no game data. Run: python -m unittest discover builder/tests
"""

import errno
import json
import os
import sys
import tempfile
import unittest
from collections import namedtuple
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from emerald3ds_builder import install as installmod  # noqa: E402
from emerald3ds_builder.errors import BuilderError  # noqa: E402
from emerald3ds_builder.install import APP_DIR, install  # noqa: E402

Usage = namedtuple("Usage", "total used free")
EXE, PAK = "Emerald3DS.3dsx", "emerald3ds.pak"


class _FailingReader:
    """A source file whose reads fail like a bad sector or a vanished file."""

    def __init__(self, f):
        self.f = f

    def read(self, *args):
        raise OSError(errno.EIO, "Input/output error (injected)")

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.f.close()
        return False


class InstallFailureTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.sd = self.root / "sd"
        self.sd.mkdir()
        self.dest = self.sd / APP_DIR
        src = self.root / "src"
        src.mkdir()
        self.exe_src = src / EXE
        self.exe_src.write_bytes(b"NEW-EXE" * 300)
        self.pak_src = src / PAK
        self.pak_src.write_bytes(b"NEW-PAK" * 200000)   # > 1 MiB chunk: several progress steps
        self.files = {EXE: self.exe_src, PAK: self.pak_src}

    def tearDown(self):
        self._tmp.cleanup()

    def _old_install(self):
        """An earlier install plus a stray .partial file someone else left behind."""
        self.dest.mkdir(parents=True)
        (self.dest / EXE).write_bytes(b"OLD-EXE")
        (self.dest / PAK).write_bytes(b"OLD-PAK")
        (self.dest / (PAK + ".partial")).write_bytes(b"NOT-OURS")
        (self.dest / (EXE + ".partial")).write_bytes(b"NOT-OURS")
        return self._snapshot()

    def _snapshot(self):
        return {p.name: p.read_bytes() for p in self.dest.iterdir()}

    def _assert_untouched(self, before):
        self.assertEqual(self._snapshot(), before)

    # -- copy failures after the executable was already staged ----------------
    def test_read_failure_on_pak_keeps_old_files_and_removes_own_temps(self):
        before = self._old_install()
        real_open = Path.open
        opened = []

        def fake_open(path, *args, **kwargs):
            f = real_open(path, *args, **kwargs)
            opened.append(Path(path).name)
            return _FailingReader(f) if Path(path) == self.pak_src else f

        with patch.object(Path, "open", fake_open):
            with self.assertRaises(BuilderError) as ctx:
                install(self.sd, self.files)
        self.assertEqual([name for name in opened if name != installmod.INSTALL_LOCK], [EXE, PAK])   # the executable was copied first
        self.assertEqual(ctx.exception.code, "install_failed")
        self.assertIn(PAK, str(ctx.exception))
        self._assert_untouched(before)

    def test_write_failure_on_pak_keeps_old_files_and_removes_own_temps(self):
        before = self._old_install()
        real_fsync = os.fsync
        calls = []

        def fsync(fd):
            calls.append(fd)
            if len(calls) == 2:   # the executable synced fine; the pak hits a full card
                raise OSError(errno.ENOSPC, "No space left on device (injected)")
            return real_fsync(fd)

        with patch.object(installmod.os, "fsync", fsync):
            with self.assertRaises(BuilderError) as ctx:
                install(self.sd, self.files)
        self.assertEqual(len(calls), 2)
        self.assertEqual(ctx.exception.code, "install_failed")
        self.assertIn("Nothing already on the card was changed", str(ctx.exception))
        self._assert_untouched(before)

    def test_failure_on_first_install_leaves_no_partial_files(self):
        with patch.object(installmod.os, "fsync", side_effect=OSError(errno.EIO, "injected")):
            with self.assertRaises(BuilderError):
                install(self.sd, self.files)
        self.assertEqual(list(self.dest.iterdir()), [])

    def test_interrupt_during_copy_removes_own_temps_and_is_not_wrapped(self):
        before = self._old_install()
        with patch.object(installmod.os, "fsync", side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                install(self.sd, self.files)
        self._assert_untouched(before)

    def test_rename_failure_recovers_on_next_install(self):
        before = self._old_install()
        real_replace = os.replace
        calls = []

        def replace(a, b):
            calls.append(Path(b).name)
            if len(calls) == 2:
                raise OSError(errno.EIO, "injected")
            return real_replace(a, b)

        with patch.object(installmod.os, "replace", replace):
            with self.assertRaises(BuilderError) as ctx:
                install(self.sd, self.files)
        self.assertEqual(ctx.exception.code, "install_failed")
        self.assertIn("incomplete", str(ctx.exception))
        after = self._snapshot()
        # Honest about what happened: the executable was replaced, the pak was not.
        self.assertEqual(after[EXE], self.exe_src.read_bytes())
        self.assertEqual(after[PAK], b"OLD-PAK")
        self.assertIn(installmod.INSTALL_JOURNAL, after)
        self.assertEqual(install(self.sd, {}), self.dest)
        recovered = self._snapshot()
        self.assertEqual(recovered[EXE], self.exe_src.read_bytes())
        self.assertEqual(recovered[PAK], self.pak_src.read_bytes())
        self.assertEqual(sorted(recovered), sorted(before))

    def test_retry_repairs_missing_or_corrupted_stage_and_preserves_saves(self):
        for damaged in (None, b"CORRUPTED-STAGE"):
            with self.subTest(damaged=damaged), tempfile.TemporaryDirectory(dir=self.root) as folder:
                sd = Path(folder)
                dest = sd / APP_DIR
                dest.mkdir(parents=True)
                (dest / EXE).write_bytes(b"OLD-EXE")
                (dest / PAK).write_bytes(b"OLD-PAK")
                saves = {name: f"player data for {name}".encode() for name in installmod.SAVE_FILES}
                for name, data in saves.items():
                    (dest / name).write_bytes(data)
                old_backup = dest / installmod.BACKUP_DIR / "existing" / installmod.SAVE_NAME
                old_backup.parent.mkdir(parents=True)
                old_backup.write_bytes(b"PREVIOUS-BACKUP")
                real_replace = os.replace

                def interrupt(source, target):
                    if Path(target) == dest / PAK:
                        raise OSError(errno.EIO, "injected interruption")
                    return real_replace(source, target)

                with patch.object(installmod.os, "replace", interrupt):
                    with self.assertRaisesRegex(BuilderError, "incomplete"):
                        install(sd, self.files)
                self.assertEqual((dest / EXE).read_bytes(), self.exe_src.read_bytes())
                self.assertEqual((dest / PAK).read_bytes(), b"OLD-PAK")
                backups = {path: path.read_bytes()
                           for path in (dest / installmod.BACKUP_DIR).rglob("*") if path.is_file()}
                self.assertEqual(len(backups), len(saves) + 1)
                journal = dest / installmod.INSTALL_JOURNAL
                journal_bytes = journal.read_bytes()
                (temp, _, _), = [row for row in json.loads(journal_bytes)["files"] if row[1] == PAK]
                stage = dest / temp
                if damaged is None:
                    stage.unlink()
                else:
                    stage.write_bytes(damaged)
                wrong = sd / "wrong.pak"
                wrong.write_bytes(b"DIFFERENT-UPDATE")
                before_retry = {path: path.read_bytes() for path in dest.rglob("*") if path.is_file()}
                with self.assertRaises(BuilderError):
                    install(sd, {EXE: self.exe_src, PAK: wrong})
                self.assertEqual({path: path.read_bytes() for path in dest.rglob("*") if path.is_file()},
                                 before_retry)

                self.assertEqual(install(sd, self.files), dest)
                self.assertEqual((dest / EXE).read_bytes(), self.exe_src.read_bytes())
                self.assertEqual((dest / PAK).read_bytes(), self.pak_src.read_bytes())
                self.assertFalse(journal.exists())
                self.assertEqual(list(dest.glob("*.partial")), [])
                self.assertEqual({name: (dest / name).read_bytes() for name in saves}, saves)
                self.assertEqual({path: path.read_bytes() for path in backups}, backups)

    def test_concurrent_attempt_does_not_touch_the_install(self):
        before = self._old_install()
        with installmod._install_lock(self.dest.parent):
            with self.assertRaises(BuilderError):
                install(self.sd, self.files)
        self._assert_untouched(before)

    def test_first_rename_failure_keeps_old_files_and_says_nothing_changed(self):
        before = self._old_install()
        calls = []

        def replace(a, b):
            calls.append(Path(b).name)
            raise OSError(errno.EACCES, "injected")

        with patch.object(installmod.os, "replace", replace):
            with self.assertRaises(BuilderError) as ctx:
                install(self.sd, self.files)
        self.assertEqual(calls, [EXE])
        self.assertEqual(ctx.exception.code, "install_failed")
        message = str(ctx.exception)
        self.assertIn("Nothing already on the card was changed", message)
        self.assertIn(EXE, message)
        self.assertNotIn("incomplete", message)
        self.assertNotIn("may not start", message)
        self._assert_untouched(before)   # old files kept, own temps removed

    # -- preflight -------------------------------------------------------------
    def test_missing_source_is_reported_before_touching_the_card(self):
        before = self._old_install()
        files = dict(self.files)
        files[PAK] = self.root / "src" / "missing.pak"
        with self.assertRaises(BuilderError) as ctx:
            install(self.sd, files)
        self.assertEqual(ctx.exception.code, "install_failed")
        self.assertIn("missing.pak", str(ctx.exception))
        self._assert_untouched(before)

    def test_folder_as_source_is_refused(self):
        files = dict(self.files)
        files[PAK] = self.root / "src"
        with self.assertRaises(BuilderError) as ctx:
            install(self.sd, files)
        self.assertEqual(ctx.exception.code, "install_failed")
        self.assertFalse(self.dest.exists())

    def test_missing_destination_folder(self):
        with self.assertRaises(BuilderError) as ctx:
            install(self.root / "no-such-card", self.files)
        self.assertIn("does not exist", str(ctx.exception))
        self.assertFalse((self.root / "no-such-card").exists())

    def test_unsafe_names_are_refused(self):
        for bad in ("../escape.pak", "sub/x.pak", "sub\\x.pak", "C:x.pak", "", ".", "..", "/abs.pak"):
            with self.assertRaises(BuilderError, msg=bad) as ctx:
                install(self.sd, {bad: self.exe_src})
            self.assertEqual(ctx.exception.code, "install_failed")
        self.assertFalse(self.dest.exists())
        self.assertFalse((self.sd / "3ds" / "escape.pak").exists())

    # -- free space ------------------------------------------------------------
    def test_unknown_free_space_skips_the_check(self):
        with patch.object(installmod.shutil, "disk_usage", side_effect=OSError(errno.ENOSYS, "x")):
            dest = install(self.sd, self.files)
        self.assertEqual((dest / PAK).read_bytes(), self.pak_src.read_bytes())
        self.assertEqual((dest / EXE).read_bytes(), self.exe_src.read_bytes())

    def test_low_space_is_refused_before_writing(self):
        before = self._old_install()
        with patch.object(installmod.shutil, "disk_usage", return_value=Usage(1 << 30, 1 << 30, 1 << 20)):
            with self.assertRaises(BuilderError) as ctx:
                install(self.sd, self.files)
        self.assertIn("not enough free space", str(ctx.exception))
        self._assert_untouched(before)

    def test_adequate_space_installs_and_reports_progress(self):
        before = self._old_install()
        seen = []
        with patch.object(installmod.shutil, "disk_usage", return_value=Usage(1 << 34, 0, 1 << 33)):
            dest = install(self.sd, self.files, seen.append)
        self.assertEqual(dest, self.dest)
        self.assertEqual((dest / EXE).read_bytes(), self.exe_src.read_bytes())
        self.assertEqual((dest / PAK).read_bytes(), self.pak_src.read_bytes())
        # Someone else's .partial files are never deleted or overwritten.
        self.assertEqual((dest / (PAK + ".partial")).read_bytes(), b"NOT-OURS")
        self.assertEqual(sorted(p.name for p in dest.iterdir()), sorted(before))
        self.assertGreater(len(seen), 2)
        self.assertEqual(seen, sorted(seen))
        self.assertEqual(seen[-1], 1.0)


if __name__ == "__main__":
    unittest.main()
