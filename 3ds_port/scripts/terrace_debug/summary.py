"""summary.py OUTDIR/summary.txt: per layout, the classes that changed by more
than 5000 px (base -> new), and the totals."""
import collections
import re
import sys

A, B = collections.Counter(), collections.Counter()
for line in open(sys.argv[1]):
    if "||" not in line:
        continue
    lay, rest = line.split(None, 1)
    a, b = rest.split("||")
    pa = {k: int(v) for k, v in re.findall(r"(\S+) \d+ cells \((\d+) px\)", a)}
    pb = {k: int(v) for k, v in re.findall(r"(\S+) \d+ cells \((\d+) px\)", b)}
    if lay != "LAYOUT_ROUTE111_NO_MIRAGE_TOWER":
        A.update(pa)
        B.update(pb)
    ch = ["%s %d->%d" % (k, pa.get(k, 0), pb.get(k, 0)) for k in pb if abs(pb.get(k, 0) - pa.get(k, 0)) > 5000]
    if ch:
        print("%-34s %+9d  %s" % (lay, sum(pb.values()) - sum(pa.values()), ", ".join(ch)))
for k in A:
    print("%-13s %10d -> %10d" % (k, A[k], B[k]))
print("total", sum(A.values()), "->", sum(B.values()))
