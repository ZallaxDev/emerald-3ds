#!/usr/bin/env python3
"""Add to tools/locales/japanese.json.gz what the matcher leaves English.

What make_japanese_manifest.py does not map:

- songs: the songs whose tracks differ in BPEJ (the English release retuned
  some sound effects, some with another number of tracks). Each is recorded
  as its source and, per BPEJ track, the track's offset and length (from its
  first command to its FINE) and the places of its pointers with the track
  and place each leads to, so localize_japanese.py writes the song's tracks
  from the player's ROM. Songs are matched through each game's gSongTable.
- graphics: an LZ graphic the matcher left unmapped (another size in BPEJ),
  found through the function that loads it: the BPEJ pointer in the same
  place of the same function's literal pool.
- untranslated: the game's C texts with no Japanese yet, as source spans,
  which localize_japanese.py writes as the port's own "not yet translated".
- tables: C tables whose English values BPEJ does not have (window
  templates, icon offsets, kana search ranges, trade mail...), as edits that
  write BPEJ's records. Each is found through BPEJ's symbol or the literal
  pools of the functions that use it, and taken only when its records have
  the table's shape (readelf, beside --nm, gives each object's statics).
- braille: braille texts with nothing to match (the Sealed Chamber's
  letters), between two the matcher mapped: BPEJ's strings between those
  two, in order, when the count and each brailleformat agree.

Only positions, offsets and labels are written, as in the rest of the
manifest: no byte of either ROM.

    python tools/locale_research/japanese_extras.py \
        --nm-elf pokeemerald.elf --english-rom baserom.gba \
        --japanese-rom emerald-ja.gba --tree .

--nm-elf is the original game's ELF (its symbol table only); --tree a tree
after `make generated` (it holds sound/songs/midi/*.s).
"""
import argparse, bisect, glob, gzip, json, os, re, struct, subprocess, sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MANIFEST = REPO / "tools/locales/japanese.json.gz"
SYMBOLS = REPO / "tools/locales/japanese-symbols.s.gz"
ROM_BASE = 0x08000000

# m4a track commands followed by a pointer: GOTO, PATT (pointer at +1), REPT (+2)
FINE = 0xB1
POINTER_AT = {0xB2: 1, 0xB3: 1, 0xB5: 2}
COMMAND_SIZE = {0xB2: 5, 0xB3: 5, 0xB5: 6}


def japanese_symbols(path=SYMBOLS):
    text = gzip.decompress(Path(path).read_bytes()).decode()
    return {m.group(1): (int(m.group(2)), int(m.group(3)))
            for m in re.finditer(r"\.equ (\S+), (\d+)\n\.size \1, (\d+)", text)}


def english_symbols(nm, elf):
    """name -> (address, size); a symbol without a size runs to the next one."""
    rows = []
    for line in subprocess.check_output([nm, "-n", "-S", elf], text=True).splitlines():
        parts = line.split()
        if len(parts) == 4:
            rows.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
        elif len(parts) == 3:
            rows.append((int(parts[0], 16), None, parts[2]))
    starts = sorted({r[0] for r in rows})
    out = {}
    for address, size, name in rows:
        if size is None:
            k = bisect.bisect_right(starts, address)
            size = starts[k] - address if k < len(starts) else 0
        out[name] = (address, size)
    return out


def decode_track(rom, offset):
    """A track from its first command to its FINE: (length, positions of its
    pointers), or None when it does not decode to a FINE."""
    i, pointers = offset, []
    while i < len(rom) and i - offset < 0x4000:
        command = rom[i]
        if command == FINE:
            return i + 1 - offset, pointers
        if command in POINTER_AT:
            pointers.append(i - offset + POINTER_AT[command])
            i += COMMAND_SIZE[command]
        else:
            i += 1
    return None


def masked(data, pointers):
    data = bytearray(data)
    for p in pointers:
        data[p:p + 4] = bytes(4)
    return bytes(data)


