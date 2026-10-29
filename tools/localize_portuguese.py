#!/usr/bin/env python3
"""Stage the Brazilian Portuguese texts of the game into a bootstrapped tree.

    python tools/localize_portuguese.py --tree build/upstream            # stage
    python tools/localize_portuguese.py --tree build/upstream --check    # validate only
    python tools/localize_portuguese.py --tree build/upstream --status   # what is translated
    python tools/localize_portuguese.py --tree build/upstream --show data/text/save.inc
                                                   # each text's hash, width and English

The catalogs in tools/locales/pt_br/ mirror the source tree: the texts of
data/text/birch_speech.inc are in tools/locales/pt_br/data/text/birch_speech.inc.txt.
An entry names a label of that file, the hash of its English text and the
Portuguese text, written as in the source (\\n, \\l, \\p, {PLAYER}...):

    [gText_Birch_AndYouAre] a27b7d7097
    E você, quem é?$

The text is the entry's lines joined as they are; a blank line ends it. The
English text is never copied here: the hash says which one was translated,
and staging stops when the source no longer has it. Labels without an entry
stay in English. Lines are measured in the normal font: a text of a .inc
or .s file (a message box, 27 tiles) may use up to MESSAGE_BOX_WIDTH pixels, a C
string (often a menu sized to its entries) only the width of its English
text. A header may end in width=N to allow N pixels.

Run by tools/bootstrap.py --port-lang pt_br, after the patches (0057 gives
the charmap ã, õ, Ã and Õ) and before the build.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "tools" / "locales" / "pt_br"
MARKER = "PT-BR"

# The message boxes are 27 tiles wide (216 px); a little margin.
MESSAGE_BOX_WIDTH = 208

# Rough pixel widths of the placeholders, the same on both sides of a
# comparison: a 7-letter name, a 12-letter item or species.
PLACEHOLDER_WIDTHS = {"PLAYER": 42, "RIVAL": 42, "STR_VAR_1": 72, "STR_VAR_2": 72, "STR_VAR_3": 72,
                      "KUN": 0, "DYNAMIC": 42}
# The battle texts' ({B_...}), by the end of the name: a POKéMON with
# "selvagem" or "inimigo" after it, an ability, move or item, a trainer
# class, a POKéMON, a trainer, a buffer (a number, a stat, a name), the
# "aliada"/"inimiga" of a side.
BATTLE_PLACEHOLDER_WIDTHS = [("_WITH_PREFIX", 100), ("ABILITY", 72), ("MOVE", 72), ("ITEM", 72),
                             ("_CLASS", 66), ("MON1_NAME", 60), ("MON2_NAME", 60),
                             ("CREATOR_NAME", 60), ("_NAME", 42), ("BUFF", 60), ("PREFIX", 36)]


def placeholder_width(name: str) -> int:
    if name in PLACEHOLDER_WIDTHS:
        return PLACEHOLDER_WIDTHS[name]
    if name.startswith("B_"):
        for ending, width in BATTLE_PLACEHOLDER_WIDTHS:
            if ending in name:
                return width
    return 0


def text_hash(text: str) -> str:
    return hashlib.sha1(text.encode("utf-8")).hexdigest()[:10]


@dataclass
class Entry:
    label: str
    digest: str
    text: str
    width: int | None
    line: int


def read_catalog(path: Path) -> list[Entry]:
    entries: list[Entry] = []
    current = None
    body: list[str] = []

    def close():
        if current is not None:
            entries.append(Entry(current[0], current[1], "".join(body), current[2], current[3]))

    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if raw.startswith("#"):
            continue
        header = re.match(r"^\[(\w+)\]\s+([0-9a-f]{10})(?:\s+width=(\d+))?\s*$", raw)
        if header:
            close()
            current = (header.group(1), header.group(2), int(header.group(3)) if header.group(3) else None, number)
            body = []
        elif not raw.strip():
            close()
            current = None
        elif current is None:
            raise ValueError("%s:%d: text outside an entry" % (path, number))
        else:
            body.append(raw)
    close()
    return entries


class Charset:
    """The charmap and the normal font's widths, to measure a line."""

    def __init__(self, tree: Path):
        self.codes: dict[str, int] = {}
        self.names: set[str] = set()
        self.sizes: dict[str, int] = {}
        for line in (tree / "charmap.txt").read_text(encoding="utf-8").splitlines():
            line = line.split("@")[0].strip()
            m = re.match(r"^'(.+)'\s*=\s*([0-9A-Fa-f]{2})\b", line)
            if m:
                char = m.group(1)
                # '\'' is the apostrophe ('\n', '\l', '\p' are control codes).
                self.codes.setdefault(char[1:] if char.startswith("\\") else char, int(m.group(2), 16))
                continue
            m = re.match(r"^(\w+)\s*=\s*([0-9A-Fa-f ]+)$", line)
            if m:
                self.names.add(m.group(1))
                self.sizes[m.group(1)] = len(m.group(2).split())
                continue
            m = re.match(r"^(\w+)\s*=", line)
            if m:
                self.names.add(m.group(1))
        fonts = (tree / "src" / "fonts.c").read_text(encoding="utf-8")
        m = re.search(r"gFontNormalLatinGlyphWidths\[\] = \{(.*?)\};", fonts, re.S)
        self.widths = [int(x) for x in re.findall(r"\d+", m.group(1))]

    def lines(self, text: str) -> list[list[str]]:
        """The text as lines (split at \\n, \\l, \\p) of characters and {tokens}."""
        lines, line, i = [], [], 0
        while i < len(text):
            c = text[i]
            if c == "\\" and i + 1 < len(text):
                if text[i + 1] in "nlp":
                    lines.append(line)
                    line = []
                i += 2
                continue
            if c == "{":
                end = text.index("}", i)
                line.append(text[i:end + 1])
                i = end + 1
                continue
            line.append(c)
            i += 1
        lines.append(line)
        return lines

    def check(self, text: str) -> list[str]:
        problems = []
        for line in self.lines(text):
            for item in line:
                if item.startswith("{"):
                    words = item[1:-1].split()
                    if not words or words[0] not in self.names:
                        problems.append("unknown placeholder " + item)
                elif item not in self.codes and item != "$":
                    problems.append("character not in the charmap: %r" % item)
        return problems

    def width(self, line: list[str]) -> int:
        total = 0
        for item in line:
            if item.startswith("{"):
                total += placeholder_width(item[1:-1].split()[0])
            elif item != "$":
                total += self.widths[self.codes[item]]
        return total

    def size(self, text: str) -> int:
        """The bytes the text takes in the game, its terminator aside."""
        total = len(re.findall(r"\\[nlp]", text))
        for line in self.lines(text):
            for item in line:
                if item.startswith("{"):
                    total += sum(self.sizes.get(word, 1) for word in item[1:-1].split())
                elif item != "$":
                    total += 1
        return total

    def widest(self, text: str) -> int:
        return max(self.width(line) for line in self.lines(text))


