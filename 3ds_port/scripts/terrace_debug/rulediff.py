"""rulediff.py VARIANT.py [LAYOUT ...]: every cell whose solved piece changes
between scripts/voxel_terraces.py and VARIANT.py (a copy of it with one rule
changed or switched off), over every layout in voxel_terraces.GROUPS or the
given ones. Run it before keeping a rule: the totals of regress.sh can stay
still while a single approved cell changes. Render each listed cell both ways
(voxel_scene.py view, voxel_view_scan.py --at) and keep the rule only if it
changes no approved cell, or changes it clearly for the better."""
import importlib.util
import os
import sys

VARIANT = os.path.abspath(sys.argv[1])  # before _common moves to 3ds_port/
import _common as cm  # noqa: E402


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


import voxel_relief_fixes as vrf  # noqa: E402
import voxel_terraces as base  # noqa: E402
variant = load(VARIANT, "voxel_terraces_variant")
ref = vrf.parse(open(cm.PRE, "rb").read())
layouts = sys.argv[2:] or list(base.GROUPS)
total = 0
for lid in layouts:
    a = base.solve(lid, ref)
    sys.modules["voxel_terraces"] = variant
    b = variant.solve(lid, ref)
    sys.modules["voxel_terraces"] = base
    cells = a["cells"]
    for key in sorted(set(a["pieces"]) | set(b["pieces"])):
        if a["pieces"].get(key) != b["pieces"].get(key):
            total += 1
            print(lid, cells.src.get(key), "base", a["pieces"].get(key), "variant", b["pieces"].get(key))
print("%d cells differ" % total)