def song_tables(en, ja, en_syms):
    """gSongTable in both games (header pointer, ms, me per song), the
    Japanese one found by its ms/me columns: the symbol list does not map it."""
    at, size = en_syms["gSongTable"]
    at -= ROM_BASE
    count = size // 8
    en_table = [struct.unpack_from("<IHH", en, at + 8 * i) for i in range(count)]
    key = [en[at + 8 * i + 4:at + 8 * i + 8] for i in range(40)]
    hits = [off for off in range(0, len(ja) - 8 * count, 4)
            if all(ja[off + 8 * i + 4:off + 8 * i + 8] == key[i] for i in range(40))]
    if len(hits) != 1:
        raise SystemExit("japanese_extras: gSongTable not found once in the Japanese ROM")
    ja_table = [struct.unpack_from("<IHH", ja, hits[0] + 8 * i) for i in range(count)]
    return en_table, ja_table


def header_tracks(rom, header):
    """A song's track offsets from its header (numTracks, numBlks, priority,
    reverb, voicegroup, then a pointer per track)."""
    at = header - ROM_BASE
    if not 0 <= at < len(rom) - 8 or not 1 <= rom[at] <= 16:
        return None
    tracks = [struct.unpack_from("<I", rom, at + 8 + 4 * k)[0] - ROM_BASE for k in range(rom[at])]
    return tracks if all(0 <= t < len(rom) for t in tracks) else None


def songs(tree, en, ja, en_syms):
    """The songs whose tracks differ in BPEJ (another command, length or
    number of tracks): each as its source, its name and, per BPEJ track, the
    track's offset and length (first command to FINE) and its pointers as
    (position, track, place in that track)."""
    found = []
    en_table, ja_table = song_tables(en, ja, en_syms)
    files = sorted(glob.glob(str(tree / "sound/songs/*.s")) + glob.glob(str(tree / "sound/songs/midi/*.s")))
    for name in files:
        song = Path(name).stem
        index = [i for i, e in enumerate(en_table) if song in en_syms and e[0] == en_syms[song][0]]
        if len(index) != 1:
            continue
        en_header = header_tracks(en, en_table[index[0]][0])
        ja_header = header_tracks(ja, ja_table[index[0]][0])
        if not en_header or not ja_header:
            continue
        e = [decode_track(en, t) for t in en_header]
        j = [decode_track(ja, t) for t in ja_header]
        if None in e or None in j:
            continue
        if len(e) == len(j) and all(
                a[0] == b[0] and a[1] == b[1]
                and masked(en[x:x + a[0]], a[1]) == masked(ja[y:y + b[0]], b[1])
                for a, b, x, y in zip(e, j, en_header, ja_header)):
            continue
        tracks = []
        for (length, pointers), at in zip(j, ja_header):
            places = []
            for p in pointers:
                target = struct.unpack_from("<I", ja, at + p)[0] - ROM_BASE
                hit = [(k, target - t) for k, (t, (n, _)) in enumerate(zip(ja_header, j)) if 0 <= target - t < n]
                if not hit:
                    break
                places.append([p, hit[0][0], hit[0][1]])
            else:
                tracks.append([at, length, places])
                continue
            break
        if len(tracks) == len(j):
            found.append([Path(name).relative_to(tree).as_posix(), song, tracks])
    return found


def lz_size(data, offset):
    """The decompressed size and the stream's own length."""
    out_size = int.from_bytes(data[offset + 1:offset + 4], "little")
    i, out = offset + 4, 0
    while out < out_size:
        flags = data[i]
        i += 1
        for _ in range(8):
            if out >= out_size:
                break
            if flags & 0x80:
                out += ((data[i] << 8 | data[i + 1]) >> 12) + 3
                i += 2
            else:
                out += 1
                i += 1
            flags <<= 1
    return out_size, i - offset


