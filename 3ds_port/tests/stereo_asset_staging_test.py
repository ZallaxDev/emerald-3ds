#!/usr/bin/env python3
"""Stereo plane packs are strictly optional engine-only assets.

This synthetic test needs no ROM or author depth pack; it prevents
staged files from accidentally contaminating the game-data ABI/pak.
"""
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "port_common"))
import staging  # noqa: E402


def test_classification():
    for name in ("stereo/mons.bin", "stereo/backs.bin"):
        assert staging.is_engine(name), name
        assert not staging.is_optional(name), name
    with tempfile.TemporaryDirectory() as folder:
        romfs = Path(folder)
        for name in ("stereo/mons.bin", "stereo/backs.bin"):
            file = romfs / name
            file.parent.mkdir(exist_ok=True, parents=True)
            file.write_bytes(b"EM3DSTM1" + bytes((0, 0, 4, 2, 64, 0, 64, 0)))
        follower = romfs / "followers" / "present.txt"
        follower.parent.mkdir()
        follower.write_text("synthetic")
        e = [name for name, _ in staging.engine_files(romfs)]
        d = [name for name, _ in staging.data_files(romfs)]
        opt = [name for name, _ in staging.optional_files(romfs)]
        assert e == ["stereo/backs.bin", "stereo/mons.bin"], e
        assert d == [], d
        assert opt == ["followers/present.txt"], opt
        _, entries = staging.compute_abi(romfs)
        assert entries == [], entries


if __name__ == "__main__":
    test_classification()
    print("PASS stereo engine-only staging and noninterference with ABI/optional files")
