"""Tests for the web builder's side of the builder: in-process generators, the
web entry point, web-manifest.json and Emerald3DS-WebPayload.zip. Synthetic
data only: no ROM is needed and none may ever be committed.

Run: python -m unittest discover -s builder/tests
"""

import io
import json
import os
import sys
import tempfile
import unittest
import zipfile
import zlib
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve()
sys.path.insert(0, str(HERE.parents[1]))
sys.path.insert(0, str(HERE.parents[2] / "tools"))

import build_web_payload  # noqa: E402
import release_audit  # noqa: E402
from emerald3ds_builder import pak, voxel, web, webmanifest  # noqa: E402
from emerald3ds_builder.errors import BuilderError  # noqa: E402

# Two generators shaped like the real ones: ROOT from __file__, a sibling module
# holding state, argparse --output. The second one depends on the first's output.
GEN_A = '''import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_state
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PORT = os.path.join(ROOT, "3ds_port")
def main():
    shared_state.RUNS.append("a")
    data = open(os.path.join(ROOT, "data", "in.bin"), "rb").read()
    out = os.path.join(PORT, "romfs", "voxel", "regions.bin")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "wb").write(bytes(b ^ len(shared_state.RUNS) for b in data))
if __name__ == "__main__":
    main()
'''
GEN_B = '''import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_state
PORT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", required=True)
    args = ap.parse_args()
    shared_state.RUNS.append("b")
    if os.path.realpath(os.getcwd()) != os.path.realpath(PORT):
        raise SystemExit("wrong working directory")
    data = open(os.path.join(PORT, "romfs", "voxel", "regions.bin"), "rb").read()
    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    open(args.output, "wb").write(data[::-1] + bytes([len(shared_state.RUNS)]))
if __name__ == "__main__":
    main()
'''
FAILING = '''raise RuntimeError("boom")
'''


def make_voxelgen(root: Path, gen_b: str = GEN_B) -> Path:
    scripts = root / "voxelgen" / "scripts"
    scripts.mkdir(parents=True)
    (scripts / "gen_voxel_regions.py").write_text(GEN_A)
    (scripts / "gen_intro_margins.py").write_text(gen_b)
    (scripts / "shared_state.py").write_text("RUNS = []\n")
    return root / "voxelgen"


def expected_outputs(data: bytes) -> list[dict]:
    a = bytes(b ^ 1 for b in data)
    b = a[::-1] + bytes([1])
    return [{"path": "voxel/regions.bin", "size": len(a), "crc": zlib.crc32(a) & 0xFFFFFFFF},
            {"path": "stage/leaves.bin", "size": len(b), "crc": zlib.crc32(b) & 0xFFFFFFFF}]


