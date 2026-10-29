"""The source side of a locale manifest: what each edit of the Spanish
manifest replaces, as an English ROM address, and the charmap to check it."""
import bisect, gzip, json, re, sys
from collections import Counter
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from match_rom import load_elf

REPO = str(Path(__file__).resolve().parents[2]) + '/'
TREE = REPO + 'build/upstream/'  # the patched tree; set by the caller


def load_charmap(path):
    cm = {}
    for line in open(path, encoding='utf-8'):
        line = line.split('@')[0].strip()
        m = re.match(r"^'(.+)'\s*=\s*([0-9A-Fa-f ]+)$", line)
        if m:
            cm.setdefault(m.group(1), bytes.fromhex(m.group(2)))
            continue
        m = re.match(r'^([A-Z_0-9]+)\s*=\s*([0-9A-Fa-f ]+)$', line)
        if m:
            cm.setdefault('{' + m.group(1) + '}', bytes.fromhex(m.group(2)))
    return cm


def encode(text, cm):
    """Encode the contents of a "..." literal; None if it uses something we do not know."""
    out = bytearray()
    i = 0
    while i < len(text):
        c = text[i]
        if c == '{':
            j = text.index('}', i)
            tok = text[i:j + 1]
            if tok in cm:
                out += cm[tok]
            else:
                inner = tok[1:-1].split()
                if not inner:
                    return None
                try:
                    out += bytes(int(x, 16) if x.startswith('0x') else int(x) for x in inner)
                except ValueError:
                    head = '{' + inner[0] + '}'
                    if head not in cm:
                        return None
                    out += cm[head]
                    for x in inner[1:]:
                        if x.startswith('0x') or x.isdigit():
                            out.append(int(x, 0))
                        elif '{' + x + '}' in cm:
                            out += cm['{' + x + '}']
                        else:
                            return None
            i = j + 1
            continue
        if c == '\\' and i + 1 < len(text):
            pair = text[i:i + 2]
            if pair in cm:
                out += cm[pair]
            elif pair == '\\"':
                out += cm['"'] if '"' in cm else b''
            else:
                return None
            i += 2
            continue
        if c not in cm:
            return None
        out += cm[c]
        i += 1
    return bytes(out)


def literal_bytes(span, kind, cm):
    if kind == 'c':
        m = re.fullmatch(r'_\("(.*)"\)', span, re.S)
        if not m:
            return None
        b = encode(m.group(1), cm)
        return None if b is None else b + b'\xff'
    if kind == 'asm':
        parts = re.findall(r'\.string\s+"((?:[^"\\]|\\.)*)"', span)
        if not parts or re.sub(r'\.string\s+"((?:[^"\\]|\\.)*)"', '', span).strip():
            return None
        out = bytearray()
        for p in parts:
            b = encode(p, cm)
            if b is None:
                return None
            out += b
        return bytes(out)
    return None


def spanish_symbols():
    es = {}
    for line in gzip.decompress(open(REPO + 'tools/locales/spanish-symbols.s.gz', 'rb').read()).decode().splitlines():
        m = re.match(r'\.equ (\S+), (\d+)', line)
        if m and int(m.group(2)):
            es[m.group(1)] = int(m.group(2)) - 0x08000000
    return es


def edits_with_en(elf, en):
    man = json.loads(gzip.decompress(open(REPO + 'tools/locales/spanish.json.gz', 'rb').read()))
    es = spanish_symbols()
    inv = sorted((a, n) for n, a in es.items())
    starts = [a for a, _ in inv]
    _, syms, _ = load_elf(elf)
    en_of = {}
    for off, size, t, name, bind in syms:
        en_of.setdefault(name, off)
    rows = []
    for f, d in man['files'].items():
        for s, e, off, size, kind in d['edits']:
            i = bisect.bisect_right(starts, off) - 1
            # several names can share one address; prefer one the ELF knows
            j = i
            cands = []
            while j >= 0 and inv[j][0] == inv[i][0]:
                cands.append(inv[j][1]); j -= 1
            k = i + 1
            while k < len(inv) and inv[k][0] == inv[i][0]:
                cands.append(inv[k][1]); k += 1
            name = next((n for n in cands if n in en_of), None)
            enaddr = en_of[name] + (off - inv[i][0]) if name else None
            rows.append(dict(file=f, start=s, end=e, es_off=off, es_size=size, kind=kind, sym=name,
                             delta=off - inv[i][0], en=enaddr))
    return man, rows


C_DEF = re.compile(r'^(?:ALIGNED\(\d+\)\s+)?(?:static\s+)?(?:ALIGNED\(\d+\)\s+)?(?:const\s+)?(?:struct\s+)?\w+[\s\*]+(?:const\s+)?\**\s*(\w+)\s*(?:\[[^\]\n]*\]\s*)*=', re.M)
ASM_DEF = re.compile(r'^(\w+)::?', re.M)


def owners(text, path):
    pat = ASM_DEF if path.endswith(('.s', '.inc')) else C_DEF
    return [(m.start(), m.group(1)) for m in pat.finditer(text)]


def owner_at(ow, pos):
    i = bisect.bisect_right([o[0] for o in ow], pos) - 1
    return ow[i][1] if i >= 0 else None
