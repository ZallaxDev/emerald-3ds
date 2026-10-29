"""Shared setup: the repository's 3ds_port on the path, cwd there, and the
pre-terrace relief used as the solver's reference (TERRACE_PRE, default
3ds_port/build/relief_pre.bin - made with pre.py)."""
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
PORT = os.path.join(ROOT, "3ds_port")
sys.path.insert(0, os.path.join(PORT, "scripts"))
os.chdir(PORT)
PRE = os.environ.get("TERRACE_PRE", os.path.join(PORT, "build", "relief_pre.bin"))


def offset(vt, lid):
    """(ox, oy) of a layout inside its terrace group."""
    m = [m for m in vt.group_of(lid) if m[0] == lid][0]
    return m[1], m[2]


def box(arg):
    return tuple(int(v) for v in arg.split(","))


def solve(lid):
    import voxel_terraces as vt
    import voxel_relief_fixes as vrf
    return vt.solve(lid, vrf.parse(open(PRE, "rb").read()))
