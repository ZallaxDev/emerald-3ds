#!/usr/bin/env python3
"""Stage the Simplified Chinese texts of the game into a bootstrapped tree.

    python tools/localize_chinese.py --tree build/upstream            # stage
    python tools/localize_chinese.py --tree build/upstream --check    # validate only
    python tools/localize_chinese.py --tree build/upstream --status   # what is translated

The texts come from rh-hideout-chinese/pokeemerald-expansion, whose translation
covers the pret labels this port builds from. The catalog mirrors the tree the
way tools/locales/pt_br does -- one `.txt` per source file, the texts of
`data/text/save.inc` in `data/text/save.inc.txt` -- so review reads it file by
file and no single catalog file grows past the repository's 1 MiB rule:

    [gText_ConfirmSave]
    Quer salvar o jogo?$

An entry names a label and its Chinese text, written as the source writes it
(\\n, \\l, \\p, {PLAYER}...). The file says nothing about which English text was
translated: the label is the key, and a label this tree does not have is simply
not staged.

A label this tree does not have is ignored; a text this tree has but the
catalog does not stays in English, so the game builds at every stage of the
work. A text is left in English when its placeholders are not in this tree's
charmap or one of its characters cannot be encoded there.

Run by tools/bootstrap.py --port-lang zh_cn, after the patches: 0083 gives the
charmap its Han code space and adds the two glyph sheets.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "tools" / "locales" / "zh_cn"
MARKER = "ZH-CN"

INC_LABEL = re.compile(r"^(\w+)::?\s*$")
INC_STRING = re.compile(r'^\s*\.string\s+"(.*)"\s*$')
C_TEXT = re.compile(r'(?:^|\s)(g?\w+)\s*\[[^\]]*\]\s*=\s*_\("((?:[^"\\]|\\.)*)"\)')
PLACEHOLDER = re.compile(r"\{([^}]*)\}")

# The charmap's own control constants: the preprocessor expands {NAME} for any
# of these, so a text keeping them is safe whatever the tree's set is.
CONTROL_CONSTANTS = {
    "PAUSE", "PLAY_BGM", "PLAY_SE", "COLOR", "HIGHLIGHT", "SHADOW",
    "COLOR_HIGHLIGHT_SHADOW", "PALETTE", "FONT", "RESET_FONT", "CLEAR", "SKIP",
    "CLEAR_TO", "ESCAPE", "SHIFT_RIGHT", "SHIFT_DOWN", "FILL_WINDOW",
    "MIN_LETTER_SPACING", "JPN", "ENG", "PAUSE_MUSIC", "RESUME_MUSIC",
    "PAUSE_UNTIL_PRESS", "WAIT_SE",
}

# Where the texts live: the same set the port's own build compiles. The C side
# is every source under src/, because the fork and pret disagree about which
# file holds a label (the main menu's texts are in src/main_menu.c there and
# src/strings.c here), and only the label is stable across the two.
SOURCE_GLOBS = ("data/text/*.inc", "data/scripts/*.inc", "data/maps/*/scripts.inc",
                "src/**/*.c")
SOURCE_FILES = ("data/event_scripts.s",)


def is_script(relative: str) -> bool:
    return relative.endswith(".inc") or relative.endswith(".s")


def inc_texts(source: str) -> dict[str, tuple[int, int, str]]:
    """label -> (first .string line, line after the block, text)."""
    lines = source.split("\n")
    found: dict[str, tuple[int, int, str]] = {}
    i = 0
    while i < len(lines):
        m = INC_LABEL.match(lines[i])
        if m:
            j = i + 1
            parts = []
            while j < len(lines) and INC_STRING.match(lines[j]):
                parts.append(INC_STRING.match(lines[j]).group(1))
                j += 1
            if parts:
                found[m.group(1)] = (i + 1, j, "".join(parts))
            i = j
            continue
        i += 1
    return found


def inc_lines(text: str) -> list[str]:
    """The text as .string lines, broken after each \\n, \\l and \\p."""
    return ['\t.string "%s"' % piece
            for piece in re.split(r"(?<=\\[nlp])", text) if piece]


def c_texts(source: str) -> dict[str, tuple[int, int]]:
    """label -> the span of the text inside _("...")."""
    return {m.group(1): (m.start(2), m.end(2)) for m in C_TEXT.finditer(source)}


def sources(tree: Path) -> list[str]:
    """Every file of the game that holds a text, as tree-relative paths."""
    out: list[str] = []
    for pattern in SOURCE_GLOBS:
        out += [p.relative_to(tree).as_posix() for p in sorted(tree.glob(pattern))]
    out += [f for f in SOURCE_FILES if (tree / f).is_file()]
    return out


def charmap(tree: Path) -> tuple[set[str], set[str]]:
    """(the characters the charmap encodes, the constants it names)."""
    chars: set[str] = set()
    names: set[str] = set(CONTROL_CONSTANTS)
    for line in (tree / "charmap.txt").read_text(encoding="utf-8").splitlines():
        line = line.split("@")[0].rstrip()
        m = re.match(r"^(?:'(.+?)'|(?P<name>[A-Za-z_]\w*))\s*=", line)
        if not m:
            continue
        if m.group(1) is not None:
            chars.add(m.group(1))
        else:
            names.add(m.group("name"))
    return chars, names


def unencodable(text: str, chars: set[str]) -> list[str]:
    stripped = PLACEHOLDER.sub("", text)
    stripped = re.sub(r"\\[nlp]", "", stripped)
    return [ch for ch in stripped if ch not in ("$", "\\", " ") and ch not in chars]


ENTRY = re.compile(r"^\[(\w+)\]\s*$")


def read_catalog_file(path: Path) -> dict[str, str]:
    """A catalog file: `[label]` then the text, until the next label."""
    entries: dict[str, str] = {}
    label = None
    body: list[str] = []

    def close() -> None:
        if label is not None:
            entries[label] = "".join(body)

    for line in path.read_text(encoding="utf-8").splitlines():
        header = ENTRY.match(line)
        if header:
            close()
            label, body = header.group(1), []
        elif label is not None:
            body.append(line)
    close()
    return entries


def load_catalog(tree: Path) -> dict[str, dict[str, str]]:
    """{tree-relative source file: {label: text}} for the files this tree has."""
    catalog: dict[str, dict[str, str]] = {}
    for path in sorted(CATALOG.rglob("*.txt")):
        relative = path.relative_to(CATALOG).as_posix()[:-len(".txt")]
        if (tree / relative).is_file():
            catalog[relative] = read_catalog_file(path)
    return catalog


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--check", action="store_true", help="validate the catalog, change nothing")
    parser.add_argument("--status", action="store_true", help="count the translated texts")
    args = parser.parse_args()

    tree = args.tree.resolve()
    catalog_by_file = load_catalog(tree)
    catalog: dict[str, str] = {label: text
                               for entries in catalog_by_file.values()
                               for label, text in entries.items()}
    chars, names = charmap(tree)
    if "啊" not in chars:
        raise SystemExit("localize_chinese: the charmap has no Han block; "
                         "apply patch 0083 first")

    files = sources(tree)
    rejected: dict[str, int] = {}
    seen: set[str] = set()

    def usable(text: str) -> bool:
        for ph in PLACEHOLDER.finditer(text):
            head = ph.group(1).split()
            if head and head[0] not in names:
                rejected["placeholder {%s}" % head[0]] = rejected.get("placeholder {%s}" % head[0], 0) + 1
                return False
        bad = unencodable(text, chars)
        if bad:
            key = "character %r" % bad[0]
            rejected[key] = rejected.get(key, 0) + 1
            return False
        return True

    staged = total = left = 0
    for relative in files:
        path = tree / relative
        source = path.read_text(encoding="utf-8")
        if is_script(relative):
            texts = inc_texts(source)
            plan = []
            for label, (start, end, old) in texts.items():
                total += 1
                seen.add(label)
                if label not in catalog or catalog[label] == old:
                    left += label not in catalog
                    continue
                if not usable(catalog[label]):
                    left += 1
                    continue
                plan.append((start, end, catalog[label]))
            if not plan:
                continue
            staged += len(plan)
            if not args.check:
                lines = source.split("\n")
                for start, end, text in sorted(plan, reverse=True):
                    lines[start:end] = inc_lines(text)
                path.write_text("\n".join(lines), encoding="utf-8")
        else:
            texts = c_texts(source)
            plan = []
            for label, (start, end) in texts.items():
                total += 1
                seen.add(label)
                if label not in catalog or catalog[label] == source[start:end]:
                    left += label not in catalog
                    continue
                if not usable(catalog[label]):
                    left += 1
                    continue
                plan.append((start, end, catalog[label]))
            if not plan:
                continue
            staged += len(plan)
            if not args.check:
                for start, end, text in sorted(plan, reverse=True):
                    source = source[:start] + text + source[end:]
                path.write_text(source, encoding="utf-8")

    untranslated = len(set(catalog) - seen)

    if args.status:
        print("%d texts in %d files: %d staged, %d still English, "
              "%d catalog entries this tree does not have"
              % (total, len(files), staged, total - staged, untranslated))
        for what, count in sorted(rejected.items(), key=lambda kv: -kv[1])[:12]:
            print("  left in English: %5d  %s" % (count, what))
        return 0

    if not args.check:
        (tree / ".emerald3ds-locale").write_text(MARKER + "\n")
    print("localize_chinese: %d of %d texts %s from %d files"
          % (staged, total, "check out" if args.check else "staged", len(files)))
    if rejected:
        for what, count in sorted(rejected.items(), key=lambda kv: -kv[1])[:8]:
            print("  left in English: %5d  %s" % (count, what))
    return 0


if __name__ == "__main__":
    sys.exit(main())
