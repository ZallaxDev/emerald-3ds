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
import re
import struct
import subprocess
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
    if kind.startswith("bits"):
        # records with bitfields: each field's width in bits, low bits first
        widths = [int(w) for w in kind[len("bits"):].split(",")]
        size = sum(widths) // 8
        if sum(widths) % 8 or len(data) % size:
            raise ValueError("Invalid bitfield array size")
        out = []
        for at in range(0, len(data), size):
            value, fields = int.from_bytes(data[at:at + size], "little"), []
            for width in widths:
                fields.append(str(value & ((1 << width) - 1)))
                value >>= width
            out.append("{" + ",".join(fields) + "}")
        return ",".join(out)
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


def track_label(song: str, track: int, place: int) -> str:
    base = "%s_%d" % (song, track + 1)
    return base if place == 0 else "%s_ja_%d" % (base, place)


def track_source(song: str, track: int, data: bytes, places: list, targets: set) -> list[str]:
    """A track written from the player's ROM: its bytes, with a label at each
    place a pointer of the song leads to, its pointers as those labels."""
    pointers = {position: track_label(song, to, place) for position, to, place in places}
    out, run, i = [track_label(song, track, 0) + ":"], [], 0

    def flush():
        if run:
            out.append("\t.byte " + encoded(bytes(run)))
            run.clear()

    while i < len(data):
        if i and (track, i) in targets:
            flush()
            out.append(track_label(song, track, i) + ":")
        if i in pointers:
            flush()
            out.append("\t.word " + pointers[i])
            i += 4
            continue
        run.append(data[i])
        if len(run) == 16:
            flush()
        i += 1
    flush()
    return out


def song_source(tree: Path, path: Path) -> None:
    """A MIDI song's source, made now as make makes it (mid2agb with its
    midi.cfg options): the main build makes it only after the localization."""
    midi = path.with_suffix(".mid")
    options = None
    for line in (path.parent / "midi.cfg").read_text(encoding="utf-8").splitlines():
        name, _, rest = line.partition(":")
        if name.strip() == midi.name:
            options = rest.split()
    tool = next((p for p in (tree / "tools/mid2agb/mid2agb.exe", tree / "tools/mid2agb/mid2agb") if p.exists()), None)
    if options is None or tool is None or not midi.exists():
        raise ValueError("Cannot make the song source: " + str(path.relative_to(tree)))
    subprocess.run([str(tool), str(midi), str(path)] + options, check=True)


def stage_song(text: str, song: str, tracks: list) -> str:
    """The song source with every track written from the player's ROM (from
    its first track's label to the separator before its header) and the
    header's track count and list to match; its other fields stay."""
    lines = text.split("\n")
    start, header = lines.index(song + "_1:"), lines.index(song + ":")
    end = header
    while end > start and (not lines[end - 1].strip() or lines[end - 1].lstrip().startswith(("/*", "@", ".align"))):
        end -= 1
    targets = {(to, place) for _, places in tracks for _, to, place in places}
    body = []
    for track, (data, places) in enumerate(tracks):
        body += ([""] if track else []) + track_source(song, track, data, places, targets)
    entry = re.compile(r"^(\s*)\.(4byte|int|word)(\s+)%s_\d+\s*$" % re.escape(song))
    count = re.compile(r"^(\s*\.byte\s+)\d+(.*)$")
    head, listed, counted = [], None, False
    for line in lines[header + 1:]:
        m = entry.match(line)
        if m:
            if listed is None:
                listed = len(head)
                head += ["%s.%s%s%s" % (m.group(1), m.group(2), m.group(3), track_label(song, k, 0))
                         for k in range(len(tracks))]
            continue
        c = count.match(line)
        if c and not counted:
            line, counted = "%s%d%s" % (c.group(1), len(tracks), c.group(2)), True
        head.append(line)
    if listed is None or not counted:
        raise ValueError("Unexpected song header: " + song)
    out = lines[:start] + body + lines[end:header + 1] + head
    return "\n".join(out)


