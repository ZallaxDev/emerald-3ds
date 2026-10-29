# terrace_debug: tools for working on the terrace rules

Small tools around `scripts/voxel_terraces.py` (mountains read as whole
levels, one tile a level). They are what the terrace rules are developed and
checked with; the method is in `docs/VOXEL_MOUNTAINS.md`.

All of them run from anywhere (they move to `3ds_port/` themselves) and take
**layout coordinates** (cells of the map, not of the terrace group). The
solver's reference is the relief before the terraces, `TERRACE_PRE`
(default `3ds_port/build/relief_pre.bin`), made once with `pre.py`.

```sh
cd 3ds_port
# the relief before the terraces (about 6 minutes; the cache speeds up later runs)
VOXEL_RELIEF_CACHE=build/relief_cache python3 scripts/terrace_debug/pre.py build/relief_pre.bin
# a trial relief: the terraces of some groups over it (about 2 minutes)
python3 scripts/terrace_debug/trial.py build/relief_pre.bin build/relief_t.bin LAYOUT_ROUTE111
# look at it
python3 scripts/voxel_view_scan.py LAYOUT_ROUTE111 --relief build/relief_t.bin --at 27,119 --view /tmp/v.png
```

| Tool | What it does |
|---|---|
| `pre.py OUT.bin` | the relief of every enabled map before the terraces (the reference of the others) |
| `trial.py PRE.bin OUT.bin [LAYOUT ...]` | PRE plus the terraces (of those layouts' groups, or all), the sea ramps and `relief_fixes.json`: what `gen_voxel_relief.py` does, faster |
| `pieces.py LAYOUT x0,y0,x1,y1` | each cell's kind, band side and solved piece (`S@3`, `W@2`, `cSW@3`...) |
| `trace.py LAYOUT x0,y0,x1,y1` | the pieces after `blocks` and after each pass (`staircases`, `level_bands`, `unstand`) |
| `watch.py LAYOUT x,y` | every assignment to that cell's piece, with the function and line that made it |
| `segs.py LAYOUT x0,y0,x1,y1` | the band columns (side, head, bands, foot) |
| `votes.py LAYOUT x,y [x,y ...]` | the votes between the terraces of those cells and the others, and the ones the solve dropped: where the drawing contradicts itself |
| `rgrid.py RELIEF.bin LAYOUT x0,y0,x1,y1` | the corner heights (NW NE SW SE) written in a relief.bin |
| `celldiff.sh LAYOUT A.bin B.bin` | the cells whose fault count changed by more than 3000 px between two reliefs |
| `regress.sh BASE.bin NEW.bin OUTDIR [LIST]` | `voxel_view_scan` of every terraced map (or the listed ones) with both reliefs, 3 at a time; read `OUTDIR/summary.txt` with `summary.py` |
| `summary.py OUTDIR/summary.txt` | per map, the fault classes that moved by more than 5000 px, and the totals |
| `rulediff.py VARIANT.py [LAYOUT ...]` | the cells whose solved piece changes between `voxel_terraces.py` and a copy with one rule changed or switched off |

`regress.sh` totals can stay still while one approved cell changes: before
keeping a rule, run `rulediff.py` over every group and render each listed
cell both ways.
