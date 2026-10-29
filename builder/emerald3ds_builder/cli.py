"""Command line: the same build the window runs, for scripts, CI and Linux/macOS.

    emerald3ds-builder build   --rom ROM --output DIR [--language pt]
    emerald3ds-builder install --rom ROM --sd E:\\ [--language pt]
    emerald3ds-builder verify  --pak FILE
    emerald3ds-builder detect
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

from . import __version__, followers, pak
from .build import Payload, build_pack, default_payload, select_payload
from .errors import BuilderError
from .install import APP_DIR, find_sd_cards, install
from .recipe import Recipe
from .rom import load_rom


LANGUAGE_HELP = ("game language, when the ROM makes more than one: pt builds the Portuguese "
                 "translation from the English ROM (default: the ROM's own language)")


def _progress(fraction: float, message: str) -> None:
    sys.stdout.write("\r[%3d%%] %-72s" % (int(fraction * 100), message))
    sys.stdout.flush()
    if fraction >= 1.0:
        sys.stdout.write("\n")


def _warn(message: str) -> None:
    """A non-fatal warning: shown, but the command still succeeds."""
    print("Warning: %s" % message, file=sys.stderr)


def cmd_build(args) -> int:
    payload = Payload(args.payload)
    out_dir = Path(args.output) / APP_DIR
    # Pick the ROM's variant before the output folder is touched. A language
    # subfolder must bring its own executables (the English ones would not run
    # its pack), so a release missing them leaves earlier files alone. The
    # default variant may stay recipe-only, as before: its missing executables
    # are skipped below.
    rom = load_rom(Path(args.rom))
    variant = select_payload(payload, rom.sha1, language=getattr(args, "language", None))
    need_executables = variant.language is not None
    if need_executables:
        variant.check(need_executables=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    info = build_pack(Path(args.rom), payload, out_dir / "emerald3ds.pak", _progress, args.keep_workdir,
                      rom=rom, need_executables=need_executables, language=getattr(args, "language", None))
    print("Times (%s): %s" % (info["rom"], ", ".join("%s %.1f s" % t for t in info["timings"])))
    for exe in info["payload"].executables():
        if exe.exists():
            shutil.copy2(exe, out_dir / exe.name)
    print("Data pack: %d files, %.1f MiB, release %s" % (info["entries"], info["bytes"] / 1048576,
                                                         info["release"]))
    if info.get("followers"):
        f = info["followers"]
        print("Followers: %d sprites, %d texts (%d downloaded)%s" % (f["sprites"], f.get("texts", 0), f["downloaded"],
              ", %d not available" % len(f["missing"]) if f["missing"] else ""))
    print("Copy the folder %s to the root of your SD card." % (Path(args.output) / "3ds"))
    return 0


def cmd_install(args) -> int:
    payload = Payload(args.payload)
    payload.check()
    sd = Path(args.sd)
    with tempfile.TemporaryDirectory(prefix="emerald3ds-") as tmp:
        pak_path = Path(tmp) / "emerald3ds.pak"
        info = build_pack(Path(args.rom), payload, pak_path, _progress, need_executables=True,
                          language=getattr(args, "language", None))
        files = {exe.name: exe for exe in info["payload"].executables()}
        files["emerald3ds.pak"] = pak_path
        dest = install(sd, files, lambda f: _progress(f, "Copying to the SD card"), warn=_warn)
    print("Installed to %s" % dest)
    return 0


def cmd_verify(args) -> int:
    payload = Payload(args.payload)
    with pak.PakReader(Path(args.pak)) as reader:
        # The pack names the ROM it was built from, and its ABI the language
        # when that ROM makes more than one: check it against that variant.
        recipe = Recipe.load(payload.for_pak(reader.rom_sha1.hex(), reader.abi).recipe)
        count = reader.verify()
        expected = {pak.path_id(e["path"]): e for e in recipe.entries + recipe.generated}
        # The optional follower sprites (followers.py) are not the recipe's.
        optional = {pak.path_id(p) for p in followers.pack_paths()}
        for pid, entry in reader.entries.items():
            if pid in optional:
                continue
            want = expected.get(pid)
            if want is None or want["crc"] != entry.crc32 or want["size"] != entry.raw_size:
                raise BuilderError("The data pack does not match this release.")
        if len(set(reader.entries) - optional) != len(expected) or reader.abi != recipe.engine_abi:
            raise BuilderError("The data pack does not match this release.")
    print("OK: %d entries, ABI %08x, release %s" % (count, recipe.engine_abi, recipe.release))
    return 0


def cmd_detect(args) -> int:
    cards = find_sd_cards()
    if not cards:
        print("No SD card with a 'Nintendo 3DS' folder was found.")
    for card in cards:
        print(card)
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="emerald3ds-builder", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="version", version=__version__)
    ap.add_argument("--payload", type=Path, default=default_payload(),
                    help="folder with the release payload (default: next to the builder)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="write 3ds/emerald3ds/ into a folder")
    b.add_argument("--rom", required=True)
    b.add_argument("--output", required=True)
    b.add_argument("--language", help=LANGUAGE_HELP)
    b.add_argument("--keep-workdir", action="store_true", help=argparse.SUPPRESS)
    i = sub.add_parser("install", help="install straight onto an SD card")
    i.add_argument("--rom", required=True)
    i.add_argument("--sd", required=True)
    i.add_argument("--language", help=LANGUAGE_HELP)
    v = sub.add_parser("verify", help="check a data pack against this release")
    v.add_argument("--pak", required=True)
    sub.add_parser("detect", help="list SD cards that look like a 3DS card")
    args = ap.parse_args(argv)
    try:
        return {"build": cmd_build, "install": cmd_install, "verify": cmd_verify,
                "detect": cmd_detect}[args.cmd](args)
    except BuilderError as exc:
        sys.stdout.write("\n")
        print("Error: %s" % exc, file=sys.stderr)
        return 1
