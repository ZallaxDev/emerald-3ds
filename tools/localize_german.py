#!/usr/bin/env python3
"""Stage the BPED localization from a user's clean German ROM.

German counterpart of tools/localize_italian.py: the same manifest layout and
validation rules - the checked-in manifest contains
source positions, ROM offsets, symbol names and sizes, never game text or
graphics. Run after bootstrap applies the pinned source patches.

Verification status: the manifest is assembled and self-checked against BPED
(terminated text sizes are compared with the terminator distance in the ROM;
raw __() buffers are instead bounded by their source's encoded capacity;
105 graphics are located and checked against their bounded retail assets;
the 275 credits entries and the four Trainer Hill floor
tables are derived from the ROM directly - the credits table was found by
structure, the floors by a verified constant delta). New integrations still
require their own hardware play testing.
"""
from __future__ import annotations

import argparse
import ast
import gzip
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder"))
sys.path.insert(0, str(ROOT / "tools"))
from locale_lists import stage_id_lists_at_offsets
from emerald3ds_builder.rom import load_rom
from emerald3ds_builder.recipe import lz77_decompress

GERMAN_SHA1 = "61c2eb2b380b1a75f0c94b767a2d4c26cd7ce4e3"


def encoded(data: bytes) -> str:
    return ",".join("0x%02X" % byte for byte in data)


def _text_literal_value(literal: str) -> str:
    # Preserve the game's non-C scroll/paragraph escapes without AST warnings.
    literal = re.sub(r"\\(.)", lambda m: "\\" + m[0] if m[1] in "lp" else m[0], literal, flags=re.S)
    return ast.literal_eval(literal)


def raw_text_charmap(tree: Path) -> tuple[dict[str, int], dict[str, int]]:
    """Read glyph/control byte widths without font assets or an implicit EOS."""
    glyphs, controls = {}, {}
    for line in (tree / "charmap.txt").read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"('(?:\\.|[^'\\])*'|\w+)\s*=\s*((?:[0-9A-Fa-f]{2}\s*)+)",
                             line.split("@", 1)[0].strip())
        if match is None:
            continue
        key, data = match.groups()
        width = len(re.findall(r"[0-9A-Fa-f]{2}", data))
        if key.startswith("'"):
            glyphs[_text_literal_value(key)] = width
        else:
            controls[key] = width
    return glyphs, controls


def raw_text_capacity(initializer: str, glyphs: dict[str, int], controls: dict[str, int]) -> int:
    """Measure a literal __() initializer, including every '$' padding byte."""
    match = re.fullmatch(r"__\s*\((.*)\)", initializer.strip(), re.S)
    literal = r'"(?:\\.|[^"\\])*"'
    if match is None or re.fullmatch(r"\s*(?:" + literal + r"\s*)+", match[1], re.S) is None:
        raise ValueError("Invalid raw text initializer")
    try:
        text = "".join(_text_literal_value(part) for part in re.findall(literal, match[1], re.S))
    except (SyntaxError, ValueError) as exc:
        raise ValueError("Invalid raw text initializer") from exc
    capacity = 0
    for token in re.finditer(r"\{([^{}]*)\}|\\[lp]|.", text, re.S):
        if token[1] is not None:
            names = token[1].split()
            if not names or any(name not in controls for name in names):
                raise ValueError("Unknown raw text control: " + token[0])
            capacity += sum(controls[name] for name in names)
        else:
            if token[0] not in glyphs:
                raise ValueError("Unknown raw text glyph: " + repr(token[0]))
            capacity += glyphs[token[0]]
    return capacity


