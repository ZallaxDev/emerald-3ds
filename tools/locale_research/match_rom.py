#!/usr/bin/env python3
"""Locate the symbols of a byte-matching BPEE build in another Emerald ROM.

Input: the English ELF linked with --emit-relocs (every pointer and call site
of the ROM is a relocation), the English ROM it reproduces, and the other
language's ROM. Output: for each English object, its address in the other ROM.

The passes, repeated until nothing new is found:
- functions whose code matches with the relocation sites masked out;
- the pointers and calls those functions and every placed data object hold;
- byte alignment between placed neighbours (objects keep their link order);
- tables the other game lays out differently: fixed-width text rows, struct
  records of another size, strings stored back to back instead of pointed to;
- rewritten functions: paired in order inside a gap, their literal pools
  aligned between known pointers;
- runs of adjacent strings, checked and filled with the order they keep.
Votes decide conflicts. Nothing of either ROM is written out.
"""
from __future__ import annotations

import bisect
import pickle
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROM = 0x08000000


def load_elf(path):
    from elftools.elf.elffile import ELFFile
    from elftools.elf.relocation import RelocationSection
    syms = []
    relocs = []
    with open(path, 'rb') as f:
        e = ELFFile(f)
        secs = []
        for s in e.iter_sections():
            if s['sh_flags'] & 2 and s['sh_type'] == 'SHT_PROGBITS' and ROM <= s['sh_addr'] < ROM + 0x2000000:
                secs.append((s['sh_addr'] - ROM, s['sh_size'], s.name))
        for sym in e.get_section_by_name('.symtab').iter_symbols():
            v = sym['st_value']
            t = sym['st_info']['type']
            if t in ('STT_FILE', 'STT_SECTION') or not sym.name or sym.name.startswith('$'):
                continue
            if not ROM <= v < ROM + 0x2000000:
                continue
            off = v - ROM
            if t == 'STT_FUNC':
                off &= ~1
            syms.append((off, sym['st_size'], t, sym.name, sym['st_info']['bind']))
        for s in e.iter_sections():
            if not isinstance(s, RelocationSection):
                continue
            for r in s.iter_relocations():
                a = r['r_offset']
                if ROM <= a < ROM + 0x2000000 and r['r_info_type'] in (2, 10):
                    relocs.append((a - ROM, r['r_info_type']))
    return secs, syms, sorted(set(relocs))


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def bl_target(b, o):
    h1, h2 = struct.unpack_from('<HH', b, o)
    if h1 & 0xf800 != 0xf000 or h2 & 0xf800 != 0xf800:
        return None
    d = ((h1 & 0x7ff) << 12) | ((h2 & 0x7ff) << 1)
    if d & 0x400000:
        d -= 0x800000
    return o + 4 + d


