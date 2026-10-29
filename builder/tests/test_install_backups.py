"""Host tests for the save backup and the DSP firmware warning in install().
Synthetic bytes only: no ROM, no game data, no real save.
Run: python -m unittest discover -s builder/tests
"""

import contextlib
import errno
import io
import os
import queue
import sys
import tempfile
import unittest
from collections import namedtuple
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from types import SimpleNamespace
from threading import Barrier
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from emerald3ds_builder import install as installmod  # noqa: E402
from emerald3ds_builder.errors import BuilderError  # noqa: E402
from emerald3ds_builder.install import APP_DIR, BACKUP_DIR, install  # noqa: E402

Usage = namedtuple("Usage", "total used free")
EXE, PAK = "Emerald3DS.3dsx", "emerald3ds.pak"
SAV, CLOCK, CLOCK_BAK = "emerald3ds.sav", "emerald3ds.clock", "emerald3ds.clock.bak"
STAMP = "20261017-120000"


class _Base(unittest.TestCase):
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
        self.pak_src.write_bytes(b"NEW-PAK" * 200000)
        self.files = {EXE: self.exe_src, PAK: self.pak_src}
        self.save_bytes = bytes(range(256)) * 512 * 12   # 1.5 MiB: several backup chunks
        stamp = patch.object(installmod, "_backup_stamp", return_value=STAMP)
        stamp.start()
        self.addCleanup(stamp.stop)

    def tearDown(self):
        self._tmp.cleanup()

    def _old_install(self, sav=True, clock=True, clock_bak=True):
        self.dest.mkdir(parents=True)
        (self.dest / EXE).write_bytes(b"OLD-EXE")
        (self.dest / PAK).write_bytes(b"OLD-PAK")
        if sav:
            (self.dest / SAV).write_bytes(self.save_bytes)
        if clock:
            (self.dest / CLOCK).write_bytes(b"CLOCK-1")
        if clock_bak:
            (self.dest / CLOCK_BAK).write_bytes(b"CLOCK-0")
        return self._snapshot()

    def _snapshot(self):
        """Every file and folder under the app folder, recursively."""
        if not self.dest.exists():
            return {}
        return {p.relative_to(self.dest).as_posix(): (p.read_bytes() if p.is_file() else "<dir>")
                for p in sorted(self.dest.rglob("*"))}

    def _backups(self):
        folder = self.dest / BACKUP_DIR
        return sorted(p.name for p in folder.iterdir()) if folder.exists() else []

    def _with_dsp(self):
        (self.sd / "3ds").mkdir(exist_ok=True)
        (self.sd / "3ds" / "dspfirm.cdc").write_bytes(b"SYNTHETIC-DSP")