def stage(tree: Path, rom_path: Path) -> None:
    manifest = json.loads(gzip.decompress((ROOT / "tools/locales/german.json.gz").read_bytes()))
    rom = load_rom(rom_path)
    if rom.sha1 != manifest["rom_sha1"] or rom.sha1 != GERMAN_SHA1:
        raise ValueError("This localization requires the clean German BPED ROM")
    source = rom.data
    writes: dict[Path, bytes] = {}
    charmap = None  # Existing numeric-only staging does not need charmap.txt.
    for relative, file in manifest["files"].items():
        path = tree / relative
        text = path.read_text(encoding="utf-8")
        if hashlib.sha256(text.encode()).hexdigest() != file["sha256"]:
            raise ValueError("Source differs from the pinned patched tree: " + relative)
        previous_end = 0
        for start, end, offset, size, kind in sorted(file["edits"]):
            if not previous_end <= start < end <= len(text) or not 0 <= offset < offset + size <= len(source):
                raise ValueError("Overlapping or out-of-range edit: " + relative)
            previous_end = end
        for start, end, offset, size, kind in sorted(file["edits"], reverse=True):
            data = source[offset:offset + size]
            if kind == "c":
                initializer = text[start:end]
                if re.match(r"\s*__\s*\(", initializer):
                    if charmap is None:
                        charmap = raw_text_charmap(tree)
                    capacity = raw_text_capacity(initializer, *charmap)
                    if size > capacity:
                        raise ValueError("Raw text replacement exceeds source capacity: "
                                         + relative + " at " + str(start)
                                         + " (size %d, capacity %d)" % (size, capacity))
                replacement = "{" + encoded(data) + "}"
            elif kind == "u8array":
                replacement = "{" + encoded(data) + "}"
            elif kind == "asm":
                replacement = "\n\t.byte " + encoded(data) + "\n"
            elif kind == "u32":
                replacement = str(struct.unpack("<I", data)[0])
            elif kind == "u16s":
                replacement = ",".join("0x%04X" % n for n in struct.unpack("<6H", data))
            elif kind == "u16array":
                replacement = ",".join("0x%04X" % n for n in struct.unpack("<%dH" % (size // 2), data))
            elif kind == "u16rows2":
                if size % 4:
                    raise ValueError("Invalid phrase word table dimensions")
                replacement = ",".join(
                    "{" + ",".join("0x%04X" % n for n in struct.unpack_from("<2H", data, index)) + "}"
                    for index in range(0, size, 4)
                )
            elif kind == "u8pages4x9":
                if size != 3 * 4 * 9:
                    raise ValueError("Invalid naming keyboard table dimensions")
                rows = ["{" + encoded(data[i:i + 9]) + "}" for i in range(0, size, 9)]
                replacement = "{" + ",".join("{" + ",".join(rows[i:i + 4]) + "}"
                                             for i in range(0, 12, 4)) + "}"
            elif kind == "u8rows9":
                if size != 3 * 9:
                    raise ValueError("Invalid naming column table dimensions")
                replacement = "{" + ",".join("{" + encoded(data[i:i + 9]) + "}"
                                             for i in range(0, size, 9)) + "}"
            elif kind == "u8rows22":
                if size != 18 * 22:
                    raise ValueError("Invalid type name table dimensions")
                replacement = "{" + ",".join("{" + encoded(data[i:i + 22]) + "}"
                                             for i in range(0, size, 22)) + "}"
            elif kind == "u16rows10":
                if size % 20:
                    raise ValueError("Invalid mail word table dimensions")
                rows = [",".join("0x%04X" % n for n in struct.unpack_from("<10H", data, i))
                        for i in range(0, size, 20)]
                replacement = "{" + ",".join("{" + row + "}" for row in rows) + "}"
            elif kind == "bard":
                if size % 48:
                    raise ValueError("Invalid bard table dimensions")
                phonemes = []
                for index in range(0, size, 8):
                    if data[index + 6:index + 8] != b"\0\0":
                        raise ValueError("Unexpected bard record padding")
                    fields = struct.unpack_from("<BbHh", data, index)
                    phonemes.append("{" + ",".join(str(n) for n in fields) + "}")
                replacement = "{" + ",".join("{" + ",".join(phonemes[i:i + 6]) + "}"
                                             for i in range(0, len(phonemes), 6)) + "}"
            else:
                raise ValueError("Unknown edit kind: " + kind)
            text = text[:start] + replacement + text[end:]
        writes[path] = text.encode()

    for relative, offset, size in manifest["graphics"]:
        if not 0 <= offset < offset + size <= len(source):
            raise ValueError("Graphic outside ROM: " + relative)
        data = source[offset:offset + size]
        writes[tree / relative] = data
        if relative.endswith(".lz"):
            writes[tree / relative[:-3]] = lz77_decompress(source, offset)
    # Assets the German ROM does not carry in the port's newer layout stay
    # English; the manifest records them so this is a decision, not an omission.

    # The German credits have 55 pages of 5 entries, like the other variants.
    entries = []
    for index in range(manifest["credits_entries"]):
        record = struct.unpack_from("<I", source, manifest["credits"] + index * 4)[0] - 0x08000000
        if not 0 <= record <= len(source) - 8:
            raise ValueError("Invalid credits record")
        raw = source[record:record + 8]
        offset = struct.unpack_from("<I", raw, 4)[0] - 0x08000000
        end = source.find(b"\xff", offset, offset + 200) if 0 <= offset < len(source) else -1
        if end < 0 or raw[0] > 40 or raw[1] not in (0, 1) or raw[2:4] != b"\0\0":
            raise ValueError("Invalid credits text")
        entries.append((record, offset, end + 1, raw[0], raw[1]))
    pages = manifest["credits_entries"] // 5
    lines = ["enum { PAGE_COUNT = %d };" % pages, "#define ENTRIES_PER_PAGE 5"]
    texts, records = {}, {}
    for record, offset, end, unknown, title in entries:
        if offset not in texts:
            texts[offset] = "sGermanCreditsText_" + str(offset)
            lines.append("static const u8 " + texts[offset] + "[] = {" + encoded(source[offset:end]) + "};")
        if record not in records:
            records[record] = "sGermanCreditsEntry_" + str(record)
            lines.append("static const struct CreditsEntry " + records[record] + " = {"
                         + str(unknown) + "," + str(title) + "," + texts[offset] + "};")
    lines.append("static const struct CreditsEntry *const sCreditsEntryPointerTable[PAGE_COUNT][ENTRIES_PER_PAGE] = {")
    for index in range(0, len(entries), 5):
        lines.append("{" + ",".join("&" + records[e[0]] for e in entries[index:index + 5]) + "},")
    lines.append("};")
    writes[tree / "src/data/credits.h"] = ("\n".join(lines) + "\n").encode()

    path = tree / "src/data/battle_frontier/trainer_hill.h"
    text = path.read_text(encoding="utf-8")
    for name, offset, size in manifest["floors"]:
        if size != 4 * 952 or not 0 <= offset < offset + size <= len(source):
            raise ValueError("Invalid Trainer Hill floor layout")
        match = re.search(r"static const struct TrainerHillFloor " + name + r"\[\]\s*=\s*\{", text)
        if match is None:
            raise ValueError("Missing floor definition: " + name)
        end, depth = match.end(), 1
        while depth:
            char = text[end]
            end += 1
            depth += (char == "{") - (char == "}")
        if text[end] != ";":
            raise ValueError("Invalid floor initializer")
        blob = "build/private_data/" + name + ".bin"
        writes[tree / blob] = source[offset:offset + size]
        assembly = ('.section .rodata.' + name + ',"a"\n.balign 4\n.global ' + name
                    + '\n.type ' + name + ', %object\n' + name + ':\n.incbin "' + blob
                    + '"\n.size ' + name + ', .-' + name + '\n')
        replacement = ("extern const struct TrainerHillFloor " + name + "[];\n"
                       '_Static_assert(sizeof(struct TrainerHillFloor) == 952, "TrainerHillFloor ABI differs");\n'
                       + "__asm__(" + json.dumps(assembly) + ");")
        text = text[:match.start()] + replacement + text[end + 1:]
    writes[path] = text.encode()
    path = tree / "include/constants/global.h"
    text = path.read_text(encoding="utf-8").replace("#define GAME_LANGUAGE (LANGUAGE_ENGLISH)",
                                                    "#define GAME_LANGUAGE (LANGUAGE_GERMAN)")
    writes[path] = text.encode()
    # The table is derived from BPED itself, not from a GBA build of this tree:
    # the layout table, the map-group pointer arrays and the tilesets are found
    # by their structure and checked against the decomp tree's own JSON and
    # INCBIN bytes; 750 untouched game objects are matched byte-for-byte;
    # localized objects retain the addresses proved by the manifest and the
    # Italian oracle. See docs/GERMAN.md. Names without evidence stay at
    # address 0, the same convention as the other locales' tables.
    symbols = ROOT / "tools/locales/german-symbols.s.gz"
    if symbols.exists():
        writes[tree / "build/german-reference.s"] = gzip.decompress(symbols.read_bytes())
    else:
        print("note: tools/locales/german-symbols.s.gz is not generated yet "
              "(the German recipe cannot be certified without it)")
    # These four list coordinates are verified against the cartridge's
    # group-table pointers and exact ID multisets, rather than borrowing
    # another language's order.
    stage_id_lists_at_offsets(tree, source, manifest.get("id_lists", {}), writes)
    # Validate all source and ROM references before modifying the staged tree.
    for path, data in writes.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    (tree / ".emerald3ds-locale").write_text("BPED experimental\n")
    print("German game data staged; console play testing is pending.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    args = parser.parse_args()
    stage(args.tree.resolve(), args.rom.resolve())


if __name__ == "__main__":
    main()
