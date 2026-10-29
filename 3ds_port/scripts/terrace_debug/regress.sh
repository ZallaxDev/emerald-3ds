#!/bin/bash
# regress.sh BASE.bin NEW.bin OUTDIR [LAYOUTFILE]: voxel_view_scan (step 4)
# of every terraced layout (or the listed ones) with both reliefs, 3 at a
# time (BASE scans cached in OUTDIR/base), then OUTDIR/summary.txt; read it
# with summary.py.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPTS="$(realpath "$HERE/..")"
BASE="$(realpath "$1")"; NEW="$(realpath "$2")"; OUT="$(realpath -m "$3")"
mkdir -p "$OUT/base" "$OUT/new"; rm -f "$OUT/summary.txt"
cd "$SCRIPTS"
if [ -n "$4" ]; then LAYS=$(cat "$4"); else
LAYS=$(python3 -c "
import voxel_terraces as vt
out = []
for g in vt.GROUPS:
    for m in vt.group_of(g):
        if m[0] not in out: out.append(m[0])
print(' '.join(out))" 2>/dev/null); fi
one() {
  [ -s "$OUT/base/$1.txt" ] || python3 voxel_view_scan.py "$1" --relief "$BASE" --step 4 --list 15 > "$OUT/base/$1.txt" 2>&1 || true
  python3 voxel_view_scan.py "$1" --relief "$NEW" --step 4 --list 15 > "$OUT/new/$1.txt" 2>&1 || true
}
export -f one; export BASE NEW OUT
echo $LAYS | tr ' ' '\n' | xargs -P 3 -I{} bash -c 'one {}'
for L in $LAYS; do
  echo "$L $(head -1 "$OUT/base/$L.txt" | cut -d: -f2) || $(head -1 "$OUT/new/$L.txt" | cut -d: -f2)" >> "$OUT/summary.txt"
done
