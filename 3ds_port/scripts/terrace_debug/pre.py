"""pre.py OUT.bin: the relief of every enabled map before the terraces (the
solver's reference for the other tools). Run with VOXEL_RELIEF_CACHE set to
reuse the cache."""
import sys
import _common  # noqa: F401
import gen_voxel_relief as gr

drawn = [lid for members in gr.DRAWN.values() for lid in members]
lids = list(gr.ENABLED) + [lid for lid in drawn if lid not in gr.ENABLED]
lids += [lid for lid in gr.ledge_layouts() if lid not in lids]
gr.export(list(dict.fromkeys(lids)), sys.argv[1])
