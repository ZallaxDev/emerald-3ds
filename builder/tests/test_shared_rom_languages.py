"""One ROM, two languages: the English ROM makes the English game and the
Portuguese fan translation (payload/pt/). Only the language the player chose
tells them apart: the manifest needs entrypoint.apiVersion 2, the builders
take a `language`, and a pack is matched to its variant by its data ABI.
Synthetic data only: no ROM is needed and none may ever be committed.

Run: python -m unittest discover -s builder/tests
"""

import io
import json
import shutil
import sys
import tempfile
import unittest
import zipfile
import zlib
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

HERE = Path(__file__).resolve()
sys.path.insert(0, str(HERE.parents[1]))
sys.path.insert(0, str(HERE.parents[2] / "tools"))
sys.path.insert(0, str(HERE.parent))

import build_web_payload  # noqa: E402
from emerald3ds_builder import cli, pak, web, webmanifest  # noqa: E402
from emerald3ds_builder.build import EXECUTABLE_NAMES, Payload, build_pack, select_payload  # noqa: E402
from emerald3ds_builder.errors import BuilderError  # noqa: E402
from emerald3ds_builder.recipe import Recipe  # noqa: E402
from test_manifest_errors import SHA1_EN, add_variant, file_ref, legacy_manifest, variant  # noqa: E402
from test_variant_payload import checked_rom, quick_build_pack  # noqa: E402


def shared_manifest(api: int = 2) -> dict:
    """English and Portuguese from the same ROM."""
    manifest = legacy_manifest()
    manifest["supportedRoms"][0].update(code="BPEE", language="en")
    manifest["entrypoint"]["apiVersion"] = api
    pt = variant("pt", SHA1_EN, "99887766")
    pt["rom"].update(code="BPEE", language="pt")
    manifest["variants"] = [pt]
    manifest["files"] += [file_ref(ref["path"]) for ref in pt["assets"].values()]
    return manifest


class SharedRomManifestTests(unittest.TestCase):
    def test_shared_rom_with_api_2_is_valid(self):
        manifest = shared_manifest()
        webmanifest.validate(manifest)
        self.assertEqual(web.supported_sha1s(manifest), (SHA1_EN, SHA1_EN))

    def test_shared_rom_needs_api_2(self):
        for api in (1, None):
            manifest = shared_manifest()
            if api is None:
                manifest["entrypoint"].pop("apiVersion")
            else:
                manifest["entrypoint"]["apiVersion"] = api
            with self.assertRaises(webmanifest.ManifestError, msg=api):
                webmanifest.validate(manifest)

    def test_same_rom_and_language_twice_is_invalid(self):
        manifest = shared_manifest()
        manifest["variants"][0]["rom"]["language"] = "en"
        with self.assertRaises(webmanifest.ManifestError):
            webmanifest.validate(manifest)
        manifest = shared_manifest()
        manifest["variants"][0]["rom"]["language"] = "EN_us"   # the same language, written otherwise
        with self.assertRaises(webmanifest.ManifestError):
            webmanifest.validate(manifest)

    def test_language_must_be_a_string(self):
        manifest = shared_manifest()
        manifest["variants"][0]["rom"]["language"] = 7
        with self.assertRaises(webmanifest.ManifestError):
            webmanifest.validate(manifest)

    def test_api_1_manifests_stay_valid(self):
        manifest = legacy_manifest()
        manifest["entrypoint"]["apiVersion"] = 1
        add_variant(manifest, "es", "2" * 40)
        webmanifest.validate(manifest)

    def test_variant_for_picks_by_language(self):
        manifest = webmanifest.validate(shared_manifest())
        pick = lambda language: webmanifest.variant_for(manifest, SHA1_EN, language)  # noqa: E731
        self.assertEqual(pick(None)["dataAbi"], manifest["dataAbi"])        # the cartridge's own language
        self.assertEqual(pick("en")["dataAbi"], manifest["dataAbi"])
        self.assertEqual(pick("pt")["dataAbi"], "99887766")
        self.assertEqual(pick("pt_BR")["dataAbi"], "99887766")
        self.assertEqual(pick("pt-br")["dataAbi"], "99887766")
        self.assertIsNone(pick("fr"))
        self.assertIsNone(webmanifest.variant_for(manifest, "f" * 40, None))

    def test_cartridge_language(self):
        self.assertEqual(webmanifest.cartridge_language({"code": "BPES"}), "es")
        self.assertEqual(webmanifest.cartridge_language({"code": "BPEJ"}), "ja")
        self.assertEqual(webmanifest.cartridge_language({}), "en")
        self.assertEqual(webmanifest.rom_language({"code": "BPEE", "language": "pt_BR"}), "pt")


