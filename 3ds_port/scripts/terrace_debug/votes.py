"""votes.py LAYOUT X,Y [X,Y ...]: every count of the drawing (run of faces
or bands) between the terraces holding the given cells (layout coordinates)
and any other: (high terrace, low terrace, levels) -> weight, with the
levels the solve gave them, and the votes it dropped among them."""
import collections
import sys
import _common as cm
import voxel_terraces as vt

lid = sys.argv[1]
ox, oy = cm.offset(vt, lid)
sol = cm.solve(lid)
c, comp, level = sol["cells"], sol["comp"], sol["level"]
want = {comp[(x + ox, y + oy)] for x, y in (cm.box(a) for a in sys.argv[2:]) if (x + ox, y + oy) in comp}


def name(t):
    return "(%d,%d)" % (t[0] - ox, t[1] - oy)


vote = collections.Counter()
for d, hi, cells, lo in vt.runs_of(c):
    if hi in comp and lo in comp and comp[hi] != comp[lo] and (comp[hi] in want or comp[lo] in want):
        head = d != "S" and any(cell in c.rim for cell in cells)
        vote[(comp[hi], comp[lo], vt.steps(c, cells, d)[1])] += 1 if head else 2
for (a, b, k), n in sorted(vote.items()):
    print("%s -> %s  %d level(s)  weight %d   solved %g -> %g" % (
        name(a), name(b), k, n, level[a] / vt.LEVEL, level[b] / vt.LEVEL))
print("dropped:", [(name(a), name(b), k, n) for (a, b, k), n in sol["dropped"] if a in want or b in want])
