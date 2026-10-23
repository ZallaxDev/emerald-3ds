#!/usr/bin/env python3
"""Write tools/locales/japanese.json.gz and japanese-symbols.s.gz.

The Japanese manifest reuses the Spanish manifest's source spans: each gives a
source position and (through the Spanish symbol list, whose tables have the
English layout) the English address of what it replaces; match_rom.py maps
that address to BPEJ. Only positions, offsets and symbol addresses are
written: no text, graphics or other bytes of either ROM.

    python tools/locale_research/make_japanese_manifest.py \
        --elf pokeemerald.elf --english-rom baserom.gba --japanese-rom emerald-ja.gba \
        --tree build/upstream

--elf is the original game built from the pinned upstream with agbcc
(`make compare`) and linked with --emit-relocs, in that pristine tree (its
sources tell which layouts the port changed); --tree is a bootstrapped tree
after `make generated`. Needs pyelftools (tools/requirements-research.txt).
"""
import bisect, glob, gzip, hashlib, json, pickle, re, struct, sys
from collections import Counter, defaultdict
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import locale_sources
import match_rom
from match_rom import Matcher, u32, ROM
from locale_sources import load_charmap, literal_bytes, edits_with_en, owners, owner_at


class Mapper:
    def __init__(self, m, P):
        self.m, self.P = m, P
        self.placed, self.how = P['placed'], P['how']
        self.remap = P.get('remap', {})
        objs = sorted((s[0], s[1], s[3], s[2]) for s in m.syms if s[1])
        self.objs = objs
        self.starts = [o[0] for o in objs]
        self.text = {o[0]: (o[1], o[2]) for o in m.text_objects()}
        self.byname = {}
        for s in m.syms:
            self.byname.setdefault(s[3], s)

    def containing(self, addr):
        """Innermost EN object holding addr (the latest start whose extent covers it)."""
        i = bisect.bisect_right(self.starts, addr) - 1
        best = None
        while i >= 0 and addr - self.starts[i] < 0x20000:
            off, size, name, t = self.objs[i]
            if off <= addr < off + size:
                if best is None or size < best[1]:
                    best = self.objs[i]
                if off == addr:
                    break
            i -= 1
        return best

    def to_jp(self, addr, owner=None):
        """BPEJ address for EN address addr, or (None, why)."""
        if owner is not None and owner in self.byname:
            s = self.byname[owner]
            if s[0] <= addr < s[0] + s[1]:
                obj = (s[0], s[1], s[3], s[2])
            else:
                obj = self.containing(addr)
        else:
            obj = self.containing(addr)
        if obj is None:
            return None, 'no-object'
        off, size, name, t = obj
        # exact starts of strings first
        if addr in self.placed and addr in self.text:
            return self.placed[addr], self.how[addr]
        if off not in self.placed:
            return None, 'unplaced:' + name
        b = self.placed[off]
        how = self.how[off]
        if name in RECORDS:
            st, st2, fields = RECORDS[name]
            i, f = divmod(addr - off, st)
            for start, length, start2 in fields:
                if start <= f < start + length:
                    return b + i * st2 + start2 + (f - start), 'record'
            return None, 'record-field'
        if off in self.remap:
            st, st2, cols = self.remap[off]
            i, f = divmod(addr - off, st)
            first = cols[0][0]
            if f < first:
                f2 = f
            else:
                c, c2 = max((c for c in cols if c[0] <= f), key=lambda c: c[0])
                f2 = c2 + (f - c)
                if f2 >= st2:
                    return None, 'remap-field'
            return b + i * st2 + f2, 'remap'
        if how == 'data-layout-differs':
            return None, 'layout:' + name
        if addr != off and not self.m.relocs_in(off, size):
            rows = self.rows(off, size, b)
            if rows:
                (w, n), w2 = rows
                i, k = divmod(addr - off, w)
                if k:
                    return None, 'inside-row:' + name
                return b + i * w2, 'rows'
        if off in self.text and addr != off:
            return None, 'inside-text:' + name
        return b + (addr - off), how

    def rows(self, off, size, b, cache={}):
        """(EN row width, rows), JP row width of a fixed-width string table."""
        if off not in cache:
            en, jp = self.m.en, self.m.jp
            cache[off] = None
            for w in range(2, 64):
                n = size // w
                def pure(row):
                    e = row.find(b'\xff')
                    return e > 0 and not row[e + 1:].strip(b'\x00\xff')
                if size % w or n < 3 or not all(pure(en[off + i * w:off + (i + 1) * w]) for i in range(n)):
                    continue
                for w2 in range(2, w + 1):
                    if all(0xFF in jp[b + i * w2:b + (i + 1) * w2] and jp[b + i * w2] != 0xFF for i in range(n)):
                        cache[off] = ((w, n), w2)
                        break
                break
        return cache[off]


