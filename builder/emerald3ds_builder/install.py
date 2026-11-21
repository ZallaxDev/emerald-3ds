"""Put the game on an SD card (or any folder laid out like one)."""

from __future__ import annotations

import errno
import hashlib
import json
import re
import os
import shutil
import stat
import string
import sys
import time
import uuid
from contextlib import contextmanager
from pathlib import Path

from .errors import BuilderError

APP_DIR = Path("3ds") / "emerald3ds"

# The player's save, real-time clock and Nuzlocke tracker, in the app folder.
SAVE_NAME = "emerald3ds.sav"
SAVE_FILES = (SAVE_NAME, "emerald3ds.clock", "emerald3ds.clock.tmp", "emerald3ds.clock.bak",
              "emerald3ds.nuzlocke", "emerald3ds.nuzlocke.tmp", "emerald3ds.nuzlocke.bak")
INSTALL_JOURNAL = ".install.json"
INSTALL_LOCK = ".emerald3ds-install.lock"
BACKUP_DIR = "backup"   # <app folder>/backup/<timestamp>/

# The 3DS sound firmware dump that homebrew needs for audio.
DSP_FIRM = Path("3ds") / "dspfirm.cdc"
DSP_WARNING = ("The 3DS sound firmware dump (dspfirm.cdc) was not found at:\n%s\n\n"
               "The game was installed. If this is your 3DS SD card, the game may have no "
               "sound until the firmware is dumped on the console (for example with the "
               "DSP1 homebrew).")


def find_sd_cards() -> list[Path]:
    """Mounted volumes that look like a 3DS SD card (they hold 'Nintendo 3DS').

    Only a suggestion: the builder never installs anywhere without the
    player choosing the destination."""
    candidates: list[Path] = []
    if os.name == "nt":
        roots = [Path("%s:\\" % letter) for letter in string.ascii_uppercase]
    elif sys.platform == "darwin":
        roots = list(Path("/Volumes").glob("*"))
    else:
        roots = [p for base in ("/media", "/run/media") for p in Path(base).glob("*/*")]
    for root in roots:
        try:
            if (root / "Nintendo 3DS").is_dir():
                candidates.append(root)
        except OSError:
            continue
    return candidates


_RETRY_HINT = ("Nothing already on the card was changed. Check that the card is still inserted, "
               "is not write-protected (the lock switch) and has free space, then try again.")


def _check_name(name) -> None:
    """Destination names must be plain file names: no folders, no drive letters."""
    if (not isinstance(name, str) or name in ("", ".", "..") or any(c in name for c in "/\\:\0")
            or Path(name).name != name
            or name.casefold() in {INSTALL_JOURNAL, BACKUP_DIR, *SAVE_FILES}):
        raise BuilderError("Refusing to install a file with an unsafe name: %r" % (name,),
                           code="install_failed")


def _create_temp(dest: Path, name: str) -> tuple[Path, int]:
    """A new, uniquely named file next to `name`; never reuses an existing file."""
    for _ in range(100):
        tmp = dest / ("%s.%s.partial" % (name, uuid.uuid4().hex[:12]))
        try:
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0), 0o666)
        except FileExistsError:
            continue
        return tmp, fd
    raise FileExistsError("could not create a temporary file in %s" % dest)


def _remove(paths) -> None:
    for path in paths:
        try:
            path.unlink()
        except OSError:
            pass


def _stage_file(dest: Path, name: str, source: Path, advance=None) -> tuple[Path, str]:
    """Copy, hash and sync the exact bytes that will be installed."""
    tmp, fd = _create_temp(dest, name)
    digest = hashlib.sha256()
    try:
        with os.fdopen(fd, "wb") as fout, source.open("rb") as fin:
            while chunk := fin.read(1 << 20):
                fout.write(chunk)
                digest.update(chunk)
                if advance is not None:
                    advance(len(chunk))
            fout.flush()
            os.fsync(fout.fileno())
    except BaseException:
        _remove([tmp])
        raise
    return tmp, digest.hexdigest()


