"""trial.py PRE.bin OUT.bin [LAYOUT ...]: PRE plus the terraces (of the given
layouts' groups, or all), the sea ramps and relief_fixes.json - what
gen_voxel_relief.py does, in two minutes instead of six."""
import shutil
import sys
import _common  # noqa: F401
import gen_voxel_relief as gr
import voxel_terraces as vt
import voxel_relief_fixes as vrf

shutil.copy(sys.argv[1], sys.argv[2])
vt.apply_file(sys.argv[2], sys.argv[3:] or None)
gr.apply_sea_ramps(sys.argv[2])
vrf.apply_file(sys.argv[2])