# The game's texts the manifest has no Japanese for keep their English in
# the tree: each is written as this text of the port's own instead ("not yet
# translated"), so no English text of the game reaches the Japanese build.
UNTRANSLATED = "(みほんやく)"
STRING_LINE = re.compile(r'^(\s*)\.string "(.*)"\s*$')


def untranslated_texts(text: str) -> tuple[str, int]:
    """Every text still written as .string lines (one ending in $) becomes
    UNTRANSLATED; Japanese texts are .byte lines after the edits."""
    lines, out, count, i = text.split("\n"), [], 0, 0
    while i < len(lines):
        m = STRING_LINE.match(lines[i])
        if not m:
            out.append(lines[i])
            i += 1
            continue
        j = i
        while j < len(lines) and STRING_LINE.match(lines[j]) and not STRING_LINE.match(lines[j]).group(2).endswith("$"):
            j += 1
        if j < len(lines) and STRING_LINE.match(lines[j]):
            out.append('%s.string "%s$"' % (m.group(1), UNTRANSLATED))
            count += 1
            i = j + 1
        else:
            out.extend(lines[i:j])
            i = j
    return "\n".join(out), count


def stage(tree: Path, rom_path: Path) -> None:
    manifest = json.loads(gzip.decompress(MANIFEST.read_bytes()))
    rom = load_rom(rom_path)
    if rom.sha1 != manifest["rom_sha1"]:
        raise ValueError("This localization requires the clean Japanese BPEJ ROM")
    source = rom.data
    writes: dict[Path, bytes] = {}
    # the game's C texts with no Japanese yet: spans of the same pinned files
    left_c = manifest.get("untranslated", {})
    for relative in sorted(set(manifest["files"]) | set(left_c)):
        file = manifest["files"].get(relative) or {"sha256": left_c[relative]["sha256"], "edits": []}
        path = tree / relative
        text = path.read_text(encoding="utf-8")
        if hashlib.sha256(text.encode()).hexdigest() != file["sha256"]:
            raise ValueError("Source differs from the pinned patched tree: " + relative)
        edits = [tuple(e) for e in file["edits"]]
        edits += [(start, end, 0, 0, "untranslated") for start, end in left_c.get(relative, {}).get("spans", [])]
        previous_end = 0
        for start, end, offset, size, kind in sorted(edits):
            if not previous_end <= start < end <= len(text) or (
                    kind != "untranslated" and not 0 <= offset < offset + size <= len(source)):
                raise ValueError("Overlapping or out-of-range edit: " + relative)
            previous_end = end
        for start, end, offset, size, kind in sorted(edits, reverse=True):
            new = ('_("%s")' % UNTRANSLATED if kind == "untranslated"
                   else replacement(kind, source[offset:offset + size]))
            text = text[:start] + new + text[end:]
        writes[path] = text.encode()

    for relative, offset, size in manifest["graphics"]:
        if not 0 <= offset < offset + size <= len(source):
            raise ValueError("Graphic outside ROM: " + relative)
        data = source[offset:offset + size]
        writes[tree / relative] = data
        if relative.endswith(".lz"):
            writes[tree / relative[:-3]] = lz77_decompress(source, offset)

    for relative, song, tracks in manifest.get("songs", []):
        if not all(0 <= offset < offset + size <= len(source) for offset, size, _ in tracks):
            raise ValueError("Track outside ROM: " + relative)
        path = tree / relative
        if path not in writes and not path.exists():
            song_source(tree, path)
        text = writes[path].decode() if path in writes else path.read_text(encoding="utf-8")
        writes[path] = stage_song(text, song, [(source[o:o + n], places) for o, n, places in tracks]).encode()

    left = 0
    for path in sorted((tree / "data").rglob("*")):
        if path.suffix not in (".s", ".inc") or not path.is_file():
            continue
        text = writes[path].decode() if path in writes else path.read_text(encoding="utf-8")
        text, count = untranslated_texts(text)
        if count:
            writes[path] = text.encode()
            left += count

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
    print("Japanese game data staged (%d texts with no Japanese marked untranslated); "
          "console play testing is pending." % left)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    args = parser.parse_args()
    stage(args.tree.resolve(), args.rom)


if __name__ == "__main__":
    main()