class RunnerTests(unittest.TestCase):
    def run_once(self, runner: str, gen_b: str = GEN_B) -> dict:
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            voxelgen = make_voxelgen(tmp / "release", gen_b)
            tree = tmp / "tree"
            (tree / "data").mkdir(parents=True)
            data = bytes(range(200))
            (tree / "data" / "in.bin").write_bytes(data)
            return voxel.run_generators(tree, voxelgen, expected_outputs(data), runner=runner)

    def test_inprocess_matches_subprocess(self):
        self.assertEqual(self.run_once("inprocess"), self.run_once("subprocess"))

    def test_inprocess_is_isolated_between_steps_and_runs(self):
        # Each step must start with fresh sibling modules (RUNS has one item),
        # also on a second build in the same interpreter.
        first = self.run_once("inprocess")
        second = self.run_once("inprocess")
        self.assertEqual(first, second)
        self.assertNotIn("shared_state", sys.modules)

    def test_inprocess_restores_the_interpreter(self):
        argv, path, cwd = sys.argv[:], sys.path[:], os.getcwd()
        self.run_once("inprocess")
        self.assertEqual((sys.argv, sys.path, os.getcwd()), (argv, path, cwd))

    def test_generator_failure_is_reported_by_both_runners(self):
        for runner in voxel.RUNNERS:
            with self.subTest(runner=runner):
                with self.assertRaises(BuilderError) as ctx:
                    self.run_once(runner, FAILING)
                self.assertEqual(ctx.exception.code, "generator_failed")
                self.assertIn("gen_intro_margins.py", ctx.exception.message)

    def test_wrong_output_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            voxelgen = make_voxelgen(tmp / "release")
            tree = tmp / "tree"
            (tree / "data").mkdir(parents=True)
            (tree / "data" / "in.bin").write_bytes(b"abc")
            wrong = expected_outputs(b"xyz")
            with self.assertRaises(BuilderError) as ctx:
                voxel.run_generators(tree, voxelgen, wrong, runner="inprocess")
            self.assertEqual(ctx.exception.code, "generator_output_mismatch")

    def test_steps_report_their_name_and_time(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            voxelgen = make_voxelgen(tmp / "release")
            tree = tmp / "tree"
            (tree / "data").mkdir(parents=True)
            data = b"abc"
            (tree / "data" / "in.bin").write_bytes(data)
            seen, timings = [], []
            voxel.run_generators(tree, voxelgen, expected_outputs(data), lambda f, d: seen.append((f, d)),
                                 runner="inprocess", timings=timings)
        self.assertEqual([d.split(",")[0] for _, d in seen],
                         ["map regions (1/2)"] * 2 + ["intro scene (2/2)"] * 2)
        self.assertEqual([f for f, _ in seen], [0.0, 0.5, 0.5, 1.0])
        self.assertEqual([name for name, _ in timings], ["gen_voxel_regions.py", "gen_intro_margins.py", "check"])

    def test_unknown_runner(self):
        with self.assertRaises(ValueError):
            voxel.run_generators(Path("."), Path("."), [], runner="thread")

    def test_relief_waits_for_the_buildings(self):
        # The relief reads buildings.bin (voxel_terraces.modelled): it must
        # find it whole, also when the buildings take longer than it does.
        buildings = '''import os, sys, time
PORT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
time.sleep(0.5)
out = os.path.join(PORT, "romfs", "voxel", "buildings.bin")
open(out + ".part", "wb").write(b"models")
os.replace(out + ".part", out)
'''
        relief = '''import os
PORT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
voxel = os.path.join(PORT, "romfs", "voxel")
seen = open(os.path.join(voxel, "buildings.bin"), "rb").read() if os.path.exists(os.path.join(voxel, "buildings.bin")) else b"none"
open(os.path.join(voxel, "relief.bin"), "wb").write(b"relief over " + seen)
'''
        expected = [{"path": "voxel/" + name, "size": len(data), "crc": zlib.crc32(data) & 0xFFFFFFFF}
                    for name, data in (("buildings.bin", b"models"), ("relief.bin", b"relief over models"))]
        for runner in voxel.RUNNERS:
            with self.subTest(runner=runner), tempfile.TemporaryDirectory() as tmp:
                tmp = Path(tmp)
                scripts = tmp / "voxelgen" / "scripts"
                scripts.mkdir(parents=True)
                (scripts / "gen_voxel_buildings.py").write_text(buildings)
                (scripts / "gen_voxel_relief.py").write_text(relief)
                voxel.run_generators(tmp / "tree", tmp / "voxelgen", expected, runner=runner)


class SyntheticPayloadMixin:
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        with redirect_stdout(io.StringIO()):
            self.synthetic = build_web_payload.make_synthetic(self.tmp / "synthetic")
        self.unpacked = self.tmp / "unpacked"
        with zipfile.ZipFile(self.synthetic["zip"]) as zf:
            zf.extractall(self.unpacked)

    def tearDown(self):
        self._tmp.cleanup()


class WebBuildTests(SyntheticPayloadMixin, unittest.TestCase):
    def build(self, rom: Path, manifest: Path | None = None) -> dict:
        stages = []
        out = self.tmp / "out" / "emerald3ds.pak"
        result = json.loads(web.run_web_build(str(rom), str(self.unpacked / "payload"), str(out),
                                              lambda *a: stages.append(a),
                                              str(manifest or self.unpacked / "web-manifest.json")))
        result["stages"] = stages
        result["pak"] = out
        return result

    def test_build_writes_a_verified_pack(self):
        result = self.build(self.synthetic["rom"])
        self.assertTrue(result["ok"], result)
        manifest = json.loads((self.unpacked / "web-manifest.json").read_text())
        self.assertEqual(result["result"]["abi"], manifest["dataAbi"])
        with pak.PakReader(result["pak"]) as reader:
            self.assertEqual(reader.verify(), 3)
            self.assertEqual("%08x" % reader.abi, manifest["dataAbi"])
        self.assertEqual([s[0] for s in result["stages"]][0], "rom")
        self.assertEqual({s[0] for s in result["stages"]}, {"rom", "data", "scenery", "verify"})
        self.assertEqual(result["stages"][-1][1], 1.0)
        steps = [s for s in result["stages"] if len(s) > 2 and "(1/" in s[2]]
        self.assertTrue(steps, result["stages"])
        self.assertEqual({s[0] for s in steps}, {"scenery"})

    def test_a_second_build_in_the_same_interpreter_gives_the_same_pack(self):
        first = self.build(self.synthetic["rom"])["pak"].read_bytes()
        second = self.build(self.synthetic["rom"])["pak"].read_bytes()
        self.assertEqual(first, second)

    def test_wrong_rom_is_reported_with_a_code(self):
        rom = self.tmp / "other.gba"
        data = bytearray(b"\xff" * 1024)
        data[0xA0:0xB0] = b"POKEMON FIREBPRE"
        rom.write_bytes(bytes(data))
        result = self.build(rom)
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "rom_wrong_game")

    def test_unknown_file_is_rejected(self):
        rom = self.tmp / "noise.gba"
        rom.write_bytes(b"\x01" * 4096)
        result = self.build(rom)
        self.assertEqual(result["error"]["code"], "rom_unsupported")

    def test_invalid_manifest_is_rejected(self):
        bad = self.tmp / "bad.json"
        manifest = json.loads((self.unpacked / "web-manifest.json").read_text())
        manifest["schemaVersion"] = 99
        bad.write_text(json.dumps(manifest))
        result = self.build(self.synthetic["rom"], bad)
        self.assertEqual(result["error"]["code"], "manifest_invalid")

    def test_second_variant_builds_from_its_own_rom(self):
        result = self.build(self.synthetic["rom_es"])
        self.assertTrue(result["ok"], result)
        manifest = json.loads((self.unpacked / "web-manifest.json").read_text())
        variant = manifest["variants"][0]
        self.assertNotEqual(variant["dataAbi"], manifest["dataAbi"])
        self.assertEqual(result["result"]["abi"], variant["dataAbi"])
        with pak.PakReader(result["pak"]) as reader:
            self.assertEqual(reader.verify(), 3)
            self.assertEqual(reader.rom_sha1.hex(), variant["rom"]["sha1"])

    def test_variant_abi_must_match(self):
        bad = self.tmp / "abi-es.json"
        manifest = json.loads((self.unpacked / "web-manifest.json").read_text())
        manifest["variants"][0]["dataAbi"] = "00000001"
        bad.write_text(json.dumps(manifest))
        result = self.build(self.synthetic["rom_es"], bad)
        self.assertEqual(result["error"]["code"], "abi_mismatch")

    def test_manifest_abi_must_match(self):
        bad = self.tmp / "abi.json"
        manifest = json.loads((self.unpacked / "web-manifest.json").read_text())
        manifest["dataAbi"] = "00000001"
        bad.write_text(json.dumps(manifest))
        result = self.build(self.synthetic["rom"], bad)
        self.assertEqual(result["error"]["code"], "abi_mismatch")


