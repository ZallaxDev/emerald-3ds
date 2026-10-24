#!/usr/bin/env python3
"""Assemble a Emerald 3DS release.

    python tools/build_release.py --version 0.1.0 --rom baserom.gba \\
        --gba-elf path/to/pokeemerald.elf

Steps (each can be skipped when its output is already there):

1. `make release` in 3ds_port: dist/Emerald3DS.3dsx and .smdh, engine files only;
2. the recipe (tools/gen_recipe.py) from the build's staging and the ROM;
3. the payload: executable, recipe and the voxel generator scripts; with
   --spanish-rom / --french-rom / --italian-rom / --german-rom also payload/es/ (resp. payload/fr/, payload/it/, payload/de/), that
   language's executable and its recipe,
   built from a copy of this tree localized from that ROM (build/release-es);
4. the standalone builder (PyInstaller, one folder, no UPX);
5. dist/Emerald3DS-v<version>-Windows.zip with the builder, the payload,
   README.txt and LICENSES/;
6. the standalone dist/Emerald3DS.3dsx and dist/Emerald3DS.smdh (quick update
   of the executable when the data ABI did not change; dist/Emerald3DS-es.3dsx
   for the Spanish variant), and the HOME Menu
   forwarder dist/Emerald3DS-Forwarder.cia (3ds_port/forwarder, needs makerom
   and bannertool; --skip-cia leaves it out);
7. dist/Emerald3DS-WebPayload.zip and dist/web-manifest.json for the web
   builder (tools/build_web_payload.py);
8. tools/release_audit.py over every ZIP (and a scan for any run of the ROMs'
   bytes), then SHA256SUMS.txt over every release asset.

Attach everything in dist/ listed in SHA256SUMS.txt, and SHA256SUMS.txt itself,
to the GitHub release; see docs/RELEASING.md.

The ROMs are read to write the recipes and to audit; they are never copied into
the release. The release never contains a data pack.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

import build_web_payload

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "3ds_port"
DIST = ROOT / "dist"
CIA_NAME = "Emerald3DS-Forwarder.cia"
GENERATORS = ["gen_voxel_regions.py", "gen_voxel_sign_masks.py",
              "gen_voxel_relief.py", "gen_voxel_buildings.py", "gen_intro_margins.py"]
VOXELGEN_FILES = ["src/voxel/voxel_regions.h", "assets/voxel/relief_fixes.json"]


def run(cmd, cwd=None, env=None):
    print("+ " + " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, check=True)


def local_closure(scripts_dir: Path, names: list[str]) -> tuple[list[str], set[str]]:
    """The generator scripts plus every sibling module they import, and the
    external modules they need bundled."""
    todo = [n for n in names if (scripts_dir / n).exists()]
    seen, external = set(), set()
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        seen.add(name)
        tree = ast.parse((scripts_dir / name).read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            mods = []
            if isinstance(node, ast.Import):
                mods = [a.name for a in node.names]
            elif isinstance(node, ast.ImportFrom) and node.module and node.level == 0:
                mods = [node.module]
            for mod in mods:
                top = mod.split(".")[0]
                if (scripts_dir / (top + ".py")).exists():
                    todo.append(top + ".py")
                else:
                    external.add(mod)
    return sorted(seen), external


def build_localized_variant(args, tag: str, out: Path, locale: dict) -> None:
    """payload/<code>/: a localized executable and its recipe. The same code as
    the English variant (a copy of this tree's sources: every file Git does not
    ignore, so no build output), localized from the clean ROM of that language
    by tools/localize_<language>.py, then built and covered like the English one."""
    tree = ROOT / "build" / ("release-" + locale["code"])
    make = args.make.split() if args.make else ["make"]
    if not args.skip_make:
        if tree.exists():
            shutil.rmtree(tree)
        listed = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                                cwd=ROOT, capture_output=True, check=True).stdout.decode("utf-8")
        for rel in sorted(set(filter(None, listed.split("\0")))):
            src = ROOT / rel
            if src.is_file():  # skips files deleted from the working tree
                dst = tree / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
        run(make + ["tools"], cwd=tree)
        run(make + ["generated"], cwd=tree)
        run([sys.executable, ROOT / "tools" / locale["script"], "--tree", tree, "--rom", locale["rom"]])
        run(make + ["release"], cwd=tree / "3ds_port")
    port = tree / "3ds_port"
    # The localized game's symbol names and addresses (no ROM contents), for the recipe's hints.
    reference = tree / "build" / (locale["stem"] + "-reference.o")
    assembler = Path(args.nm).with_name(Path(args.nm).name[:-2] + "as")  # arm-none-eabi-nm -> -as
    run([assembler, "-o", reference,
         tree / "build" / (locale["stem"] + "-reference.s")])
    recipe = DIST / ("emerald3ds-%s.recipe" % locale["code"])
    if not args.skip_recipe:
        run([sys.executable, ROOT / "tools/gen_recipe.py", "--romfs", port / "romfs", "--rom", locale["rom"],
             "--out", recipe, "--release", tag, "--elf", port / "emerald3ds.elf",
             "--gba-elf", reference, "--image-elf", port / "build/gamedata_image.elf",
             "--image-map", port / "build/gamedata_image.map", "--nm", args.nm, "--decomp", tree,
             "--report", DIST / ("recipe-literal-report-%s.txt" % locale["code"])])
    out.mkdir(parents=True, exist_ok=True)
    for name in ("Emerald3DS.3dsx", "Emerald3DS.smdh"):
        shutil.copy2(port / "dist" / name, out / name)
    shutil.copy2(recipe, out / "emerald3ds.recipe")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True)
    ap.add_argument("--rom", type=Path, required=True)
    ap.add_argument("--gba-elf", type=Path, required=True, help="the original game's ELF (symbol names)")
    ap.add_argument("--spanish-rom", type=Path, default=None,
                    help="clean Spanish (BPES) ROM: also build the Spanish variant (payload/es/)")
    ap.add_argument("--french-rom", type=Path, default=None,
                    help="clean French (BPEF) ROM: also build the French variant (payload/fr/)")
    ap.add_argument("--italian-rom", type=Path, default=None,
                    help="clean Italian (BPEI) ROM: also build the Italian variant (payload/it/)")
    ap.add_argument("--german-rom", type=Path, default=None,
                    help="clean German (BPED) ROM: also build the German variant (payload/de/)")
    ap.add_argument("--nm", default=os.environ.get("NM", "arm-none-eabi-nm"))
    ap.add_argument("--make", default=None, help="command that runs make in 3ds_port (default: make)")
    ap.add_argument("--skip-make", action="store_true")
    ap.add_argument("--skip-recipe", action="store_true")
    ap.add_argument("--skip-exe", action="store_true")
    ap.add_argument("--skip-cia", action="store_true", help="no HOME Menu forwarder (needs makerom, bannertool)")
    ap.add_argument("--allow-local-mk", action="store_true",
                    help="build even though 3ds_port/local.mk adds this machine's settings")
    args = ap.parse_args()

    # The English variant is built in this tree, so 3ds_port/local.mk (ignored
    # by Git, e.g. a measurement aid such as -DCTR_MEASURE_AID) would reach the
    # release; the localized copies never see it.
    if (PORT / "local.mk").exists() and not args.allow_local_mk:
        sys.exit("3ds_port/local.mk exists and would change the English build: move it away "
                 "and rebuild (make clean), or pass --allow-local-mk if it is meant to ship")

    # The version the title screen shows is the port Makefile's.
    shown = re.search(r"^APP_VERSION := (\S+)$", (PORT / "Makefile").read_text(encoding="utf-8"), re.M)
    if not shown or shown.group(1) != args.version:
        sys.exit("3ds_port/Makefile has APP_VERSION %s, not %s: bump it first"
                 % (shown.group(1) if shown else "(none)", args.version))
    tag = "v" + args.version
    release = DIST / ("Emerald3DS-%s-Windows" % tag)
    payload = release / "payload"

    if not args.skip_make:
        run((args.make.split() if args.make else ["make"]) + ["release"], cwd=PORT)
        # the buildings' proofs even when buildings.bin is up to date (the
        # builder only exports and trusts this build's CRC); a localized tree
        # is a fresh copy, so its make makes and verifies the file anew
        run((args.make.split() if args.make else ["make"]) + ["verify-voxel-buildings"], cwd=PORT)
    DIST.mkdir(parents=True, exist_ok=True)
    recipe = DIST / "emerald3ds.recipe"
    if not args.skip_recipe:
        run([sys.executable, ROOT / "tools/gen_recipe.py", "--romfs", PORT / "romfs", "--rom", args.rom,
             "--out", recipe, "--release", tag, "--elf", PORT / "emerald3ds.elf",
             "--gba-elf", args.gba_elf, "--image-elf", PORT / "build/gamedata_image.elf",
             "--image-map", PORT / "build/gamedata_image.map", "--nm", args.nm,
             "--report", DIST / "recipe-literal-report.txt"])

    if release.exists():
        shutil.rmtree(release)
    payload.mkdir(parents=True)
    for name in ("Emerald3DS.3dsx", "Emerald3DS.smdh"):
        shutil.copy2(PORT / "dist" / name, payload / name)
    shutil.copy2(recipe, payload / "emerald3ds.recipe")
    scripts, external = local_closure(PORT / "scripts", GENERATORS)
    (payload / "voxelgen" / "scripts").mkdir(parents=True)
    for name in scripts:
        shutil.copy2(PORT / "scripts" / name, payload / "voxelgen" / "scripts" / name)
    for rel in VOXELGEN_FILES:
        dst = payload / "voxelgen" / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(PORT / rel, dst)
    locales = []
    if args.spanish_rom:
        locales.append({"code": "es", "stem": "spanish", "rom": args.spanish_rom,
                        "script": "localize_spanish.py"})
    if args.french_rom:
        locales.append({"code": "fr", "stem": "french", "rom": args.french_rom,
                        "script": "localize_french.py"})
    if args.italian_rom:
        locales.append({"code": "it", "stem": "italian", "rom": args.italian_rom,
                        "script": "localize_italian.py"})
    if args.german_rom:
        locales.append({"code": "de", "stem": "german", "rom": args.german_rom,
                        "script": "localize_german.py"})
    for locale in locales:
        build_localized_variant(args, tag, payload / locale["code"], locale)

    if not args.skip_exe:
        build = ROOT / "builder" / "build"
        build.mkdir(parents=True, exist_ok=True)
        (build / "hidden-imports.txt").write_text("\n".join(sorted(external)) + "\n", encoding="utf-8")
        run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean",
             "--distpath", build / "dist", "--workpath", build / "pyi", "emerald3ds-builder.spec"],
            cwd=ROOT / "builder")
        shutil.copytree(build / "dist" / "Emerald3DS-Builder", release, dirs_exist_ok=True)

    shutil.copy2(ROOT / "builder" / "README-release.txt", release / "README.txt")
    licenses = release / "LICENSES"
    licenses.mkdir()
    for rel in ("LICENSE-PORT.md", "NOTICE.md", "AI_DISCLOSURE.md"):
        src = ROOT / rel if (ROOT / rel).exists() else ROOT / "public" / rel  # public/ in the workspace
        if src.exists():
            shutil.copy2(src, licenses / rel)
    for rel in ("3ds_port/src/voxel/NOTICE.md",):
        shutil.copy2(ROOT / rel, licenses / "voxel-NOTICE.md")

    archive = DIST / ("Emerald3DS-%s-Windows.zip" % tag)
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in sorted(release.rglob("*")):
            if path.is_file():
                zf.write(path, path.relative_to(DIST).as_posix())

    standalone = []
    for name in ("Emerald3DS.3dsx", "Emerald3DS.smdh"):
        shutil.copy2(payload / name, DIST / name)
        standalone.append(DIST / name)
    for locale in locales:
        # Release asset names are unique; they are installed as Emerald3DS.3dsx all the same.
        asset = DIST / ("Emerald3DS-%s.3dsx" % locale["code"])
        shutil.copy2(payload / locale["code"] / "Emerald3DS.3dsx", asset)
        standalone.append(asset)
    cia = None
    if not args.skip_cia:
        forwarder = PORT / "forwarder"
        run((args.make.split() if args.make else ["make"]) + ["clean", "all"], cwd=forwarder)
        cia = DIST / CIA_NAME
        shutil.copy2(forwarder / "dist" / CIA_NAME, cia)
    web_zip, web_manifest = build_web_payload.build(payload, args.version, DIST,
                                                    cia_forwarder=CIA_NAME if cia else None)

    for rom in [args.rom] + [locale["rom"] for locale in locales]:
        run([sys.executable, ROOT / "tools/release_audit.py", "--zip", archive, "--strict", "--rom", rom])
        run([sys.executable, ROOT / "tools/release_audit.py", "--zip", web_zip, "--strict", "--rom", rom,
             "--web-payload"])
    sums = DIST / "SHA256SUMS.txt"
    assets = [archive] + standalone + ([cia] if cia else []) + [web_zip, web_manifest]
    lines = ["%s  %s" % (sha256(path), path.name) for path in assets]
    for path in sorted(payload.glob("*")) + sorted(p for locale in locales for p in payload.glob(locale["code"] + "/*")):
        if path.is_file():
            lines.append("%s  payload/%s" % (sha256(path), path.relative_to(payload).as_posix()))
    sums.write_text("\n".join(lines) + "\n", encoding="ascii")
    for path in assets:
        print("release asset: %s (%.1f MiB)" % (path, path.stat().st_size / 1048576))
    print("release asset: %s" % sums)


if __name__ == "__main__":
    main()
