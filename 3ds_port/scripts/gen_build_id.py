#!/usr/bin/env python3
"""gen_build_id.py PORT PATCHES OUT: the build's identity for local wireless play.

A SHA-256 of what the build is made from, not of what it is made with: the
patch set over the pinned pokeemerald (PATCHES, the digest tools/bootstrap.py
leaves in the tree, .emerald3ds-patches; every patch names the commit it
applies to) and the port's own code and data tools (PORT, 3ds_port, with
../tools/port_common beside it). Two systems link only when theirs are the
same (3ds_port/src/3ds_local.c).

The ROM is left out: each language's build stages its texts and graphics from
its own ROM, and those leave the structures the link code exchanges as they
are, so the English, Spanish, French, Italian, German and Japanese builds of
one release link, as the GBA games do. Any change to the code is another
build, whose link messages could disagree with this one's.

Every file under those folders counts, so a tree that kept files of another
branch (bootstrap copies over, it does not delete) has another id than a
fresh one; CI and releases start from a fresh tree.
"""
import hashlib
import sys
from pathlib import Path

# Under PORT: the code and the way it is built (not local.mk, a machine's own).
PORT_PARTS = ("src", "include", "compat", "scripts", "Makefile", "full.mk", "emerald3ds.ld.in")
SKIP = ("__pycache__",)


def files(root: Path, part: str) -> list[Path]:
    path = root / part
    if path.is_file():
        return [path]
    if not path.is_dir():
        raise SystemExit("gen_build_id.py: %s not found" % path)
    return sorted((p for p in path.rglob("*") if p.is_file() and not any(s in p.parts for s in SKIP)),
                  key=lambda p: p.relative_to(root).as_posix())


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    port, patches, out = (Path(p).resolve() for p in sys.argv[1:])
    if not patches.is_file():
        print("gen_build_id.py: %s not found (run tools/bootstrap.py)" % patches, file=sys.stderr)
        return 1
    digest = hashlib.sha256()
    digest.update(b"emerald3ds build id 2\0")
    digest.update(patches.read_text(encoding="ascii").strip().encode() + b"\0")
    sources = [(port, part) for part in PORT_PARTS] + [(port.parent, "tools/port_common")]
    for root, part in sources:
        for path in files(root, part):
            # by name and content, the same on any system and checkout
            digest.update(path.relative_to(root).as_posix().encode() + b"\0")
            digest.update(path.read_bytes().replace(b"\r\n", b"\n") + b"\0")
    out.parent.mkdir(parents=True, exist_ok=True)
    data = digest.digest()
    if not out.exists() or out.read_bytes() != data:
        out.write_bytes(data)
    print("build id: %s" % data.hex()[:16])
    return 0


if __name__ == "__main__":
    sys.exit(main())
