#!/bin/bash
# celldiff.sh LAYOUT A.bin B.bin: the cells whose fault pixels (voxel_view_scan,
# step 4) changed by more than 3000 between two reliefs, per class.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPTS="$HERE/.."
TMP="${TMPDIR:-/tmp}/terrace_debug"; mkdir -p "$TMP"
A="$(realpath "$2")"; B="$(realpath "$3")"
for R in "$A" "$B"; do
  (cd "$SCRIPTS" && python3 voxel_view_scan.py "$1" --relief "$R" --step 4 --list 5000 2>/dev/null | tail -n +2) \
    > "$TMP/cd_$1_$(basename "$R").txt"
done
python3 - "$1" "$TMP/cd_$1_$(basename "$A").txt" "$TMP/cd_$1_$(basename "$B").txt" <<'PY'
import re, sys
def load(f):
    d = {}
    for line in open(f):
        p = line.split(None, 1)
        if len(p) == 2:
            d[p[0]] = {k: int(v) for k, v in re.findall(r'([\w-]+) (\d+) px', p[1])}
    return d
a, b = load(sys.argv[2]), load(sys.argv[3])
rows = []
for k in set(a) | set(b):
    for cls in ("hole", "side", "flat", "deformed", "standing", "drawn-flat", "drawn-raised"):
        d = b.get(k, {}).get(cls, 0) - a.get(k, {}).get(cls, 0)
        if abs(d) > 3000:
            rows.append((d, k, cls, a.get(k, {}).get(cls, 0), b.get(k, {}).get(cls, 0)))
for r in sorted(rows, reverse=True):
    print(sys.argv[1], "%-7s %-12s %8d -> %8d" % r[1:])
PY
