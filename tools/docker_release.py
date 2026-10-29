#!/usr/bin/env python3
"""Make a release in the pinned environment: the same toolchain, Python and
Pillow every time, so the voxel data (and its precompiled programs) never
depend on the maintainer's machine.

    python tools/docker_release.py --rom baserom.gba [--spanish-rom esmeralda.gba ...]
                                   [build_release.py options, e.g. --portuguese --skip-cia]

Needs Docker and the network (Git and the image registries); the release
itself runs offline in the container. Steps:

1. a fresh tree, build/release-tree (tools/bootstrap.py: the pinned
   pokeemerald, the patches, this checkout's 3ds_port/, builder/, tools/);
2. the image (tools/release_env/Dockerfile) from upstream.lock: devkitARM by
   digest, Python and Pillow by version ([python], [devkitarm]);
3. the original game's ELF, whose symbols the recipe needs: the pinned
   pokeemerald, unpatched, built with the pinned agbcc ([agbcc]) in
   build/gba-elf; it must rebuild the supported ROM byte for byte;
4. in the container: the decomp's tools and generated files, then
   tools/build_release.py (make release, the recipes, the voxel precompile,
   the payloads, the audits) with the ELF and the ROMs given.

The assets end in build/release-tree/dist/ and are copied to dist/.

The Windows builder's executable is not made here (PyInstaller makes one for
the system it runs on): build_release.py runs with --skip-exe, and the
Windows ZIP is then completed on Windows (docs/RELEASING.md). The HOME Menu
forwarder needs makerom and bannertool in 3ds_port/tools-bin/ (Linux builds);
without them pass --skip-cia. Linux, macOS or WSL hosts (the checkout is
mounted at its own path).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TREE = ROOT / "build" / "release-tree"
GBA = ROOT / "build" / "gba-elf"
ENV = ROOT / "tools" / "release_env"
ROM_OPTIONS = ("rom", "spanish_rom", "french_rom", "italian_rom", "german_rom", "japanese_rom")


def run(cmd, cwd=None):
    print("+ " + " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], cwd=cwd, check=True)


def checkout(path: Path, repo: str, commit: str) -> None:
    """`commit` of `repo` in `path`, fetched shallow."""
    if not (path / ".git").exists():
        path.mkdir(parents=True, exist_ok=True)
        run(["git", "init", "-q"], cwd=path)
        run(["git", "remote", "add", "origin", repo], cwd=path)
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=path, capture_output=True, text=True).stdout.strip()
    if head != commit:
        run(["git", "fetch", "-q", "--depth", "1", "origin", commit], cwd=path)
        run(["git", "checkout", "-q", "--force", "--detach", commit], cwd=path)


def image(lock: dict) -> str:
    """The release image, built once per upstream.lock and Dockerfile."""
    py = lock["python"]
    digest = hashlib.sha256((ENV / "Dockerfile").read_bytes()
                            + repr((py["image"], py["pillow"], lock["devkitarm"]["image"])).encode()).hexdigest()
    tag = "emerald3ds-release:" + digest[:16]
    if subprocess.run(["docker", "image", "inspect", tag], capture_output=True).returncode != 0:
        run(["docker", "build", "-t", tag, "--build-arg", "PYTHON_IMAGE=" + py["image"],
             "--build-arg", "DEVKITARM_IMAGE=" + lock["devkitarm"]["image"],
             "--build-arg", "PILLOW=" + py["pillow"], ENV])
    return tag


def docker_run(tag: str, script: str, mounts: list[tuple[Path, str, bool]]) -> None:
    cmd = ["docker", "run", "--rm", "--network", "none", "-e", "HOME=/tmp"]
    if hasattr(os, "getuid"):
        cmd += ["--user", "%d:%d" % (os.getuid(), os.getgid())]
    for src, dst, ro in mounts:
        cmd += ["-v", "%s:%s%s" % (src, dst, ":ro" if ro else "")]
    run(cmd + [tag, "bash", "-ec", script])


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", type=Path, required=True)
    ap.add_argument("--spanish-rom", type=Path)
    ap.add_argument("--french-rom", type=Path)
    ap.add_argument("--italian-rom", type=Path)
    ap.add_argument("--german-rom", type=Path)
    ap.add_argument("--japanese-rom", type=Path)
    ap.add_argument("--keep-tree", action="store_true",
                    help="reuse build/release-tree as it is (default: made anew, nothing generated left)")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    args, passthrough = ap.parse_known_args()
    lock = tomllib.loads((ROOT / "upstream.lock").read_text(encoding="utf-8"))
    version = re.search(r"^APP_VERSION := (\S+)$", (ROOT / "3ds_port" / "Makefile").read_text(encoding="utf-8"),
                        re.M).group(1)

    if TREE.exists() and not args.keep_tree:
        shutil.rmtree(TREE)
    run([sys.executable, ROOT / "tools" / "bootstrap.py", "--dir", TREE])
    tag = image(lock)

    checkout(GBA / "pokeemerald", lock["pokeemerald"]["repository"], lock["pokeemerald"]["commit"])
    checkout(GBA / "agbcc", lock["agbcc"]["repository"], lock["agbcc"]["commit"])
    elf, gba = GBA / "pokeemerald" / "pokeemerald.elf", GBA / "pokeemerald" / "pokeemerald.gba"
    if not (elf.exists() and gba.exists() and hashlib.sha1(gba.read_bytes()).hexdigest() == lock["rom"]["sha1"]):
        docker_run(tag, "cd /gba/agbcc && ./build.sh && ./install.sh /gba/pokeemerald && "
                        "cd /gba/pokeemerald && make -j%d tools && make -j%d" % (args.jobs, args.jobs),
                   [(GBA, "/gba", False)])
    if hashlib.sha1(gba.read_bytes()).hexdigest() != lock["rom"]["sha1"]:
        sys.exit("docker_release: the pinned pokeemerald did not rebuild the supported ROM")

    mounts = [(ROOT, str(ROOT), False), (GBA, "/gba", True)]
    options = []
    for name in ROM_OPTIONS:
        rom = getattr(args, name)
        if rom is None:
            continue
        rom = rom.resolve()
        mounts.append((rom, "/roms/%s%s" % (name, rom.suffix), True))
        options += ["--" + name.replace("_", "-"), "/roms/%s%s" % (name, rom.suffix)]
    make = "make -j%d" % args.jobs
    script = " && ".join([
        "cd %s" % TREE,
        "%s tools" % make,
        "%s generated" % make,
        " ".join(["python3", "tools/build_release.py", "--version", version, "--gba-elf",
                  "/gba/pokeemerald/pokeemerald.elf", "--make", "'%s'" % make, "--skip-exe"] + options
                 + ["'%s'" % a for a in passthrough]),
    ])
    docker_run(tag, script, mounts)
    (ROOT / "dist").mkdir(exist_ok=True)
    for path in sorted((TREE / "dist").iterdir()):
        if path.is_file():
            shutil.copy2(path, ROOT / "dist" / path.name)
            print("docker_release: dist/%s" % path.name)


if __name__ == "__main__":
    main()