def _backup_stamp() -> str:
    """Local time, to the second; uniqueness comes from _make_backup_dir."""
    return time.strftime("%Y%m%d-%H%M%S")


def _existing_saves(dest: Path) -> list[tuple[str, Path, os.stat_result]]:
    """The save files to back up: none unless the save itself exists, then the
    save plus whichever clock and Nuzlocke companions exist (base, .tmp and
    .bak files, as listed in SAVE_FILES). Only reads metadata."""
    found = []
    for name in SAVE_FILES:
        path = dest / name
        try:
            st = path.stat()
        except (FileNotFoundError, NotADirectoryError):
            if name == SAVE_NAME:
                return []   # do not inspect companions without a save
            continue
        except OSError as exc:
            raise BuilderError("The existing save file could not be read:\n%s\n(%s)" % (path, exc),
                               "Nothing was changed on the destination.", code="install_failed") from exc
        if not stat.S_ISREG(st.st_mode):
            raise BuilderError("The existing save file is not a regular file:\n%s" % path,
                               "Nothing was changed on the destination.", code="install_failed")
        found.append((name, path, st))
    return found


def _make_backup_dir(dest: Path) -> tuple[Path, bool]:
    """Reserve <dest>/backup/<stamp>[-N].partial/ exclusively.

    The partial directory reserves its final name among concurrent backups.
    Skip names whose final backup already exists, including empty folders.
    Returns the staging directory and whether backup/ was created here."""
    parent = dest / BACKUP_DIR
    created_parent = False
    try:
        parent.mkdir()
        created_parent = True
    except FileExistsError:
        pass
    stamp = _backup_stamp()
    for n in range(1, 1000):
        final = parent / (stamp if n == 1 else "%s-%d" % (stamp, n))
        folder = final.with_name(final.name + ".partial")
        try:
            folder.mkdir()
        except FileExistsError:
            continue
        except BaseException:
            if created_parent:
                _rmdir(parent)
            raise
        if os.path.lexists(final):
            _rmdir(folder)
            continue
        return folder, created_parent
    if created_parent:
        _rmdir(parent)
    raise FileExistsError("could not create a new backup folder in %s" % parent)


def _rmdir(path: Path) -> None:
    try:
        path.rmdir()   # only ever succeeds on an empty folder
    except OSError:
        pass


def _backup_saves(dest: Path, saves, advance) -> Path:
    """Copy (and sync) every file in `saves` into a new backup folder.

    The originals are only opened for reading. Either every file is copied in
    full before the staging folder is atomically renamed to its final name,
    or cleanup is attempted and the error raised. If cleanup fails, leftovers
    remain visibly marked .partial; other backups are never touched."""
    folder, created_parent = _make_backup_dir(dest)
    written: list[Path] = []
    try:
        for name, src, st in saves:
            target = folder / name
            fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0), 0o666)
            written.append(target)
            copied = 0
            with os.fdopen(fd, "wb") as fout, src.open("rb") as fin:
                while True:
                    chunk = fin.read(1 << 20)
                    if not chunk:
                        break
                    fout.write(chunk)
                    copied += len(chunk)
                    advance(len(chunk))
                fout.flush()
                os.fsync(fout.fileno())
            if copied != st.st_size:
                raise OSError(errno.EIO, "%s changed size while it was being backed up" % name)
            try:
                os.utime(target, ns=(st.st_atime_ns, st.st_mtime_ns))
            except OSError:
                pass   # keeping the original time is a courtesy only
        final = folder.with_name(folder.name.removesuffix(".partial"))
        folder.rename(final)
    except BaseException:
        _remove(written)
        _rmdir(folder)
        if created_parent:
            _rmdir(dest / BACKUP_DIR)
        raise
    return final


