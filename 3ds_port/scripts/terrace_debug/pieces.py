"""pieces.py LAYOUT x0,y0,x1,y1: kind, band side and solved piece (kind@level)
of each cell in the box (layout coordinates), and the band segments."""
import sys
import _common as cm
import voxel_terraces as vt

lid = sys.argv[1]
x0, y0, x1, y1 = cm.box(sys.argv[2])
sol = cm.solve(lid)
ox, oy = cm.offset(vt, lid)
c, P = sol["cells"], sol["pieces"]
for y in range(y0, y1):
    out = []
    for x in range(x0, x1):
        n = (x + ox, y + oy)
        piece = ("%s@%g" % (P[n][0], P[n][1] / vt.LEVEL) if n in P else
                 "f%g" % (sol["flat"][n] / vt.LEVEL) if n in sol["flat"] else "-")
        out.append("%-16s" % ("%s%s %s" % (c.kind.get(n), c.band.get(n, ""), piece)))
    print("%3d " % y + " ".join(out))
