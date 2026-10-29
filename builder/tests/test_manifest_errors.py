"""Regression tests: a malformed web-manifest.json always raises ManifestError
(never KeyError/TypeError), and the web entry point reports it as
"manifest_invalid". The manifests are built here from placeholder values: no
tools, ROM, payload or game data is needed and none may ever be committed.

Run: python -m unittest discover -s builder/tests
"""

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve()
sys.path.insert(0, str(HERE.parents[1]))

from emerald3ds_builder import web, webmanifest  # noqa: E402

SHA1_EN = "1" * 40
SHA1_ES = "2" * 40
SHA1_DE = "3" * 40


def file_ref(path: str, release_asset=None, digit: str = "a") -> dict:
    ref = {"path": path, "sha256": digit * 64, "size": 10}
    if release_asset is not None:
        ref["releaseAsset"] = release_asset
    return ref


def variant(lang: str, sha1: str, abi: str) -> dict:
    return {"rom": {"sha1": sha1, "name": "Pokemon Emerald (%s)" % lang}, "dataAbi": abi,
            "recipeRelease": "v1.0.0",
            "assets": {"threeDsx": file_ref("payload/%s/Emerald3DS.3dsx" % lang, "Emerald3DS-%s.3dsx" % lang),
                       "smdh": file_ref("payload/%s/Emerald3DS.smdh" % lang, "Emerald3DS-%s.smdh" % lang),
                       "recipe": file_ref("payload/%s/emerald3ds.recipe" % lang)}}


def legacy_manifest() -> dict:
    """A minimal valid default-variant-only manifest (no `variants`)."""
    return {
        "schemaVersion": webmanifest.SCHEMA_VERSION,
        "kind": webmanifest.KIND,
        "releaseVersion": "1.0.0",
        "releaseTag": "v1.0.0",
        "builderVersion": "1.0.0",
        "dataAbi": "73ac5164",
        "packSchema": 1,
        "supportedRoms": [{"sha1": SHA1_EN, "name": "Pokemon Emerald (USA, Europe)"}],
        "entrypoint": dict(webmanifest.ENTRYPOINT),
        "assets": {"threeDsx": file_ref("payload/Emerald3DS.3dsx", "Emerald3DS.3dsx"),
                   "smdh": file_ref("payload/Emerald3DS.smdh", "Emerald3DS.smdh"),
                   "recipe": file_ref("payload/emerald3ds.recipe"),
                   "ciaForwarder": None},
        "install": {"dir": webmanifest.INSTALL_DIR, "files": list(webmanifest.INSTALL_FILES)},
        "files": [file_ref("payload/Emerald3DS.3dsx"), file_ref("payload/Emerald3DS.smdh"),
                  file_ref("payload/emerald3ds.recipe"),
                  file_ref("python/emerald3ds_builder/web.py")],
    }


def variant_manifest() -> dict:
    """legacy_manifest plus a Spanish language variant."""
    manifest = legacy_manifest()
    es = variant("es", SHA1_ES, "11223344")
    manifest["variants"] = [es]
    manifest["files"] += [file_ref(ref["path"]) for ref in es["assets"].values()]
    return manifest


def add_variant(manifest: dict, lang: str, sha1: str) -> None:
    extra = variant(lang, sha1, "55667788")
    manifest.setdefault("variants", []).append(extra)
    manifest["files"] += [file_ref(ref["path"]) for ref in extra["assets"].values()]


