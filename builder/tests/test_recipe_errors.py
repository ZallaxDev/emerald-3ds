"""Recipe container decoding: a damaged header, compressed metadata or literal
pool, invalid JSON, or missing/wrongly typed top-level metadata must fail with
RecipeError. Malformed entry and operation contents are out of scope here.
Synthetic containers only: no ROM or game data is needed and none may ever
be committed.
Run: python -m unittest discover builder/tests
"""

import json
import lzma
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from emerald3ds_builder import recipe as rcp  # noqa: E402
from emerald3ds_builder.build import Payload  # noqa: E402

SHA = "ab" * 20


def good_meta(**changes):
    meta = {"schema": rcp.SCHEMA, "engine_abi": 7, "rom_sha1": SHA, "release": "t",
            "entries": [{"path": "a", "size": 1, "crc": 2, "ops": [["F", 0, 1]]}],
            "bitmaps": [], "generated": [], "inputs": [], "vtree": {}}
    meta.update(changes)
    return meta


def container(meta=None, meta_raw=None, literals=b"xyz", blob=None, tail=None):
    """MAGIC, u32 length, LZMA metadata, LZMA literals; any part replaceable."""
    if blob is None:
        if meta_raw is None:
            meta_raw = json.dumps(good_meta() if meta is None else meta).encode("utf-8")
        blob = lzma.compress(meta_raw)
    if tail is None:
        tail = lzma.compress(literals)
    return rcp.MAGIC + struct.pack("<I", len(blob)) + blob + tail