def graphics(tree, en, ja, en_syms, ja_syms, mapped):
    found = []
    functions = sorted((a & ~1, n) for n, (a, s) in en_syms.items() if a & 1 or s)
    starts = [f[0] for f in functions]
    for source in sorted(glob.glob(str(tree / "src/**/*.[ch]"), recursive=True)):
        text = Path(source).read_text(encoding="utf-8", errors="replace")
        for name, path in re.findall(r"(\w+)\[\]\s*=\s*INCBIN_\w+\(\"([^\"]+\.lz)\"\)", text):
            if path in mapped or name not in en_syms or ja_syms.get(name, (0, 0))[0]:
                continue
            target = en_syms[name][0]
            pool = struct.pack("<I", target)
            candidates = set()
            at = en.find(pool)
            while at >= 0:
                if at % 4 == 0:
                    k = bisect.bisect_right(starts, at + ROM_BASE) - 1
                    function = functions[k][1] if k >= 0 else None
                    if function in ja_syms and ja_syms[function][0]:
                        place = (ja_syms[function][0] & ~1) - ROM_BASE + (at + ROM_BASE - functions[k][0])
                        q = struct.unpack_from("<I", ja, place)[0] - ROM_BASE if place % 4 == 0 else -1
                        if 0 <= q < len(ja) - 4 and ja[q] == 0x10:
                            candidates.add(q)
                at = en.find(pool, at + 1)
            if len(candidates) != 1:
                continue
            offset = candidates.pop()
            en_out, _ = lz_size(en, target - ROM_BASE)
            ja_out, length = lz_size(ja, offset)
            if not en_out // 2 <= ja_out <= en_out * 2:
                continue
            found.append([path, offset, length])
    return found


C_TEXT = re.compile(r'_\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)')
C_PART = re.compile(r'"((?:[^"\\]|\\.)*)"')
# shorter texts are menu words the port's own patches also use (YES, CANCEL...)
MIN_TEXT = 24


def untranslated(tree, en, ja, manifest):
    """The game's C texts the manifest leaves English: a _("...") literal of
    src/ that the English ROM has, BPEJ has not, and no edit replaces. Kept
    as source spans of the pinned files, for localize_japanese.py to write
    the port's own "not yet translated" text there."""
    import hashlib
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from locale_sources import load_charmap, encode
    charmap = load_charmap(str(tree / "charmap.txt"))
    # charmap.txt writes the apostrophe as '\'' and C texts as a plain '
    if "\\'" in charmap:
        charmap.setdefault("'", charmap["\\'"])
    out = {}
    for source in sorted(glob.glob(str(tree / "src/**/*.[ch]"), recursive=True)):
        relative = Path(source).relative_to(tree).as_posix()
        text = Path(source).read_text(encoding="utf-8")
        pinned = manifest["files"].get(relative)
        sha = hashlib.sha256(text.encode()).hexdigest()
        if pinned and pinned["sha256"] != sha:
            continue
        edits = sorted((e[0], e[1]) for e in pinned["edits"]) if pinned else []
        starts = [e[0] for e in edits]
        spans = []
        for m in C_TEXT.finditer(text):
            parts = [encode(p, charmap) for p in C_PART.findall(m.group(1))]
            if None in parts:
                continue
            data = b"".join(parts) + b"\xff"
            k = bisect.bisect_right(starts, m.start()) - 1
            if (k >= 0 and edits[k][1] > m.start()) or (k + 1 < len(edits) and edits[k + 1][0] < m.end()):
                continue
            if len(data) >= MIN_TEXT and data in en and data not in ja:
                spans.append([m.start(), m.end()])
        if spans:
            out[relative] = {"sha256": sha, "spans": spans}
    return out


# Table element types this step writes from BPEJ, as the manifest's struct
# formats, and what a record must look like to be taken.
TABLE_TYPES = {
    "u8": ("B", None),
    "u16": ("H", None),
    # BPEJ's record and the English one: the list ends (DUMMY_WIN_TEMPLATE) alike
    "struct WindowTemplate": ("6BH", lambda r, e: (r[0] == 0xFF) == (e[0] == 0xFF) and (
        r[0] == 0xFF or (r[0] < 4 and r[3] <= 32 and r[4] <= 32 and r[5] < 16))),
    "struct MenuInfoIcon": ("2BH", lambda r, e: r[0] <= 128 and r[1] <= 128),
    # left:5, top:3, width, height, footerId
    "struct EasyChatPhraseFrameDimensions": ("bits5,3,8,8,8", lambda r, e: r[2] <= 30 and r[3] <= 20 and r[4] < 4),
}
TABLE = re.compile(r"^(?:static )?const (u8|u16|struct \w+) (\w+)((?:\[[^\]]*\])+)\s*=\s*\{",
                   re.M)


