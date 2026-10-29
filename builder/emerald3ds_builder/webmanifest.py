"""web-manifest.json: the contract between a release and the web builder.

Every release carries `Emerald3DS-WebPayload.zip`, and next to it the same
`web-manifest.json` as a separate small asset so the website can read a
release's capabilities without downloading the payload. Layout of the ZIP:

    web-manifest.json
    payload/Emerald3DS.3dsx, payload/Emerald3DS.smdh, payload/emerald3ds.recipe
    payload/<lang>/...            other languages: their own executable and recipe
    payload/voxelgen/...          the generator scripts (as in the Windows ZIP)
    python/emerald3ds_builder/... this package, without the desktop window
    licenses/...

It never holds a ROM, a data pack or anything extracted from the game.

The top-level `supportedRoms`, `dataAbi` and `assets` describe the default
variant (the English game). The optional `variants` list adds one entry per
other language: {"rom": {...}, "dataAbi", "recipeRelease", "assets": {threeDsx,
smdh, recipe}}. A website that predates `variants` ignores it and keeps
building the default variant only.

One ROM may make more than one language: the Portuguese fan translation is
built from the English ROM, so its variant lists that ROM again with its own
`language` ("pt"). Only the language tells such variants apart, so a manifest
that shares a ROM needs `entrypoint.apiVersion` 2, whose run_web_build takes
the language the player chose.

`schemaVersion` changes only when an existing field changes meaning or goes
away; new optional fields keep the version. The website refuses a schema it
does not know, so a release can never be half-understood. `validate()` is used
by the generator, by tools/release_audit.py and by the web adapter, so all
three agree on what a valid manifest is.
"""

from __future__ import annotations

import re

SCHEMA_VERSION = 1
KIND = "emerald3ds-web-payload"
MANIFEST_NAME = "web-manifest.json"
PAYLOAD_NAME = "Emerald3DS-WebPayload.zip"
# apiVersion 2: run_web_build takes the game language (one ROM, several languages).
ENTRYPOINT = {"pythonPath": "python", "module": "emerald3ds_builder.web",
              "function": "run_web_build", "apiVersion": 2}
API_VERSIONS = (1, 2)
LANGUAGE_API = 2
# A cartridge's own language, from the last letter of its game code.
CARTRIDGE_LANGUAGES = {"E": "en", "S": "es", "F": "fr", "I": "it", "D": "de", "J": "ja"}
INSTALL_DIR = "3ds/emerald3ds"
INSTALL_FILES = ["Emerald3DS.3dsx", "Emerald3DS.smdh", "emerald3ds.pak"]
# Files that may never be inside a web payload, whatever their name says.
FORBIDDEN_SUFFIXES = (".gba", ".agb", ".pak", ".sav", ".pyc", ".pyo")

_HEX = {"sha1": re.compile(r"^[0-9a-f]{40}$"), "sha256": re.compile(r"^[0-9a-f]{64}$"),
        "abi": re.compile(r"^[0-9a-f]{8}$")}


class ManifestError(ValueError):
    pass


def safe_path(path: str) -> bool:
    return (bool(path) and not path.startswith("/") and "\\" not in path and ":" not in path
            and ".." not in path.split("/"))


def _is_int(value) -> bool:
    # bool is a subclass of int, but `true` is never a valid size or version.
    return isinstance(value, int) and not isinstance(value, bool)


def _is_hex(kind: str, value) -> bool:
    # fullmatch: `$` alone would also accept a trailing newline.
    return isinstance(value, str) and _HEX[kind].fullmatch(value) is not None


def _file_ref(value, where: str, need_release_asset: bool = False) -> None:
    if not isinstance(value, dict):
        raise ManifestError("%s must be an object" % where)
    if not isinstance(value.get("path"), str) or not safe_path(value["path"]):
        raise ManifestError("%s.path is not a safe relative path" % where)
    if not _is_hex("sha256", value.get("sha256")):
        raise ManifestError("%s.sha256 must be 64 lowercase hex digits" % where)
    if not _is_int(value.get("size")) or value["size"] < 0:
        raise ManifestError("%s.size must be a non-negative integer" % where)
    if need_release_asset and value.get("releaseAsset") is not None and not isinstance(value["releaseAsset"], str):
        raise ManifestError("%s.releaseAsset must be a file name or null" % where)


def _assets(assets, where: str) -> None:
    if not isinstance(assets, dict):
        raise ManifestError("%s must be an object" % where)
    for key in ("threeDsx", "smdh", "recipe"):
        _file_ref(assets.get(key), "%s.%s" % (where, key), need_release_asset=True)


def _rom(rom, where: str) -> None:
    if not isinstance(rom, dict):
        raise ManifestError("%s must be an object" % where)
    if not _is_hex("sha1", rom.get("sha1")):
        raise ManifestError("%s.sha1 must be 40 lowercase hex digits" % where)
    if not isinstance(rom.get("name"), str):
        raise ManifestError("%s.name must be a string" % where)
    for key in ("code", "language"):
        if rom.get(key) is not None and not isinstance(rom[key], str):
            raise ManifestError("%s.%s must be a string" % (where, key))


def cartridge_language(rom: dict) -> str:
    """The language of the cartridge itself (BPES -> "es"); English when unknown."""
    code = rom.get("code") or ""
    return CARTRIDGE_LANGUAGES.get(code[-1:], "en")


