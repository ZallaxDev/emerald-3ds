#!/usr/bin/env python3
"""Put the EASY CHAT words back in alphabetical order after translation.

    python tools/easy_chat_order.py --tree build/upstream

The EASY CHAT screen lists words in two orders that pret writes by hand
for the English words: each group by .alphabeticalOrder (in
src/data/easy_chat/easy_chat_group_*.h), and the ABC mode by first letter
(src/data/easy_chat/easy_chat_words_by_letter.h). Once the words, moves
and species are staged in Portuguese, this rewrites both from the staged
texts: accents fold to their letter (É with E, Ç with C), anything that
does not start with a letter goes to "others". Run by
tools/localize_portuguese.py after it stages the catalogs; it changes
nothing a save holds (the words are saved by their constants).
"""
from __future__ import annotations

import argparse
import re
import unicodedata
from pathlib import Path

DATA = "src/data/easy_chat"
STRING = re.compile(r'\bconst u8 (\w+)\[\] = _\("((?:[^"\\]|\\.)*)"\);')
WORD_INFO = re.compile(r'\[EC_INDEX\((EC_WORD_\w+)\)\] =\s*\{\s*\.text = (\w+),\s*\.alphabeticalOrder = '
                       r'EC_INDEX\((EC_WORD_\w+)\),', re.S)
TABLE_ENTRY = re.compile(r'\[(MOVE|SPECIES)_(\w+)\] = _\("((?:[^"\\]|\\.)*)"\)')
LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def sort_key(text: str) -> str:
    text = re.sub(r"\{[^}]*\}", "", text).upper()
    # POKéMON's é is part of the name, not an accent to fold.
    folded = unicodedata.normalize("NFD", text)
    return "".join(ch for ch in folded if not unicodedata.combining(ch))


def table(tree: Path, relative: str, kind: str) -> dict[str, str]:
    source = (tree / relative).read_text(encoding="utf-8")
    return {name: text for k, name, text in TABLE_ENTRY.findall(source) if k == kind}


def word_texts(tree: Path) -> tuple[dict[str, str], dict[Path, list[tuple[str, str]]]]:
    """{EC_WORD constant: text}, and each group file's (constant, text) in order."""
    texts, groups = {}, {}
    for path in sorted((tree / DATA).glob("easy_chat_group_*.h")):
        source = path.read_text(encoding="utf-8")
        strings = dict(STRING.findall(source))
        entries = [(const, strings[symbol]) for const, symbol, _ in WORD_INFO.findall(source)]
        if entries:
            groups[path] = entries
            texts.update(entries)
    return texts, groups


def reorder_groups(groups: dict[Path, list[tuple[str, str]]]) -> None:
    """Each group's .alphabeticalOrder: the i-th entry names the i-th word in order."""
    for path, entries in groups.items():
        ordered = [const for const, text in sorted(entries, key=lambda e: (sort_key(e[1]), e[0]))]
        slots = iter(ordered)
        source = path.read_text(encoding="utf-8")

        def put(match: re.Match) -> str:
            return match.group(0)[:match.start(3) - match.start(0)] + next(slots) + match.group(0)[match.end(3) - match.start(0):]

        path.write_text(WORD_INFO.sub(put, source), encoding="utf-8", newline="\n")


ENTRY = re.compile(r"\s*(DOUBLE_SPECIES_NAME\s*(EC_\w+\(\w+\)),\s*(EC_\w+\(\w+\))|(EC_\w+(?:\(\w+\))?)),")


def reorder_letters(tree: Path, words: dict[str, str]) -> None:
    path = tree / DATA / "easy_chat_words_by_letter.h"
    source = path.read_text(encoding="utf-8")
    moves = table(tree, "src/data/text/move_names.h", "MOVE")
    species = table(tree, "src/data/text/species_names.h", "SPECIES")

    def text_of(token: str) -> str:
        m = re.fullmatch(r"(EC_\w+)\((\w+)\)", token)
        if not m:
            return words[token]
        return moves[m[2]] if m[1].startswith("EC_MOVE") else species[m[2]]

    arrays = {}
    for letter in ["Others"] + list(LETTERS):
        m = re.search(r"const u16 gEasyChatWordsByLetter_%s\[\] = \{(.*?)\n\};" % letter, source, re.S)
        arrays[letter] = m
    entries = []
    for m in arrays.values():
        for e in ENTRY.finditer(m.group(1)):
            if e.group(2):
                entries.append((text_of(e.group(2)), "    DOUBLE_SPECIES_NAME\n    %s,\n    %s," % (e.group(2), e.group(3))))
            else:
                entries.append((text_of(e.group(4)), "    %s," % e.group(4)))
    buckets = {letter: [] for letter in arrays}
    for text, line in entries:
        key = sort_key(text)
        buckets[key[0] if key and key[0] in LETTERS else "Others"].append((key, line))
    # From the last array back, so the earlier spans stay valid.
    for letter, m in sorted(arrays.items(), key=lambda kv: kv[1].start(), reverse=True):
        lines = [line for _, line in sorted(buckets[letter], key=lambda kl: kl[0])] if letter != "Others" \
            else [line for _, line in buckets[letter]]
        source = source[:m.start(1)] + "\n" + "\n".join(lines) + source[m.end(1):]
    path.write_text(source, encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tree", type=Path, required=True)
    tree = parser.parse_args().tree.resolve()
    words, groups = word_texts(tree)
    reorder_groups(groups)
    reorder_letters(tree, words)
    print("easy_chat_order: %d words in %d groups, ABC lists rewritten" % (len(words), len(groups)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
