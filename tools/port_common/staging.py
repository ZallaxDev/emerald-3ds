#!/usr/bin/env python3
"""Split the build's RomFS staging into engine files and game data.

The 3DS build writes every generated file under 3ds_port/romfs/. Two kinds of
file live there:

* engine files describe the executable itself (asset index, relocation
  tables, shaders, the ABI) or are original port art; they always ship inside
  the 3DSX;
* game data is everything derived from the game: graphics, maps, scripts,
  songs, linked tables, voxel data. A development build may embed it in its
  RomFS; a release never does, and the player's builder produces it as
  emerald3ds.pak from their own ROM.

A third kind, optional data, is neither: files the port can do without that
come from elsewhere than the ROM, today the follower sprites
(builder/emerald3ds_builder/followers.py). They go into a pack and the loose
development data, never into a release's RomFS, the recipe or the ABI.

Commands:

    staging.py abi      --romfs DIR               write DIR/engine/abi.bin
    staging.py engine   --romfs DIR --out DIR2    copy only the engine files
    staging.py pak      --romfs DIR --out FILE    pack the game data
    staging.py devdata  --romfs DIR --out DIR2    loose game data + marker
    staging.py list     --romfs DIR               show the classification
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import shutil
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "builder"))
from emerald3ds_builder import pak  # noqa: E402
from emerald3ds_builder.rom import ROM_PROFILES  # noqa: E402

ENGINE_PATTERNS = [
    "boot.txt",
    "data.embedded",
    "shaders/*",
    "engine/*",
    "assets/asset_index.bin",
    "assets/asset_ptr_index.bin",
    "assets/asset_map.txt",
    "assets/asset_ptr_map.txt",
    "scripts/*.rel",
    "sound/*.rel",
    "gamedata/*.rel",
    "voxel/trees.rgba5551",
    "bottom/battle.bin",
    "bottom/menu.bin",
    "battle/hud.bin",
]
OPTIONAL_PATTERNS = [
    "followers/*",
]
SUPPORTED_ROM_SHA1 = bytes.fromhex("f3ae088181bf583e55daf962a92bb46f4f1d07b7")


def is_engine(rel: str) -> bool:
    return any(fnmatch.fnmatchcase(rel, p) for p in ENGINE_PATTERNS)


def is_optional(rel: str) -> bool:
    return any(fnmatch.fnmatchcase(rel, p) for p in OPTIONAL_PATTERNS)


def walk(romfs: Path):
    for path in sorted(romfs.rglob("*")):
        if path.is_file() and path.name != ".gitignore":
            yield path.relative_to(romfs).as_posix(), path


def data_files(romfs: Path):
    return [(rel, path) for rel, path in walk(romfs) if not is_engine(rel) and not is_optional(rel)]


def optional_files(romfs: Path):
    return [(rel, path) for rel, path in walk(romfs) if is_optional(rel)]


def engine_files(romfs: Path):
    return [(rel, path) for rel, path in walk(romfs) if is_engine(rel)]


def compute_abi(romfs: Path) -> tuple[int, list[tuple[str, int, int]]]:
    items = []
    for rel, path in data_files(romfs):
        data = path.read_bytes()
        items.append((rel, len(data), zlib.crc32(data) & 0xFFFFFFFF))
    return pak.engine_abi(items), items


def cmd_abi(args) -> None:
    abi, items = compute_abi(args.romfs)
    out = args.romfs / "engine" / "abi.bin"
    out.parent.mkdir(parents=True, exist_ok=True)
    blob = struct.pack("<I", abi)
    if not out.exists() or out.read_bytes() != blob:
        out.write_bytes(blob)
    manifest = args.romfs / "engine" / "data_manifest.txt"
    text = "".join("%08x %9d %s\n" % (c, s, p) for p, s, c in items)
    if not manifest.exists() or manifest.read_text() != text:
        manifest.write_text(text, encoding="ascii", newline="\n")
    print("staging: engine ABI %08x over %d data files (%.1f MiB)"
          % (abi, len(items), sum(s for _, s, _ in items) / 1048576))


def cmd_engine(args) -> None:
    if args.out.exists():
        shutil.rmtree(args.out)
    for rel, path in engine_files(args.romfs):
        if rel == "data.embedded":
            continue
        dst = args.out / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, dst)
    print("staging: engine RomFS -> %s" % args.out)


# The ROM each language's tree is built from (its localize_*.py), by the
# GAME_LANGUAGE its include/constants/global.h compiles.
GAME_LANGUAGE_ROMS = {"LANGUAGE_ENGLISH": "BPEE", "LANGUAGE_SPANISH": "BPES",
                      "LANGUAGE_FRENCH": "BPEF", "LANGUAGE_ITALIAN": "BPEI",
                      "LANGUAGE_GERMAN": "BPED"}


def cmd_pak(args) -> None:
    abi, _ = compute_abi(args.romfs)
    profile = None
    if args.game_config is not None:
        configuration = args.game_config.read_text(encoding="utf-8")
        language = re.search(r"^\s*#\s*define\s+GAME_LANGUAGE\s+\(?\s*(LANGUAGE_[A-Z]+)\s*\)?\s*$",
                             configuration, re.MULTILINE)
        code = GAME_LANGUAGE_ROMS.get(language.group(1)) if language else None
        if code is None:
            raise ValueError("Cannot determine the compiled game's supported ROM from its configuration")
        profile = bytes.fromhex(ROM_PROFILES[code])
    sha1 = pak.sha1_of(args.rom) if args.rom else profile or SUPPORTED_ROM_SHA1
    if profile is not None and sha1 != profile:
        raise ValueError("The supplied ROM does not match the compiled game's language")
    files = ((rel, path.read_bytes()) for rel, path in data_files(args.romfs) + optional_files(args.romfs))
    info = pak.write_pak(args.out, files, abi, sha1)
    with pak.PakReader(args.out) as reader:
        reader.verify()
    print("staging: %d entries, %.1f MiB, ABI %08x -> %s"
          % (info["entries"], info["bytes"] / 1048576, abi, args.out))


def cmd_devdata(args) -> None:
    abi, _ = compute_abi(args.romfs)
    count = 0
    for rel, path in data_files(args.romfs) + optional_files(args.romfs):
        dst = args.out / rel
        if dst.exists() and dst.stat().st_size == path.stat().st_size \
                and dst.read_bytes() == path.read_bytes():
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, dst)
        count += 1
    (args.out / ".emerald3ds-dev").write_text("abi %08x\n" % abi, encoding="ascii")
    print("staging: %d changed data files -> %s (ABI %08x)" % (count, args.out, abi))


def cmd_list(args) -> None:
    for rel, _ in walk(args.romfs):
        print("%-8s %s" % ("engine" if is_engine(rel) else "optional" if is_optional(rel) else "data", rel))


def main(argv=None) -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("abi", "engine", "pak", "devdata", "list"):
        p = sub.add_parser(name)
        p.add_argument("--romfs", type=Path, required=True)
        if name in ("engine", "pak", "devdata"):
            p.add_argument("--out", type=Path, required=True)
        if name == "pak":
            p.add_argument("--game-config", type=Path, default=None,
                           help="compiled game's global.h; selects and validates its ROM fingerprint")
            p.add_argument("--rom", type=Path, default=None,
                           help="record this ROM's SHA-1 (default: configured game's ROM, otherwise English)")
    args = ap.parse_args(argv)
    {"abi": cmd_abi, "engine": cmd_engine, "pak": cmd_pak,
     "devdata": cmd_devdata, "list": cmd_list}[args.cmd](args)


if __name__ == "__main__":
    main()