def placeholders(text: str) -> list[str]:
    return sorted(re.findall(r"\{[^}]*\}", text))


# --- sources ----------------------------------------------------------------

INC_LABEL = re.compile(r"^(\w+)::?\s*$")
INC_STRING = re.compile(r'^\s*\.string\s+"(.*)"\s*$')
C_STRING = re.compile(r"_\(")
C_NAMED = re.compile(r"\bu8\s+(\w+)\[\w*\]\s*=\s*$")
C_DESIGNATED = re.compile(r"\[(\w+)\]\s*=\s*$")
C_FIELD = re.compile(r"\.\w+\s*=\s*$")
C_ENTRY = re.compile(r"\[(\w+)\]\s*=\s*\{")

# Fixed-size names: the array's length, less the terminator.
NAME_BYTES = {"src/data/items.h": 13, "src/data/text/move_names.h": 12,
              "src/data/pokemon/pokedex_entries.h": 11}
# Files holding names and other texts: the labels that are names, and their size.
NAME_PREFIX_BYTES = {"src/data/text/abilities.h": ("ABILITY_", 12),
                     "src/battle_main.c": ("TYPE_", 9)}
# Lists drawn in one window: every entry may use the widest English one (of
# its own kind, in a file of names and descriptions).
WIDEST_IN_FILE = {"src/data/text/item_descriptions.h", "src/data/text/move_names.h",
                  "src/data/text/move_descriptions.h", "src/data/text/abilities.h",
                  "src/battle_main.c", "src/data/pokemon/pokedex_entries.h",
                  "src/data/pokemon/pokedex_text.h"}
