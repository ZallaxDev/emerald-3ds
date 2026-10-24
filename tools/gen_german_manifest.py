#!/usr/bin/env python3
"""Generate tools/locales/german.json.gz and german-symbols.s.gz.

The German counterpart of the other locale manifests: source positions in the
pinned patched tree, ROM offsets, sizes and symbol names, never game text or
graphics. It is made from two builds of the German pret decompilation
(https://github.com/WalterWoshid/pokeemerald-german), which rebuilds both
BPEE and BPED byte for byte:

    make                 -> pokeemerald.gba / pokeemerald.elf      (BPEE)
    sh de_before.sh && make emerald_de
                         -> pokeemerald_de.gba / pokeemerald_de.elf (BPED)

The edit spans are those of the French, Italian and Spanish manifests. Each
span's English bytes (encoded with the tree's preproc) are found in the BPEE
build, inside the symbol the other manifests place it in; the same symbol of
the BPED build gives the German string. Symbols the older German tree names
differently are reached through the pointers of objects both builds share.

    python tools/gen_german_manifest.py --tree build/upstream \\
        --en-elf pokeemerald.elf --en-rom pokeemerald.gba \\
        --de-elf pokeemerald_de.elf --de-rom pokeemerald_de.gba

Nothing from either ROM is written: the outputs hold offsets and symbols only.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import gzip
import hashlib
import json
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder"))
from emerald3ds_builder.recipe import lz77_decompress

GERMAN_SHA1 = "61c2eb2b380b1a75f0c94b767a2d4c26cd7ce4e3"
ENGLISH_SHA1 = "f3ae088181bf583e55daf962a92bb46f4f1d07b7"
LOCALES = ROOT / "tools/locales"
REFERENCES = ("italian", "french", "spanish")
ROM_BASE = 0x08000000
TEXT = ("c", "asm")

# Objects the German tree knows under another name.
ALIASES = {"sBardSoundTemplates_" + n: "gBardSounds_" + n for n in (
    "Actions Adjectives Battle Endings Events Feelings Greetings Hobbies Lifestyle Misc Moves "
    "People Pokemon Speech Status Time Trainer TrendySaying Voices").split()}
ALIASES.update({
    "gFrontierPassMapScreen_Gfx": "gFrontierPassMapAndCard_Gfx",
    "gCreditsCopyrightEnd_Gfx": "sCreditsCopyrightEnd_Gfx",
})
# Row tables whose German width differs from English; the kind fixes the shape.
ROWS = {"u8rows22": lambda n: n == 18 * 22, "u8rows9": lambda n: n == 27,
        "u16rows10": lambda n: n % 20 == 0}
# Fixed-width rows that are one character wider in German (9 keyboard columns).
STRIDE = {"sKeyboardChars": (8, 9)}
# German-only additions: the port's patches reference german_the_end; the
# others are German in BPED alone (no reference language has them): the
# Pokédex start menu one column further right, and the Berry Blender's
# decimal comma.
EXTRA_GRAPHICS = (("graphics/credits/german_the_end.bin.lz", "sCreditsCopyrightEnd_Tilemap"),
                  ("graphics/pokedex/start_menu_main.bin.lz", "gPokedexStartMenuMain_Tilemap"),
                  ("graphics/pokedex/start_menu_search_results.bin.lz", "gPokedexStartMenuSearchResults_Tilemap"),
                  ("graphics/berry_blender/outer.4bpp.lz", "gBerryBlenderOuter_Gfx"))
# Graphics the German tree keeps as unnamed statics: the word in the literal
# pool of a function both builds share (same size) points at the German one.
CODE_REFERENCED = {
    "graphics/pokemon_jump/bonuses.4bpp.lz": ("LoadPokeJumpGfx", 0x13C),
    "graphics/pokemon_jump/bonuses.bin.lz": ("LoadPokeJumpGfx", 0x140),
}
# The other languages' own "THE END" tilemaps are not German data.
SKIP_GRAPHICS = {
    "graphics/credits/spanish_the_end.bin.lz": "ES-only port asset; the German tilemap is added as german_the_end.bin.lz",
    "graphics/credits/french_the_end.bin.lz": "FR-only port asset",
    "graphics/credits/italian_the_end.bin.lz": "IT-only port asset",
}
# Labels of the cartridge header, which is never text.
HEADER = "RomHeader"
# Easy-chat speeches no reference localizes; German picks other words.
WORD_TABLES = {"src/battle_pike.c": ("sNPCSpeeches",)}
FLOORS = ("sFloors_Normal", "sFloors_Variety", "sFloors_Unique", "sFloors_Expert")
# The German branches of the port's patches declare the strings only the German
# ROM has as empty literals under their decomp names; they are filled from it.
GERMAN_BRANCH = "#if defined(PORT_BRIDGE) && GAME_LANGUAGE == LANGUAGE_GERMAN"
GERMAN_ONLY = re.compile(r'\b(\w+)\[\]\s*=\s*(_\(""\))')


def elf_symbols(path: Path) -> dict[str, tuple[int, int, str]]:
    """name -> (address, size, 'func' | 'object' | 'other') from an ELF32 symtab."""
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise SystemExit("%s is not an ELF32 file" % path)
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    symbols: dict[str, tuple[int, int, str]] = {}
    copies: dict[str, list[tuple[str, tuple[int, int, str]]]] = {}
    for sh in sections:
        if sh[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        source = ""
        for offset in range(sh[4], sh[4] + sh[5], 16):
            name_off, value, size, info, _, shndx = struct.unpack_from("<IIIBBH", data, offset)
            if not name_off:
                continue
            start = strtab[4] + name_off
            name = data[start:data.index(b"\0", start)].decode("ascii", "replace")
            if info & 0xF == 4:  # STT_FILE: the local symbols after it are this file's
                source = name
                continue
            if shndx == 0:
                continue
            kind = {1: "object", 2: "func"}.get(info & 0xF, "other")
            if name.startswith((".L", "$")):
                continue
            symbols.setdefault(name, (value, size, kind))
            if not info >> 4:  # STB_LOCAL
                copies.setdefault(name, []).append((source, (value, size, kind)))
    # A static name used in several files (sText_PleaseWaitAWhile) is kept per
    # file too, name@file, so each copy can be found in the other build.
    for name, found in copies.items():
        if len(found) > 1:
            for source, symbol in found:
                symbols.setdefault("%s@%s" % (name, source.rsplit("/", 1)[-1]), symbol)
    return symbols


def reference_symbols(language: str) -> dict[str, tuple[int, int]]:
    text = gzip.decompress((LOCALES / (language + "-symbols.s.gz")).read_bytes()).decode()
    values, symbols = {}, {}
    for line in text.splitlines():
        if line.startswith(".equ "):
            name, value = line[5:].split(", ")
            values[name] = int(value)
        elif line.startswith(".size "):
            name, value = line[6:].split(", ")
            symbols[name] = (values.get(name, 0), int(value))
    return symbols


class Index:
    """ROM objects by address: the innermost sized one, else the label before."""

    def __init__(self, symbols):
        items = sorted((a - ROM_BASE, s, n) for n, (a, s, *_) in symbols.items()
                       if ROM_BASE <= a < ROM_BASE + 0x2000000)
        self.items, self.addresses = items, [i[0] for i in items]

    def containing(self, offset: int):
        index = bisect.bisect_right(self.addresses, offset) - 1
        label = None
        while index >= 0 and offset - self.addresses[index] < 0x40000:
            address, size, name = self.items[index]
            if size and address <= offset < address + size:
                return name, offset - address
            if label is None and size == 0:
                label = (name, offset - address)
            index -= 1
        return label


def encode(tree: Path, spans: list[tuple[str, str]]) -> list[bytes]:
    """English bytes of each (kind, source text) span, through the tree's preproc."""
    preproc = [str(tree / "tools/preproc/preproc")]
    charmap = str(tree / "charmap.txt")
    c_items = [t for k, t in spans if k == "c"]
    s_items = [t for k, t in spans if k == "asm"]
    with tempfile.TemporaryDirectory() as temp:
        c_path, s_path = Path(temp, "x.c"), Path(temp, "x.s")
        c_path.write_text("".join("X%d = %s;\n" % (i, t.replace("\n", " ")) for i, t in enumerate(c_items)),
                          encoding="utf-8")
        s_path.write_text("".join("X%d:\n%s\n" % (i, t) for i, t in enumerate(s_items)), encoding="utf-8")
        c_out = subprocess.run(preproc + [str(c_path), charmap], capture_output=True, text=True, check=True).stdout
        s_out = subprocess.run(preproc + [str(s_path), charmap], capture_output=True, text=True, check=True).stdout
    c_bytes = []
    for line in c_out.splitlines():
        if line.startswith("X"):
            body = line[line.index("{") + 1:line.rindex("}")] if "}" in line else ""
            c_bytes.append(bytes(int(x, 16) for x in body.replace(" ", "").split(",") if x))
    s_bytes, current = [], None
    for line in s_out.splitlines():
        if line.startswith("X") and line.endswith(":"):
            if current is not None:
                s_bytes.append(bytes(current))
            current = []
        elif line.strip().startswith(".byte") and current is not None:
            current += [int(x, 16) for x in line.strip()[5:].replace(" ", "").split(",") if x]
    if current is not None:
        s_bytes.append(bytes(current))
    if len(c_bytes) != len(c_items) or len(s_bytes) != len(s_items):
        raise SystemExit("preproc output does not match the spans")
    c_iter, s_iter = iter(c_bytes), iter(s_bytes)
    return [next(c_iter) if k == "c" else next(s_iter) for k, _ in spans]