class BackupContentTests(_Base):
    def test_full_save_set_is_backed_up_and_originals_untouched(self):
        self._old_install()
        sav_mtime = (self.dest / SAV).stat().st_mtime_ns
        self._with_dsp()
        dest = install(self.sd, self.files)
        self.assertEqual(dest, self.dest)
        self.assertEqual(self._backups(), [STAMP])
        backup = self.dest / BACKUP_DIR / STAMP
        self.assertEqual(sorted(p.name for p in backup.iterdir()), sorted([SAV, CLOCK, CLOCK_BAK]))
        self.assertEqual((backup / SAV).read_bytes(), self.save_bytes)
        self.assertEqual((backup / CLOCK).read_bytes(), b"CLOCK-1")
        self.assertEqual((backup / CLOCK_BAK).read_bytes(), b"CLOCK-0")
        # The originals stay where they are, unchanged.
        self.assertEqual((self.dest / SAV).read_bytes(), self.save_bytes)
        self.assertEqual((self.dest / SAV).stat().st_mtime_ns, sav_mtime)
        self.assertEqual((self.dest / CLOCK).read_bytes(), b"CLOCK-1")
        self.assertEqual((self.dest / CLOCK_BAK).read_bytes(), b"CLOCK-0")
        # And the game itself was updated.
        self.assertEqual((self.dest / EXE).read_bytes(), self.exe_src.read_bytes())
        self.assertEqual((self.dest / PAK).read_bytes(), self.pak_src.read_bytes())
        self.assertFalse([p for p in self.dest.iterdir() if p.name.endswith(".partial")])

    def test_all_optional_companions_are_backed_up_verbatim_and_originals_untouched(self):
        self._old_install()
        companions = {
            "emerald3ds.clock": b"CLOCK-1",
            "emerald3ds.clock.tmp": b"RECOVERABLE-CLOCK\x00\xff",
            "emerald3ds.clock.bak": b"CLOCK-0",
            "emerald3ds.nuzlocke": b"TRACKER\x00\x01",
            "emerald3ds.nuzlocke.tmp": b"RECOVERABLE-TRACKER\xfe",
            "emerald3ds.nuzlocke.bak": b"OLD-TRACKER\xff",
        }
        originals = {SAV: self.save_bytes, **companions}
        for name, data in originals.items():
            (self.dest / name).write_bytes(data)
        mtimes = {name: (self.dest / name).stat().st_mtime_ns for name in originals}
        real_open = Path.open

        def open_file(path, mode="r", *args, **kwargs):
            if Path(path).parent == self.dest and Path(path).name in originals:
                self.assertEqual(mode, "rb")
            return real_open(path, mode, *args, **kwargs)

        with patch.object(Path, "open", open_file):
            install(self.sd, self.files)
        backup = self.dest / BACKUP_DIR / STAMP
        self.assertEqual(sorted(p.name for p in backup.iterdir()), sorted(originals))
        for name, data in originals.items():
            self.assertEqual((backup / name).read_bytes(), data)
            self.assertEqual((self.dest / name).read_bytes(), data)
            self.assertEqual((self.dest / name).stat().st_mtime_ns, mtimes[name])

    def test_missing_all_optional_companions_still_backs_up_the_save(self):
        self._old_install(clock=False, clock_bak=False)
        install(self.sd, self.files)
        backup = self.dest / BACKUP_DIR / STAMP
        self.assertEqual([p.name for p in backup.iterdir()], [SAV])
        self.assertEqual((backup / SAV).read_bytes(), self.save_bytes)
        self.assertEqual((self.dest / SAV).read_bytes(), self.save_bytes)

    def test_only_existing_clock_companions_are_backed_up(self):
        self._old_install(clock=False)
        install(self.sd, self.files)
        backup = self.dest / BACKUP_DIR / STAMP
        self.assertEqual(sorted(p.name for p in backup.iterdir()), sorted([SAV, CLOCK_BAK]))

    def test_no_save_means_no_backup_and_clocks_are_kept(self):
        self._old_install(sav=False)
        install(self.sd, self.files)
        self.assertFalse((self.dest / BACKUP_DIR).exists())
        self.assertEqual((self.dest / CLOCK).read_bytes(), b"CLOCK-1")
        self.assertEqual((self.dest / CLOCK_BAK).read_bytes(), b"CLOCK-0")

    def test_no_save_skips_invalid_clock_companions(self):
        self._old_install(sav=False, clock=False)
        (self.dest / CLOCK).mkdir()
        real_stat = Path.stat

        def stat(path, *args, **kwargs):
            if Path(path) in (self.dest / CLOCK, self.dest / CLOCK_BAK):
                raise AssertionError("clock companions must not be inspected without a save")
            return real_stat(path, *args, **kwargs)

        with patch.object(Path, "stat", stat):
            install(self.sd, self.files)
        self.assertTrue((self.dest / CLOCK).is_dir())
        self.assertEqual((self.dest / CLOCK_BAK).read_bytes(), b"CLOCK-0")
        self.assertFalse((self.dest / BACKUP_DIR).exists())
        self.assertEqual((self.dest / EXE).read_bytes(), self.exe_src.read_bytes())

    def test_partial_reservations_and_existing_empty_backup_are_not_reused(self):
        self._old_install()
        older = self.dest / BACKUP_DIR / STAMP
        older.mkdir(parents=True)
        first, _ = installmod._make_backup_dir(self.dest)
        second, _ = installmod._make_backup_dir(self.dest)
        self.assertEqual(first.name, STAMP + "-2.partial")
        self.assertEqual(second.name, STAMP + "-3.partial")
        install(self.sd, self.files)
        self.assertEqual(self._backups(), [STAMP, STAMP + "-2.partial", STAMP + "-3.partial", STAMP + "-4"])
        self.assertEqual(list(older.iterdir()), [])
        self.assertEqual((self.dest / BACKUP_DIR / (STAMP + "-4") / SAV).read_bytes(), self.save_bytes)

    def test_concurrent_backups_have_unique_complete_final_folders(self):
        self._old_install()
        saves = installmod._existing_saves(self.dest)
        ready = Barrier(4)

        def backup():
            ready.wait(timeout=10)
            return installmod._backup_saves(self.dest, saves, lambda count: None)

        with ThreadPoolExecutor(max_workers=4) as workers:
            futures = [workers.submit(backup) for _ in range(4)]
            folders = [future.result(timeout=10) for future in futures]
        self.assertEqual(len(set(folders)), 4)
        self.assertEqual(self._backups(), [STAMP, STAMP + "-2", STAMP + "-3", STAMP + "-4"])
        for folder in folders:
            self.assertEqual((folder / SAV).read_bytes(), self.save_bytes)
            self.assertEqual((folder / CLOCK).read_bytes(), b"CLOCK-1")
            self.assertEqual((folder / CLOCK_BAK).read_bytes(), b"CLOCK-0")

    def test_first_install_makes_no_backup(self):
        install(self.sd, self.files)
        self.assertFalse((self.dest / BACKUP_DIR).exists())

    def test_backups_are_unique_and_older_ones_are_kept(self):
        self._old_install()
        older = self.dest / BACKUP_DIR / "20200101-000000"
        older.mkdir(parents=True)
        (older / SAV).write_bytes(b"ANCIENT")
        for _ in range(3):
            install(self.sd, self.files)   # same timestamp every time
        self.assertEqual(self._backups(), ["20200101-000000", STAMP, STAMP + "-2", STAMP + "-3"])
        self.assertEqual((older / SAV).read_bytes(), b"ANCIENT")
        for name in (STAMP, STAMP + "-2", STAMP + "-3"):
            self.assertEqual((self.dest / BACKUP_DIR / name / SAV).read_bytes(), self.save_bytes)

    def test_progress_covers_backup_and_ends_at_one(self):
        self._old_install()
        seen = []
        install(self.sd, self.files, seen.append)
        self.assertEqual(seen, sorted(seen))
        self.assertEqual(seen[-1], 1.0)


