"""The Chinese glyph sheets against the charmap, on the host.

A Han character is two bytes (the first 0x01-0x1E minus the two the Latin texts
need, the second 0x00-0xF6); the sentence punctuation is two bytes as well, the
lead 0x1F and 0xF7-0xFF. Slot n of a sheet holds the character whose arithmetic
gives n, so this checks the three places that must agree:

  - charmap.txt, the codes a text is compiled with;
  - src/chinese_text.c's SheetIndex/PunctIndex, which turn a code into a slot;
  - graphics/fonts/chinese_*.png, one glyph per slot.

Run it on the bootstrapped tree, where the patch has been applied:

    python 3ds_port/tests/chinese_text_test.py --tree build/upstream
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

HAN_LEAD_MIN, HAN_LEAD_MAX = 0x01, 0x1E
HAN_LEAD_HOLES = {0x06, 0x1B}   # é and É, which POKéMON needs
HAN_LO_MAX = 0xF6
PUNCT_HI = 0x1E                 # the charmap's first byte for every mark
PUNCT_LO_MIN = 0x71             # 。, then 、，！？：—～・
PUNCT_SLOT = 7026               # the sheet slot of 。 (7025 is stray art)


def failures(problems: list[str], what: str) -> int:
    for p in problems:
        print("chinese_text_test: " + p, file=sys.stderr)
    return 1 if problems else 0


def sheet_index(hi: int, lo: int) -> int:
    if hi > 0x1B:
        hi -= 1
    if hi > 0x06:
        hi -= 1
    return ((hi - 1) << 8) | lo


def read_charmap(tree: Path) -> dict[str, list[list[int]]]:
    """character -> the byte sequences the charmap gives it.

    A sequence is written as bytes of their own ("1F F7") or, for a Han
    character, as one four-digit word ("10A1"); both are the same bytes."""
    out: dict[str, list[list[int]]] = {}
    for line in (tree / "charmap.txt").read_text(encoding="utf-8").splitlines():
        line = line.split("@")[0].rstrip()
        m = re.match(r"^'(.+?)'\s*=\s*([0-9A-Fa-f ]+)$", line)
        if not m:
            continue
        codes: list[int] = []
        for token in m.group(2).split():
            if len(token) == 4:
                codes += [int(token[:2], 16), int(token[2:], 16)]
            else:
                codes.append(int(token, 16))
        out.setdefault(m.group(1), []).append(codes)
    return out


def sheet_slots(png: Path) -> tuple[int, int, int]:
    """(glyph count, bit depth, colour type) of a .latfont-shaped PNG."""
    data = png.read_bytes()
    width, height = struct.unpack(">II", data[16:24])
    depth, colour = data[24], data[25]
    return (width // 16) * (height // 16), depth, colour


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", type=Path, required=True)
    args = ap.parse_args()
    tree = args.tree.resolve()

    charmap = read_charmap(tree)
    problems: list[str] = []

    # 1. Every Han character's first byte is one the engine accepts, and its
    #    slot arithmetic lands below the punctuation, without collisions.
    slots: dict[int, str] = {}
    punct_codes: list[int] = []
    for ch, codes in charmap.items():
        for code in codes:
            if len(code) == 2:
                hi, lo = code
                if hi == PUNCT_HI and PUNCT_LO_MIN <= lo <= 0xFF:
                    punct_codes.append(lo)
                    continue
                if not (HAN_LEAD_MIN <= hi <= HAN_LEAD_MAX):
                    problems.append("%r = %02X %02X: first byte outside 01-1E"
                                    % (ch, hi, lo))
                    continue
                if hi in HAN_LEAD_HOLES:
                    problems.append("%r = %02X %02X: first byte is one the Latin "
                                    "accents need" % (ch, hi, lo))
                    continue
                if lo > HAN_LO_MAX:
                    problems.append("%r = %02X %02X: second byte above F6"
                                    % (ch, hi, lo))
                    continue
                slot = sheet_index(hi, lo)
                if slot in slots:
                    problems.append("%r and %r share slot %d" % (ch, slots[slot], slot))
                slots[slot] = ch

    # 2. The punctuation is a contiguous run from PUNCT_FIRST, one code each.
    punct_codes.sort()
    expect = list(range(PUNCT_LO_MIN, PUNCT_LO_MIN + len(punct_codes)))
    if punct_codes != expect:
        problems.append("punctuation codes %s are not the run %02X..%02X"
                        % ([hex(c) for c in punct_codes], PUNCT_LO_MIN,
                           PUNCT_LO_MIN + len(punct_codes) - 1))
    if punct_codes and slots and max(slots) >= PUNCT_SLOT:
        problems.append("Han slots run to %d, into the punctuation at %d"
                        % (max(slots), PUNCT_SLOT))


    # 3. The sheets hold those slots.
    needed = max([max(slots) + 1 if slots else 0,
                  PUNCT_SLOT + len(punct_codes)])
    for name in ("chinese_normal.png", "chinese_small.png"):
        png = tree / "graphics/fonts" / name
        if not png.is_file():
            png = Path(__file__).resolve().parents[1] / "assets/fonts" / name
        if not png.is_file():
            problems.append("%s is missing" % name)
            continue
        count, depth, colour = sheet_slots(png)
        if depth != 8 or colour != 3:
            problems.append("%s is %d-bit colour type %d; a .latfont PNG is 8-bit "
                            "indexed" % (name, depth, colour))
        if count < needed:
            problems.append("%s holds %d glyphs, the charmap needs %d"
                            % (name, count, needed))

    if problems:
        return failures(problems, "Chinese glyph sheets")

    print("chinese_text_test: %d Han characters, %d punctuation marks, "
          "slots 0..%d, sheets hold them" % (len(slots), len(punct_codes), needed - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