class Builds:
    def __init__(self, en_elf, en_rom, de_elf, de_rom):
        self.en, self.de = elf_symbols(en_elf), elf_symbols(de_elf)
        self.en_rom, self.de_rom = en_rom.read_bytes(), de_rom.read_bytes()
        for rom, sha in ((self.en_rom, ENGLISH_SHA1), (self.de_rom, GERMAN_SHA1)):
            if hashlib.sha1(rom).hexdigest() != sha:
                raise SystemExit("A build does not match its cartridge (expected SHA-1 %s)" % sha)
        self.en_index = Index(self.en)

    def de_name(self, name):
        for candidate in (name, ALIASES.get(name), ("s" + name[1:]) if name[:1] == "g" else None,
                          ("g" + name[1:]) if name[:1] == "s" else None):
            if candidate and candidate in self.de:
                return candidate
        return None

    def en_name(self, name):
        for candidate in (name, ALIASES.get(name)):
            if candidate and candidate in self.en:
                return candidate
        return None

    def follow(self, en_offset):
        """German offset of an English object, through pointers from shared objects."""
        target = struct.pack("<I", en_offset + ROM_BASE)
        found, hit = set(), self.en_rom.find(target)
        while hit >= 0:
            if hit % 4 == 0:
                holder = self.en_index.containing(hit)
                if holder and holder[0] in self.de and self.en[holder[0]][1] == self.de[holder[0]][1] \
                        and self.en[holder[0]][1]:
                    pointer, = struct.unpack_from("<I", self.de_rom, self.de[holder[0]][0] - ROM_BASE + holder[1])
                    found.add(pointer - ROM_BASE)
            hit = self.en_rom.find(target, hit + 1)
        return found.pop() if len(found) == 1 else None

    def de_object(self, en_name):
        """(German offset, German size) of the object an English symbol names."""
        name = self.de_name(en_name)
        if name:
            return self.de[name][0] - ROM_BASE, self.de[name][1]
        if en_name in self.en:
            address = self.follow(self.en[en_name][0] - ROM_BASE)
            if address is not None:
                return address, self.en[en_name][1]
        return None