JAPANESE_SHA1 = 'd7cf8f156ba9c455d164e1ea780a6bf1945465c2'
CREDITS_TABLE, CREDITS_PAGES = 0x5C7590, 45
# The title screen's background palettes (pokemon_logo.gbapal and
# rayquaza_and_clouds.gbapal, one INCBIN): BPEJ has 9 of them, the logo's 5
# then Rayquaza's and the clouds' (patch 0044). The English banner palette has
# no Japanese counterpart; 0044 does not load it, it holds BPEJ's first one.
TITLE_PALETTES = 0x517B58
EXTRA_GRAPHICS = [
    ['graphics/title_screen/pokemon_logo.gbapal', TITLE_PALETTES, 5 * 32],
    ['graphics/title_screen/rayquaza_and_clouds.gbapal', TITLE_PALETTES + 5 * 32, 4 * 32],
    ['graphics/title_screen/emerald_version.gbapal', TITLE_PALETTES, 32],
]
# Pointer-free struct arrays laid out otherwise in BPEJ: (EN record size,
# BPEJ record size, [(EN field start, length, BPEJ field start)]).
# gApprentices: the English game keeps a name per language, the Japanese one
# its own only; every name slot takes the Japanese name.
RECORDS = {
    'gApprentices': (88, 48, [(8 * k, 8, 0) for k in range(6)] + [(48, 40, 8)]),
}
# struct fields that BPEJ fills to their width without a terminator
FIXED_FIELDS = {'gPokedexEntries': 6}
# BPEJ's own glyphs for the fonts the game draws Japanese text with
FONTS = [('graphics/fonts/normal.hwjpnfont', 0x2A0F14, 16384)]


def lz_info(b, o):
    """(compressed size, decompressed size) of an LZ77 stream, or None."""
    if o + 4 > len(b) or b[o] != 0x10:
        return None
    n = int.from_bytes(b[o + 1:o + 4], 'little')
    out, p = 0, o + 4
    try:
        while out < n:
            flags = b[p]; p += 1
            for k in range(8):
                if out >= n:
                    break
                if flags & (0x80 >> k):
                    v = b[p] << 8 | b[p + 1]; p += 2
                    if (v & 0xFFF) + 1 > out:
                        return None
                    out += (v >> 12) + 3
                else:
                    p += 1; out += 1
    except IndexError:
        return None
    return (p - o + 3) & ~3 if False else p - o, n


def select_graphics(m, P, en, jp, elf=None):
    import glob
    inc = re.compile(r'(\w+)\s*(?:\[[^\]]*\])+\s*=\s*INCBIN_U(?:8|16|32)\(([^)]*)\)')
    pairs, joined = {}, {}
    for f in sorted(glob.glob(locale_sources.TREE + 'src/**/*.[ch]', recursive=True)):
        for mm in inc.finditer(open(f, encoding='utf-8', errors='ignore').read()):
            paths = re.findall(r'"([^"]+)"', mm.group(2))
            if len(paths) == 1:
                pairs.setdefault(mm.group(1), paths[0])
            elif paths:
                joined.setdefault(mm.group(1), paths)
    syms = {}
    for s in m.syms:
        syms.setdefault(s[3], s)
    picked, used = [], Counter()
    for name, path in sorted(pairs.items()):
        if name not in syms or syms[name][0] not in P['placed']:
            continue
        o, size = syms[name][:2]
        b = P['placed'][o]
        if path.endswith('.lz'):
            e, j = lz_info(en, o), lz_info(jp, b)
            # BPEJ's own art even where its size differs: whatever the pack
            # holds must come from the player's ROM
            if not e or not j or en[o:o + e[0]] == jp[b:b + j[0]]:
                continue
            picked.append([path, b, j[0]])
        else:
            if en[o:o + size] == jp[b:b + size]:
                continue
            picked.append([path, b, size])
        used[b] += 1
    # several files in one INCBIN: BPEJ's object, cut where the English files end
    built = Path(elf).resolve().parent if elf else None
    for name, paths in sorted(joined.items()):
        if name not in syms or syms[name][0] not in P['placed'] or built is None:
            continue
        if any(p.endswith('.lz') for p in paths) or not all((built / p).exists() for p in paths):
            continue
        o, size = syms[name][:2]
        sizes = [(built / p).stat().st_size for p in paths]
        b = P['placed'][o]
        if sum(sizes) != size or en[o:o + size] == jp[b:b + size]:
            continue
        for path, n in zip(paths, sizes):
            picked.append([path, b, n])
            used[b] += 1
            b += n
    # map layouts (asm labels, no sizes): BPEJ's own where its blocks differ,
    # e.g. a Trick House puzzle; the voxel world is built from them
    if built is not None:
        label = re.compile(r'^(\w+)::\s*\n\s*\.incbin "([^"]+)"', re.M)
        layouts = open(locale_sources.TREE + 'data/layouts/layouts.inc', encoding='utf-8').read()
        for name, path in label.findall(layouts):
            if name not in syms or syms[name][0] not in P['placed'] or not (built / path).exists():
                continue
            o, size = syms[name][0], (built / path).stat().st_size
            b = P['placed'][o]
            if en[o:o + size] != jp[b:b + size]:
                picked.append([path, b, size])
                used[b] += 1
    picked = [g for g in picked if used[g[1]] == 1]
    extra = {g[0] for g in EXTRA_GRAPHICS + [list(f) for f in FONTS]}
    picked = [g for g in picked if g[0] not in extra]
    picked += [list(f) for f in FONTS] + [list(g) for g in EXTRA_GRAPHICS]
    return sorted(picked)