class BackupFailureTests(_Base):
    def _assert_game_untouched(self, before):
        self.assertEqual(self._snapshot(), before)

    def test_read_failure_on_save_keeps_everything_and_replaces_nothing(self):
        before = self._old_install()
        real_open = Path.open
        save = self.dest / SAV
        replaced = []
        real_replace = os.replace

        def fake_open(path, *args, **kwargs):
            if Path(path) == save:
                raise OSError(errno.EIO, "Input/output error (injected)")
            return real_open(path, *args, **kwargs)

        def replace(a, b):
            replaced.append(Path(b).name)
            return real_replace(a, b)

        with patch.object(Path, "open", fake_open), patch.object(installmod.os, "replace", replace):
            with self.assertRaises(BuilderError) as ctx:
                install(self.sd, self.files)
        self.assertEqual(ctx.exception.code, "install_failed")
        self.assertIn("Backing up the save", str(ctx.exception))
        self.assertEqual(replaced, [])
        self._assert_game_untouched(before)   # no backup folder, no temps left

    def test_write_failure_midway_removes_own_backup_but_keeps_older(self):
        self._old_install()
        older = self.dest / BACKUP_DIR / "20200101-000000"
        older.mkdir(parents=True)
        (older / SAV).write_bytes(b"ANCIENT")
        before = self._snapshot()
        real_fsync = os.fsync
        calls = []

        def fsync(fd):
            calls.append(fd)
            if len(calls) == 4:   # exe, pak, save synced; the clock hits a full card
                raise OSError(errno.ENOSPC, "No space left on device (injected)")
            return real_fsync(fd)

        with patch.object(installmod.os, "fsync", fsync):
            with self.assertRaises(BuilderError) as ctx:
                install(self.sd, self.files)
        self.assertEqual(len(calls), 4)
        self.assertIn("Nothing already on the card was changed", str(ctx.exception))
        self._assert_game_untouched(before)

    def test_copy_and_cleanup_failure_leave_only_a_marked_partial_backup(self):
        self._old_install()
        older = self.dest / BACKUP_DIR / "20200101-000000"
        older.mkdir(parents=True)
        (older / SAV).write_bytes(b"ANCIENT")
        real_open, real_unlink, real_rmdir = Path.open, Path.unlink, Path.rmdir
        save = self.dest / SAV

        class FailingRead:
            def __init__(self, stream):
                self.stream = stream
                self.reads = 0

            def __enter__(self):
                return self

            def __exit__(self, *args):
                self.stream.close()

            def read(self, size):
                self.reads += 1
                if self.reads == 2:
                    raise OSError(errno.EIO, "card I/O failed (injected)")
                return self.stream.read(size)

        def open_file(path, *args, **kwargs):
            stream = real_open(path, *args, **kwargs)
            return FailingRead(stream) if Path(path) == save else stream

        def unlink(path, *args, **kwargs):
            if Path(path).parent.name.endswith(".partial"):
                raise OSError(errno.EROFS, "cleanup failed (injected)")
            return real_unlink(path, *args, **kwargs)

        def rmdir(path, *args, **kwargs):
            if Path(path).name.endswith(".partial"):
                raise OSError(errno.EROFS, "cleanup failed (injected)")
            return real_rmdir(path, *args, **kwargs)

        with patch.object(Path, "open", open_file), patch.object(Path, "unlink", unlink), \
                patch.object(Path, "rmdir", rmdir):
            with self.assertRaises(BuilderError):
                install(self.sd, self.files)
        self.assertEqual(self._backups(), ["20200101-000000", STAMP + ".partial"])
        partial = self.dest / BACKUP_DIR / (STAMP + ".partial") / SAV
        self.assertEqual(partial.read_bytes(), self.save_bytes[:1 << 20])
        self.assertEqual((older / SAV).read_bytes(), b"ANCIENT")
        self.assertEqual((self.dest / SAV).read_bytes(), self.save_bytes)
        self.assertEqual((self.dest / EXE).read_bytes(), b"OLD-EXE")
        self.assertEqual((self.dest / PAK).read_bytes(), b"OLD-PAK")
        self.assertFalse(list(self.dest.glob("*.partial")))
        # A later successful install must preserve the failed attempt too.
        install(self.sd, self.files)
        self.assertEqual(self._backups(), ["20200101-000000", STAMP + "-2", STAMP + ".partial"])
        self.assertEqual(partial.read_bytes(), self.save_bytes[:1 << 20])

    def test_failed_backup_promotion_aborts_before_game_replacement(self):
        before = self._old_install()
        with patch.object(Path, "rename", side_effect=OSError(errno.EIO, "rename failed (injected)")):
            with self.assertRaises(BuilderError):
                install(self.sd, self.files)
        self._assert_game_untouched(before)

    def test_backup_folder_creation_failure_keeps_everything(self):
        before = self._old_install()
        real_mkdir = Path.mkdir

        def mkdir(path, *args, **kwargs):
            if Path(path).parent.name == BACKUP_DIR:
                raise OSError(errno.EROFS, "Read-only file system (injected)")
            return real_mkdir(path, *args, **kwargs)

        with patch.object(Path, "mkdir", mkdir):
            with self.assertRaises(BuilderError) as ctx:
                install(self.sd, self.files)
        self.assertEqual(ctx.exception.code, "install_failed")
        self._assert_game_untouched(before)   # the backup/ folder it made is removed again

    def test_save_changing_size_during_backup_is_refused(self):
        self._old_install()
        real = installmod._existing_saves

        def existing(dest):
            found = real(dest)
            (dest / SAV).write_bytes(self.save_bytes + b"GREW")   # the size seen earlier is now wrong
            return found

        with patch.object(installmod, "_existing_saves", existing):
            with self.assertRaises(BuilderError) as ctx:
                install(self.sd, self.files)
        self.assertIn("changed size", str(ctx.exception))
        self.assertEqual((self.dest / EXE).read_bytes(), b"OLD-EXE")
        self.assertEqual((self.dest / PAK).read_bytes(), b"OLD-PAK")
        self.assertFalse((self.dest / BACKUP_DIR).exists())

    def test_interrupt_during_backup_cleans_up_and_is_not_wrapped(self):
        before = self._old_install()
        real_fsync = os.fsync
        calls = []

        def fsync(fd):
            calls.append(fd)
            if len(calls) == 3:
                raise KeyboardInterrupt
            return real_fsync(fd)

        with patch.object(installmod.os, "fsync", fsync):
            with self.assertRaises(KeyboardInterrupt):
                install(self.sd, self.files)
        self._assert_game_untouched(before)

    def test_save_that_is_not_a_file_is_refused_before_writing(self):
        self._old_install(sav=False)
        (self.dest / SAV).mkdir()
        before = self._snapshot()
        with self.assertRaises(BuilderError) as ctx:
            install(self.sd, self.files)
        self.assertIn("not a regular file", str(ctx.exception))
        self._assert_game_untouched(before)