class ManifestTests(SyntheticPayloadMixin, unittest.TestCase):
    def manifest(self) -> dict:
        return json.loads((self.unpacked / "web-manifest.json").read_text())

    def test_generated_manifest_is_valid_and_matches_the_files(self):
        manifest = webmanifest.validate(self.manifest())
        self.assertEqual(manifest["schemaVersion"], webmanifest.SCHEMA_VERSION)
        self.assertIsNone(manifest["assets"]["ciaForwarder"])
        self.assertEqual(manifest["assets"]["threeDsx"]["releaseAsset"], "Emerald3DS.3dsx")
        self.assertEqual(manifest["install"]["files"], ["Emerald3DS.3dsx", "Emerald3DS.smdh", "emerald3ds.pak"])
        # the manifest attached next to the ZIP is the one inside it
        self.assertEqual(json.loads(self.synthetic["manifest"].read_text()), manifest)

    def test_invalid_manifests_are_refused(self):
        cases = {
            "schema": lambda m: m.update(schemaVersion=2),
            "kind": lambda m: m.update(kind="other"),
            "abi": lambda m: m.update(dataAbi="XYZ"),
            "roms": lambda m: m.update(supportedRoms=[]),
            "cia missing": lambda m: m["assets"].pop("ciaForwarder"),
            "unsafe path": lambda m: m["files"][0].update(path="../etc/passwd"),
            "pak listed": lambda m: m["files"].append({"path": "payload/emerald3ds.pak", "sha256": "0" * 64,
                                                       "size": 1}),
            "hash": lambda m: m["assets"]["smdh"].update(sha256="abc"),
        }
        for name, change in cases.items():
            with self.subTest(name):
                manifest = self.manifest()
                change(manifest)
                with self.assertRaises(webmanifest.ManifestError):
                    webmanifest.validate(manifest)

    def test_variants_are_described(self):
        manifest = self.manifest()
        self.assertEqual(len(manifest["supportedRoms"]), 1)
        [variant] = manifest["variants"]
        self.assertEqual(variant["assets"]["threeDsx"]["path"], "payload/es/Emerald3DS.3dsx")
        self.assertEqual(variant["assets"]["threeDsx"]["releaseAsset"], "Emerald3DS-es.3dsx")
        shas = [v["rom"]["sha1"] for v in webmanifest.rom_variants(manifest)]
        self.assertEqual(shas, [manifest["supportedRoms"][0]["sha1"], variant["rom"]["sha1"]])

    def test_invalid_variants_are_refused(self):
        cases = {
            "not a list": lambda m: m.update(variants={}),
            "abi": lambda m: m["variants"][0].update(dataAbi="XYZ"),
            # One ROM may make two languages (English and Portuguese), never one twice.
            "same rom, same language": lambda m: m["variants"][0]["rom"].update(
                sha1=m["supportedRoms"][0]["sha1"], language=m["supportedRoms"][0]["language"]),
            "same rom, api 1": lambda m: (m["variants"][0]["rom"].update(sha1=m["supportedRoms"][0]["sha1"]),
                                          m["entrypoint"].update(apiVersion=1)),
            "file not listed": lambda m: m["variants"][0]["assets"]["recipe"].update(path="payload/es/other"),
        }
        for name, change in cases.items():
            with self.subTest(name):
                manifest = self.manifest()
                change(manifest)
                with self.assertRaises(webmanifest.ManifestError):
                    webmanifest.validate(manifest)

    def test_cia_forwarder_is_described_when_present(self):
        files = build_web_payload.collect(self.synthetic["payload"])
        manifest = build_web_payload.make_manifest(files, "1.0.0", "v1.0.0", "Emerald3DS-Forwarder.cia")
        self.assertEqual(manifest["assets"]["ciaForwarder"]["releaseAsset"], "Emerald3DS-Forwarder.cia")
        self.assertEqual(manifest["assets"]["ciaForwarder"]["target"], "sdmc:/3ds/emerald3ds/Emerald3DS.3dsx")


