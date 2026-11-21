#!/usr/bin/env python3
"""Controlled same-runner A/B of full voxel generator export paths.

Run only from a bootstrapped release-image tree with its generated inputs:
  python tools/benchmark_pillow_generators_ab.py \
    --tree build/upstream --baseline-dir benchmark-base \
    --output benchmark-results --runs 5

Only the three Pillow-related Python files are exchanged. The same prepared
upstream tree, interpreter, Pillow, assets and hash seed are used throughout.
Alternate AB / BA order to reduce systematic warm-cache or runner drift.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

from PIL import __version__ as pillow_version

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("voxel_image.py", "voxel_building.py", "gen_voxel_buildings.py", "gen_voxel_relief.py")
STEPS = ("buildings", "relief")


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_report(report: dict, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "results.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def select_version(scripts: Path, source: Path) -> None:
    for name in SOURCES:
        original = source / name
        if not original.is_file():
            raise FileNotFoundError(original)
        shutil.copyfile(original, scripts / name)
    # Avoid importing stale .pyc files after changing the shared import module.
    shutil.rmtree(scripts / "__pycache__", ignore_errors=True)


def execute(script: Path, destination: Path, tree: Path, extra_args=()) -> dict:
    output = destination / (script.stem.removeprefix("gen_voxel_") + ".bin")
    command = [sys.executable, str(script), "--output", str(output), *extra_args]
    log = destination / (output.stem + ".log")
    start = time.perf_counter()
    with log.open("w", encoding="utf-8") as stream:
        proc = subprocess.run(command, cwd=tree, stdout=stream, stderr=subprocess.STDOUT,
                              env=os.environ.copy(), check=False)
    elapsed = time.perf_counter() - start
    if proc.returncode or not output.is_file():
        tail = "\n".join(log.read_text(encoding="utf-8", errors="replace").splitlines()[-35:])
        raise RuntimeError(
            f"{script.name} exited {proc.returncode} after {elapsed:.1f}s:\n{tail}"
        )
    return {"seconds": elapsed, "sha256": digest(output),
            "bytes": output.stat().st_size, "path": str(output)}


def run_pass(version: str, iteration: int, source: Path,
             scripts: Path, tree: Path, destination: Path,
             verify_buildings: bool = False) -> dict:
    print(f"=== {version} #{iteration} ===", flush=True)
    select_version(scripts, source)
    out = destination / f"{iteration:02d}-{version}"
    out.mkdir(parents=True, exist_ok=True)
    results = {}
    for step in STEPS:
        args = (("--terrace-jobs", "4") if step == "relief" else
                ("--verify", "--diagnostics", "failures") if verify_buildings else ())
        results[step] = execute(scripts / f"gen_voxel_{step}.py", out, tree, args)
        print(f"{version} #{iteration} {step}: "
              f"{results[step]['seconds']:.3f}s, {results[step]['sha256']}", flush=True)
        if step == "buildings":
            target = tree / "3ds_port/romfs/voxel/buildings.bin"
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(results[step]["path"], target)
            sidecar = out / "windowlights.bin"
            if sidecar.is_file():
                results["windowlights"] = {"sha256": digest(sidecar),
                                           "bytes": sidecar.stat().st_size}
    return results


def summarize(report: dict, dest: Path) -> None:
    samples = report["samples"]
    variants = ("baseline", "candidate")
    lines = [
        "# Pillow full-generator controlled A/B benchmark",
        "",
        f"Python: {report['python']}",
        f"Pillow: {report['pillow']}",
        f"Hash seed: {report['hash_seed']}",
        f"Runs per variant: {report['runs']}",
        "Order alternates baseline→candidate and candidate→baseline.",
        "Building verification enabled: %s" % report["verify_buildings"],
        "",
        "| Generator | Baseline median | Candidate median | Saved | Improvement | SHA-256 |",
        "|---|---:|---:|---:|---:|---|",
    ]
    for step in STEPS:
        timings = {version: [sample["results"][step]["seconds"] for sample in samples
                             if sample["version"] == version] for version in variants}
        b = statistics.median(timings["baseline"])
        c = statistics.median(timings["candidate"])
        saved = b - c
        hashes = {sample["results"][step]["sha256"] for sample in samples}
        if len(hashes) != 1:
            raise AssertionError(f"{step}: source-dependent output hashes: {sorted(hashes)}")
        h = next(iter(hashes))
        report["summary"][step] = {
            "baseline_seconds": timings["baseline"],
            "candidate_seconds": timings["candidate"],
            "baseline_median_seconds": b, "candidate_median_seconds": c,
            "median_saved_seconds": saved, "median_saved_percent": 100 * saved / b,
            "sha256": h, "all_outputs_byte_identical": True
        }
        lines.append(f"| {step} | {b:.3f}s | {c:.3f}s | {saved:+.3f}s | "
                     f"{100 * saved / b:+.2f}% | \`{h}\` |")
    sidecars = [sample["results"].get("windowlights") for sample in samples]
    if any(sidecars):
        if not all(sidecars) or len({x["sha256"] for x in sidecars}) != 1:
            raise AssertionError("windowlights sidecar differs between runs")
        lines.append(f"\nWindow-light sidecar SHA-256: \`{sidecars[0]['sha256']}\`.")
    lines += ["", "All measured output hashes match across both variants and every run.",
              "Building times include image proofs when --verify-buildings is enabled; "
              "relief times are full --output export.", ""]
    (dest / "summary.md").write_text("\n".join(lines), encoding="utf-8")
    write_report(report, dest)
    print("\n".join(lines), flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", required=True, type=Path)
    parser.add_argument("--baseline-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--verify-buildings", action="store_true",
                        help="also time the full building-image verification proofs")
    args = parser.parse_args()
    if args.runs < 2:
        parser.error("--runs must be >= 2")
    tree = args.tree.resolve()
    baseline = (args.baseline_dir / "3ds_port/scripts").resolve()
    candidate = (ROOT / "3ds_port/scripts").resolve()
    out = args.output.resolve()
    staged = tree / "3ds_port/scripts"
    for folder in (tree, baseline, candidate, staged):
        if not folder.is_dir():
            raise FileNotFoundError(folder)
    # Make the comparison deterministic even if a caller forgets to export it.
    os.environ["PYTHONHASHSEED"] = "0"
    os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
    report = {
        "python": sys.version, "pillow": pillow_version,
        "platform": platform.platform(), "hash_seed": os.environ["PYTHONHASHSEED"],
        "runs": args.runs, "verify_buildings": args.verify_buildings,
        "method": "sequential full export, alternate AB/BA",
        "samples": [], "summary": {},
    }
    out.mkdir(parents=True, exist_ok=True)
    try:
        for iteration in range(1, args.runs + 1):
            order = (("baseline", "candidate") if iteration % 2
                     else ("candidate", "baseline"))
            for version in order:
                sources = baseline if version == "baseline" else candidate
                result = run_pass(version, iteration, sources, staged, tree, out,
                                  args.verify_buildings)
                report["samples"].append({"version": version, "iteration": iteration,
                                          "results": result})
                write_report(report, out)
        summarize(report, out)
    finally:
        # Do not leave the prepared source tree pointed at baseline.
        select_version(staged, candidate)


if __name__ == "__main__":
    main()
