#!/usr/bin/env python3
"""Stage the Simplified Chinese texts of the game into a bootstrapped tree.

    python tools/localize_chinese.py --tree build/upstream            # stage
    python tools/localize_chinese.py --tree build/upstream --check    # validate only
    python tools/localize_chinese.py --tree build/upstream --status   # what is translated

The texts come from rh-hideout-chinese/pokeemerald-expansion, whose translation
covers the same pret labels this port builds from. They are kept as one JSON
catalog (tools/locales/zh_cn/texts.json), keyed by source file and label,
exactly as the fork wrote them.

Only the labels this tree actually has are staged: one the fork added for
expansion content is ignored, and one this tree has but the catalog does not
stays in English, so the game builds at every stage of the work.

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
# A {CONSTANT} or {CONSTANT arg} in a text: the charmap's name, expanded by the
# preprocessor. The fork's translation is written for pokeemerald-expansion,
# which has a few placeholders pret does not; a text using one stays English.
PLACEHOLDER = re.compile(r"\{([^}]*)\}")
INC_STRING = re.compile(r'^\s*\.string\s+"(.*)"\s*$')
C_TEXT = re.compile(r'(g\w+)\s*\[[^\]]*\]\s*=\s*_\("((?:[^"\\]|\\.)*)"\)')


def is_script(relative: str) -> bool:
    return relative.endswith(".inc") or relative.endswith(".s")


def inc_texts(source: str) -> dict[str, tuple[int, int, str]]:
    """label -> (first .string line, line after the block, text) of each block."""
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
    pieces = re.split(r"(?<=\\[nlp])", text)
    return ['\t.string "%s"' % piece for piece in pieces if piece]


def c_texts(source: str) -> dict[str, tuple[int, int]]:
    """label -> (start, end) of the text inside _("...")."""
    return {m.group(1): (m.start(2), m.end(2)) for m in C_TEXT.finditer(source)}


def charmap_constants(tree: Path) -> set[str]:
    """The names a text may write as {NAME}: charmap.txt's constants."""
    source = (tree / "charmap.txt").read_text(encoding="utf-8")
    out = set()
    for line in source.splitlines():
        line = line.split("@")[0].rstrip()
        m = re.match(r"^(?:'(?P<ch>.+)'|(?P<name>[A-Za-z_]\w*))\s*=", line)
        if m and m.group("name"):
            out.add(m.group("name"))
    return out


def unsupported(text: str, constants: set[str]) -> list[str]:
    """The placeholders of a text this tree's charmap cannot expand."""
    return [m.group(1).split()[0] for m in PLACEHOLDER.finditer(text)
            if m.group(1).split() and m.group(1).split()[0] not in constants]


def han_codes(tree: Path) -> set[str]:
    """The characters the Chinese charmap maps, plus the Latin ones."""
    source = (tree / "charmap.txt").read_text(encoding="utf-8")
    return {m.group(1) for m in re.finditer(r"^'(.+?)'\s*=", source, re.M)}


def unencodable(text: str, codes: set[str]) -> list[str]:
    """The characters of a text the charmap cannot encode. Placeholders,
    control escapes and the terminator are not characters here."""
    stripped = PLACEHOLDER.sub("", text)
    stripped = re.sub(r"\\[nlp]", "", stripped)
    out = []
    for ch in stripped:
        if ch in ("$", "\\", " "):
            continue
        if ch not in codes:
            out.append(ch)
    return out


def present(tree: Path, catalog: dict) -> list[str]:
    return [rel for rel in catalog if (tree / rel).is_file()]


def stage_script(source: str, entries: dict, usable) -> tuple[str, int]:
    lines = source.split("\n")
    texts = inc_texts(source)
    plan = []
    for label, text in entries.items():
        if label not in texts or not usable(label, text):
            continue
        start, end, old = texts[label]
        if old == text:
            continue
        plan.append((start, end, text))
    for start, end, text in sorted(plan, reverse=True):
        lines[start:end] = inc_lines(text)
    return "\n".join(lines), len(plan)


def stage_c(source: str, entries: dict, usable) -> tuple[str, int]:
    texts = c_texts(source)
    plan = []
    for label, text in entries.items():
        if label not in texts or not usable(label, text):
            continue
        start, end = texts[label]
        if source[start:end] == text:
            continue
        plan.append((start, end, text))
    for start, end, text in sorted(plan, reverse=True):
        source = source[:start] + text + source[end:]
    return source, len(plan)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--check", action="store_true", help="validate the catalog, change nothing")
    parser.add_argument("--status", action="store_true", help="count the translated texts")
    args = parser.parse_args()

    tree = args.tree.resolve()
    catalog = json.loads((CATALOG / "texts.json").read_text(encoding="utf-8"))
    if "'啊'" not in (tree / "charmap.txt").read_text(encoding="utf-8"):
        raise SystemExit("localize_chinese: the charmap has no Han block; apply patch 0083 first")

    constants = charmap_constants(tree) | {"PAUSE", "PLAY_BGM", "PLAY_SE", "COLOR", "HIGHLIGHT",
                                           "SHADOW", "COLOR_HIGHLIGHT_SHADOW", "PALETTE", "FONT",
                                           "RESET_FONT", "CLEAR", "SKIP", "CLEAR_TO", "ESCAPE",
                                           "SHIFT_RIGHT", "SHIFT_DOWN", "FILL_WINDOW",
                                           "MIN_LETTER_SPACING", "JPN", "ENG", "PAUSE_MUSIC",
                                           "RESUME_MUSIC", "PAUSE_UNTIL_PRESS", "WAIT_SE"}
    codes = han_codes(tree)
    rejected: dict[str, int] = {}

    def usable(label: str, text: str) -> bool:
        """Whether this text can be staged: its placeholders exist, its
        characters encode, and it keeps the dynamics of the original."""
        bad = unsupported(text, constants)
        if bad:
            rejected["placeholder " + bad[0]] = rejected.get("placeholder " + bad[0], 0) + 1
            return False
        missing = unencodable(text, codes)
        if missing:
            rejected["character %r" % missing[0]] = rejected.get("character %r" % missing[0], 0) + 1
            return False
        return True

    files = present(tree, catalog)
    if args.status:
        total = staged = 0
        for relative in files:
            source = (tree / relative).read_text(encoding="utf-8")
            texts = inc_texts(source) if is_script(relative) else c_texts(source)
            have = sum(1 for label in catalog[relative] if label in texts)
            staged += have
            total += len(texts)
            print("%5d / %-5d %s" % (have, len(texts), relative))
        print("%5d / %-5d texts staged from the Chinese catalog" % (staged, total))
        return 0

    applied = 0
    for relative in files:
        path = tree / relative
        source = path.read_text(encoding="utf-8")
        staged_source, count = (stage_script(source, catalog[relative], usable) if is_script(relative)
                                else stage_c(source, catalog[relative], usable))
        applied += count
        if count and not args.check:
            path.write_text(staged_source, encoding="utf-8")

    if args.check:
        print("localize_chinese: %d texts in %d files check out" % (applied, len(files)))
        return 0

    (tree / ".emerald3ds-locale").write_text(MARKER + "\n")
    print("localize_chinese: %d texts staged from %d files" % (applied, len(files)))
    for what, count in sorted(rejected.items(), key=lambda kv: -kv[1])[:12]:
        print("  left in English: %4d  %s" % (count, what))
    return 0


if __name__ == "__main__":
    sys.exit(main())
