#!/usr/bin/env python3
"""Benchmark ROM-to-pack time locally with release inputs or a synthetic fixture."""

import argparse
import hashlib
import json
import platform
import statistics
import tempfile
import time
from pathlib import Path

from build_web_payload import make_synthetic
from emerald3ds_builder.build import Payload, build_pack
from emerald3ds_builder.rom import load_rom


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", type=Path)
    parser.add_argument("--payload", type=Path)
    parser.add_argument("--runner", choices=("subprocess", "inprocess"), default="subprocess")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--followers", action="store_true", help="include the desktop follower downloader")
    args = parser.parse_args()
    if (args.rom is None) != (args.payload is None) or args.runs < 1:
        parser.error("provide both --rom and --payload, and a positive run count")
    if args.followers and args.rom is None:
        parser.error("follower downloads require real release inputs")

    with tempfile.TemporaryDirectory(prefix="emerald3ds-player-bench-") as folder:
        work = Path(folder)
        synthetic = args.rom is None
        if synthetic:
            fixture = make_synthetic(work)
            args.rom, args.payload = fixture["rom"], fixture["payload"]
            # The normal builder accepts only known cartridge hashes. This
            # generated fixture carries its own hash in the published manifest.
            manifest = json.loads(fixture["manifest"].read_text(encoding="utf-8"))
            from emerald3ds_builder.web import supported_sha1s
            supported = supported_sha1s(manifest)

        def run(index):
            rom_seconds = None
            if synthetic:
                start = time.perf_counter()
                rom = load_rom(args.rom, supported)
                rom_seconds = time.perf_counter() - start
            else:
                rom = None
            pack = work / f"pack-{index}.pak"
            summary = build_pack(args.rom, Payload(args.payload), pack,
                                 runner=args.runner, rom=rom, follower_fetcher=args.followers)
            phases = dict(summary["timings"])
            if rom_seconds is not None:
                phases["rom"] = round(rom_seconds, 4)
            with pack.open("rb") as stream:
                sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
            pack.unlink()
            return {"phases": phases, "total_seconds": round(summary["total_seconds"] + (rom_seconds or 0), 4),
                    "bytes": summary["bytes"], "abi": f"{summary['abi']:08x}", "sha256": sha256}

        results = [run(i) for i in range(args.runs)]
        if len({result["sha256"] for result in results}) != 1:
            raise SystemExit("The repeated builds produced different packs")
        print(json.dumps({"fixture": "synthetic" if synthetic else "release", "runner": args.runner,
                          "followers": args.followers, "python": platform.python_version(),
                          "median_seconds": statistics.median(row["total_seconds"] for row in results),
                          "runs": results}, indent=2))


if __name__ == "__main__":
    main()