# Texts drawn in a window of so many lines (the POKéDEX description: 4).
MAX_LINES = {"src/data/pokemon/pokedex_text.h": 4}
# Room measured from the window. Item names: the BAG and the marts list them
# in the narrow font from x=8 to the count or price right-aligned at 120;
# 80 px of the (wider) normal font leaves them room.
# Battle messages: the battle's message box is 26 tiles wide (208 px); the
# texts of the other windows (menus, the level-up box) carry width=.
# Type names: the battle's move type window is 64 px, with TIPO/ left out
# (patch 0060); TERRESTRE, the longest, is 54.
FILE_WIDTH = {"src/data/items.h": 80, "src/battle_message.c": MESSAGE_BOX_WIDTH,
              "src/battle_main.c": 54}


def inc_texts(source: str) -> dict[str, tuple[int, int, str]]:
    """label -> (first line, end line, text) of each .string block."""
    lines = source.split("\n")
    found = {}
    i = 0
    while i < len(lines):
        m = INC_LABEL.match(lines[i])
        if m:
            start = j = i + 1
            parts = []
            while j < len(lines) and INC_STRING.match(lines[j]):
                parts.append(INC_STRING.match(lines[j]).group(1))
                j += 1
            if parts:
                found[m.group(1)] = (start, j, "".join(parts))
            i = j
            continue
        i += 1
    return found


def inc_lines(text: str) -> list[str]:
    """The text as .string lines, broken after each \\n, \\l and \\p."""
    pieces = re.split(r"(?<=\\[nlp])", text)
    return ['\t.string "%s"' % piece for piece in pieces if piece]


def c_label(source: str, start: int) -> str | None:
    """The label of the _( ) at start: NAME of u8 NAME[] = _(, KEY of
    [KEY] = _(, or KEY of the entry [KEY] = { ... .name = _( it is in."""
    before = source[max(0, start - 200):start]
    for pattern in (C_NAMED, C_DESIGNATED):
        m = pattern.search(before)
        if m:
            return m.group(1)
    if C_FIELD.search(before):
        entries = list(C_ENTRY.finditer(source, 0, start))
        if entries:
            return entries[-1].group(1)
    return None


FOREIGN = re.compile(r"GAME_LANGUAGE\s*==\s*LANGUAGE_(SPANISH|FRENCH|ITALIAN|GERMAN)")


def foreign_spans(source: str) -> list[tuple[int, int]]:
    """The parts of a C source that only another language's build compiles:
    the branch of an #if on GAME_LANGUAGE == LANGUAGE_SPANISH, _FRENCH, _ITALIAN
    or _GERMAN (unless it names CTR_LANG_PT_BR too or negates the test), up to
    its #else, #elif or #endif.
    The English text is the one in the other branch."""
    spans, stack, pos = [], [], 0
    for line in source.splitlines(keepends=True):
        word = line.strip().split(None, 1)[0] if line.strip().startswith("#") else ""
        foreign = (word in ("#if", "#elif") and FOREIGN.search(line) is not None
                   and "CTR_LANG_PT_BR" not in line and "!" not in line)
        if word in ("#if", "#ifdef", "#ifndef"):
            stack.append(pos + len(line) if foreign else None)
        elif word in ("#else", "#elif", "#endif") and stack:
            if stack[-1] is not None:
                spans.append((stack[-1], pos))
            if word == "#endif":
                stack.pop()
            else:
                stack[-1] = pos + len(line) if foreign else None
        pos += len(line)
    return spans


def is_script(relative: str) -> bool:
    """A file of assembler texts (label::, .string), as data/event_scripts.s."""
    return relative.endswith((".inc", ".s"))