def rom_language(rom: dict) -> str:
    """The game language a variant builds: its `language` ("pt_BR" -> "pt"),
    else its cartridge's."""
    given = (rom.get("language") or "").strip().lower().replace("-", "_").split("_")[0]
    return given or cartridge_language(rom)


def variant_for(manifest: dict, sha1: str, language: str | None = None) -> dict | None:
    """The variant this ROM builds in `language`; without one, the cartridge's
    own language (English for the English ROM, not Portuguese). Mirrors
    variantForSha1 on the website."""
    matches = [v for v in rom_variants(manifest) if v["rom"]["sha1"] == sha1]
    if language:
        want = rom_language({"language": language})
        return next((v for v in matches if rom_language(v["rom"]) == want), None)
    return next((v for v in matches if rom_language(v["rom"]) == cartridge_language(v["rom"])),
                matches[0] if matches else None)


def rom_variants(manifest: dict) -> list[dict]:
    """Every ROM the release builds from, default variant first:
    [{"rom": {...}, "dataAbi": "...", "assets": {...}}]."""
    out = [{"rom": rom, "dataAbi": manifest["dataAbi"], "assets": manifest["assets"]}
           for rom in manifest["supportedRoms"]]
    for variant in manifest.get("variants") or []:
        out.append({"rom": variant["rom"], "dataAbi": variant["dataAbi"], "assets": variant["assets"]})
    return out


def validate(manifest: dict) -> dict:
    """Raise ManifestError unless `manifest` is a valid schema-1 manifest."""
    if not isinstance(manifest, dict):
        raise ManifestError("the manifest must be a JSON object")
    if manifest.get("schemaVersion") != SCHEMA_VERSION:
        raise ManifestError("unsupported schemaVersion %r" % manifest.get("schemaVersion"))
    if manifest.get("kind") != KIND:
        raise ManifestError("kind must be %r" % KIND)
    for key in ("releaseVersion", "releaseTag", "builderVersion"):
        if not isinstance(manifest.get(key), str) or not manifest[key]:
            raise ManifestError("%s must be a non-empty string" % key)
    if not _is_hex("abi", manifest.get("dataAbi")):
        raise ManifestError("dataAbi must be 8 lowercase hex digits")
    if not _is_int(manifest.get("packSchema")):
        raise ManifestError("packSchema must be an integer")
    roms = manifest.get("supportedRoms")
    if not isinstance(roms, list) or not roms:
        raise ManifestError("supportedRoms must list at least one ROM")
    for i, rom in enumerate(roms):
        _rom(rom, "supportedRoms[%d]" % i)
    variants = manifest.get("variants")
    if variants is not None:
        if not isinstance(variants, list):
            raise ManifestError("variants must be a list")
        for i, variant in enumerate(variants):
            where = "variants[%d]" % i
            if not isinstance(variant, dict):
                raise ManifestError("%s must be an object" % where)
            _rom(variant.get("rom"), where + ".rom")
            if not _is_hex("abi", variant.get("dataAbi")):
                raise ManifestError("%s.dataAbi must be 8 lowercase hex digits" % where)
            _assets(variant.get("assets"), where + ".assets")
    entry = manifest.get("entrypoint")
    if not isinstance(entry, dict) or not all(isinstance(entry.get(k), str) for k in ("pythonPath", "module", "function")):
        raise ManifestError("entrypoint must name pythonPath, module and function")
    # apiVersion is optional (older manifests may lack it); when given it must be
    # the version of the run_web_build contract this package implements.
    if "apiVersion" in entry:
        if not _is_int(entry["apiVersion"]):
            raise ManifestError("entrypoint.apiVersion must be an integer")
        if entry["apiVersion"] not in API_VERSIONS:
            raise ManifestError("unsupported entrypoint.apiVersion %r" % entry["apiVersion"])
    assets = manifest.get("assets")
    _assets(assets, "assets")
    # Every field rom_variants() reads has been checked above, so it cannot raise.
    # One ROM may make several languages, each once; telling them apart
    # takes the language argument of API 2.
    pairs = [(v["rom"]["sha1"], rom_language(v["rom"])) for v in rom_variants(manifest)]
    if len(set(pairs)) != len(pairs):
        raise ManifestError("a ROM is listed twice for the same language")
    if len({sha for sha, _ in pairs}) != len(pairs) and entry.get("apiVersion", 1) < LANGUAGE_API:
        raise ManifestError("a ROM shared by two languages needs entrypoint.apiVersion %d" % LANGUAGE_API)
    if "ciaForwarder" not in assets:
        raise ManifestError("assets.ciaForwarder must be present (null while there is none)")
    cia = assets["ciaForwarder"]
    if cia is not None:
        if not isinstance(cia, dict) or not isinstance(cia.get("releaseAsset"), str):
            raise ManifestError("assets.ciaForwarder must be null or name its release asset")
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        raise ManifestError("files must list the payload's files")
    seen = set()
    for i, item in enumerate(files):
        _file_ref(item, "files[%d]" % i)
        if item["path"] in seen:
            raise ManifestError("files lists %s twice" % item["path"])
        if item["path"].lower().endswith(FORBIDDEN_SUFFIXES):
            raise ManifestError("files may not contain %s" % item["path"])
        seen.add(item["path"])
    for i, variant in enumerate(rom_variants(manifest)):
        for key in ("threeDsx", "smdh", "recipe"):
            if variant["assets"][key]["path"] not in seen:
                raise ManifestError("the %s of ROM %s is not in files" % (key, variant["rom"]["sha1"]))
    return manifest