def symbols_text(m, P):
    """Symbol names with their BPEJ addresses (0 when unknown), like spanish-symbols.s."""
    lines = ['.syntax unified']
    seen = set()
    for off, size, t, name, bind in sorted(m.syms, key=lambda s: s[3]):
        if name in seen or not re.fullmatch(r'[A-Za-z_.$][\w.$]*', name):
            continue
        seen.add(name)
        b = P['placed'].get(off)
        ok = b is not None and not P['how'].get(off, '').endswith('differs')
        kind = '%function' if t == 'STT_FUNC' else '%object'
        addr = (b + ROM + (1 if t == 'STT_FUNC' else 0)) if ok else 0
        lines += ['.global ' + name, '.type %s, %s' % (name, kind), '.equ %s, %d' % (name, addr),
                  '.size %s, %d' % (name, size if ok else 0)]
    return '\n'.join(lines) + '\n'


def fingerprint(man, relative):
    """The Spanish manifest's fingerprint (same patched tree), else the tree's own."""
    if relative in man['files']:
        return man['files'][relative]['sha256']
    return hashlib.sha256(open(locale_sources.TREE + relative, 'rb').read()).hexdigest()


def array_body(text, name):
    """(start, end) of the initializer inside the braces of C array `name`."""
    m = re.search(r'\b' + re.escape(name) + r'\s*(?:\[[^\]]*\])+\s*=\s*\{', text)
    if not m:
        return None
    depth, i = 1, m.end()
    while depth:
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        i += 1
    return m.end(), i - 1


# The alphabetical Easy Chat lists: the Japanese game has 45 groups (others
# and 44 kana, in the order of its keyboard); the English game reuses the
# first 27 for others and A-Z and keeps the last 18 as UnusedJapanese*.
EASY_CHAT_LETTERS_FILE = 'src/data/easy_chat/easy_chat_words_by_letter.h'
EASY_CHAT_LETTERS = ['gEasyChatWordsByLetter_Others'] + ['gEasyChatWordsByLetter_' + c for c in
                     'ABCDEFGHIJKLMNOPQRSTUVWXYZ'] + ['gEasyChatWordsByLetter_UnusedJapanese' + k for k in (
                     'Hi', 'Fu', 'He', 'Ho', 'Ma', 'Mi', 'Mu', 'Me', 'Mo', 'Ya', 'Yu', 'Yo',
                     'Ra', 'Ri', 'Ru', 'Re', 'Ro', 'Wa')]
JP_EASY_CHAT_LETTER_POINTERS = 0x579D04   # {words, numWords} x 45


# Orderings by name: the Japanese game sorts them in kana order (same length).
SORTED_ARRAYS = [
    ('src/data/pokemon/pokedex_orders.h', 'gPokedexOrder_Alphabetical'),
    ('src/data/easy_chat/easy_chat_group_pokemon.h', 'gEasyChatGroup_Pokemon'),
    ('src/data/easy_chat/easy_chat_group_pokemon2.h', 'gEasyChatGroup_Pokemon2'),
]


# Plain-data structs whose BPEJ values are the Japanese game's own layout
# (window positions and sizes for its text). The formats read the ROM, whose
# agbcc structs are padded to 4 bytes; the edit writes the fields only.
STRUCT_FORMATS = {
    'WindowTemplate': '<BBBBBBH',
    'BattleWindowText': '<BBBBBBBBBBxx',
}