class ContainerErrorTests(unittest.TestCase):
    def assertRecipeError(self, data, cause=None):
        with self.assertRaises(rcp.RecipeError) as ctx:
            rcp.Recipe.from_bytes(data)
        if cause is not None:
            # The raw exception the old code let escape is kept as the cause.
            self.assertIsInstance(ctx.exception.__cause__, cause)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "emerald3ds.recipe"
            path.write_bytes(data)
            with self.assertRaises(rcp.RecipeError):
                rcp.Recipe.load(path)
        return ctx.exception

    def test_not_a_recipe(self):
        for data in (b"", b"PK\x03\x04", b"XXXXXXXX\x00\x00\x00\x00"):
            err = self.assertRecipeError(data)
            self.assertIn("not a Emerald 3DS recipe", str(err))

    def test_truncated_header(self):
        full = container()
        for cut in (3, 8, 9, 11):   # inside the magic, then inside the length field
            err = self.assertRecipeError(full[:cut])
            self.assertIn("header truncated", str(err))

    def test_truncated_metadata(self):
        full = container()
        (size,) = struct.unpack_from("<I", full, 8)
        for cut in (12, 12 + size // 2, 12 + size - 1):
            err = self.assertRecipeError(full[:cut])
            self.assertIn("metadata truncated", str(err))

    def test_length_field_larger_than_file(self):
        data = bytearray(container())
        struct.pack_into("<I", data, 8, 0xFFFFFFFF)
        self.assertRecipeError(bytes(data))

    def test_corrupt_compressed_metadata(self):
        blob = bytearray(lzma.compress(json.dumps(good_meta()).encode()))
        blob[len(blob) // 2:] = b"\0" * (len(blob) - len(blob) // 2)
        self.assertRecipeError(container(blob=bytes(blob)), lzma.LZMAError)
        self.assertRecipeError(container(blob=b"not lzma at all"), lzma.LZMAError)

    def test_invalid_json(self):
        self.assertRecipeError(container(meta_raw=b"{\"schema\": 1,"), ValueError)
        self.assertRecipeError(container(meta_raw=b"\x80\x81 not utf-8"), ValueError)

    def test_metadata_not_an_object(self):
        for raw in (b"[]", b"null", b"42", b"\"text\""):
            err = self.assertRecipeError(container(meta_raw=raw))
            self.assertIn("not an object", str(err))

    def test_unsupported_schema(self):
        meta = good_meta(schema=2)
        err = self.assertRecipeError(container(meta))
        self.assertIn("unsupported recipe schema", str(err))
        meta.pop("schema")
        self.assertRecipeError(container(meta))

    def test_missing_required_metadata(self):
        for key in ("engine_abi", "rom_sha1", "release", "entries"):
            meta = good_meta()
            del meta[key]
            err = self.assertRecipeError(container(meta))
            self.assertIn(repr(key), str(err))

    def test_wrong_metadata_types(self):
        cases = {"engine_abi": ["1", 1.5, True, None], "rom_sha1": [None, 5],
                 "release": [3, None], "entries": [{}, "x", None, [1], [["F", 0, 1]]],
                 "bitmaps": [{}, None], "generated": [{}, "x", [7], ["x"]], "inputs": [7],
                 "vtree": [[], "x"]}
        for key, values in cases.items():
            for value in values:
                with self.subTest(key=key, value=value):
                    err = self.assertRecipeError(container(good_meta(**{key: value})))
                    self.assertIn(repr(key), str(err))

    def test_damaged_literal_pool(self):
        good_tail = lzma.compress(b"literal pool" * 50)
        for tail in (b"", good_tail[:len(good_tail) // 2], b"\0" * 32):
            with self.subTest(tail=len(tail)):
                err = self.assertRecipeError(container(tail=tail), lzma.LZMAError)
                self.assertIn("literal pool", str(err))


class ValidContainerTests(unittest.TestCase):
    def test_roundtrip_preserves_format_and_fields(self):
        r = rcp.Recipe(engine_abi=0xDEADBEEF, rom_sha1=SHA, release="v1",
                       entries=[{"path": "a", "size": 1, "crc": 2, "ops": [["F", 0, 1]]}],
                       bitmaps=[[0, 1, -1, -2]], generated=[{"path": "g", "size": 0, "crc": 0}],
                       inputs=[{"path": "i"}], vtree={"k": 1}, literals=b"\x00\x01lit")
        data = r.to_bytes()
        self.assertEqual(data[:8], rcp.MAGIC)
        (size,) = struct.unpack_from("<I", data, 8)
        self.assertEqual(json.loads(lzma.decompress(data[12:12 + size]))["rom_sha1"], SHA)
        self.assertEqual(lzma.decompress(data[12 + size:]), b"\x00\x01lit")
        self.assertEqual(rcp.Recipe.from_bytes(data), r)

    def test_optional_metadata_may_be_absent(self):
        meta = {"schema": rcp.SCHEMA, "engine_abi": 0, "rom_sha1": SHA, "release": "",
                "entries": []}
        back = rcp.Recipe.from_bytes(container(meta, literals=b""))
        self.assertEqual((back.bitmaps, back.generated, back.inputs, back.vtree, back.literals),
                         ([], [], [], {}, b""))

    def test_unknown_extra_metadata_is_ignored(self):
        back = rcp.Recipe.from_bytes(container(good_meta(future_field={"x": 1})))
        self.assertEqual(back.rom_sha1, SHA)


class RomSha1Tests(unittest.TestCase):
    def _write(self, tmp, data):
        path = Path(tmp) / "emerald3ds.recipe"
        path.write_bytes(data)
        return path

    def test_reads_only_the_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            # A literal pool that cannot be decompressed is never looked at.
            path = self._write(tmp, container(tail=b"\0garbage literal pool"))
            self.assertEqual(rcp.recipe_rom_sha1(path), SHA)
            with self.assertRaises(rcp.RecipeError):
                rcp.Recipe.load(path)
            # Neither is metadata the full load would reject.
            path = self._write(tmp, container(good_meta(engine_abi="x", entries=None)))
            self.assertEqual(rcp.recipe_rom_sha1(path), SHA)

    def test_damaged_headers_raise_recipe_error(self):
        full = container()
        (size,) = struct.unpack_from("<I", full, 8)
        cases = [b"", b"nope", full[:8], full[:11], full[:12], full[:12 + size // 2],
                 container(blob=b"not lzma"), container(meta_raw=b"{oops"),
                 container(meta_raw=b"[1, 2]")]
        missing = good_meta()
        del missing["rom_sha1"]
        cases += [container(missing), container(good_meta(rom_sha1=None))]
        with tempfile.TemporaryDirectory() as tmp:
            for i, data in enumerate(cases):
                with self.subTest(case=i):
                    with self.assertRaises(rcp.RecipeError):
                        rcp.recipe_rom_sha1(self._write(tmp, data))

    def test_missing_file_is_still_an_os_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(OSError):
                rcp.recipe_rom_sha1(Path(tmp) / "absent.recipe")


class VariantSelectionTests(unittest.TestCase):
    def test_damaged_variant_is_skipped_not_raised(self):
        # Payload.for_rom catches RecipeError but not LZMAError/struct.error:
        # a cut-off recipe in one variant used to abort the search.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "payload"
            (root / "de").mkdir(parents=True)
            (root / "es").mkdir()
            full = container()
            (size,) = struct.unpack_from("<I", full, 8)
            (root / "emerald3ds.recipe").write_bytes(full[:12 + size - 5])
            (root / "de" / "emerald3ds.recipe").write_bytes(full[:10])
            other = good_meta(rom_sha1="cd" * 20)
            (root / "es" / "emerald3ds.recipe").write_bytes(container(other))
            payload = Payload(root)
            self.assertEqual(payload.for_rom("cd" * 20).root.name, "es")
            self.assertIs(payload.for_rom("ef" * 20), payload)


if __name__ == "__main__":
    unittest.main()