def record_size(fmt):
    if fmt.startswith("bits"):
        return sum(int(w) for w in fmt[4:].split(",")) // 8
    return struct.calcsize(fmt)


def records(fmt, data):
    """A table's records: a struct format, or "bits" and each field's width
    in bits, low bits first (agbcc's bitfields)."""
    if not fmt.startswith("bits"):
        return list(struct.iter_unpack(fmt, data))
    widths, size = [int(w) for w in fmt[4:].split(",")], record_size(fmt)
    out = []
    for at in range(0, len(data) - size + 1, size):
        value, fields = int.from_bytes(data[at:at + size], "little"), []
        for width in widths:
            fields.append(value & ((1 << width) - 1))
            value >>= width
        out.append(tuple(fields))
    return out


def objects(readelf, elf):
    """Each object file's own symbols, from the symbol table's FILE entries:
    object name -> {symbol: (address, size)}, and every function's address."""
    per, current, functions = {}, None, []
    for line in subprocess.check_output([readelf, "-sW", elf], text=True).splitlines():
        parts = line.split()
        if len(parts) < 8:
            continue
        kind, name = parts[3], parts[7]
        if kind == "FILE":
            current = per.setdefault(name.rsplit(".", 1)[0], {})
        elif kind == "OBJECT" and current is not None:
            current[name] = (int(parts[1], 16), int(parts[2], 0))
        if kind == "FUNC":
            functions.append(((int(parts[1], 16) & ~1), int(parts[2], 0), name))
    return per, sorted(functions)