class WebPayloadContentsTests(SyntheticPayloadMixin, unittest.TestCase):
    def names(self) -> list[str]:
        with zipfile.ZipFile(self.synthetic["zip"]) as zf:
            return zf.namelist()

    def test_layout(self):
        names = self.names()
        self.assertIn("web-manifest.json", names)
        for required in ("payload/Emerald3DS.3dsx", "payload/Emerald3DS.smdh", "payload/emerald3ds.recipe",
                         "python/emerald3ds_builder/web.py", "python/emerald3ds_builder/voxel.py",
                         "licenses/LICENSE-PORT.md"):
            self.assertIn(required, names)
        self.assertTrue(all(n == "web-manifest.json" or n.startswith(("payload/", "python/", "licenses/"))
                            for n in names))

    def test_no_rom_no_pack_no_desktop_modules(self):
        names = self.names()
        self.assertFalse([n for n in names if n.lower().endswith((".gba", ".agb", ".pak", ".sav", ".pyc"))])
        self.assertNotIn("python/emerald3ds_builder/gui.py", names)
        rom = self.synthetic["rom"].read_bytes()
        with zipfile.ZipFile(self.synthetic["zip"]) as zf:
            for name in names:
                data = zf.read(name)
                self.assertNotEqual(data[:8], pak.MAGIC, name)
                self.assertNotIn(rom[0x200:0x300], data, name)

    def test_zip_is_deterministic(self):
        first = self.synthetic["zip"].read_bytes()
        with redirect_stdout(io.StringIO()):
            again = build_web_payload.make_synthetic(self.tmp / "again")
        self.assertEqual(first, again["zip"].read_bytes())

    def test_release_audit_accepts_the_payload(self):
        findings = release_audit.audit_web_payload(release_audit.zip_files(self.synthetic["zip"]))
        self.assertEqual(findings, [])

    def test_release_audit_catches_tampering_and_packs(self):
        bad = self.tmp / "bad.zip"
        with zipfile.ZipFile(self.synthetic["zip"]) as src, zipfile.ZipFile(bad, "w") as dst:
            for name in src.namelist():
                data = src.read(name)
                if name == "payload/Emerald3DS.smdh":
                    data += b"!"
                dst.writestr(name, data)
            dst.writestr("payload/emerald3ds.pak", pak.MAGIC + b"\0" * 56)
        entries = release_audit.zip_files(bad)
        rules = {f.rule for f in release_audit.audit_web_payload(entries)}
        self.assertIn("web-payload-hash-mismatch", rules)
        self.assertIn("web-payload-unlisted-file", rules)
        generic = {f.rule for f in release_audit.audit(entries, release_audit.Allow(), [], None, "zip")}
        self.assertIn("data-pack", generic)
        self.assertIn("forbidden-type", generic)

    def test_release_audit_requires_the_manifest(self):
        bad = self.tmp / "nomanifest.zip"
        with zipfile.ZipFile(bad, "w") as dst:
            dst.writestr("payload/Emerald3DS.3dsx", b"x")
        rules = {f.rule for f in release_audit.audit_web_payload(release_audit.zip_files(bad))}
        self.assertEqual(rules, {"web-manifest-missing"})


if __name__ == "__main__":
    unittest.main()