def c_texts(source: str) -> dict[str, tuple[int, int, str]]:
    """label -> (start, end, text) of each _("...") with a label (see
    c_label), start and end around the string literals inside _( ). Texts
    only another language's build compiles are left out (foreign_spans)."""
    found = {}
    skip = foreign_spans(source)
    for m in C_STRING.finditer(source):
        if any(a <= m.start() < b for a, b in skip):
            continue
        label = c_label(source, m.start())
        if label is None or label in found:
            continue
        i = m.end()
        parts = []
        while True:
            while source[i] in " \t\r\n":
                i += 1
            if source[i] != '"':
                break
            j = i + 1
            while source[j] != '"':
                j += 2 if source[j] == "\\" else 1
            parts.append(source[i + 1:j])
            i = j + 1
        if parts and source[i] == ")":
            found[label] = (m.end(), i, "".join(parts))
    return found


@dataclass
class Result:
    path: str
    entries: int
    applied: int
    problems: list[str]


def localize(tree: Path, relative: str, charset: Charset, write: bool) -> Result:
    catalog = CATALOG / (relative + ".txt")
    entries = read_catalog(catalog)
    path = tree / relative
    source = path.read_text(encoding="utf-8")
    is_inc = is_script(relative)
    texts = inc_texts(source) if is_inc else c_texts(source)
    problems, edits = [], []
    seen = set()
    prefix, prefix_bytes = NAME_PREFIX_BYTES.get(relative, (None, 0))

    def kind(label: str) -> bool:
        return prefix is not None and label.startswith(prefix)

    file_widest = {}
    if relative in WIDEST_IN_FILE:
        for label, (_, _, text) in texts.items():
            file_widest[kind(label)] = max(file_widest.get(kind(label), 0), charset.widest(text))
    for entry in entries:
        where = "%s:%d [%s]" % (catalog.relative_to(ROOT), entry.line, entry.label)
        if entry.label in seen:
            problems.append(where + ": listed twice")
            continue
        seen.add(entry.label)
        if entry.label not in texts:
            problems.append(where + ": no such text in " + relative)
            continue
        start, end, english = texts[entry.label]
        if text_hash(english) != entry.digest:
            problems.append(where + ": the English text changed (hash %s)" % text_hash(english))
            continue
        local = [where + ": " + p for p in charset.check(entry.text)]
        if is_inc and not entry.text.endswith("$"):
            local.append(where + ": must end in $ like the English text")
        if '"' in entry.text:
            local.append(where + ': use “ ” rather than "')
        if placeholders(entry.text) != placeholders(english):
            local.append(where + ": placeholders %s, the English text has %s"
                         % (placeholders(entry.text), placeholders(english)))
        if not local and relative in NAME_BYTES and charset.size(entry.text) > NAME_BYTES[relative]:
            local.append(where + ": %d bytes, at most %d" % (charset.size(entry.text), NAME_BYTES[relative]))
        if not local and kind(entry.label) and charset.size(entry.text) > prefix_bytes:
            local.append(where + ": %d bytes, at most %d" % (charset.size(entry.text), prefix_bytes))
        if not local and relative in MAX_LINES and len(charset.lines(entry.text)) > MAX_LINES[relative]:
            local.append(where + ": %d lines, at most %d" % (len(charset.lines(entry.text)), MAX_LINES[relative]))
        if not local and relative in MAX_LINES and "\\p" in entry.text:
            local.append(where + ": no \\p in a window of %d lines" % MAX_LINES[relative])
        if not local:
            limit = entry.width or charset.widest(english)
            if is_inc and not entry.width:
                limit = max(limit, MESSAGE_BOX_WIDTH)
            if relative in WIDEST_IN_FILE and not entry.width:
                limit = max(limit, file_widest[kind(entry.label)])
            if relative in FILE_WIDTH and not entry.width:
                limit = max(limit, FILE_WIDTH[relative])
            for number, line in enumerate(charset.lines(entry.text), 1):
                if charset.width(line) > limit:
                    local.append(where + ": line %d is %d px wide, at most %d"
                                 % (number, charset.width(line), limit))
        problems += local
        edits.append((start, end, entry.text))
    if write and not problems:
        if is_inc:
            lines = source.split("\n")
            for start, end, text in sorted(edits, reverse=True):
                lines[start:end] = inc_lines(text)
            source = "\n".join(lines)
        else:
            for start, end, text in sorted(edits, reverse=True):
                source = source[:start] + '"' + text + '"' + source[end:]
        path.write_text(source, encoding="utf-8", newline="")
    return Result(relative, len(entries), len(edits), problems)