# name -> (base manifest factory, change). Each must raise ManifestError.
MALFORMED = {
    # F5: variants present, top-level assets missing or broken
    "variants, no assets": (variant_manifest, lambda m: m.pop("assets")),
    "variants, assets null": (variant_manifest, lambda m: m.update(assets=None)),
    "variants, assets a list": (variant_manifest, lambda m: m.update(assets=[])),
    "variants, assets.threeDsx missing": (variant_manifest, lambda m: m["assets"].pop("threeDsx")),
    "variants, assets.recipe a string": (variant_manifest, lambda m: m["assets"].update(recipe="x")),
    "legacy, no assets": (legacy_manifest, lambda m: m.pop("assets")),
    # malformed variants
    "variant not an object": (variant_manifest, lambda m: m["variants"].append("es")),
    "variant without rom": (variant_manifest, lambda m: m["variants"][0].pop("rom")),
    "variant rom not an object": (variant_manifest, lambda m: m["variants"][0].update(rom="es")),
    "variant without assets": (variant_manifest, lambda m: m["variants"][0].pop("assets")),
    "variant without dataAbi": (variant_manifest, lambda m: m["variants"][0].pop("dataAbi")),
    "variant asset missing": (variant_manifest, lambda m: m["variants"][0]["assets"].pop("smdh")),
    # duplicate ROM SHA-1s, with and without variants
    "duplicate in supportedRoms": (legacy_manifest,
                                   lambda m: m["supportedRoms"].append(dict(m["supportedRoms"][0]))),
    "duplicate in supportedRoms, empty variants": (
        legacy_manifest, lambda m: (m.update(variants=[]),
                                    m["supportedRoms"].append(dict(m["supportedRoms"][0])))),
    "variant repeats default": (variant_manifest,
                                lambda m: m["variants"][0]["rom"].update(sha1=SHA1_EN)),
    "two variants, same rom": (variant_manifest, lambda m: add_variant(m, "de", SHA1_ES)),
    # bool is not an integer
    "packSchema true": (legacy_manifest, lambda m: m.update(packSchema=True)),
    "packSchema string": (legacy_manifest, lambda m: m.update(packSchema="1")),
    "files size true": (legacy_manifest, lambda m: m["files"][0].update(size=True)),
    "files size false": (legacy_manifest, lambda m: m["files"][0].update(size=False)),
    "asset size true": (legacy_manifest, lambda m: m["assets"]["smdh"].update(size=True)),
    "variant asset size true": (variant_manifest,
                                lambda m: m["variants"][0]["assets"]["recipe"].update(size=True)),
    "negative size": (legacy_manifest, lambda m: m["files"][0].update(size=-1)),
    # entrypoint.apiVersion
    "apiVersion newer": (legacy_manifest, lambda m: m["entrypoint"].update(apiVersion=3)),
    "apiVersion zero": (legacy_manifest, lambda m: m["entrypoint"].update(apiVersion=0)),
    "apiVersion true": (legacy_manifest, lambda m: m["entrypoint"].update(apiVersion=True)),
    "apiVersion string": (legacy_manifest, lambda m: m["entrypoint"].update(apiVersion="1")),
    "apiVersion null": (legacy_manifest, lambda m: m["entrypoint"].update(apiVersion=None)),
    "entrypoint missing": (legacy_manifest, lambda m: m.pop("entrypoint")),
    # strict hex fields
    "sha1 with newline": (legacy_manifest, lambda m: m["supportedRoms"][0].update(sha1=SHA1_EN + "\n")),
    "sha1 as a number": (legacy_manifest, lambda m: m["supportedRoms"][0].update(sha1=int("1" * 40))),
    "rom not an object": (legacy_manifest, lambda m: m.update(supportedRoms=["rom"])),
    "dataAbi with newline": (legacy_manifest, lambda m: m.update(dataAbi="73ac5164\n")),
    "variant dataAbi with newline": (variant_manifest,
                                     lambda m: m["variants"][0].update(dataAbi="11223344\n")),
    "sha256 with newline": (legacy_manifest, lambda m: m["files"][0].update(sha256="a" * 64 + "\n")),
    # other top-level shapes
    "files entry not an object": (legacy_manifest, lambda m: m["files"].append("payload/x")),
    "files missing": (legacy_manifest, lambda m: m.pop("files")),
    "variant asset not in files": (variant_manifest,
                                   lambda m: m["files"].pop()),
}