def symbol_files(elf):
    """(name, address) -> source file of every local symbol (STT_FILE order)."""
    from elftools.elf.elffile import ELFFile
    out, current = {}, None
    with open(elf, 'rb') as f:
        for sym in ELFFile(f).get_section_by_name('.symtab').iter_symbols():
            kind = sym['st_info']['type']
            if kind == 'STT_FILE':
                current = sym.name
            elif sym['st_info']['bind'] == 'STB_LOCAL' and current:
                out[(sym.name, sym['st_value'])] = re.sub(r'\.o$', '.c', current)
    return out


def struct_edits(m, mp, jp, elf):
    """Struct arrays of STRUCT_FORMATS types: BPEJ's values where they differ."""
    files = symbol_files(elf)
    sources = {}
    for f in glob.glob(locale_sources.TREE + 'src/**/*.c', recursive=True):
        sources.setdefault(Path(f).name, []).append(f)
    out = []
    for off, size, t, name, bind in m.syms:
        b = mp.placed.get(off)
        if t != 'STT_OBJECT' or b is None or m.relocs_in(off, size) or m.en[off:off + size] == jp[b:b + size]:
            continue
        src = files.get((name, off + ROM))
        paths = sources.get(src, []) if src else []
        for path in paths:
            text = open(path, encoding='utf-8').read()
            mm = re.search(r'\bconst struct (\w+) ' + re.escape(name) + r'\s*\[', text)
            if not mm or mm.group(1) not in STRUCT_FORMATS:
                continue
            fmt = STRUCT_FORMATS[mm.group(1)]
            if size % struct.calcsize(fmt):
                continue
            # only where the port left the English definition as it is: a
            # layout the port changed for the 3DS stays the port's
            relative = path[len(locale_sources.TREE):]
            span = array_body(text, name)
            pristine_path = Path(elf).resolve().parent / relative
            if span is None or not pristine_path.exists():
                continue
            pristine = pristine_path.read_text(encoding='utf-8')
            pspan = array_body(pristine, name)
            if pspan is None or pristine[pspan[0]:pspan[1]] != text[span[0]:span[1]]:
                continue
            out.append((path[len(locale_sources.TREE):], name, b, size, 'struct' + fmt[1:]))
    return out


def array_edits(jp, mp):
    """Whole-array replacements: (file, name, offset, size, kind)."""
    out = []
    for f, name in SORTED_ARRAYS:
        off, size = mp.byname[name][0], mp.byname[name][1]
        if off not in mp.placed:
            raise SystemExit('make_japanese_manifest: %s not found in BPEJ' % name)
        out.append((f, name, mp.placed[off], size, 'u16array'))
    for i, name in enumerate(EASY_CHAT_LETTERS):
        words, count = struct.unpack_from('<IH', jp, JP_EASY_CHAT_LETTER_POINTERS + 8 * i)
        out.append((EASY_CHAT_LETTERS_FILE, name, words - ROM, 2 * count, 'u16array'))
    return out