def nth_string(rom: bytes, start: int, count: int) -> int:
    offset = start
    for _ in range(count):
        offset = rom.index(b"\xff", offset) + 1
    return offset


def in_data(builds: Builds, offset: int) -> bool:
    """Whether an English offset is in a data object, not code or the header."""
    hit = builds.en_index.containing(offset)
    return bool(hit) and not hit[0].startswith(HEADER) and builds.en[hit[0]][2] != "func"


def whole_string(builds: Builds, offset: int) -> bool:
    """Whether an English match starts a string in a data object."""
    hit = builds.en_index.containing(offset)
    return in_data(builds, offset) and (hit[1] == 0 or builds.en_rom[offset - 1] in (0x00, 0xFF))


def textlike(data: bytes, want: bytes) -> bool:
    """A translation of want: mostly letters, digits, punctuation and control
    codes (no tiles or code), with the same placeholders and control codes."""
    plain = sum(b == 0 or 0xA1 <= b <= 0xF6 or b >= 0xF7 for b in data)
    return (data.endswith(b"\xff") and plain * 4 >= len(data) * 3
            and (len(data) > 1 or len(want) == 1)
            and all(data.count(c) == want.count(c) for c in (b"\xfc", b"\xfd")))


def place_text(builds: Builds, en_offset: int, want: bytes, kind: str):
    """German (offset, size) of the English string at en_offset, or a reason."""
    hit = builds.en_index.containing(en_offset)
    if not hit:
        return None, "no English symbol"
    name, rel = hit
    if name.startswith(HEADER):
        return None, "English match in the cartridge header"
    if name in STRIDE:
        if b"\xff" in want or name not in builds.de:
            return None, "unexpected keyboard row"
        narrow, wide = STRIDE[name]
        return (builds.de[name][0] - ROM_BASE + rel // narrow * wide + rel % narrow, wide), name
    en_start, en_size = builds.en[name][0] - ROM_BASE, builds.en[name][1]
    target = builds.de_object(name)
    if target is None:
        return None, "symbol %s has no German counterpart" % name
    de_start, de_size = target
    if rel and en_size and en_size == de_size and kind == "c":
        offset = de_start + rel                       # names in fixed-size rows and fields
    else:
        strings = builds.en_rom[en_start:en_offset].count(b"\xff")
        if rel and not strings:
            if en_size != de_size:
                return None, "layout of %s differs" % name
            offset = de_start + rel
        else:
            offset = nth_string(builds.de_rom, de_start, strings)
    terminators = want.count(b"\xff")
    size = nth_string(builds.de_rom, offset, terminators) - offset if terminators else len(want)
    if not 0 < size <= 4096:
        return None, "implausible German size"
    return (offset, size), name


def text_edits(builds: Builds, tree: Path):
    manifests = {l: json.loads(gzip.decompress((LOCALES / (l + ".json.gz")).read_bytes())) for l in REFERENCES}
    indexes = {l: Index(reference_symbols(l)) for l in REFERENCES}
    spans, digests = collections.defaultdict(dict), {}
    for language in REFERENCES:
        for relative, file in manifests[language]["files"].items():
            digests[relative] = file["sha256"]
            for start, end, offset, size, kind in file["edits"]:
                spans[relative].setdefault((start, end, kind), []).append((language, offset, size))
    texts = {}
    for relative in spans:
        text = (tree / relative).read_text(encoding="utf-8")
        if hashlib.sha256(text.encode()).hexdigest() != digests[relative]:
            raise SystemExit("Source differs from the pinned patched tree: " + relative)
        texts[relative] = text
    order = [(f, key) for f in sorted(spans) for key in sorted(spans[f])]
    english = iter(encode(tree, [(k, texts[f][s:e]) for f, (s, e, k) in order if k in TEXT]))
    files, unmapped, stats = collections.defaultdict(list), [], collections.Counter()
    for relative, (start, end, kind) in order:
        # Some references found one-byte strings (the empty placeholders) in the
        # ROM header; take the reference that names the string's own object.
        candidates = [(language, offset, size, indexes[language].containing(offset))
                      for language, offset, size in spans[relative][(start, end, kind)]]
        language, offset, size, hit = next(
            (c for c in candidates if c[3] and not c[3][0].startswith(HEADER)), candidates[0])
        name, rel = hit if hit else (None, 0)
        name = builds.en_name(name) if name else None
        if kind in TEXT:
            want = next(english)
            guess = None
            if name:
                en_start, en_size = builds.en[name][0] - ROM_BASE, builds.en[name][1]
                if builds.en_rom[en_start + rel:en_start + rel + len(want)] == want:
                    guess = en_start + rel
                else:
                    found = builds.en_rom.find(want, en_start, en_start + max(en_size, 0x4000) + len(want))
                    guess = found if found >= 0 and whole_string(builds, found) else None
                if guess is not None and not in_data(builds, guess):
                    guess = None
            if guess is None:
                # Not inside the expected object: accept a unique German reading.
                hits, found = [], builds.en_rom.find(want)
                while found >= 0 and len(hits) < 64:
                    hits.append(found)
                    found = builds.en_rom.find(want, found + 1)
                # Only whole strings in data: not the tail of a longer string, not code.
                hits = [h for h in hits if whole_string(builds, h)]
                placed = [place_text(builds, h, want, kind)[0] for h in hits]
                placed = [p if p and textlike(builds.de_rom[p[0]:p[0] + p[1]], want) else None for p in placed]
                readings = {builds.de_rom[p[0]:p[0] + p[1]] for p in placed if p}
                if hits and len(readings) == 1:
                    guess = hits[placed.index(next(p for p in placed if p))]
                else:
                    stats["unmapped"] += 1
                    unmapped.append({"file": relative, "kind": kind, "start": start,
                                     "reason": "English text not found in a unique place (%d hits, %d German readings); stays English"
                                               % (len(hits), len(readings))})
                    continue
            placed, why = place_text(builds, guess, want, kind)
            if placed is None:
                stats["unmapped"] += 1
                unmapped.append({"file": relative, "kind": kind, "start": start, "reason": why + "; stays English"})
                continue
            files[relative].append([start, end, placed[0], placed[1], kind])
            stats["text"] += 1
            continue
        target = builds.de_object(name) if name else None
        if target is None:
            stats["unmapped"] += 1
            unmapped.append({"file": relative, "kind": kind, "start": start, "reason": "no German object"})
            continue
        de_start, de_size = target
        en_size = builds.en[name][1]
        if kind in ROWS:
            if rel or not ROWS[kind](de_size):
                raise SystemExit("%s: German %s is %d bytes, not the %s shape" % (relative, name, de_size, kind))
            files[relative].append([start, end, de_start, de_size, kind])
        elif en_size != de_size:
            if kind != "u16array" or rel:
                raise SystemExit("%s: %s is %d bytes in English, %d in German" % (relative, name, en_size, de_size))
            files[relative].append([start, end, de_start, de_size, kind])
        else:
            files[relative].append([start, end, de_start + rel, size, kind])
        stats["data"] += 1
    return {f: {"sha256": digests[f], "edits": v} for f, v in sorted(files.items())}, unmapped, stats


def word_table_edits(builds: Builds, tree: Path, files: dict, stats) -> None:
    """Easy-chat word tables no reference localizes, whose German words differ."""
    for relative, names in WORD_TABLES.items():
        text = (tree / relative).read_text(encoding="utf-8")
        digest = hashlib.sha256(text.encode()).hexdigest()
        entry = files.setdefault(relative, {"sha256": digest, "edits": []})
        if entry["sha256"] != digest:
            raise SystemExit("Source differs from the pinned patched tree: " + relative)
        for name in names:
            match = re.search(r"\b%s\[[^=]*=\s*\{" % name, text)
            if not match or name not in builds.de or builds.en[name][1] != builds.de[name][1]:
                raise SystemExit("%s: %s does not have the English shape" % (relative, name))
            depth, end = 1, match.end()
            while depth:
                depth += {"{": 1, "}": -1}.get(text[end], 0)
                end += 1
            address, size, _ = builds.de[name]
            entry["edits"].append([match.end(), end - 1, address - ROM_BASE, size, "u16array"])
            stats["data"] += 1
        entry["edits"].sort()


def german_only_edits(builds: Builds, tree: Path, files: dict, stats) -> None:
    """Spans for the empty strings the German branches declare."""
    for path in sorted((tree / "src").glob("*.c")):
        text = path.read_text(encoding="utf-8")
        if GERMAN_BRANCH not in text:
            continue
        relative = path.relative_to(tree).as_posix()
        edits, offset, stack = [], 0, []
        for line in text.splitlines(keepends=True):
            directive = line.strip()
            if directive.startswith("#if"):
                stack.append(directive == GERMAN_BRANCH)
            elif directive.startswith("#el"):
                if stack:
                    stack[-1] = False
            elif directive.startswith("#endif"):
                if stack:
                    stack.pop()
            elif any(stack):
                for match in GERMAN_ONLY.finditer(line):
                    name = match.group(1)
                    if name not in builds.de:
                        raise SystemExit("%s: no German object %s" % (relative, name))
                    address, size, _ = builds.de[name]
                    start = address - ROM_BASE
                    end = builds.de_rom.find(b"\xff", start, start + size)
                    if end < 0:
                        raise SystemExit("%s: German %s is not a string" % (relative, name))
                    edits.append([offset + match.start(2), offset + match.end(2), start, end + 1 - start, "c"])
            offset += len(line)
        if not edits:
            continue
        digest = hashlib.sha256(text.encode()).hexdigest()
        entry = files.setdefault(relative, {"sha256": digest, "edits": []})
        if entry["sha256"] != digest:
            raise SystemExit("Source differs from the pinned patched tree: " + relative)
        entry["edits"] = sorted(entry["edits"] + edits)
        stats["german only"] += len(edits)
        stats["text"] += len(edits)


def lz77_size(rom: bytes, offset: int) -> int:
    """Compressed length of the LZ77 stream at offset, padded to 4 as stored."""
    size, out, pos = int.from_bytes(rom[offset + 1:offset + 4], "little"), 0, offset + 4
    while out < size:
        flags = rom[pos]
        pos += 1
        for bit in range(8):
            if out >= size:
                break
            if flags & (0x80 >> bit):
                out += (rom[pos] >> 4) + 3
                pos += 2
            else:
                out += 1
                pos += 1
    return (pos - offset + 3) & ~3


def code_referenced(builds: Builds, relative: str):
    function, at = CODE_REFERENCED[relative]
    en, de = builds.en.get(function), builds.de.get(function)
    if not en or not de or en[1] != de[1]:
        raise SystemExit("%s: %s differs between the builds" % (relative, function))
    offset = struct.unpack_from("<I", builds.de_rom, (de[0] & ~1) - ROM_BASE + at)[0] - ROM_BASE
    return [relative, offset, lz77_size(builds.de_rom, offset)]


def graphics(builds: Builds):
    seen, out, skipped, decompressed = {}, [], [], 0
    for language in REFERENCES:
        manifest = json.loads(gzip.decompress((LOCALES / (language + ".json.gz")).read_bytes()))
        index = Index(reference_symbols(language))
        for relative, offset, _ in manifest["graphics"]:
            seen.setdefault(relative, index.containing(offset))
    for relative, hit in sorted(seen.items()):
        if relative in CODE_REFERENCED:
            out.append(code_referenced(builds, relative))
            continue
        if relative in SKIP_GRAPHICS:
            skipped.append([relative, SKIP_GRAPHICS[relative]])
            continue
        target = builds.de_object(hit[0]) if hit and hit[1] == 0 else None
        if target is None:
            skipped.append([relative, "no German object for %s" % (hit[0] if hit else "?")])
            continue
        out.append([relative, target[0], target[1]])
    for relative, name in EXTRA_GRAPHICS:
        out.append([relative, builds.de[name][0] - ROM_BASE, builds.de[name][1]])
    for relative, offset, size in out:
        if relative.endswith(".lz"):
            lz77_decompress(builds.de_rom, offset)
            decompressed += 1
    return out, skipped, decompressed


def symbols_file(builds: Builds) -> bytes:
    lines = [".syntax unified"]
    for name, (address, size, kind) in sorted(builds.de.items()):
        if not ROM_BASE <= address < ROM_BASE + 0x2000000 or "@" in name:
            continue
        if kind == "func":
            address, size = 0, 0
        lines += [".global " + name, ".type %s, %%%s" % (name, "function" if kind == "func" else "object"),
                  ".equ %s, %d" % (name, address), ".size %s, %d" % (name, size)]
    return gzip.compress(("\n".join(lines) + "\n").encode(), mtime=0)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--en-elf", type=Path, required=True)
    parser.add_argument("--en-rom", type=Path, required=True)
    parser.add_argument("--de-elf", type=Path, required=True)
    parser.add_argument("--de-rom", type=Path, required=True)
    args = parser.parse_args()
    builds = Builds(args.en_elf, args.en_rom, args.de_elf, args.de_rom)
    files, unmapped, stats = text_edits(builds, args.tree.resolve())
    word_table_edits(builds, args.tree.resolve(), files, stats)
    german_only_edits(builds, args.tree.resolve(), files, stats)
    files = dict(sorted(files.items()))
    gfx, skipped, decompressed = graphics(builds)
    credits = builds.de["sCreditsEntryPointerTable"]
    floors = []
    for name in FLOORS:
        address, size, _ = builds.de[name]
        floors.append([name, address - ROM_BASE, size])
    manifest = {
        "version": "0.1-de", "rom_sha1": GERMAN_SHA1, "status": "experimental",
        "files": files, "graphics": gfx, "graphics_skipped": skipped,
        "credits": credits[0] - ROM_BASE, "credits_entries": credits[1] // 4,
        "floors": floors, "unmapped_source_texts": unmapped,
        "checks": {"lz77_decompressions": decompressed, "text_edits": stats["text"],
                   "data_edits": stats["data"], "unmapped": stats["unmapped"]},
    }
    (LOCALES / "german.json.gz").write_bytes(
        gzip.compress(json.dumps(manifest, separators=(",", ":")).encode(), mtime=0))
    (LOCALES / "german-symbols.s.gz").write_bytes(symbols_file(builds))
    print("german.json.gz: %d text (%d German-only), %d data edits, %d unmapped, %d graphics (%d skipped)"
          % (stats["text"], stats["german only"], stats["data"], stats["unmapped"], len(gfx), len(skipped)))


if __name__ == "__main__":
    main()
