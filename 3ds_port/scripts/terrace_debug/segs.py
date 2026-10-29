"""segs.py LAYOUT x0,y0,x1,y1: the band columns (side, head, bands, foot)
touching the box, in layout coordinates."""
import sys
import _common as cm
import voxel_terraces as vt

lid = sys.argv[1]
x0, y0, x1, y1 = cm.box(sys.argv[2])
ox, oy = cm.offset(vt, lid)
c = vt.Cells(vt.group_of(lid))


def f(p):
    return None if p is None else (p[0] - ox, p[1] - oy)


for side, head, cells, foot in vt.segments(c):
    pts = [p for p in [head, foot] + list(cells) if p]
    if any(x0 + ox <= p[0] < x1 + ox and y0 + oy <= p[1] < y1 + oy for p in pts):
        print(side, f(head), [f(p) for p in cells], f(foot))
