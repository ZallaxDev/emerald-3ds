"""rgrid.py RELIEF.bin LAYOUT x0,y0,x1,y1: corner heights (levels, NW NE SW SE)
of each cell as written in a relief.bin ("-": no lattice of its own)."""
import json
import os
import sys
import _common as cm
import reliefbin

L, _ = reliefbin.load(sys.argv[1])
lays = json.load(open(os.path.join(cm.ROOT, "data", "layouts", "layouts.json")))["layouts"]
g = L[[i + 1 for i, e in enumerate(lays) if e["id"] == sys.argv[2]][0]]["grids"]
x0, y0, x1, y1 = cm.box(sys.argv[3])
print("     " + "".join("%-13d" % x for x in range(x0, x1)))
for y in range(y0, y1):
    row = []
    for x in range(x0, x1):
        a = g.get((x, y))
        row.append("-" if a is None else " ".join("%g" % (v / 16.0) for v in (a[0][0], a[0][4], a[4][0], a[4][4])))
    print("%3d  " % y + "".join("%-13s" % s for s in row))
