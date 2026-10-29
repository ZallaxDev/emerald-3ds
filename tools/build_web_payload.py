#!/usr/bin/env python3
"""Package a release's payload for the web builder.

    python tools/build_web_payload.py --payload dist/Emerald3DS-v0.1.3-Windows/payload \\
        --version 0.1.3 --out dist

Writes dist/Emerald3DS-WebPayload.zip and dist/web-manifest.json (the same
manifest as inside the ZIP, attached to the release on its own so the website
can read it without downloading the payload). tools/build_release.py calls this
for every release; it can also be run on the payload/ folder of an already
published Windows ZIP.

The contents and the manifest are described in
builder/emerald3ds_builder/webmanifest.py. The ZIP is deterministic: sorted
entries and fixed timestamps, so the same inputs give the same bytes.

    python tools/build_web_payload.py --synthetic OUT

writes a synthetic payload and a synthetic "ROM" for exercising the web
builder without any game data (see make_synthetic below).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder"))

from emerald3ds_builder import __version__ as BUILDER_VERSION, pak  # noqa: E402
from emerald3ds_builder.recipe import Recipe  # noqa: E402
from emerald3ds_builder.rom import KNOWN_CODES, ROM_PROFILES, ROM_SIZE  # noqa: E402
from emerald3ds_builder.webmanifest import (ENTRYPOINT, INSTALL_DIR, INSTALL_FILES, KIND,  # noqa: E402
                                            MANIFEST_NAME, PAYLOAD_NAME, SCHEMA_VERSION, validate)

PACKAGE = ROOT / "builder" / "emerald3ds_builder"
# The desktop-only modules are left out: the browser has no window and no SD card.
PACKAGE_EXCLUDE = {"gui.py", "install.py", "cli.py", "__main__.py"}
LICENSES = [("LICENSE-PORT.md", "LICENSE-PORT.md"), ("NOTICE.md", "NOTICE.md"),
            ("AI_DISCLOSURE.md", "AI_DISCLOSURE.md"), ("3ds_port/src/voxel/NOTICE.md", "voxel-NOTICE.md")]
FIXED_TIME = (2020, 1, 1, 0, 0, 0)
VARIANT_FILES = ("Emerald3DS.3dsx", "Emerald3DS.smdh", "emerald3ds.recipe")
ROM_REGIONS = {"BPEE": ("USA, Europe", "en"), "BPES": ("Spain", "es"), "BPEF": ("France", "fr"),
               "BPEI": ("Italy", "it"), "BPED": ("Germany", "de"), "BPEJ": ("Japan", "ja")}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def collect(payload: Path, package: Path = PACKAGE, licenses_root: Path = ROOT) -> dict[str, bytes]:
    """Every file of the web payload except the manifest: {zip path: bytes}."""
    files: dict[str, bytes] = {}
    for name in VARIANT_FILES:
        src = payload / name
        if not src.is_file():
            raise SystemExit("build_web_payload: %s is missing" % src)
        files["payload/" + name] = src.read_bytes()
    # Other languages: payload/<lang>/ with their own executable and recipe.
    for sub in sorted(p for p in payload.iterdir() if p.is_dir() and (p / "emerald3ds.recipe").is_file()):
        for name in VARIANT_FILES:
            src = sub / name
            if not src.is_file():
                raise SystemExit("build_web_payload: %s is missing" % src)
            files["payload/%s/%s" % (sub.name, name)] = src.read_bytes()
    voxelgen = payload / "voxelgen"
    if voxelgen.is_dir():
        for path in sorted(voxelgen.rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts and path.suffix not in (".pyc", ".pyo"):
                files["payload/voxelgen/" + path.relative_to(voxelgen).as_posix()] = path.read_bytes()
    for path in sorted(package.glob("*.py")):
        if path.name not in PACKAGE_EXCLUDE:
            files["python/emerald3ds_builder/" + path.name] = path.read_bytes()
    for src, dst in LICENSES:
        path = licenses_root / src
        if not path.is_file():
            path = licenses_root / "public" / src  # the private workspace keeps them in public/
        if path.is_file():
            files["licenses/" + dst] = path.read_bytes()
    return files


def make_manifest(files: dict[str, bytes], version: str, tag: str, cia_forwarder: str | None = None,
                  synthetic: bool = False) -> dict:
    recipe = Recipe.from_bytes(files["payload/emerald3ds.recipe"])

    def rom_entry(sha1: str, folder: str | None = None) -> dict:
        """The ROM a recipe is made from (BPES, BPEF, BPEI, BPED or BPEJ for the
        other languages) and the language it builds: the payload folder's
        (payload/pt/ is the Portuguese translation, made from the English ROM),
        else the cartridge's."""
        if synthetic:
            return {"sha1": sha1, "name": "Synthetic test ROM (not a game)", "code": "TEST",
                    "region": "", "language": folder or ""}
        code = next((c for c, sha in ROM_PROFILES.items() if sha == sha1), "BPEE")
        region, language = ROM_REGIONS.get(code, ("", ""))
        return {"sha1": sha1, "name": KNOWN_CODES[code], "code": code, "region": region,
                "language": folder or language}

    def ref(path: str, release_asset: str | None = None) -> dict:
        out = {"path": path, "sha256": sha256(files[path]), "size": len(files[path])}
        if release_asset is not None:
            out["releaseAsset"] = release_asset
        return out

    manifest = {
        "schemaVersion": SCHEMA_VERSION,
        "kind": KIND,
        "releaseVersion": version,
        "releaseTag": tag,
        "recipeRelease": recipe.release,
        "builderVersion": BUILDER_VERSION,
        "packSchema": pak.SCHEMA,
        "dataAbi": "%08x" % recipe.engine_abi,
        "supportedRoms": [rom_entry(recipe.rom_sha1)],
        "pythonRuntime": {"minimumVersion": "3.11", "packages": ["pillow"], "stdlib": ["lzma", "zlib"]},
        "pyodideCompatibility": {"inprocessGenerators": True, "subprocess": False},
        "entrypoint": dict(ENTRYPOINT),
        "install": {"directory": INSTALL_DIR, "files": list(INSTALL_FILES)},
        "assets": {
            "threeDsx": ref("payload/Emerald3DS.3dsx", "Emerald3DS.3dsx"),
            "smdh": ref("payload/Emerald3DS.smdh", "Emerald3DS.smdh"),
            "recipe": ref("payload/emerald3ds.recipe"),
            "ciaForwarder": ({"releaseAsset": cia_forwarder,
                              "target": "sdmc:/3ds/emerald3ds/Emerald3DS.3dsx"} if cia_forwarder else None),
        },
        "files": [{"path": p, "sha256": sha256(d), "size": len(d)} for p, d in sorted(files.items())],
    }
    variants = []
    for lang in sorted({p.split("/")[1] for p in files if p.count("/") == 2 and p.startswith("payload/")
                        and p.endswith("/emerald3ds.recipe")}):
        sub = "payload/%s/" % lang
        variant_recipe = Recipe.from_bytes(files[sub + "emerald3ds.recipe"])
        variants.append({
            "rom": rom_entry(variant_recipe.rom_sha1, lang),
            "dataAbi": "%08x" % variant_recipe.engine_abi,
            "recipeRelease": variant_recipe.release,
            # Release asset names must be unique: Emerald3DS-es.3dsx, installed
            # on the SD card as Emerald3DS.3dsx all the same.
            "assets": {"threeDsx": ref(sub + "Emerald3DS.3dsx", "Emerald3DS-%s.3dsx" % lang),
                       "smdh": ref(sub + "Emerald3DS.smdh"),
                       "recipe": ref(sub + "emerald3ds.recipe")},
        })
    if variants:
        manifest["variants"] = variants
    if synthetic:
        manifest["synthetic"] = True
    return validate(manifest)


