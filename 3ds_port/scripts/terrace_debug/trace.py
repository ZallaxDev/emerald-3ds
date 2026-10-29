"""trace.py LAYOUT x0,y0,x1,y1: the pieces in the box after blocks and after
each post pass (staircases, level_bands, unstand) - which rule did what."""
import sys
import _common as cm
import voxel_terraces as vt

lid = sys.argv[1]
x0, y0, x1, y1 = cm.box(sys.argv[2])
ox, oy = cm.offset(vt, lid)


def show(tag, pieces):
    print("--", tag)
    for y in range(y0, y1):
        print("%3d " % y + " ".join("%-9s" % ("%s@%g" % (pieces[(x + ox, y + oy)][0],
                                                       pieces[(x + ox, y + oy)][1] / vt.LEVEL)
                                             if (x + ox, y + oy) in pieces else "-")
                                    for x in range(x0, x1)))


for name in ("blocks", "staircases", "level_bands", "unstand"):
    def wrap(*a, _f=getattr(vt, name), _n=name, **k):
        r = _f(*a, **k)
        show(_n, r if _n == "blocks" else a[1])
        return r
    setattr(vt, name, wrap)
show("final", cm.solve(lid)["pieces"])