def closing_brace(text, at):
    depth = 0
    for i in range(at, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if not depth:
                return i
    return -1


def ja_pointer(en, ja, ja_syms, functions, address, fits):
    """The BPEJ address of what an English address is, from the literal
    pools that hold it: the word at the same place of the same functions,
    or, where BPEJ's function is laid out otherwise, the one word of its
    pools whose target fits. A function whose name others share (statics)
    is placed between the nearest ones with a name of their own."""
    starts = [f[0] for f in functions]
    count = {}
    for f in functions:
        count[f[2]] = count.get(f[2], 0) + 1

    def ja_start(k):
        name = functions[k][2]
        if count[name] == 1 and ja_syms.get(name, (0, 0))[0]:
            return (ja_syms[name][0] & ~1) - ROM_BASE
        return None

    found, near = set(), set()
    pool = struct.pack("<I", address)
    at = en.find(pool)
    while at >= 0:
        k = bisect.bisect_right(starts, at + ROM_BASE) - 1
        if at % 4 == 0 and k >= 0:
            start, size, name = functions[k]
            begin = ja_start(k)
            if begin is not None:
                place = begin + (at + ROM_BASE - start)
                if place % 4 == 0 and 0 <= place < len(ja) - 4 and fits(struct.unpack_from("<I", ja, place)[0] - ROM_BASE):
                    found.add(struct.unpack_from("<I", ja, place)[0] - ROM_BASE)
                end = begin + size + 256
            else:
                before = next((ja_start(i) for i in range(k - 1, max(k - 32, -1), -1) if ja_start(i) is not None), None)
                after = next((ja_start(i) for i in range(k + 1, min(k + 32, len(functions))) if ja_start(i) is not None), None)
                if before is None or after is None or not 0 < after - before < 0x10000:
                    at = en.find(pool, at + 1)
                    continue
                begin, end = before, after
            for word in range(begin + (-begin) % 4, min(end, len(ja) - 4), 4):
                target = struct.unpack_from("<I", ja, word)[0] - ROM_BASE
                if fits(target):
                    near.add(target)
        at = en.find(pool, at + 1)
    for candidates in (found, near):
        if len(candidates) == 1:
            return candidates.pop()
    return None


def tables(tree, en, ja, ja_syms, readelf, elf, manifest):
    """The game's tables whose English values BPEJ does not have (its own
    layout: window templates, icon offsets...), as edits with BPEJ's
    records, for the C files that define them."""
    import hashlib
    per, functions = objects(readelf, elf)
    out = {}
    for source in sorted(glob.glob(str(tree / "src/**/*.[ch]"), recursive=True)):
        relative = Path(source).relative_to(tree).as_posix()
        text = Path(source).read_text(encoding="utf-8")
        # a table of a header belongs to the object of the file that includes it
        owners = [Path(source).stem] if source.endswith(".c") else [
            Path(c).stem for c in glob.glob(str(tree / "src/*.c"))
            if '#include "' + relative[len("src/"):] + '"' in Path(c).read_text(encoding="utf-8", errors="replace")]
        if len(owners) != 1 or owners[0] not in per:
            continue
        symbols = per[owners[0]]
        pinned = manifest["files"].get(relative)
        sha = hashlib.sha256(text.encode()).hexdigest()
        if pinned and pinned["sha256"] != sha:
            continue
        taken = [(e[0], e[1]) for e in (pinned["edits"] if pinned else [])]
        taken += [tuple(s) for s in manifest.get("untranslated", {}).get(relative, {}).get("spans", [])]
        edits = []
        for m in TABLE.finditer(text):
            base, name, dims = m.group(1), m.group(2), re.findall(r"\[([^\]]*)\]", m.group(3))
            if name not in symbols or base not in TABLE_TYPES:
                continue
            address, size = symbols[name]
            data = en[address - ROM_BASE:address - ROM_BASE + size]
            # only what a pack would carry from the English ROM: a run of it
            # as long as the release audit's window that BPEJ does not have
            if size < 32 or len(dims) > 2 or all(data[i:i + 32] in ja for i in range(0, size - 31, 4)):
                continue
            start, end = m.end(), closing_brace(text, m.end() - 1)
            if end < 0 or any(a < end and start < b for a, b in taken):
                continue
            fmt, valid = TABLE_TYPES[base]
            if fmt.startswith("bits"):
                kind = fmt
            elif base.startswith("struct"):
                kind = "struct" + fmt
            elif len(dims) == 2 and dims[1].strip():
                try:
                    kind = "struct%d%s" % (int(eval(dims[1], {"__builtins__": {}, "MAIL_WORDS_COUNT": 9})), fmt)
                except Exception:
                    kind = None
            else:
                kind = "u8array" if base == "u8" else "u16array"
            if kind is None:
                continue
            record = kind if kind.startswith("bits") else "<" + (kind[len("struct"):] if kind.startswith("struct") else fmt)

            def fits(at):
                """BPEJ's table of the same shape: records the type allows
                and, for plain numbers, zeros in the same places or at least
                half of the bytes alike."""
                if not 0 <= at <= len(ja) - size or at % 2 or size % record_size(record):
                    return False
                ja_data = ja[at:at + size]
                if ja_data == data:
                    return False
                if valid:
                    return all(valid(r, e) for r, e in zip(records(record, ja_data), records(record, data)))
                element = "<%d%s" % (size // struct.calcsize(fmt), fmt)
                zeros = [[v == 0 for v in struct.unpack(element, d)] for d in (ja_data, data)]
                return zeros[0] == zeros[1] or sum(a == b for a, b in zip(ja_data, data)) * 2 >= size

            # a name only one object has is BPEJ's own symbol, if it is that table
            alone = sum(name in s for s in per.values()) == 1
            at = ja_syms[name][0] - ROM_BASE if alone and ja_syms.get(name, (0, 0))[1] == size else -1
            if not 0 <= at <= len(ja) - size or ja[at:at + size] == data:
                at = ja_pointer(en, ja, ja_syms, functions, address, fits)
            if at is None:
                continue
            edits.append([start, end, at, size, kind])
        if edits:
            out[relative] = {"sha256": sha, "edits": edits}
    return out


BRAILLE_BLOCK = re.compile(r"^\w+:\n\t(brailleformat ([\d, ]+)\n(?:\t\.braille .*\n)*\t\.braille .*\$\")", re.M)


def braille(tree, ja, manifest):
    """The braille texts the matcher leaves English between two it mapped
    (the Sealed Chamber's letters have no text to match): BPEJ's strings
    between those two, in order, when there are as many and each begins
    with its text's own brailleformat numbers."""
    import hashlib
    out = {}
    for relative, entry in manifest["files"].items():
        path = tree / relative
        if not relative.endswith(".inc") or not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        if hashlib.sha256(text.encode()).hexdigest() != entry["sha256"]:
            continue
        blocks = [(m.start(1), m.end(1), bytes(int(n) for n in m.group(2).split(",")))
                  for m in BRAILLE_BLOCK.finditer(text)]
        edited = {e[0]: e for e in entry["edits"]}
        added = []
        mapped = [i for i, b in enumerate(blocks) if b[0] in edited]
        for i, k in zip(mapped, mapped[1:]):
            if k - i < 2:
                continue
            first, last = edited[blocks[i][0]], edited[blocks[k][0]]
            at, strings = first[2] + first[3], []
            while at < last[2] and len(strings) < k - i:
                end = ja.find(b"\xff", at, last[2])
                if end < 0:
                    break
                strings.append((at, end + 1 - at))
                at = end + 1
            if at != last[2] or len(strings) != k - i - 1:
                continue
            gap = blocks[i + 1:k]
            if all(ja[s:s + 6] == b[2] for (s, _), b in zip(strings, gap)):
                added += [[b[0], b[1], s, n, "asm"] for (s, n), b in zip(strings, gap)]
        if added:
            out[relative] = added
    return out


def extras(nm, elf, english_rom, japanese_rom, tree, manifest, symbols=SYMBOLS):
    en = Path(english_rom).read_bytes()
    ja = Path(japanese_rom).read_bytes()
    en_syms, ja_syms = english_symbols(nm, elf), japanese_symbols(symbols)
    mapped = {g[0] for g in manifest["graphics"]}
    added = graphics(Path(tree), en, ja, en_syms, ja_syms, mapped)
    manifest["graphics"] = manifest["graphics"] + added
    manifest["songs"] = songs(Path(tree), en, ja, en_syms)
    manifest["untranslated"] = untranslated(Path(tree), en, ja, manifest)
    readelf = re.sub(r"nm(\.exe)?$", r"readelf\1", nm)
    for relative, entry in tables(Path(tree), en, ja, ja_syms, readelf, elf, manifest).items():
        pinned = manifest["files"].setdefault(relative, {"sha256": entry["sha256"], "edits": []})
        pinned["edits"] = sorted(pinned["edits"] + entry["edits"])
        print("  tables", relative, [e[2:] for e in entry["edits"]])
    for relative, edits in braille(Path(tree), ja, manifest).items():
        manifest["files"][relative]["edits"] = sorted(manifest["files"][relative]["edits"] + edits)
        print("  braille", relative, [e[2:] for e in edits])
    return added


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nm", default=os.environ.get("NM", "arm-none-eabi-nm"))
    ap.add_argument("--nm-elf", required=True)
    ap.add_argument("--english-rom", required=True)
    ap.add_argument("--japanese-rom", required=True)
    ap.add_argument("--tree", default=str(REPO))
    args = ap.parse_args()
    manifest = json.loads(gzip.decompress(MANIFEST.read_bytes()))
    added = extras(args.nm, args.nm_elf, args.english_rom, args.japanese_rom, args.tree, manifest)
    MANIFEST.write_bytes(gzip.compress(json.dumps(manifest, separators=(",", ":")).encode(), 9, mtime=0))
    print("japanese_extras: %d graphics, %d songs" % (len(added), len(manifest["songs"])))
    for g in added:
        print("  graphic", g[0])
    for s in manifest["songs"]:
        print("  song", s[0], len(s[2]), "track(s)")
    for relative, entry in manifest["untranslated"].items():
        print("  untranslated", relative, len(entry["spans"]))


if __name__ == "__main__":
    main()