def _warn_missing_dsp(sd_root: Path, warn) -> None:
    if warn is None:
        return
    firm = sd_root / DSP_FIRM
    try:
        present = firm.is_file()
    except OSError:
        present = False
    if not present:
        try:
            warn(DSP_WARNING % firm)
        except Exception:
            pass   # the install already succeeded; a broken display must not undo that


@contextmanager
def _install_lock(parent: Path):
    """A process lock released by the OS even if the builder exits mid-install."""
    parent.mkdir(parents=True, exist_ok=True)
    with (parent / INSTALL_LOCK).open("a+b") as lock:
        lock.seek(0)
        if os.name == "nt":
            import msvcrt

            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl

            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def _sync_directory(path: Path) -> None:
    if os.name == "nt":
        return
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    except OSError as exc:
        if exc.errno not in (errno.EINVAL, errno.ENOTSUP):
            raise
    finally:
        os.close(fd)


def _publish_journal(dest: Path, staged) -> None:
    records = [(tmp.name, final.name, digest) for tmp, final, digest in staged]
    tmp, fd = _create_temp(dest, INSTALL_JOURNAL)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump({"version": 1, "files": records}, stream)
            stream.flush()
            os.fsync(stream.fileno())
        tmp.rename(dest / INSTALL_JOURNAL)
        _sync_directory(dest)
    finally:
        _remove([tmp])


def _clear_journal(dest: Path) -> None:
    (dest / INSTALL_JOURNAL).unlink()
    _sync_directory(dest)


def _matches_digest(path: Path, digest: str) -> bool:
    try:
        if not stat.S_ISREG(path.lstat().st_mode):
            return False
        with path.open("rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest() == digest
    except FileNotFoundError:
        return False


def _recover_install(dest: Path, files: dict[str, Path]) -> None:
    journal = dest / INSTALL_JOURNAL
    try:
        with journal.open("rb") as stream:
            data = stream.read(64 * 1024 + 1)
    except FileNotFoundError:
        return
    if len(data) > 64 * 1024:
        raise ValueError("install journal exceeds its size limit")
    payload = json.loads(data)
    if not isinstance(payload, dict) or payload.get("version") != 1 or not isinstance(payload.get("files"), list):
        raise ValueError("unrecognized install journal")
    entries = []
    seen = set()
    for record in payload["files"]:
        if not isinstance(record, list) or len(record) != 3:
            raise ValueError("invalid install journal entry")
        temp, name, digest = record
        _check_name(name)
        if (name.casefold() in seen or not isinstance(temp, str)
                or not re.fullmatch(re.escape(name) + r"\.[0-9a-f]{12}\.partial", temp)
                or not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest)):
            raise ValueError("invalid install journal entry")
        seen.add(name.casefold())
        entries.append((dest / temp, dest / name, digest))

    sources = {name.casefold(): Path(source) for name, source in files.items()}
    pending = []
    repairs = []
    stale = []
    try:
        for staged, final, digest in entries:
            if _matches_digest(staged, digest):
                pending.append((staged, final, None))
            elif _matches_digest(final, digest):
                stale.append(staged)
            else:
                source = sources.get(final.name.casefold())
                if source is None or not source.is_file():
                    raise ValueError("missing a source for the interrupted install: %s" % final.name)
                repaired, actual = _stage_file(dest, final.name, source)
                repairs.append(repaired)
                if actual != digest:
                    raise ValueError("source does not match the interrupted install: %s" % final.name)
                pending.append((staged, final, repaired))
        for staged, final, repaired in pending:
            if repaired is not None:
                # Keep a journal-owned, verified stage if the final rename fails.
                os.replace(repaired, staged)
            os.replace(staged, final)
        _remove(stale)
        _sync_directory(dest)
        _clear_journal(dest)
    finally:
        _remove(repairs)


