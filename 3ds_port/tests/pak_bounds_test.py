"""Run the same synthetic pack layout cases through the desktop and native readers."""

import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "builder"))
from emerald3ds_builder.pak import ENTRY, HEADER, PakError, PakReader, write_pak


def main(native):
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "layout.pak"
        abi = 123
        write_pak(path, [("one.bin", b"test"), ("empty.bin", b"")], abi,
                  bytes.fromhex("f3ae088181bf583e55daf962a92bb46f4f1d07b7"))
        valid = path.read_bytes()
        fields = HEADER.unpack_from(valid)

        def modified(offset, fmt, value):
            data = bytearray(valid)
            struct.pack_into(fmt, data, offset, value)
            if offset >= HEADER.size:
                index = data[fields[5]:fields[5] + fields[4] * ENTRY.size]
                struct.pack_into("<I", data, 56, zlib.crc32(index))
            struct.pack_into("<I", data, 60, zlib.crc32(data[:60]))
            return data

        cases = {
            "valid": valid,
            "truncated header": valid[:63],
            "truncated index": valid[:80],
            "zero entries": modified(36, "<I", 0),
            "index points into header": modified(40, "<Q", 8),
            "index crosses into data": modified(48, "<Q", fields[5] + ENTRY.size),
            "data starts after EOF": modified(48, "<Q", len(valid) + 1),
            "payload points into index": modified(fields[5] + 16, "<Q", HEADER.size),
            "payload after EOF": modified(fields[5] + 16, "<Q", len(valid) + 1),
            "unsupported compression": modified(fields[5] + 12, "<I", 1),
        }
        for name, data in cases.items():
            path.write_bytes(data)
            expected = name == "valid"
            try:
                with PakReader(path):
                    accepted = True
            except PakError:
                accepted = False
            assert accepted == expected, (name, accepted)
            result = subprocess.run([native, str(path), f"{abi:x}", "--check-layout"])
            assert result.returncode == (0 if expected else 1), (name, result.returncode)
        print("PASS shared data-pack layout cases")


if __name__ == "__main__":
    main(sys.argv[1])