class FreeSpaceTests(_Base):
    def _need(self, with_backup):
        n = self.exe_src.stat().st_size + self.pak_src.stat().st_size
        if with_backup:
            n += len(self.save_bytes) + len(b"CLOCK-1") + len(b"CLOCK-0")
        return n + (4 << 20)

    def test_backup_bytes_count_toward_free_space(self):
        before = self._old_install()
        free = self._need(with_backup=True) - 1
        self.assertGreater(free, self._need(with_backup=False))   # would fit without the backup
        with patch.object(installmod.shutil, "disk_usage", return_value=Usage(1 << 34, 0, free)):
            with self.assertRaises(BuilderError) as ctx:
                install(self.sd, self.files)
        self.assertIn("not enough free space", str(ctx.exception))
        self.assertEqual(self._snapshot(), before)

    def test_exactly_enough_space_including_backup_installs(self):
        self._old_install()
        free = self._need(with_backup=True)
        with patch.object(installmod.shutil, "disk_usage", return_value=Usage(1 << 34, 0, free)):
            install(self.sd, self.files)
        self.assertEqual(self._backups(), [STAMP])
        self.assertEqual((self.dest / PAK).read_bytes(), self.pak_src.read_bytes())


class DspWarningTests(_Base):
    def test_missing_dsp_firmware_warns_but_installs(self):
        warnings = []
        dest = install(self.sd, self.files, warn=warnings.append)
        self.assertEqual(dest, self.dest)
        self.assertEqual(len(warnings), 1)
        self.assertIn("dspfirm.cdc", warnings[0])
        self.assertEqual((dest / PAK).read_bytes(), self.pak_src.read_bytes())

    def test_present_dsp_firmware_gives_no_warning(self):
        self._with_dsp()
        warnings = []
        install(self.sd, self.files, warn=warnings.append)
        self.assertEqual(warnings, [])

    def test_no_callback_keeps_the_old_signature_working(self):
        dest = install(self.sd, self.files, None)
        self.assertEqual(dest, self.dest)

    def test_broken_callback_does_not_fail_the_install(self):
        dest = install(self.sd, self.files, warn=Mock(side_effect=RuntimeError("display gone")))
        self.assertEqual((dest / EXE).read_bytes(), self.exe_src.read_bytes())

    def test_failed_install_does_not_warn(self):
        warnings = []
        with patch.object(installmod.shutil, "disk_usage", return_value=Usage(1 << 30, 1 << 30, 1 << 20)):
            with self.assertRaises(BuilderError):
                install(self.sd, self.files, warn=warnings.append)
        self.assertEqual(warnings, [])


