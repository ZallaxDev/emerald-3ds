"""Read relief.bin (see src/voxel/voxel_relief.c): per layout its cells'
lattices in pixels, and the cut records."""
import struct


def load(path):
    b = open(path, "rb").read()
    assert b[:4] == b"VXL4"
    n, side = struct.unpack_from("<HH", b, 4)
    G = side * side
    out = {}
    for k in range(n):
        lid, cells, w, h, off, base = struct.unpack_from("<HHHHIh", b, 8 + 14 * k)
        unit = 2 if h & 0x4000 else 1
        wide = bool(w & 0x4000)         # s16 pixels a height (past 16 levels)
        if wide:
            unit = 1
        grids = {}
        for c in range(cells):
            o = off + c * (2 + G * (2 if wide else 1))
            x, y = b[o], b[o + 1]
            vals = struct.unpack_from("<%d%s" % (G, "h" if wide else "b"), b, o + 2)
            grids[(x, y)] = [[vals[j * side + i] * unit for i in range(side)] for j in range(side)]
        out[lid] = dict(base=base, w=w & 0x03FF, h=h & 0x03FF, level=bool(w & 0x8000), grids=grids)
    at = struct.unpack_from("<I", b, len(b) - 8)[0]
    nv, nc = struct.unpack_from("<HH", b, at)
    variants = [struct.unpack_from("<HH16H", b, at + 4 + 36 * k) for k in range(nv)]
    cuts = {}
    o = at + 4 + 36 * nv
    for c in range(nc):
        lay, x, y, var, foot, ground, wall, sides, flags = struct.unpack_from("<HBBHhHHBB", b, o + 14 * c)
        cuts[(lay, x, y)] = dict(variant=var, foot=foot, ground=ground, wall=wall, sides=sides, flags=flags,
                                 drawing=variants[var] if var < nv else None)
    return out, cuts


if __name__ == "__main__":
    import sys
    L, C = load(sys.argv[1])
    lid = int(sys.argv[2])
    x0, x1, y0, y1 = map(int, sys.argv[3:7])
    g = L[lid]["grids"]
    for cy in range(y0, y1):
        for j in range(5):
            print("%2d.%d " % (cy, j) + "|".join(
                (" ".join("%3d" % v for v in g[(cx, cy)][j]) if (cx, cy) in g else " " * 19) for cx in range(x0, x1)))
        print("     " + "|".join("%-19s" % ("" if (lid, cx, cy) not in C else "f%d v%d s%x" % (
            C[(lid, cx, cy)]["foot"], C[(lid, cx, cy)]["variant"], C[(lid, cx, cy)]["sides"])) for cx in range(x0, x1)))
