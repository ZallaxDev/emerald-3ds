#!/usr/bin/env python3
"""Move a locale manifest's source positions from one patched tree to another.

When a patch changes a file that a manifest edits, its positions and
fingerprint go stale. This maps every edit through a line diff of the old and
new file, checks that each span still covers the same text, and rewrites the
fingerprints. No ROM is needed: the manifest only holds positions.

    python tools/locale_research/rebase_manifest.py tools/locales/spanish.json.gz \\
        --old build/upstream-before --new build/upstream
"""
from __future__ import annotations

import argparse
import bisect
import gzip
import hashlib
import json
from difflib import SequenceMatcher
from pathlib import Path


def line_starts(text: str) -> list[int]:
    starts = [0]
    for line in text.splitlines(keepends=True):
        starts.append(starts[-1] + len(line))
    return starts


def rebase_file(old: str, new: str, edits: list) -> list:
    old_lines = old.splitlines(keepends=True)
    new_lines = new.splitlines(keepends=True)
    old_at, new_at = line_starts(old), line_starts(new)
    line_map = {}
    for block in SequenceMatcher(None, old_lines, new_lines, autojunk=False).get_matching_blocks():
        for k in range(block.size):
            line_map[block.a + k] = block.b + k

    def move(pos: int, end: bool) -> int:
        line = bisect.bisect_right(old_at, pos - (1 if end else 0)) - 1
        if line not in line_map:
            raise ValueError("an edited line changed")
        return new_at[line_map[line]] + pos - old_at[line]

    moved = []
    for start, stop, *rest in edits:
        a, b = move(start, False), move(stop, True)
        if old[start:stop] != new[a:b]:
            raise ValueError("span at %d no longer covers the same text" % start)
        moved.append([a, b, *rest])
    return moved


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("manifest", type=Path)
    ap.add_argument("--old", type=Path, required=True, help="tree the manifest was made for")
    ap.add_argument("--new", type=Path, required=True, help="tree to move it to")
    args = ap.parse_args()
    manifest = json.loads(gzip.decompress(args.manifest.read_bytes()))
    changed = 0
    for relative, file in manifest["files"].items():
        old = (args.old / relative).read_text(encoding="utf-8")
        if hashlib.sha256(old.encode()).hexdigest() != file["sha256"]:
            raise SystemExit("%s: the old tree is not the one the manifest was made for" % relative)
        new = (args.new / relative).read_text(encoding="utf-8")
        if new == old:
            continue
        try:
            file["edits"] = rebase_file(old, new, file["edits"])
        except ValueError as exc:
            raise SystemExit("%s: %s" % (relative, exc))
        file["sha256"] = hashlib.sha256(new.encode()).hexdigest()
        changed += 1
        print("rebased", relative)
    args.manifest.write_bytes(gzip.compress(json.dumps(manifest, separators=(",", ":")).encode(), 9, mtime=0))
    print("%d file(s) rebased" % changed)


if __name__ == "__main__":
    main()
