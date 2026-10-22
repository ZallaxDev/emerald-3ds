#!/usr/bin/env python3
"""Build a complete source tree: pinned upstream + Emerald 3DS patches + port.

    python tools/bootstrap.py                 # -> build/upstream
    python tools/bootstrap.py --make -j8      # and build the 3DSX there
    python tools/bootstrap.py --clean         # start again from the pinned commit
    python tools/bootstrap.py --make --spanish-rom esmeralda.gba   # Spanish (BPES) build
    python tools/bootstrap.py --make --french-rom emeraude.gba     # French (BPEF) build
    python tools/bootstrap.py --make --italian-rom smeraldo.gba    # Italian (BPEI) build
    python tools/bootstrap.py --make --port-lang pt_br   # Brazilian Portuguese over the English game
    python tools/bootstrap.py --make --german-rom smaragd.gba      # German (BPED) build

1. Reads upstream.lock and fetches exactly that commit of pret/pokeemerald into
   build/upstream (a shallow fetch of one commit).
2. Applies patches/pokeemerald/*.patch in order. The tree is reset to the
   pinned commit first whenever the patch set changed since the last run.
3. Places 3ds_port/, builder/ and tools/ inside it, so the Makefile's relative
   paths (../tools/port_common, ../builder) resolve as in development.
4. With --make: builds the decomp tools (`make tools`), the generated
   includes (`make generated`) and the 3DSX (`make -C 3ds_port`). Run it from
   a shell where devkitPro, a host compiler and Python are available (on
   Windows, devkitPro's MSYS2 shell with MinGW64 on PATH).
5. With --spanish-rom / --french-rom / --italian-rom / --german-rom: after the
   generated includes, tools/localize_spanish.py (docs/SPANISH.md),
   tools/localize_french.py or tools/localize_italian.py or
   tools/localize_german.py stages the texts and graphics of that language
   from the player's clean ROM.
   Switching language back and forth resets the tree.
6. With --port-lang pt_br: the port's own interface (bottom screen labels,
   extras, data pack errors) in Brazilian Portuguese, and the game's texts
   from tools/locales/pt_br (tools/localize_portuguese.py, docs/PORTUGUESE.md).
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
import os
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OVERLAY = ["3ds_port", "builder", "tools"]

if "/opt/devkitpro" not in os.environ.get("PATH", ""):
    extra_paths = [p for p in ("/opt/homebrew/opt/make/libexec/gnubin", "/opt/homebrew/bin",
                               "/opt/devkitpro/tools/bin", "/opt/devkitpro/devkitARM/bin")
                   if os.path.isdir(p)]
    if extra_paths:
        os.environ["PATH"] = os.pathsep.join(extra_paths + [os.environ.get("PATH", "")])
os.environ.setdefault("DEVKITPRO", "/opt/devkitpro")
os.environ.setdefault("DEVKITARM", "/opt/devkitpro/devkitARM")
libpng_pc = "/opt/homebrew/opt/libpng/lib/pkgconfig"
if os.path.exists(libpng_pc):
    current_pkg = os.environ.get("PKG_CONFIG_PATH", "")
    os.environ["PKG_CONFIG_PATH"] = f"{libpng_pc}{os.pathsep}{current_pkg}" if current_pkg else libpng_pc


def run(cmd, cwd=None):
    print("+ " + " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], cwd=cwd, check=True)


def patch_digest(patches: list[Path]) -> str:
    digest = hashlib.sha256()
    for p in patches:
        digest.update(p.name.encode())
        digest.update(p.read_bytes())
    return digest.hexdigest()


def fetch(tree: Path, repo: str, commit: str) -> None:
    tree.mkdir(parents=True, exist_ok=True)
    if not (tree / ".git").exists():
        run(["git", "init", "-q"], cwd=tree)
        run(["git", "remote", "add", "origin", repo], cwd=tree)
    run(["git", "fetch", "-q", "--depth", "1", "origin", commit], cwd=tree)
    run(["git", "checkout", "-q", "--force", "--detach", commit], cwd=tree)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", type=Path, default=ROOT / "build" / "upstream")
    ap.add_argument("--clean", action="store_true", help="reset the tree to the pinned commit first")
    ap.add_argument("--spanish-rom", type=Path, help="stage the Spanish data from a clean BPES ROM")
    ap.add_argument("--french-rom", type=Path, help="stage the French data from a clean BPEF ROM")
    ap.add_argument("--italian-rom", type=Path, help="stage the Italian data from a clean BPEI ROM")
    ap.add_argument("--port-lang", choices=["en", "pt_br"], default="en",
                    help="language of the port's own interface (PORT_LANG=)")
    ap.add_argument("--german-rom", type=Path, help="stage the German data from a clean BPED ROM")
    ap.add_argument("--make", action="store_true", help="build the tools and the 3DSX afterwards")
    ap.add_argument("-j", "--jobs", type=int, default=4)
    ap.add_argument("--python", default=sys.executable, help="Python the build calls (PYTHON=)")
    args = ap.parse_args()
    if args.port_lang == "pt_br" and (args.spanish_rom or args.french_rom or args.italian_rom or args.german_rom):
        ap.error("--port-lang pt_br goes over the English game, not the Spanish, French, Italian or German one")

    lock = tomllib.loads((ROOT / "upstream.lock").read_text(encoding="utf-8"))
    repo, commit = lock["pokeemerald"]["repository"], lock["pokeemerald"]["commit"]
    tree = args.dir.resolve()
    previous_locale = (tree / ".emerald3ds-locale").exists()
    previous_name = (tree / ".emerald3ds-locale").read_text(encoding="utf-8").strip() if previous_locale else ""
    locale = None
    if args.spanish_rom:
        locale = ("spanish", args.spanish_rom, "localize_spanish.py", "spanish.json.gz")
    elif args.french_rom:
        locale = ("french", args.french_rom, "localize_french.py", "french.json.gz")
    elif args.italian_rom:
        locale = ("italian", args.italian_rom, "localize_italian.py", "italian.json.gz")
    elif args.german_rom:
        locale = ("german", args.german_rom, "localize_german.py", "german.json.gz")
    portuguese = args.port_lang == "pt_br"
    patches = sorted((ROOT / "patches" / "pokeemerald").glob("*.patch"))
    marker = tree / ".emerald3ds-patches"
    digest = patch_digest(patches)

    head = ""
    if (tree / ".git").exists():
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=tree, capture_output=True,
                              text=True).stdout.strip()
    stale = not marker.exists() or marker.read_text().strip() != digest
    if args.clean or locale or portuguese or previous_locale or head != commit or stale:
        if head != commit:
            fetch(tree, repo, commit)
        run(["git", "reset", "-q", "--hard", commit], cwd=tree)
        (tree / ".emerald3ds-locale").unlink(missing_ok=True)
        if locale or portuguese or previous_locale:
            # Objects and staged data of the other language must not be
            # reused: rebuild every native object when the language changes.
            shutil.rmtree(tree / "3ds_port/build", ignore_errors=True)
            shutil.rmtree(tree / "3ds_port/romfs", ignore_errors=True)
            (tree / "src/data/region_map/region_map_entries.h").unlink(missing_ok=True)
        if previous_name.startswith(("BPES", "BPEF", "BPEI", "BPED")):
            import gzip
            import json
            manifest_name = {"BPEF": "french.json.gz", "BPES": "spanish.json.gz",
                             "BPEI": "italian.json.gz", "BPED": "german.json.gz"}.get(
                                 previous_name.split()[0] if previous_name else "", "spanish.json.gz")
            manifest = json.loads(gzip.decompress((ROOT / "tools/locales" / manifest_name).read_bytes()))
            for relative, _, _ in manifest["graphics"]:
                for name in (relative, relative[:-3]) if relative.endswith(".lz") else (relative,):
                    # Tracked art was restored by the reset; generated locale
                    # art must be regenerated from the pinned source PNGs.
                    tracked = subprocess.run(["git", "ls-files", "--error-unmatch", name],
                                             cwd=tree, capture_output=True).returncode == 0
                    if not tracked:
                        (tree / name).unlink(missing_ok=True)
        if args.clean:
            run(["git", "clean", "-q", "-fdx"], cwd=tree)
        # Files a previous patch set created are untracked after the reset;
        # remove the ones these patches create so they apply again.
        for patch in patches:
            for line in patch.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.startswith("+++ b/"):
                    created = tree / line[6:]
                    if created.exists() and subprocess.run(
                            ["git", "ls-files", "--error-unmatch", line[6:]], cwd=tree,
                            capture_output=True).returncode != 0:
                        created.unlink()
        for patch in patches:
            run(["git", "apply", "--whitespace=nowarn", patch], cwd=tree)
        marker.write_text(digest + "\n")
    else:
        print("bootstrap: upstream %s with %d patches already in place" % (commit[:12], len(patches)))

    for name in OVERLAY:
        src = ROOT / name
        if src.exists():
            shutil.copytree(src, tree / name, dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "build", "dist"))
    shutil.copy2(ROOT / "upstream.lock", tree / "upstream.lock")
    print("bootstrap: tree ready at %s" % tree)

    if args.make or locale:
        run(["make", "tools", "-j%d" % args.jobs], cwd=tree)
        run(["make", "generated", "-j%d" % args.jobs], cwd=tree)
    if locale:
        run([args.python, ROOT / "tools" / locale[2], "--tree", tree,
             "--rom", locale[1].resolve()])
    if portuguese:
        run([args.python, ROOT / "tools/localize_portuguese.py", "--tree", tree])
    if args.make:
        run(["make", "-C", "3ds_port", "-j%d" % args.jobs, "PYTHON=%s" % args.python,
             "PORT_LANG=%s" % args.port_lang], cwd=tree)
    return 0


if __name__ == "__main__":
    sys.exit(main())
