#!/usr/bin/env python3
"""Move the locale manifests' edit spans onto a changed patched tree.

A patch that touches a file a manifest edits changes its fingerprint and the
positions of the spans after the change. When the change leaves every span's
own text alone, the spans only move: this maps each one through a diff of the
old and the new file and records the new fingerprint. A span that the change
touches stops the run; that manifest must then be regenerated.

    python tools/bootstrap.py --dir build/old          # before the patch
    python tools/bootstrap.py --dir build/new          # with it
    python tools/rebase_locale_manifests.py --old build/old --new build/new
"""
from __future__ import annotations

import argparse
import bisect
import difflib
import gzip
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCALES = ROOT / "tools/locales"


def line_starts(text: str) -> list[int]:
    starts = [0]
    for index, char in enumerate(text):
        if char == "\n":
            starts.append(index + 1)
    return starts


def mapper(old: str, new: str):
    """A function from old character offsets inside unchanged lines to new ones."""
    old_lines, new_lines = old.splitlines(keepends=True), new.splitlines(keepends=True)
    old_starts, new_starts = line_starts(old), line_starts(new)
    blocks = [b for b in difflib.SequenceMatcher(None, old_lines, new_lines, autojunk=False).get_matching_blocks()
              if b.size]
    keys = [old_starts[b.a] for b in blocks]

    def move(start: int, end: int):
        index = bisect.bisect_right(keys, start) - 1
        if index < 0:
            return None
        block = blocks[index]
        first, last = old_starts[block.a], old_starts[block.a + block.size] if block.a + block.size < len(old_starts) else len(old)
        if not first <= start < end <= last:
            return None
        shift = new_starts[block.b] - first
        return start + shift, end + shift
    return move


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--old", type=Path, required=True, help="the tree the manifests were made for")
    parser.add_argument("--new", type=Path, required=True, help="the changed tree")
    parser.add_argument("--only", nargs="*", help="manifest names (default: every tools/locales/*.json.gz)")
    args = parser.parse_args()
    names = args.only or sorted(p.name[:-8] for p in LOCALES.glob("*.json.gz"))
    for name in names:
        path = LOCALES / (name + ".json.gz")
        manifest = json.loads(gzip.decompress(path.read_bytes()))
        moved = 0
        for relative, file in manifest["files"].items():
            old = (args.old / relative).read_text(encoding="utf-8")
            if hashlib.sha256(old.encode()).hexdigest() != file["sha256"]:
                raise SystemExit("%s: %s does not match the old tree" % (name, relative))
            new = (args.new / relative).read_text(encoding="utf-8")
            if new == old:
                continue
            move = mapper(old, new)
            edits = []
            for start, end, offset, size, kind in file["edits"]:
                target = move(start, end)
                if target is None or new[target[0]:target[1]] != old[start:end]:
                    raise SystemExit("%s: the change touches an edit in %s at %d; regenerate it" % (name, relative, start))
                edits.append([target[0], target[1], offset, size, kind])
            file["edits"] = edits
            file["sha256"] = hashlib.sha256(new.encode()).hexdigest()
            moved += 1
        if moved:
            path.write_bytes(gzip.compress(json.dumps(manifest, separators=(",", ":")).encode(), mtime=0))
        print("%s: %d file(s) moved" % (name, moved))


if __name__ == "__main__":
    main()
