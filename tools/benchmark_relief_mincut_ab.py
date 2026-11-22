#!/usr/bin/env python3
"""Experiment-only controlled A/B of the relief terrace finish stage.

Run inside the pinned release image with a prepared tree and base relief.
Both versions work from the SAME immutable base relief; only voxel_terraces.py
is switched. Alternating A/B and B/A order reduces runner/cache bias.
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


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def sampled_tree_rss_kib(pid: int) -> int | None:
    """Sum visible Linux RSS across a process and all its descendants.

    This includes shared memory in multiple processes, so it is a comparative
    sampled RSS indicator, NOT unique physical memory or an exact peak.
    A missing /proc yields None rather than rejecting a valid benchmark.
    """
    if not Path("/proc").is_dir():
        return None
    page_kib = os.sysconf("SC_PAGE_SIZE") / 1024
    total = 0
    seen = set()
    pending = [pid]
    while pending:
        child = pending.pop()
        if child in seen:
            continue
        seen.add(child)
        proc = Path("/proc") / str(child)
        try:
            # Include all Python worker descendants, but tolerate process
            # exit/creation races between two /proc reads.
            pending.extend(int(x) for x in
                           (proc / "task" / str(child) / "children").read_text().split())
            resident_pages = int((proc / "statm").read_text().split()[1])
        except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError, IndexError):
            continue
        total += int(resident_pages * page_kib)
    return total


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", required=True, type=Path)
    parser.add_argument("--baseline-dir", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--jobs", type=int, choices=(1, 4), default=4)
    args = parser.parse_args()
    if args.runs < 2:
        parser.error("--runs must be at least 2")
    tree = args.tree.resolve()
    old = (args.baseline_dir / "3ds_port/scripts/voxel_terraces.py").resolve()
    new = (ROOT / "3ds_port/scripts/voxel_terraces.py").resolve()
    staged = tree / "3ds_port/scripts/voxel_terraces.py"
    base_relief = tree / "3ds_port/build/voxel_relief_base.bin"
    generator = tree / "3ds_port/scripts/gen_voxel_relief.py"
    for path in (old, new, staged, base_relief, generator):
        if not path.is_file():
            raise FileNotFoundError(path)
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["PYTHONHASHSEED"] = "0"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env.pop("VOXEL_RELIEF_CACHE", None)
    env.pop("VOXEL_RELIEF_EXPORT", None)
    env.pop("VOXEL_RELIEF_NO_FIXES", None)
    env.pop("VOXEL_TERRACES", None)
    samples = []
    report = {"python": sys.version, "pillow": pillow_version,
              "platform": platform.platform(), "base_sha256": digest(base_relief),
              "jobs": args.jobs, "runs": args.runs, "samples": samples}
    try:
        for iteration in range(1, args.runs + 1):
            order = ("baseline", "candidate") if iteration % 2 else ("candidate", "baseline")
            for version in order:
                shutil.copyfile(old if version == "baseline" else new, staged)
                shutil.rmtree(staged.parent / "__pycache__", ignore_errors=True)
                target = output / f"{iteration}-{version}.bin"
                logfile = output / f"{iteration}-{version}.log"
                command = [sys.executable, str(generator), "--base-input", str(base_relief),
                           "--output", str(target), "--terrace-jobs", str(args.jobs)]
                started = time.perf_counter()
                sampled_peak = None
                with logfile.open("w", encoding="utf-8") as log:
                    with subprocess.Popen(command, cwd=tree, env=env,
                                          stdout=log, stderr=subprocess.STDOUT) as proc:
                        while True:
                            rss = sampled_tree_rss_kib(proc.pid)
                            if rss is not None:
                                sampled_peak = max(sampled_peak or 0, rss)
                            if proc.poll() is not None:
                                break
                            time.sleep(0.10)
                        returncode = proc.wait()
                seconds = time.perf_counter() - started
                if returncode or not target.is_file():
                    tail = "\n".join(logfile.read_text(encoding="utf-8",
                                    errors="replace").splitlines()[-30:])
                    raise RuntimeError(f"{version} #{iteration} failed (exit {returncode}):\n{tail}")
                sample = {"version": version, "iteration": iteration, "seconds": seconds,
                          "sha256": digest(target), "bytes": target.stat().st_size,
                          "sampled_peak_tree_rss_kib": sampled_peak}
                samples.append(sample)
                (output / "results.json").write_text(json.dumps(report, indent=2) + "\n",
                                                      encoding="utf-8")
                print(f"{version} #{iteration}: {seconds:.3f}s "
                      f"sampled_tree_RSS={sampled_peak} KiB "
                      f"sha256={sample['sha256']}", flush=True)
        unique = {(x["sha256"], x["bytes"]) for x in samples}
        if len(unique) != 1:
            raise AssertionError(f"Base and candidate relief outputs differ: {unique}")
        b = statistics.median(x["seconds"] for x in samples if x["version"] == "baseline")
        c = statistics.median(x["seconds"] for x in samples if x["version"] == "candidate")
        memory = {version: [x["sampled_peak_tree_rss_kib"] for x in samples
                            if x["version"] == version
                            and x["sampled_peak_tree_rss_kib"] is not None]
                  for version in ("baseline", "candidate")}
        rss_medians = {name: statistics.median(values) if values else None
                       for name, values in memory.items()}
        report["summary"] = {"baseline_median_seconds": b, "candidate_median_seconds": c,
                             "saved_seconds": b - c, "improvement_percent": (b - c) * 100 / b,
                             "output_byte_identical": True,
                             "median_sampled_peak_tree_rss_kib": rss_medians}
        memory_note = ("Sampled process-tree summed RSS (shared pages double-counted): "
                       f"baseline {rss_medians['baseline']} KiB, "
                       f"candidate {rss_medians['candidate']} KiB. "
                       "Samples every 100ms; not an exact physical peak.\n")
        summary = ("# Terrace finish A/B\n\n"
                   f"Python {sys.version.split()[0]}, Pillow {pillow_version}, {args.jobs} workers\n"
                   f"{args.runs} alternating pairs, identical baseline data and hash seed 0\n"
                   f"Baseline median: {b:.3f}s\nCandidate median: {c:.3f}s\n"
                   f"Saved: {b-c:+.3f}s ({(b-c)*100/b:+.2f}%)\n"
                   f"{memory_note}"
                   f"All complete relief outputs matched SHA-256 and size: {next(iter(unique))[0]}\n")
        (output / "summary.md").write_text(summary, encoding="utf-8")
        print(summary, flush=True)
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n",
                                              encoding="utf-8")
    finally:
        # Restore the candidate source even when a comparison fails.
        shutil.copyfile(new, staged)
        shutil.rmtree(staged.parent / "__pycache__", ignore_errors=True)


if __name__ == "__main__":
    main()