class Matcher:
    def __init__(self, elf, en, jp):
        self.en, self.jp = en, jp
        self.secs, syms, self.relocs = load_elf(elf)
        self.reloc_sites = [r[0] for r in self.relocs]
        self.reloc_kind = dict(self.relocs)
        # symbol extents: explicit size, else up to the next symbol
        syms.sort()
        starts = sorted({s[0] for s in syms})
        sec_end = {}
        for a, n, name in self.secs:
            sec_end[name] = a + n
        sec_starts = sorted(self.secs)
        self.syms = []
        for off, size, t, name, bind in syms:
            if not size:
                i = bisect.bisect_right(starts, off)
                nxt = starts[i] if i < len(starts) else len(en)
                j = bisect.bisect_right([s[0] for s in sec_starts], off) - 1
                end = sec_starts[j][0] + sec_starts[j][1] if j >= 0 else len(en)
                size = min(nxt, end) - off
            self.syms.append((off, size, t, name, bind))
        self.by_off = defaultdict(list)
        for s in self.syms:
            self.by_off[s[0]].append(s)
        self.votes = defaultdict(Counter)   # en addr -> jp addr votes
        self.placed = {}                    # en object start -> jp start (accepted)
        self.how = {}
        self.done_gaps = set()
        ordered = sorted(self.syms)
        self.sym_starts = [x[0] for x in ordered]
        self.sym_ends_before, run = [], 0
        for x in ordered:
            self.sym_ends_before.append(run)
            run = max(run, x[0] + x[1])

    def in_code(self, off):
        return any(a <= off < a + n for a, n, name in self.secs if name in ('.text', 'lib_text'))

    def pointer_ok(self, ev, jv):
        if not ROM <= jv < ROM + 0x2000000:
            return False
        if ev & 1 and self.in_code(ev - ROM):   # Thumb function pointer
            return bool(jv & 1) and self.in_code(jv - ROM)
        return True

    def relocs_in(self, a, n):
        i = bisect.bisect_left(self.reloc_sites, a)
        j = bisect.bisect_left(self.reloc_sites, a + n)
        return self.relocs[i:j]

    def segments(self, a, n):
        """Unmasked byte runs of [a, a+n) as (start, end) relative offsets."""
        segs, pos = [], 0
        for site, kind in self.relocs_in(a, n):
            r = site - a
            if r > pos:
                segs.append((pos, r))
            pos = max(pos, r + 4)
        if pos < n:
            segs.append((pos, n))
        return segs

    def masked_equal(self, a, n, b, segs=None):
        if b < 0 or b + n > len(self.jp):
            return False
        en, jp = self.en, self.jp
        for s, e in (segs if segs is not None else self.segments(a, n)):
            if en[a + s:a + e] != jp[b + s:b + e]:
                return False
        return True

    def harvest(self, a, n, b):
        """Record what the JP object at b says about the targets of a's relocations."""
        en, jp = self.en, self.jp
        for site, kind in self.relocs_in(a, n):
            js = b + site - a
            if js + 4 > len(jp):
                continue
            if kind == 2:
                ev, jv = u32(en, site), u32(jp, js)
                if ROM <= ev < ROM + 0x2000000 and self.pointer_ok(ev, jv):
                    self.votes[ev - ROM][jv - ROM] += 1
            else:
                et, jt = bl_target(en, site), bl_target(jp, js)
                if et is not None and jt is not None:
                    self.votes[et][jt] += 1

    def match_functions(self, lo=0, hi=0x2E9530):
        jp = self.jp
        K = 12
        index = defaultdict(list)
        for i in range(lo, min(hi + 0x40000, len(jp)) - K, 2):
            index[jp[i:i + K]].append(i)
        found = amb = miss = 0
        self.fn_candidates = {}
        for off, size, t, name, bind in self.syms:
            if t != 'STT_FUNC' or size < 8 or off >= hi:
                continue
            segs = self.segments(off, size)
            best = max(segs, key=lambda s: s[1] - s[0], default=None)
            if not best or best[1] - best[0] < K:
                miss += 1
                continue
            key = self.en[off + best[0]:off + best[0] + K]
            cands = [c - best[0] for c in index.get(key, ())]
            cands = [c for c in cands if self.masked_equal(off, size, c, segs)]
            if len(cands) == 1:
                self.accept(off, size, cands[0], 'fn')
                found += 1
            elif cands:
                self.fn_candidates[off] = cands
                amb += 1
            else:
                miss += 1
        print('functions: %d matched, %d ambiguous, %d unmatched' % (found, amb, miss))

    def accept(self, a, n, b, how):
        if a in self.placed:
            return False
        self.placed[a] = b
        self.how[a] = how
        self.harvest(a, n, b)
        return True

    def resolved(self, addr):
        c = self.votes.get(addr)
        if not c:
            return None
        (b, n), = c.most_common(1)
        return b

    def candidates(self, off, size, keys, exact=False, func=False):
        """JP starts implied by votes for addresses inside the object. Text objects
        (no pointers) only trust votes for their exact start: BPEJ text has other
        lengths. Votes just before the object count only where no other symbol is."""
        lo_addr = off
        if not exact:
            i = bisect.bisect_left(self.sym_starts, off)
            prev = self.sym_ends_before[i] if i else 0
            lo_addr = max(off - 8, prev)
        lo = bisect.bisect_left(keys, lo_addr)
        hi = bisect.bisect_left(keys, off + (1 if exact else max(size, 1)))
        cand = Counter()
        for k in keys[lo:hi]:
            for jv, n in self.votes[k].items():
                if func:
                    cand[(jv & ~1) - ((k & ~1) - off)] += n
                else:
                    cand[jv - (k - off)] += n
        return cand

    def data_score(self, off, size, b):
        ok = bad = 0
        for site, kind in self.relocs_in(off, size):
            if kind != 2:
                continue
            ev = u32(self.en, site)
            if not ROM <= ev < ROM + 0x2000000:
                continue
            js = b + site - off
            if js < 0 or js + 4 > len(self.jp):
                bad += 1
            elif self.pointer_ok(ev, u32(self.jp, js)):
                ok += 1
            else:
                bad += 1
        return ok, bad

    def propagate(self):
        """Place objects whose start some reference revealed; repeat to a fixpoint."""
        rounds = 0
        while True:
            rounds += 1
            new = 0
            keys = sorted(self.votes)
            for off, size, t, name, bind in self.syms:
                if off in self.placed:
                    continue
                cand = self.candidates(off, size, keys, exact=not self.relocs_in(off, size) and t != 'STT_FUNC',
                                       func=t == 'STT_FUNC')
                if not cand:
                    continue
                best = None
                for b, n in cand.most_common(4):
                    if b < 0 or b >= len(self.jp):
                        continue
                    if t == 'STT_FUNC':
                        score = (2 if self.masked_equal(off, size, b) else 1, n)
                    else:
                        ok, bad = self.data_score(off, size, b)
                        score = (2 if bad == 0 else (1 if ok > 4 * bad else 0), n)
                    if best is None or score > best[0]:
                        best = (score, b)
                if best is None:
                    continue
                (q, n), b = best
                self.placed[off] = b
                if t == 'STT_FUNC':
                    self.how[off] = 'fn-ref' if q == 2 else 'fn-start'
                    if q == 2:
                        self.harvest(off, size, b)
                else:
                    ok, bad = self.data_score(off, size, b)
                    if bad == 0:
                        self.how[off] = 'data' if ok else 'leaf'
                        if ok:
                            self.harvest(off, size, b)
                    else:
                        self.how[off] = 'data-layout-differs'
                new += 1
            print('round %d: %d placed (total %d)' % (rounds, new, len(self.placed)), flush=True)
            if not new:
                break

    def anchors(self):
        """Placed object starts, kept only where EN and JP orders agree."""
        pts = sorted((a, b) for a, b in self.placed.items()
                     if not self.how[a].endswith('differs'))
        # longest increasing subsequence on JP addresses
        import bisect as bs
        tails, idx, prev = [], [], [None] * len(pts)
        for i, (a, b) in enumerate(pts):
            k = bs.bisect_left(tails, b)
            if k == len(tails):
                tails.append(b)
                idx.append(i)
            else:
                tails[k] = b
                idx[k] = i
            prev[i] = idx[k - 1] if k else None
        out, i = [], idx[-1] if idx else None
        while i is not None:
            out.append(pts[i])
            i = prev[i]
        return out[::-1]

    @staticmethod
    def blocks(x, y, k=6):
        """Matching blocks (i, j, n) of x and y, in order: unique k-mers chained by
        LIS and extended both ways; small leftover gaps refined with difflib."""
        from difflib import SequenceMatcher
        if not x or not y:
            return []
        if len(x) <= 96 and len(y) <= 96:
            return [(b.a, b.b, b.size) for b in SequenceMatcher(None, x, y, autojunk=False).get_matching_blocks()
                    if b.size >= 3]
        def uniq(s):
            seen = {}
            for i in range(len(s) - k + 1):
                w = s[i:i + k]
                seen[w] = -1 if w in seen else i
            return seen
        ux, uy = uniq(x), uniq(y)
        pairs = sorted((i, uy[w]) for w, i in ux.items() if i >= 0 and uy.get(w, -1) >= 0)
        # LIS on j
        tails, tidx, prev = [], [], [None] * len(pairs)
        for n, (i, j) in enumerate(pairs):
            p = bisect.bisect_left(tails, j)
            if p == len(tails):
                tails.append(j); tidx.append(n)
            else:
                tails[p] = j; tidx[p] = n
            prev[n] = tidx[p - 1] if p else None
        chain, n = [], tidx[-1] if tidx else None
        while n is not None:
            chain.append(pairs[n]); n = prev[n]
        chain.reverse()
        out = []
        ci = cj = 0   # end of the previous block
        for i, j in chain:
            if i < ci or j < cj or (out and i - out[-1][0] != j - out[-1][1] and i < ci):
                continue
            if out and i - out[-1][0] == j - out[-1][1] and i < out[-1][0] + out[-1][2] + 1:
                continue
            s, t = i, j
            while s > ci and t > cj and x[s - 1] == y[t - 1]:
                s -= 1; t -= 1
            e, f = i + k, j + k
            while e < len(x) and f < len(y) and x[e] == y[f]:
                e += 1; f += 1
            # refine the gap before this block
            gx, gy = x[ci:s], y[cj:t]
            if gx and gy and k > 3 and len(gx) <= 4096 and len(gy) <= 4096:
                for a, b, nn in Matcher.blocks(gx, gy, k - 1):
                    out.append((ci + a, cj + b, nn))
            out.append((s, t, e - s))
            ci, cj = e, f
        gx, gy = x[ci:], y[cj:]
        if gx and gy and k > 3 and len(gx) <= 4096 and len(gy) <= 4096 and (ci or cj):
            for a, b, nn in Matcher.blocks(gx, gy, k - 1):
                out.append((ci + a, cj + b, nn))
        return out

    def gap_align(self, max_gap=1 << 20):
        en, jp = self.en, self.jp
        anchors = self.anchors()
        sym_starts = sorted({s[0] for s in self.syms})
        added = placed = 0
        for (a1, b1), (a2, b2) in zip(anchors, anchors[1:]):
            la, lb = a2 - a1, b2 - b1
            if la <= 0 or lb <= 0 or la > max_gap or lb > max_gap or max(la, lb) > 4 * min(la, lb) + 4096:
                continue
            key = (a1, a2, b1, b2)
            if key in self.done_gaps:
                continue
            self.done_gaps.add(key)
            rel = self.relocs_in(a1, la)
            starts = sym_starts[bisect.bisect_right(sym_starts, a1):bisect.bisect_left(sym_starts, a2)]
            if not rel and all(s in self.placed for s in starts):
                continue
            blocks = sorted(self.blocks(en[a1:a2], jp[b1:b2]))
            bstarts = [blk[0] for blk in blocks]

            def mapped(x):
                i = bisect.bisect_right(bstarts, x - a1) - 1
                if i < 0:
                    return None
                ba, bb, bn = blocks[i]
                r = x - a1 - ba
                return b1 + bb + r if r < bn else None
            for site, kind in rel:
                p = mapped(site - 1)
                q = mapped(site + 4) if kind == 2 else mapped(site - 2)
                if p is None and q is None:
                    continue
                js = p + 1 if p is not None else (q - 4 if kind == 2 else q + 2)
                if kind == 2:
                    ev, jv = u32(en, site), u32(jp, js)
                    if ROM <= ev < ROM + 0x2000000 and self.pointer_ok(ev, jv):
                        self.votes[ev - ROM][jv - ROM] += 1
                        added += 1
                else:
                    et, jt = bl_target(en, site), bl_target(jp, js)
                    if et is not None and jt is not None:
                        self.votes[et][jt] += 1
                        added += 1
            for s in starts:
                if s in self.placed:
                    continue
                p = mapped(s)
                if p is not None and mapped(s + 3) == p + 3:
                    self.votes[s][p] += 1
                    placed += 1
        print('gap align: %d votes, %d starts' % (added, placed), flush=True)
        return added + placed

    # EN pointer tables that BPEJ stores as fixed-width text rows: row shift
    FIXED_TEXT = {'gAbilityDescriptionPointers': 0, 'gMoveDescriptionPointers': 1}

    @staticmethod
    def text_row(row):
        return 0xFF in row and row[0] != 0xFF and not any(
            row[k + 3] in (8, 9) and row[k + 2] < 0x80 and k % 4 == 0 for k in range(0, len(row) - 3, 4)
            if False)

    def fixed_rows(self):
        self.rows = getattr(self, 'rows', {})
        n = 0
        for off, size, t, name, bind in self.syms:
            if name not in self.FIXED_TEXT or off not in self.placed or name in self.rows:
                continue
            shift, b, count = self.FIXED_TEXT[name], self.placed[off], size // 4
            widths = [w for w in range(8, 97) if all(
                self.text_row(self.jp[b + (i + shift) * w:b + (i + shift + 1) * w]) for i in range(count))]
            if len(widths) != 1:
                print('fixed rows: %s ambiguous widths %s' % (name, widths[:5]))
                continue
            w = widths[0]
            self.rows[name] = (b, w, shift)
            self.how[off] = 'fixed-rows'
            for i in range(count):
                et = u32(self.en, off + 4 * i) - ROM
                self.votes[et][b + (i + shift) * w] += 100
            n += 1
        return n

    def find_stride(self, b, count, st, cols, has):
        jp = self.jp
        if b < 0:
            return None
        for st2 in sorted(range(4, st + 33, 2), key=lambda x: abs(x - st)):
            if b + st2 * count > len(jp):
                continue
            pos, ok = 0, []
            for c in cols:
                c2 = next((c2 for c2 in range(pos, st2 - 3, 2) if all(
                    (ROM <= u32(jp, b + i * st2 + c2) < ROM + 0x2000000) if has[c][i]
                    else u32(jp, b + i * st2 + c2) == 0 or ROM <= u32(jp, b + i * st2 + c2) < ROM + 0x2000000
                    for i in range(count)) and sum(has[c]) > 0), None)
                if c2 is None:
                    break
                ok.append(c2)
                pos = c2 + 4
            if len(ok) == len(cols):
                return st2, ok
        return None

    def struct_remaps(self):
        """Arrays of structs whose BPEJ records have another size: find the
        stride and the pointer columns, then read the JP pointers record by record."""
        self.remap = getattr(self, 'remap', {})
        en, jp = self.en, self.jp
        n = 0
        for off, size, t, name, bind in self.syms:
            if self.how.get(off) != 'data-layout-differs' or off in self.remap:
                continue
            sites = [s for s, k in self.relocs_in(off, size) if k == 2 and ROM <= u32(en, s) < ROM + 0x2000000]
            if len(sites) < 4:
                continue
            best = None
            for st in range(4, 129, 2):
                if size % st or size // st < 4:
                    continue
                count = size // st
                per = Counter((x - off) % st for x in sites)
                if len(per) > 6:
                    continue
                fill = max(per.values()) / count
                if best is None or fill > best[0] + 1e-9:
                    best = (fill, st, sorted(per))
            best = best[1:] if best and best[0] >= 0.9 else None
            if not best:
                continue
            st, cols = best
            count = size // st
            has = {c: [ROM <= u32(en, off + i * st + c) < ROM + 0x2000000 for i in range(count)] for c in cols}
            keys = sorted(self.votes)
            bases = [self.placed[off]] + [x for x, _ in self.candidates(off, size, keys).most_common(6)]
            found = None
            for b in dict.fromkeys(bases):
                found = self.find_stride(b, count, st, cols, has)
                if found:
                    self.placed[off] = b
                    break
            if not found:
                continue
            b = self.placed[off]
            st2, cols2 = found
            self.remap[off] = (st, st2, list(zip(cols, cols2)))
            self.how[off] = 'remapped'
            for i in range(count):
                for c, c2 in zip(cols, cols2):
                    ev, jv = u32(en, off + i * st + c), u32(jp, b + i * st2 + c2)
                    if ROM <= ev < ROM + 0x2000000 and self.pointer_ok(ev, jv):
                        self.votes[ev - ROM][jv - ROM] += 1
            print('remap %s: %d x %d -> %d, cols %s -> %s' % (name, count, st, st2, cols, cols2))
            n += 1
        return n

    def text_objects(self):
        """EN objects that are exactly one 0xFF-terminated string (plus zero padding)."""
        en = self.en
        out = []
        seen = set()
        for off, size, t, name, bind in sorted(self.syms):
            if t == 'STT_FUNC' or off in seen or not size or size > 4096:
                continue
            seen.add(off)
            end = en.find(b'\xff', off, off + size)
            if end < 0 or self.relocs_in(off, size) or end == off:
                continue
            if any(en[end + 1:off + size]):
                continue
            out.append((off, end + 1, off + size))
        return out

    def text_chains(self, fill=True):
        """Consecutive EN strings are consecutive in BPEJ too: check placements, fill holes."""
        jp = self.jp
        objs = self.text_objects()
        agree = disagree = filled = 0
        self.chain_bad = getattr(self, 'chain_bad', set())

        def jp_next(jstart, en_next):
            e = jp.find(b'\xff', jstart, jstart + 4096)
            if e < 0:
                return None
            n = e + 1
            if en_next % 4 == 0 and (n % 4):
                n += 4 - n % 4 if not any(jp[n:n + (4 - n % 4)]) else 0
            return n
        for _ in range(4):
            changed = 0
            for (a, ae, aend), (b, be, bend) in zip(objs, objs[1:]):
                if b != aend and not (b - ae <= 3 and b % 4 == 0):
                    continue  # not adjacent strings
                ja, jb = self.placed.get(a), self.placed.get(b)
                if ja is not None:
                    nb = jp_next(ja, b)
                    if nb is None:
                        continue
                    if jb is not None:
                        if jb == nb:
                            agree += 1
                        else:
                            disagree += 1
                            self.chain_bad.add((a, b))
                    elif fill:
                        self.placed[b] = nb
                        self.how[b] = 'chain'
                        filled += 1; changed += 1
                elif jb is not None and fill:
                    # previous string ends right before jb (allow alignment padding)
                    p = jb - 1
                    while p > jb - 4 and jp[p] == 0:
                        p -= 1
                    if jp[p] != 0xFF:
                        continue
                    q = jp.rfind(b'\xff', jb - 4096, p)
                    if q < 0:
                        continue
                    # only when the string before a is also a string in EN
                    self.placed[a] = q + 1
                    self.how[a] = 'chain-back'
                    filled += 1; changed += 1
            if not changed:
                break
        print('text chains: %d agree, %d disagree, %d filled' % (agree, disagree, filled), flush=True)
        return filled

    def jp_after(self, j, en_next):
        """Where the BPEJ string after the one at j starts (mirroring EN alignment)."""
        jp = self.jp
        e = jp.find(b'\xff', j, j + 4096)
        if e < 0:
            return None
        n = e + 1
        if en_next % 4 == 0 and n % 4 and not any(jp[n:n + 4 - n % 4]):
            n += 4 - n % 4
        return n

    def text_runs(self):
        objs = self.text_objects()
        runs, cur = [], []
        for o in objs:
            if cur and (o[0] == cur[-1][2] or (o[0] - cur[-1][1] <= 3 and o[0] % 4 == 0)):
                cur.append(o)
            else:
                if len(cur) > 1:
                    runs.append(cur)
                cur = [o]
        if len(cur) > 1:
            runs.append(cur)
        return runs

    def repair_chains(self, skip=()):
        """Per run of adjacent EN strings choose one JP start each (Viterbi over the
        voted candidates and the chain prediction); fill strings between two
        consistent neighbours."""
        fixed = changed = filled = 0
        for run in self.text_runs():
            if any(o[0] in skip for o in run):
                continue
            states = []
            for o in run:
                c = Counter({b: n for b, n in self.votes.get(o[0], {}).items()})
                if o[0] in self.placed:
                    c[self.placed[o[0]]] += 0  # keep current choice as a state
                states.append(c)
            # Viterbi: score(state) = votes + 3 * consistent links
            best = [{b: (n, None) for b, n in states[0].items()}]
            for i in range(1, len(run)):
                cur = {}
                pred_cache = {}
                for b, n in states[i].items():
                    top = (n - 0.5, None)  # starting fresh costs a little
                    for pb, (ps, _) in best[-1].items():
                        if pb not in pred_cache:
                            pred_cache[pb] = self.jp_after(pb, run[i][0])
                        link = 3 if pred_cache[pb] == b else 0
                        if ps + n + link > top[0]:
                            top = (ps + n + link, pb)
                    cur[b] = top
                # chain-only state: predicted from the best previous state
                if best[-1]:
                    pb, (ps, _) = max(best[-1].items(), key=lambda kv: kv[1][0])
                    nb = self.jp_after(pb, run[i][0])
                    if nb is not None and nb not in cur:
                        cur[nb] = (ps + 0.25, pb)
                best.append(cur)
            # backtrack
            choice = [None] * len(run)
            if best[-1]:
                b, (sc, prev) = max(best[-1].items(), key=lambda kv: kv[1][0])
                choice[-1] = b
                for i in range(len(run) - 1, 0, -1):
                    prev = best[i][choice[i]][1] if choice[i] in best[i] else None
                    choice[i - 1] = prev
            # voted choices are accepted; a stretch of unvoted strings only when the
            # chain from the voted string before it lands exactly on the voted one after
            voted = [choice[i] is not None and states[i].get(choice[i], 0) > 0 for i in range(len(run))]
            accept = list(voted)
            i = 0
            while i < len(run):
                if voted[i]:
                    i += 1
                    continue
                j = i
                while j < len(run) and not voted[j]:
                    j += 1
                if i > 0 and j < len(run):
                    pred, ok, path = choice[i - 1], True, []
                    for k in range(i, j + 1):
                        pred = self.jp_after(pred, run[k][0]) if pred is not None else None
                        if pred is None:
                            ok = False
                            break
                        path.append(pred)
                    if ok and path[-1] == choice[j]:
                        for k in range(i, j):
                            choice[k] = path[k - i]
                            accept[k] = True
                i = j
            for i, o in enumerate(run):
                if not accept[i]:
                    continue
                b = choice[i]
                old = self.placed.get(o[0])
                if old == b:
                    continue
                self.placed[o[0]] = b
                self.how[o[0]] = 'chain' if not voted[i] else self.how.get(o[0], 'leaf')
                if old is None:
                    filled += 1
                else:
                    fixed += 1
        print('chain repair: %d corrected, %d filled' % (fixed, filled), flush=True)
        return fixed + filled

    def pool_align(self):
        """Rewritten functions: align the EN function's pointer constants, in order,
        with the pointer-like words of its BPEJ body; anchors are pointers whose
        mapping is known, the words between them are read off in order."""
        en, jp = self.en, self.jp
        jfuncs = sorted(b for a, b in self.placed.items() if self.how.get(a, '').startswith('fn'))
        added = 0

        def kind(v):
            if ROM <= v < ROM + 0x2000000:
                return 'rom'
            if 0x02000000 <= v < 0x02040000 or 0x03000000 <= v < 0x03008000:
                return 'ram'
            return None
        for off, size, t, name, bind in self.syms:
            if t != 'STT_FUNC' or self.how.get(off) != 'fn-start':
                continue
            b = self.placed[off]
            i = bisect.bisect_right(jfuncs, b)
            jend = jfuncs[i] if i < len(jfuncs) else b + size * 2
            jend = min(jend, b + size * 2 + 64)
            ens = [(s, u32(en, s)) for s, k in self.relocs_in(off, size) if k == 2]
            ens = [(s, v) for s, v in ens if kind(v)]
            jps = [(p, u32(jp, p)) for p in range((b + 3) & ~3, jend - 3, 4) if kind(u32(jp, p))]
            if not ens or not jps:
                continue
            # anchors: EN pointers whose BPEJ target is known, found in the JP words in order
            anchors, y0 = [], 0
            for x, (s_, ev) in enumerate(ens):
                if kind(ev) != 'rom':
                    continue
                known = self.resolved(ev - ROM)
                if known is None:
                    continue
                y = next((y for y in range(y0, len(jps)) if jps[y][1] == known + ROM), None)
                if y is not None:
                    anchors.append((x, y))
                    y0 = y + 1
            # between consecutive anchors: same count and the same RAM/ROM pattern -> pair in order
            for (xa, ya), (xb, yb) in zip(anchors, anchors[1:]):
                e_seg, j_seg = ens[xa + 1:xb], jps[ya + 1:yb]
                if not e_seg or len(e_seg) != len(j_seg):
                    continue
                if [kind(v) for _, v in e_seg] != [kind(v) for _, v in j_seg]:
                    continue
                for (s_, ev), (p, jv) in zip(e_seg, j_seg):
                    if kind(ev) == 'rom' and self.resolved(ev - ROM) is None and self.pointer_ok(ev, jv):
                        self.votes[ev - ROM][jv - ROM] += 1
                        added += 1
        print('pool align: %d votes' % added, flush=True)
        return added

    def chained_tables(self):
        """EN pointer tables of strings that BPEJ stores as the strings themselves,
        one after another: entry i is the i-th string from the table's start."""
        en, jp = self.en, self.jp
        texts = {o[0] for o in self.text_objects()}
        n = 0
        for off, size, t, name, bind in self.syms:
            if self.how.get(off) != 'data-layout-differs' or size % 4 or size < 8:
                continue
            targets = [u32(en, off + 4 * i) - ROM for i in range(size // 4)]
            if not all(x in texts for x in targets):
                continue
            b = self.placed[off]
            if any(ROM <= u32(jp, b + k) < ROM + 0x2000000 for k in range(0, 8, 4) if (b + k) % 4 == 0):
                continue
            pos, ok = b, True
            starts = []
            for i in range(len(targets)):
                e = jp.find(b'\xff', pos, pos + 256)
                if e < 0 or e == pos:
                    ok = False
                    break
                starts.append(pos)
                pos = e + 1
            if not ok:
                continue
            for x, j in zip(targets, starts):
                self.votes[x][j] += 50
            self.how[off] = 'chained-strings'
            n += 1
            print('chained strings %s: %d' % (name, len(targets)))
        return n

    def function_gaps(self):
        """Unplaced EN functions between two placed ones: when the BPEJ gap holds
        exactly as many function prologues (push {..., lr}), pair them in order."""
        en, jp = self.en, self.jp
        funcs = sorted((s[0], s[1]) for s in self.syms if s[2] == 'STT_FUNC' and s[1] >= 4)
        n = 0
        placed = [(a, self.placed[a]) for a, _ in funcs if a in self.placed]
        for (a1, b1), (a2, b2) in zip(placed, placed[1:]):
            if b2 <= b1:
                continue
            inner = [a for a, _ in funcs if a1 < a < a2 and a not in self.placed]
            if not inner or len(inner) > 12:
                continue
            # every EN function in the gap must start with a push {.., lr}
            if not all(en[a + 1] == 0xB5 for a in inner):
                continue
            pro = [p for p in range(b1 + 2, b2, 2) if jp[p + 1] == 0xB5 and
                   (jp[p - 1] in (0x47, 0xBD) or jp[p - 2:p] == b'\0\0' or
                    ROM <= u32(jp, p - 4) < ROM + 0x4000000 or 0x02000000 <= u32(jp, p - 4) < 0x04000000)]
            if len(pro) != len(inner):
                continue
            for a, p in zip(inner, pro):
                self.votes[a | 1][p | 1] += 3
                n += 1
        print('function gaps: %d paired' % n, flush=True)
        return n

    def report(self):
        by = Counter()
        tot = Counter()
        for off, size, t, name, bind in self.syms:
            sec = next((s[2] for s in self.secs if s[0] <= off < s[0] + s[1]), '?')
            tot[sec] += 1
            if off in self.placed:
                by[(sec, self.how[off])] += 1
        for sec in tot:
            print('%-24s %6d symbols: ' % (sec, tot[sec]) +
                  ', '.join('%s %d' % (h, n) for (s, h), n in sorted(by.items()) if s == sec))
        conflicts = sum(1 for c in self.votes.values() if len(c) > 1)
        print('addresses with conflicting votes:', conflicts, 'of', len(self.votes))


def run(elf, en: bytes, jp: bytes) -> "Matcher":
    m = Matcher(elf, en, jp)
    print('%d symbols, %d relocations' % (len(m.syms), len(m.relocs)))
    m.votes[0][0] += 1
    m.match_functions()
    m.propagate()
    for it in range(12):
        if not (m.gap_align() + m.fixed_rows() + m.struct_remaps() + m.chained_tables()
                + m.function_gaps() + m.pool_align()):
            break
        m.propagate()
    rowt = set()
    for name in Matcher.FIXED_TEXT:
        for off, size, t, nm, bind in m.syms:
            if nm == name:
                rowt |= {u32(m.en, off + 4 * i) - ROM for i in range(size // 4)}
    for it in range(3):
        if not m.repair_chains(skip=rowt):
            break
        m.propagate()
    m.text_chains(fill=False)
    m.report()
    return m


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--elf', required=True, help='English ELF linked with --emit-relocs')
    ap.add_argument('--english-rom', required=True)
    ap.add_argument('--other-rom', required=True)
    ap.add_argument('--out', required=True, help='pickle of the placements (for inspection)')
    args = ap.parse_args()
    m = run(args.elf, Path(args.english_rom).read_bytes(), Path(args.other_rom).read_bytes())
    with open(args.out, 'wb') as f:
        pickle.dump(placements(m), f)


def placements(m: "Matcher") -> dict:
    return {'placed': m.placed, 'how': m.how, 'votes': dict(m.votes),
            'rows': getattr(m, 'rows', {}), 'remap': getattr(m, 'remap', {})}


if __name__ == '__main__':
    main()