class FrontEndWarningTests(_Base):
    """The warning reaches the CLI's stderr and the window's event queue."""

    def _fake_build(self):
        exe = self.exe_src

        def build(rom, payload, pak_path, progress, *rest, need_executables=True, language=None):
            Path(pak_path).write_bytes(b"SYNTHETIC-PAK")
            return {"payload": SimpleNamespace(executables=lambda: [exe])}
        return build

    def test_cli_prints_warning_and_succeeds(self):
        try:
            from emerald3ds_builder import cli
        except ImportError as exc:   # optional build dependencies not installed here
            self.skipTest("cli unavailable: %s" % exc)
        args = SimpleNamespace(payload=None, rom="synthetic.gba", sd=str(self.sd))
        out, err = io.StringIO(), io.StringIO()
        with patch.object(cli, "Payload", Mock()), patch.object(cli, "build_pack", self._fake_build()), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertEqual(cli.cmd_install(args), 0)
        self.assertIn("Warning:", err.getvalue())
        self.assertIn("dspfirm.cdc", err.getvalue())
        self.assertIn("Installed to %s" % self.dest, out.getvalue())

    def test_gui_worker_queues_warning_before_done(self):
        try:
            from emerald3ds_builder import gui
        except ImportError as exc:   # no tkinter here
            self.skipTest("gui unavailable: %s" % exc)
        fake = SimpleNamespace(events=queue.Queue(), payload=Mock())
        with patch.object(gui, "build_pack", self._fake_build()):
            gui.App.work(fake, Path("synthetic.gba"), self.sd, None)
        kinds = []
        while not fake.events.empty():
            kinds.append(fake.events.get_nowait())
        names = [k[0] for k in kinds if k[0] != "progress"]
        self.assertEqual(names, ["warning", "done"])
        self.assertIn("dspfirm.cdc", [k for k in kinds if k[0] == "warning"][0][1])

    def test_gui_poll_shows_warning_with_result_on_main_thread(self):
        try:
            from emerald3ds_builder import gui
        except ImportError as exc:
            self.skipTest("gui unavailable: %s" % exc)
        # Like the real App: poll() reschedules itself through root.after(100, self.poll).
        fake = SimpleNamespace(events=queue.Queue(), warnings=[], finish=Mock(), root=Mock(), poll=Mock())
        fake.events.put(("warning", "synthetic warning"))
        fake.events.put(("done", str(self.dest)))
        with patch.object(gui, "messagebox") as box:
            gui.App.poll(fake)
        fake.root.after.assert_called_once_with(100, fake.poll)
        fake.finish.assert_called_once_with()
        box.showinfo.assert_not_called()
        box.showwarning.assert_called_once()
        self.assertIn("synthetic warning", box.showwarning.call_args[0][1])
        self.assertEqual(fake.warnings, [])


if __name__ == "__main__":
    unittest.main()
