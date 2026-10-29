"""watch.py LAYOUT x,y: every assignment to the cell's piece after blocks,
with the function and line that made it."""
import sys
import traceback
import _common as cm
import voxel_terraces as vt

lid = sys.argv[1]
x, y = cm.box(sys.argv[2])
ox, oy = cm.offset(vt, lid)
T = (x + ox, y + oy)


class Watched(dict):
    def __setitem__(self, k, v):
        if k == T:
            fr = traceback.extract_stack()[-2]
            print("set", v[0], v[1] / vt.LEVEL, "at", fr.name, fr.lineno)
        dict.__setitem__(self, k, v)


_blocks = vt.blocks


def blocks(*a, **k):
    r = Watched(_blocks(*a, **k))
    print("blocks:", r.get(T))
    return r


vt.blocks = blocks
cm.solve(lid)