def install(sd_root: Path, files: dict[str, Path], progress=None, warn=None) -> Path:
    """Stage and sync a complete update, with recovery if file replacement is interrupted."""
    sd_root = Path(sd_root)
    if not sd_root.is_dir():
        raise BuilderError("The destination folder does not exist:\n%s" % sd_root)
    for name in files:
        _check_name(name)
    if len({name.casefold() for name in files}) != len(files):
        raise BuilderError("The update contains duplicate destination names.", code="install_failed")

    dest = sd_root / APP_DIR
    try:
        with _install_lock(dest.parent):
            _recover_install(dest, files)
            if files:
                _install(sd_root, dest, files, progress)
    except (OSError, ValueError, RecursionError) as exc:
        raise BuilderError("Could not prepare or recover the install in %s:\n%s" % (dest, exc),
                           "Check the card and retry with the same release and build options.", code="install_failed") from exc
    _warn_missing_dsp(sd_root, warn)
    return dest


def _install(sd_root: Path, dest: Path, files: dict[str, Path], progress) -> None:
    need = 0
    for src in files.values():
        try:
            st = Path(src).stat()
        except OSError as exc:
            raise BuilderError("A file to install could not be read:\n%s\n(%s)" % (src, exc),
                               "Nothing was changed on the destination.", code="install_failed") from exc
        if not stat.S_ISREG(st.st_mode):
            raise BuilderError("A file to install is not a regular file:\n%s" % src,
                               "Nothing was changed on the destination.", code="install_failed")
        need += st.st_size
    saves = _existing_saves(dest)
    need += sum(st.st_size for _, _, st in saves)   # the backup is written to the same card
    try:
        free = shutil.disk_usage(sd_root).free
    except OSError:
        free = None   # free space unknown: skip this advisory check, the copy still reports errors
    if free is not None and free < need + (4 << 20):
        raise BuilderError("There is not enough free space on the destination (%d MiB needed)."
                           % ((need >> 20) + 4))
    try:
        dest.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise BuilderError("Could not create the folder %s:\n%s" % (dest, exc), _RETRY_HINT,
                           code="install_failed") from exc

    staged: list[tuple[Path, Path, str]] = []
    temps: list[Path] = []
    done = 0

    def advance(count: int) -> None:
        nonlocal done
        done += count
        if progress:
            progress(done / max(need, 1))

    name = None
    try:
        for name, src in files.items():
            tmp, digest = _stage_file(dest, name, Path(src), advance)
            temps.append(tmp)
            staged.append((tmp, dest / name, digest))
    except OSError as exc:
        _remove(temps)
        raise BuilderError("Copying %s to %s failed:\n%s" % (name, dest, exc), _RETRY_HINT,
                           code="install_failed") from exc
    except BaseException:
        _remove(temps)
        raise

    # Back up the save set, complete, before any game file is replaced.
    if saves:
        try:
            _backup_saves(dest, saves, advance)
        except OSError as exc:
            _remove(temps)
            raise BuilderError("Backing up the save in %s failed, so the game was not updated:\n%s"
                               % (dest, exc), _RETRY_HINT, code="install_failed") from exc
        except BaseException:
            _remove(temps)
            raise

    try:
        _publish_journal(dest, staged)
    except BaseException:
        # Once the journal is visible, its stages belong to recovery.
        if not (dest / INSTALL_JOURNAL).exists():
            _remove(temps)
        raise

    committed = 0
    try:
        for tmp, final, _ in staged:
            os.replace(tmp, final)
            committed += 1
        _sync_directory(dest)
        _clear_journal(dest)
    except OSError as exc:
        if committed == 0:
            _clear_journal(dest)
            _remove(temps)
            raise BuilderError("Putting %s in place in %s failed:\n%s" % (staged[0][1].name, dest, exc),
                               _RETRY_HINT, code="install_failed") from exc
        raise BuilderError("The install in %s is incomplete: %d of %d files were updated before "
                           "an error (%s)." % (dest, committed, len(staged), exc),
                           "Run the installer again to finish the interrupted update.", code="install_failed") from exc