# Made by `make generated` (from region_map_sections.json): staged once it
# exists, as bootstrap.py --make does after the generated includes.
GENERATED = {"src/data/region_map/region_map_entries.h"}


def present(tree: Path) -> list[str]:
    """The catalogs whose source is in the tree; a generated one may not be yet."""
    missing = [r for r in catalogs() if r in GENERATED and not (tree / r).exists()]
    for relative in missing:
        print("localize_portuguese: %s not generated yet, left for the build" % relative)
    return [r for r in catalogs() if r not in missing]


def catalogs() -> list[str]:
    # referencia/ holds material to translate from, not catalogs.
    return sorted(str(p.relative_to(CATALOG).with_suffix("")).replace("\\", "/")
                  for p in CATALOG.rglob("*.txt") if p.relative_to(CATALOG).parts[0] != "referencia")


def status(tree: Path) -> None:
    """How many texts each source has and how many are translated."""
    done = {}
    for relative in catalogs():
        done[relative] = len(read_catalog(CATALOG / (relative + ".txt")))
    sources = sorted(tree.glob("data/text/*.inc")) + sorted(tree.glob("data/maps/*/scripts.inc"))
    sources += sorted(tree.glob("data/scripts/*.inc")) + [tree / "data/event_scripts.s", tree / "src/strings.c"]
    total = translated = 0
    for path in sources:
        relative = path.relative_to(tree).as_posix()
        source = path.read_text(encoding="utf-8")
        count = len(inc_texts(source) if is_script(relative) else c_texts(source))
        total += count
        translated += done.get(relative, 0)
        if done.get(relative):
            print("%5d / %-5d %s" % (done[relative], count, relative))
    print("%5d / %-5d texts in data/text, the map and shared scripts, data/event_scripts.s and src/strings.c"
          % (translated, total))


def show(tree: Path, relative: str) -> None:
    """Each text of a source, to translate from (printed, never written here)."""
    charset = Charset(tree)
    source = (tree / relative).read_text(encoding="utf-8")
    for label, (_, _, text) in (inc_texts(source) if is_script(relative) else c_texts(source)).items():
        print("[%s] %s  (%d px)\n%s\n" % (label, text_hash(text), charset.widest(text), text))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--check", action="store_true", help="validate the catalogs, change nothing")
    parser.add_argument("--status", action="store_true", help="count the translated texts")
    parser.add_argument("--show", metavar="SOURCE", help="print the texts of a source, with their hashes")
    args = parser.parse_args()
    tree = args.tree.resolve()
    if args.status:
        status(tree)
        return 0
    if args.show:
        show(tree, args.show)
        return 0
    charset = Charset(tree)
    if "ã" not in charset.codes:
        raise SystemExit("localize_portuguese: the charmap has no ã; apply patch 0057 first")
    results = [localize(tree, relative, charset, write=False) for relative in present(tree)]
    problems = [p for r in results for p in r.problems]
    if problems:
        print("\n".join(problems), file=sys.stderr)
        raise SystemExit("localize_portuguese: %d problem(s), nothing staged" % len(problems))
    if args.check:
        print("localize_portuguese: %d texts in %d files check out"
              % (sum(r.applied for r in results), len(results)))
        return 0
    # Validate everything before changing anything.
    for relative in present(tree):
        localize(tree, relative, charset, write=True)
    # The EASY CHAT lists are ordered by hand for English: order them by the
    # staged texts (words, moves) so the ABC mode and each group read right.
    import easy_chat_order
    words, groups = easy_chat_order.word_texts(tree)
    easy_chat_order.reorder_groups(groups)
    easy_chat_order.reorder_letters(tree, words)
    (tree / ".emerald3ds-locale").write_text(MARKER + "\n")
    print("localize_portuguese: %d texts staged from %d files"
          % (sum(r.applied for r in results), len(results)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