class ValidManifestTests(unittest.TestCase):
    def test_legacy_default_only_manifest_is_valid(self):
        manifest = legacy_manifest()
        self.assertIs(webmanifest.validate(manifest), manifest)
        self.assertEqual([v["rom"]["sha1"] for v in webmanifest.rom_variants(manifest)], [SHA1_EN])

    def test_language_variant_manifest_is_valid(self):
        manifest = variant_manifest()
        webmanifest.validate(manifest)
        self.assertEqual([v["rom"]["sha1"] for v in webmanifest.rom_variants(manifest)], [SHA1_EN, SHA1_ES])
        self.assertEqual(web.supported_sha1s(manifest), (SHA1_EN, SHA1_ES))

    def test_several_variants_are_valid(self):
        manifest = variant_manifest()
        add_variant(manifest, "de", SHA1_DE)
        webmanifest.validate(manifest)

    def test_optional_fields_may_be_absent_or_null(self):
        cases = {
            "variants null": lambda m: m.update(variants=None),
            "variants empty": lambda m: m.update(variants=[]),
            "no apiVersion": lambda m: m["entrypoint"].pop("apiVersion"),
            "no install": lambda m: m.pop("install"),
            "no releaseAsset": lambda m: m["assets"]["threeDsx"].pop("releaseAsset"),
            "releaseAsset null": lambda m: m["assets"]["smdh"].update(releaseAsset=None),
            "cia forwarder": lambda m: m["assets"].update(ciaForwarder={"releaseAsset": "Emerald3DS-Forwarder.cia"}),
            "unknown extra field": lambda m: m.update(futureField={"x": 1}),
        }
        for name, change in cases.items():
            with self.subTest(name):
                manifest = legacy_manifest()
                change(manifest)
                webmanifest.validate(manifest)

    def test_zero_size_is_valid(self):
        manifest = legacy_manifest()
        manifest["files"][-1]["size"] = 0
        webmanifest.validate(manifest)

    def test_entrypoint_api_version_matches_the_web_entry_point(self):
        self.assertEqual(webmanifest.ENTRYPOINT["apiVersion"], web.API_VERSION)


class MalformedManifestTests(unittest.TestCase):
    def test_malformed_manifests_raise_manifest_error(self):
        for name, (factory, change) in MALFORMED.items():
            with self.subTest(name):
                manifest = factory()
                webmanifest.validate(copy.deepcopy(manifest))  # the base is valid
                change(manifest)
                # assertRaises lets KeyError/TypeError through as test errors
                with self.assertRaises(webmanifest.ManifestError):
                    webmanifest.validate(manifest)

    def test_not_an_object(self):
        for value in (None, [], "manifest", 1, True):
            with self.subTest(value=value):
                with self.assertRaises(webmanifest.ManifestError):
                    webmanifest.validate(value)

    def test_manifest_error_is_a_value_error(self):
        # web.py and tools catch ValueError; keep the subclass relationship.
        self.assertTrue(issubclass(webmanifest.ManifestError, ValueError))


class WebEntryClassificationTests(unittest.TestCase):
    """The manifest is validated before the ROM or payload is touched, so these
    paths need not exist."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def run_with(self, text: str) -> dict:
        path = self.tmp / "web-manifest.json"
        path.write_text(text, encoding="utf-8")
        result = json.loads(web.run_web_build(str(self.tmp / "missing.gba"), str(self.tmp / "payload"),
                                              str(self.tmp / "out" / "emerald3ds.pak"), None, str(path)))
        self.assertFalse(result["ok"], result)
        return result["error"]

    def test_malformed_manifests_are_manifest_invalid(self):
        for name, (factory, change) in MALFORMED.items():
            with self.subTest(name):
                manifest = factory()
                change(manifest)
                error = self.run_with(json.dumps(manifest))
                self.assertEqual(error["code"], "manifest_invalid", error)

    def test_unparsable_and_non_object_manifests_are_manifest_invalid(self):
        for text in ("{", "[]", "null", "\"manifest\""):
            with self.subTest(text=text):
                self.assertEqual(self.run_with(text)["code"], "manifest_invalid")

    def test_missing_manifest_file_is_manifest_invalid(self):
        result = json.loads(web.run_web_build(str(self.tmp / "missing.gba"), str(self.tmp / "payload"),
                                              str(self.tmp / "out.pak"), None, str(self.tmp / "nothing.json")))
        self.assertEqual(result["error"]["code"], "manifest_invalid")

    def test_valid_manifest_gets_past_manifest_validation(self):
        # It then fails on the missing ROM, but never as manifest_invalid.
        for factory in (legacy_manifest, variant_manifest):
            with self.subTest(factory.__name__):
                error = self.run_with(json.dumps(factory()))
                self.assertNotEqual(error["code"], "manifest_invalid", error)


if __name__ == "__main__":
    unittest.main()