def jp_string_size(jp, j, end, limit=4096):
    """Bytes of the BPEJ string at j, up to its terminator or end (the next
    placed object): some templates fill their field with spaces (0x00 in
    Japanese) and have no terminator, the code writes one after its digits."""
    end = min(end, j + limit)
    e = jp.find(b'\xff', j, end)
    if e >= 0:
        return e + 1 - j
    return end - j if end < j + limit else None


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--english-rom', required=True)
    ap.add_argument('--japanese-rom', required=True)
    ap.add_argument('--tree', required=True, help='bootstrapped tree after make generated')
    ap.add_argument('--out-dir', default=locale_sources.REPO + 'tools/locales')
    ap.add_argument('--report', help='list of the edits left in English')
    args = ap.parse_args()
    locale_sources.TREE = str(Path(args.tree).resolve()) + '/'
    elf = args.elf
    en, jp = open(args.english_rom, 'rb').read(), open(args.japanese_rom, 'rb').read()
    if hashlib.sha1(jp).hexdigest() != JAPANESE_SHA1:
        raise SystemExit('make_japanese_manifest: not the clean BPEJ ROM')
    m = match_rom.run(elf, en, jp)
    P = match_rom.placements(m)
    mp = Mapper(m, P)
    jp_starts = sorted(set(P['placed'].values()))
    cm = load_charmap(locale_sources.TREE + 'charmap.txt')
    man, rows = edits_with_en(elf, en)
    texts, own = {}, {}
    stats = Counter()
    why = Counter()
    files = defaultdict(list)
    last_found = {}
    unmapped = []
    for r in rows:
        f = r['file']
        if f not in texts:
            try:
                texts[f] = open(locale_sources.TREE + f, encoding='utf-8').read()
            except FileNotFoundError:
                texts[f] = None
            own[f] = owners(texts[f], f) if texts[f] else []
        t = texts[f]
        owner = owner_at(own[f], r['start']) if t else None
        span = t[r['start']:r['end']] if t else ''
        enaddr = r['en']
        if owner and owner != r['sym'] and owner in mp.byname:
            # the Spanish symbol is not this span's owner: find the English text inside the owner
            lit = literal_bytes(span, r['kind'], cm) if t else None
            s = mp.byname[owner]
            enaddr = None
            if lit:
                key = (f, owner)
                start = last_found.get(key, s[0])
                p = en.find(lit, start, s[0] + s[1])
                if p >= 0:
                    enaddr = p
                    last_found[key] = p + 1
        if enaddr is None:
            stats['no-en-address'] += 1
            why['no-en-address:' + f] += 1
            continue
        j, how = mp.to_jp(enaddr, owner)
        if j is None:
            unmapped.append((f, owner, span[:70].replace('\n', ' ')))
            stats['unmapped'] += 1
            why[how] += 1
            continue
        kind = r['kind']
        if kind not in ('c', 'asm', 'u32', 'u16s', 'u16array'):
            stats['skip-kind-' + kind] += 1  # Spanish-only table layouts
            continue
        if re.search('[\u3040-\u30ff]', span):
            stats['skip-already-japanese'] += 1  # the English source keeps the Japanese text
            continue
        fixed = FIXED_FIELDS.get(owner)
        if fixed and kind == 'c' and how == 'remap' and 0xFF not in jp[j:j + fixed]:
            files[f].append([r['start'], r['end'], j, fixed, 'cfixed'])
            stats['mapped-cfixed'] += 1
            continue
        if kind in ('c', 'asm'):
            size = jp_string_size(jp, j, jp_starts[bisect.bisect_right(jp_starts, j)])
            if jp[j + size - 1] != 0xFF:
                # only where the English string has no terminator either
                # (__() in the source); otherwise the placement is wrong
                obj = mp.containing(enaddr)
                if obj is None or obj[0] != enaddr or 0xFF in en[enaddr:enaddr + obj[1]]:
                    stats['no-terminator'] += 1
                    continue
                stats['unterminated-' + kind] += 1
            if size is None:
                stats['no-terminator'] += 1
                continue
        else:
            size = r['es_size']  # same layout in every language
        files[f].append([r['start'], r['end'], j, size, kind])
        stats['mapped-' + kind] += 1
    print(sorted(stats.items()))
    print(why.most_common(20))
    for f, name, offset, size, kind in array_edits(jp, mp) + struct_edits(m, mp, jp, elf):
        text = open(locale_sources.TREE + f, encoding='utf-8').read()
        span = array_body(text, name)
        if span is None:
            if kind.startswith('struct'):
                stats['struct-not-an-array'] += 1
                continue
            raise SystemExit('make_japanese_manifest: no array ' + name)
        a, b = span
        files[f] = [e for e in files.get(f, []) if not (a <= e[0] < b)] + [[a, b, offset, size, kind]]
        stats['array-' + kind] += 1
    if args.report:
        with open(args.report, 'w', encoding='utf-8') as uf:
            for row in unmapped:
                uf.write('%s\t%s\t%s\n' % row)
    graphics = select_graphics(m, P, en, jp, elf)
    print('graphics:', len(graphics))
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'japanese-symbols.s.gz').write_bytes(gzip.compress(symbols_text(m, P).encode(), 9, mtime=0))
    # file fingerprints are the Spanish manifest's own (same patched tree)
    result = {
        'version': 1,
        'rom_sha1': JAPANESE_SHA1,
        'status': 'experimental',
        'unmapped_source_texts': len(unmapped),
        'files': {f: {'sha256': fingerprint(man, f), 'edits': sorted(e)} for f, e in sorted(files.items())},
        'graphics': graphics,
        'credits': {'table': CREDITS_TABLE, 'pages': CREDITS_PAGES},
    }
    (out / 'japanese.json.gz').write_bytes(
        gzip.compress(json.dumps(result, separators=(',', ':')).encode(), 9, mtime=0))
    print('wrote', out / 'japanese.json.gz', 'and', out / 'japanese-symbols.s.gz')

if __name__ == '__main__':
    main()