def write_zip(out: Path, manifest: dict, files: dict[str, bytes]) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    entries = dict(files)
    entries[MANIFEST_NAME] = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    tmp = out.with_name(out.name + ".tmp")
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for name in sorted(entries):
            info = zipfile.ZipInfo(name, FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            zf.writestr(info, entries[name])
    tmp.replace(out)


def build(payload: Path, version: str, out_dir: Path, cia_forwarder: str | None = None) -> tuple[Path, Path]:
    files = collect(payload)
    manifest = make_manifest(files, version, "v" + version, cia_forwarder)
    archive = out_dir / PAYLOAD_NAME
    write_zip(archive, manifest, files)
    manifest_path = out_dir / MANIFEST_NAME
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return archive, manifest_path


SYNTHETIC_GENERATOR = '''"""Synthetic stand-in for gen_intro_margins.py (web builder tests only).

It keeps module state on purpose: a runner that leaks modules between runs
would see CALLS > 1."""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import synthetic_state  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", required=True)
    args = ap.parse_args()
    synthetic_state.CALLS += 1
    if synthetic_state.CALLS != 1:
        raise SystemExit("state leaked between runs")
    with open(os.path.join(ROOT, "data", "synthetic", "input.bin"), "rb") as f:
        data = f.read()
    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(bytes(reversed(data)) * 4)


if __name__ == "__main__":
    main()
'''


def make_synthetic(out_dir: Path) -> dict[str, Path]:
    """A complete synthetic web payload, built through the real recipe, pack and
    manifest code: a fake 64 KiB "ROM" (code TEST, no game data), a recipe with
    one file of every operation kind, one in-process generator, and placeholder
    executables; plus a second variant (payload/es/: its own fake ROM, recipe,
    executable and data ABI), as a release with a localized variant has. Only for
    tests and local development of the web builder."""
    out_dir.mkdir(parents=True, exist_ok=True)

    def fake_rom(seed: int, code: bytes) -> bytearray:
        rom = bytearray((i * 7 + seed) & 0xFF for i in range(64 * 1024))
        rom[0xA0:0xB0] = b"SYNTHETIC\0\0\0" + code
        return rom

    def entry(path: str, data: bytes, ops: list) -> dict:
        return {"path": path, "size": len(data), "crc": zlib.crc32(data) & 0xFFFFFFFF, "ops": ops}

    def make_recipe(rom: bytearray, fill: int) -> Recipe:
        padded = bytes(rom) + b"\xff" * (ROM_SIZE - len(rom))
        gen_input = bytes(rom[0x1000:0x1100])
        gen_output = bytes(reversed(gen_input)) * 4
        entries = [entry("data/synthetic_copy.bin", bytes(rom[0x200:0x600]), [["C", 0x200, 0x400]]),
                   entry("data/synthetic_fill.bin", bytes([fill]) * 512, [["F", fill, 512]])]
        generated = [{"path": "stage/leaves.bin", "size": len(gen_output),
                      "crc": zlib.crc32(gen_output) & 0xFFFFFFFF, "generator": "gen_intro_margins.py"}]
        inputs = [entry("data/synthetic/input.bin", gen_input, [["C", 0x1000, 0x100]])]
        items = [(e["path"], e["size"], e["crc"]) for e in entries + generated]
        vtree = {"tilesets": {}, "layouts_table_label": "gMapLayouts", "layouts": [], "maps": [],
                 "map_types": {}, "connection_directions": {}, "metatiles_h": [], "headers_h": [],
                 "metatile_behaviors": [["MB_NORMAL", 0]]}
        return Recipe(engine_abi=pak.engine_abi(items), rom_sha1=hashlib.sha1(padded).hexdigest(),
                      release="v0.0.0-synthetic", entries=entries, generated=generated, inputs=inputs,
                      vtree=vtree)

    rom, rom_es = fake_rom(3, b"TEST"), fake_rom(5, b"TES2")
    payload = out_dir / "payload"
    (payload / "voxelgen" / "scripts").mkdir(parents=True, exist_ok=True)
    (payload / "es").mkdir(parents=True, exist_ok=True)
    (payload / "Emerald3DS.3dsx").write_bytes(b"3DSX" + b"SYNTHETIC EXECUTABLE PLACEHOLDER\n")
    (payload / "Emerald3DS.smdh").write_bytes(b"SMDH" + b"SYNTHETIC ICON PLACEHOLDER\n")
    make_recipe(rom, 0x5A).save(payload / "emerald3ds.recipe")
    (payload / "es" / "Emerald3DS.3dsx").write_bytes(b"3DSX" + b"SYNTHETIC SECOND VARIANT PLACEHOLDER\n")
    (payload / "es" / "Emerald3DS.smdh").write_bytes(b"SMDH" + b"SYNTHETIC ICON PLACEHOLDER\n")
    make_recipe(rom_es, 0xA5).save(payload / "es" / "emerald3ds.recipe")
    (payload / "voxelgen" / "scripts" / "gen_intro_margins.py").write_text(SYNTHETIC_GENERATOR, encoding="utf-8")
    (payload / "voxelgen" / "scripts" / "synthetic_state.py").write_text("CALLS = 0\n", encoding="utf-8")

    files = collect(payload)
    manifest = make_manifest(files, "0.0.0-synthetic", "v0.0.0-synthetic", synthetic=True)
    archive = out_dir / PAYLOAD_NAME
    write_zip(archive, manifest, files)
    (out_dir / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    rom_path = out_dir / "synthetic-rom.bin"
    rom_path.write_bytes(bytes(rom))
    rom_es_path = out_dir / "synthetic-rom-es.bin"
    rom_es_path.write_bytes(bytes(rom_es))
    return {"payload": payload, "zip": archive, "manifest": out_dir / MANIFEST_NAME, "rom": rom_path,
            "rom_es": rom_es_path}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--payload", type=Path, help="a release's payload/ folder")
    ap.add_argument("--version", help="release version without the v, e.g. 0.1.3")
    ap.add_argument("--out", type=Path, default=ROOT / "dist")
    ap.add_argument("--cia-forwarder", default=None, help="release asset name of the CIA forwarder, if any")
    ap.add_argument("--synthetic", type=Path, help="write a synthetic test payload into this folder instead")
    args = ap.parse_args()
    if args.synthetic:
        paths = make_synthetic(args.synthetic)
        for key, path in paths.items():
            print("synthetic %s: %s" % (key, path))
        return
    if not args.payload or not args.version:
        ap.error("--payload and --version are required (or --synthetic)")
    archive, manifest = build(args.payload, args.version, args.out, args.cia_forwarder)
    print("web payload: %s (%.1f MiB)" % (archive, archive.stat().st_size / 1048576))
    print("web manifest: %s" % manifest)


if __name__ == "__main__":
    main()
