#!/usr/bin/env python3
"""Stage the BPEJ localization from a user's clean Japanese ROM.

The checked-in manifest contains source positions and ROM offsets, never game
text or graphics. Run after bootstrap applies the pinned source patches and
`make generated` has written the generated includes. The manifest is made by
tools/locale_research/make_japanese_manifest.py (docs/JAPANESE.md).
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder"))
from emerald3ds_builder.rom import load_rom
from emerald3ds_builder.recipe import lz77_decompress

MANIFEST = ROOT / "tools/locales/japanese.json.gz"
SYMBOLS = ROOT / "tools/locales/japanese-symbols.s.gz"


def encoded(data: bytes) -> str:
    return ",".join("0x%02X" % byte for byte in data)


def replacement(kind: str, data: bytes) -> str:
    if kind == "c":
        return "{" + encoded(data) + "}"
    if kind == "cfixed":
        # a field the game fills to its width with spaces and no terminator
        return "{" + encoded(data.rstrip(b"\0") + b"\xff") + "}"
    if kind == "asm":
        return "\n\t.byte " + encoded(data) + "\n"
    if kind == "u32":
        return str(struct.unpack("<I", data)[0])
    if kind == "u16s":
        return ",".join("0x%04X" % n for n in struct.unpack("<6H", data))
    if kind == "u16array":
        return ",".join("0x%04X" % n for n in struct.unpack("<%dH" % (len(data) // 2), data))
    if kind == "u8array":
        return ",".join("%d" % n for n in data)
    if kind.startswith("struct"):
        # an array of plain structs: the ROM's records (agbcc pads them to 4
        # bytes, the format skips the padding) as positional initializers
        fmt = "<" + kind[len("struct"):]
        if len(data) % struct.calcsize(fmt):
            raise ValueError("Invalid struct array size")
        return ",".join("{" + ",".join(str(v) for v in record) + "}"
                        for record in struct.iter_unpack(fmt, data))
    raise ValueError("Unknown edit kind: " + kind)


def credits_header(source: bytes, table: int, pages: int) -> bytes:
    """src/data/credits.h rebuilt from the ROM's own pages."""
    entries = []
    for index in range(pages * 5):
        record = struct.unpack_from("<I", source, table + index * 4)[0] - 0x08000000
        if not 0 <= record <= len(source) - 8:
            raise ValueError("Invalid credits record")
        raw = source[record:record + 8]
        offset = struct.unpack_from("<I", raw, 4)[0] - 0x08000000
        end = source.find(b"\xff", offset, offset + 200) if 0 <= offset < len(source) else -1
        if end < 0 or raw[0] > 40 or raw[1] not in (0, 1) or raw[2:4] != b"\0\0":
            raise ValueError("Invalid credits text")
        entries.append((record, offset, end + 1, raw[0], raw[1]))
    lines = ["enum { PAGE_COUNT = %d };" % pages, "#define ENTRIES_PER_PAGE 5"]
    texts, records = {}, {}
    for record, offset, end, unknown, title in entries:
        if offset not in texts:
            texts[offset] = "sJapaneseCreditsText_" + str(offset)
            lines.append("static const u8 " + texts[offset] + "[] = {" + encoded(source[offset:end]) + "};")
        if record not in records:
            records[record] = "sJapaneseCreditsEntry_" + str(record)
            lines.append("static const struct CreditsEntry " + records[record] + " = {"
                         + str(unknown) + "," + str(title) + "," + texts[offset] + "};")
    lines.append("static const struct CreditsEntry *const sCreditsEntryPointerTable[PAGE_COUNT][ENTRIES_PER_PAGE] = {")
    for index in range(0, len(entries), 5):
        lines.append("{" + ",".join("&" + records[e[0]] for e in entries[index:index + 5]) + "},")
    lines.append("};")
    return ("\n".join(lines) + "\n").encode()


def stage(tree: Path, rom_path: Path) -> None:
    manifest = json.loads(gzip.decompress(MANIFEST.read_bytes()))
    rom = load_rom(rom_path)
    if rom.sha1 != manifest["rom_sha1"]:
        raise ValueError("This localization requires the clean Japanese BPEJ ROM")
    source = rom.data
    writes: dict[Path, bytes] = {}
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
            text = text[:start] + replacement(kind, source[offset:offset + size]) + text[end:]
        writes[path] = text.encode()

    for relative, offset, size in manifest["graphics"]:
        if not 0 <= offset < offset + size <= len(source):
            raise ValueError("Graphic outside ROM: " + relative)
        data = source[offset:offset + size]
        writes[tree / relative] = data
        if relative.endswith(".lz"):
            writes[tree / relative[:-3]] = lz77_decompress(source, offset)

    credits = manifest["credits"]
    writes[tree / "src/data/credits.h"] = credits_header(source, credits["table"], credits["pages"])

    path = tree / "include/constants/global.h"
    text = path.read_text(encoding="utf-8")
    if "#define GAME_LANGUAGE (LANGUAGE_ENGLISH)" not in text:
        raise ValueError("Unexpected include/constants/global.h")
    writes[path] = text.replace("#define GAME_LANGUAGE (LANGUAGE_ENGLISH)",
                                "#define GAME_LANGUAGE (LANGUAGE_JAPANESE)").encode()
    writes[tree / "build/japanese-reference.s"] = gzip.decompress(SYMBOLS.read_bytes())
    # Every source and ROM reference was validated before the tree is modified.
    for path, data in writes.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    (tree / ".emerald3ds-locale").write_text("BPEJ experimental\n")
    print("Japanese game data staged; console play testing is pending.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    args = parser.parse_args()
    stage(args.tree.resolve(), args.rom)


if __name__ == "__main__":
    main()