class SharedRomPayloadTests(unittest.TestCase):
    """The synthetic payload plus payload/pt/: a recipe made from the English
    (default) ROM with other game data, so another data ABI."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        with redirect_stdout(io.StringIO()):
            self.synthetic = build_web_payload.make_synthetic(self.tmp / "synthetic")
        self.root = Path(self.synthetic["payload"])
        self.pt_dir = self.root / "pt"
        self.pt_dir.mkdir()
        recipe = Recipe.load(self.root / "emerald3ds.recipe")
        fill = next(e for e in recipe.entries if e["path"] == "data/synthetic_fill.bin")
        fill["ops"] = [["F", 0x77, fill["size"]]]
        fill["crc"] = zlib.crc32(bytes([0x77]) * fill["size"]) & 0xFFFFFFFF
        recipe.engine_abi = pak.engine_abi([(e["path"], e["size"], e["crc"])
                                            for e in recipe.entries + recipe.generated])
        recipe.save(self.pt_dir / "emerald3ds.recipe")
        self.pt_abi = recipe.engine_abi
        self.en_abi = Recipe.load(self.root / "emerald3ds.recipe").engine_abi
        self.assertNotEqual(self.pt_abi, self.en_abi)
        (self.pt_dir / "Emerald3DS.3dsx").write_bytes(b"3DSX" + b"SYNTHETIC PORTUGUESE PLACEHOLDER\n")
        (self.pt_dir / "Emerald3DS.smdh").write_bytes(b"SMDH" + b"SYNTHETIC ICON PLACEHOLDER\n")
        self.payload = Payload(self.root)
        self.rom = checked_rom(self.synthetic["rom"])

    def tearDown(self):
        self._tmp.cleanup()

    def build(self, language=None) -> int:
        out = self.tmp / ("pak-%s" % language) / "emerald3ds.pak"
        out.parent.mkdir()
        with patch("emerald3ds_builder.build.load_rom", return_value=self.rom):
            quick_build_pack(self.synthetic["rom"], self.payload, out, rom=self.rom, language=language)
        with pak.PakReader(out) as reader:
            return reader.abi

    def test_languages_of_a_rom(self):
        self.assertEqual(self.payload.languages(self.rom.sha1), ["en", "pt"])
        self.assertEqual(self.payload.languages(checked_rom(self.synthetic["rom_es"]).sha1), ["es"])

    def test_the_rom_s_own_language_by_default(self):
        self.assertEqual(self.build(), self.en_abi)
        self.assertEqual(self.build("en"), self.en_abi)

    def test_portuguese_when_asked(self):
        self.assertEqual(self.build("pt"), self.pt_abi)
        self.assertEqual(select_payload(self.payload, self.rom.sha1, language="pt_BR").root, self.pt_dir)

    def test_a_language_the_rom_does_not_make_is_refused(self):
        with self.assertRaises(BuilderError) as caught:
            select_payload(self.payload, self.rom.sha1, language="fr")
        self.assertEqual(caught.exception.code, "rom_mismatch")
        # The Spanish ROM does not make Portuguese either: Spanish never stands in.
        with self.assertRaises(BuilderError):
            select_payload(self.payload, checked_rom(self.synthetic["rom_es"]).sha1, language="pt")

    def test_pack_is_matched_by_its_abi(self):
        self.assertEqual(self.payload.for_pak(self.rom.sha1, self.pt_abi).root, self.pt_dir)
        self.assertEqual(self.payload.for_pak(self.rom.sha1, self.en_abi).root, self.root)

    def test_cli_build_and_verify_portuguese(self):
        output = self.tmp / "output"
        out, err = io.StringIO(), io.StringIO()
        with patch("emerald3ds_builder.cli.load_rom", return_value=self.rom), \
                patch("emerald3ds_builder.build.load_rom", return_value=self.rom), \
                patch("emerald3ds_builder.cli.build_pack", side_effect=quick_build_pack), \
                redirect_stdout(out), redirect_stderr(err):
            code = cli.main(["--payload", str(self.root), "build", "--rom", str(self.synthetic["rom"]),
                             "--output", str(output), "--language", "pt"])
            self.assertEqual(code, 0, err.getvalue())
            app = output / "3ds" / "emerald3ds"
            for name in EXECUTABLE_NAMES:
                self.assertEqual((app / name).read_bytes(), (self.pt_dir / name).read_bytes(), name)
            self.assertEqual(cli.main(["--payload", str(self.root), "verify", "--pak",
                                       str(app / "emerald3ds.pak")]), 0, err.getvalue())

    def test_web_payload_and_build(self):
        files = build_web_payload.collect(self.root)
        manifest = build_web_payload.make_manifest(files, "0.0.0-synthetic", "v0.0.0-synthetic", synthetic=True)
        self.assertEqual(manifest["entrypoint"]["apiVersion"], 2)
        pt = next(v for v in manifest["variants"] if v["assets"]["recipe"]["path"] == "payload/pt/emerald3ds.recipe")
        self.assertEqual(pt["rom"]["sha1"], self.rom.sha1)
        self.assertEqual(pt["dataAbi"], "%08x" % self.pt_abi)
        self.assertEqual(pt["assets"]["threeDsx"]["releaseAsset"], "Emerald3DS-pt.3dsx")
        archive = self.tmp / "web.zip"
        build_web_payload.write_zip(archive, manifest, files)
        unpacked = self.tmp / "unpacked"
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(unpacked)
        shutil.rmtree(self.root)   # the web build reads the unpacked payload only
        for language, abi in ((None, self.en_abi), ("pt", self.pt_abi), ("en", self.en_abi)):
            out = self.tmp / ("web-%s" % language) / "emerald3ds.pak"
            result = json.loads(web.run_web_build(str(self.synthetic["rom"]), str(unpacked / "payload"), str(out),
                                                  None, str(unpacked / "web-manifest.json"), language))
            self.assertTrue(result["ok"], result)
            self.assertEqual(result["apiVersion"], 2)
            self.assertEqual(result["result"]["abi"], "%08x" % abi, language)
        result = json.loads(web.run_web_build(str(self.synthetic["rom"]), str(unpacked / "payload"),
                                              str(self.tmp / "fr.pak"), None,
                                              str(unpacked / "web-manifest.json"), "fr"))
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "rom_mismatch")


if __name__ == "__main__":
    unittest.main()
