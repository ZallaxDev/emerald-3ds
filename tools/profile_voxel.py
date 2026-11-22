#!/usr/bin/env python3
"""Profile prepared voxel generators without changing release version pins.

Run bootstrap.py and prepare the voxel graphics inputs first. Use separate
output directories for baseline and candidate runs. Profiled wall times
include instrumentation overhead; use --profiler none for timing comparisons.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import pstats
import shutil
import subprocess
import sys
import time

from PIL import __version__ as pillow_version

# Stop tracing cleanly from inside the child, so a bounded pass still saves
# its statistics. Killing a traced process externally can lose the profile.
BOUNDED_TRACE = '''
import cProfile, runpy, signal, sys
profile_path, seconds, script = sys.argv[1:4]
sys.argv = [script] + sys.argv[4:]
class ProfileLimit(BaseException):
    pass
def stop(signum, frame):
    raise ProfileLimit()
signal.signal(signal.SIGALRM, stop)
profile = cProfile.Profile()
limited = False
signal.setitimer(signal.ITIMER_REAL, float(seconds))
try:
    profile.runcall(runpy.run_path, script, run_name="__main__")
except ProfileLimit:
    limited = True
    print("Stopped at profiling time limit; output may be incomplete.")
finally:
    signal.setitimer(signal.ITIMER_REAL, 0)
    profile.disable()
    profile.dump_stats(profile_path)
if limited:
    sys.exit(124)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True,
                        help="prepared bootstrap tree containing graphics inputs")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--profiler", choices=("tracing", "sampling", "none"), default="tracing")
    parser.add_argument("--seconds", type=float,
                        help="bound each tracing pass (POSIX only); saves a partial profile")
    parser.add_argument("--steps", nargs="+", choices=("buildings", "relief"),
                        default=["buildings", "relief"])
    args = parser.parse_args()
    if args.seconds is not None and (args.seconds <= 0 or args.profiler != "tracing" or os.name != "posix"):
        parser.error("--seconds requires tracing on POSIX and a positive limit")
    tree, out = args.tree.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = {"python": sys.version, "pillow": pillow_version,
              "platform": platform.platform(), "profiler": args.profiler,
              "seconds_limit": args.seconds,
              "jit": sys._jit.is_enabled() if hasattr(sys, "_jit") else None,
              "hash_seed": os.environ.get("PYTHONHASHSEED", "random"),
              "mode": "generator --output (without --verify)", "steps": []}
    # Relief reads buildings.bin, so always order buildings before relief.
    for name in (step for step in ("buildings", "relief") if step in args.steps):
        script = tree / "3ds_port/scripts" / ("gen_voxel_" + name + ".py")
        binary, profile = out / (name + ".bin"), out / (name + ".pstats")
        binary.unlink(missing_ok=True)
        command = [sys.executable]
        if args.seconds is not None:
            command += ["-c", BOUNDED_TRACE, str(profile), str(args.seconds)]
        elif args.profiler == "tracing":
            command += ["-m", "cProfile", "-o", str(profile)]
        elif args.profiler == "sampling":
            command += ["-m", "profiling.sampling", "run", "--pstats", "-o", str(profile)]
        command += [str(script), "--output", str(binary)]
        print("Profiling", name, flush=True)
        start = time.perf_counter()
        with (out / (name + ".log")).open("w", encoding="utf-8") as log:
            result = subprocess.run(command, cwd=tree, stdout=log, stderr=subprocess.STDOUT)
        limited = args.seconds is not None and result.returncode == 124
        if result.returncode != 0 and not limited:
            raise subprocess.CalledProcessError(result.returncode, command)
        elapsed = time.perf_counter() - start
        step = {"name": name, "wall_seconds": elapsed, "completed": not limited and binary.is_file()}
        if step["completed"]:
            with binary.open("rb") as generated:
                digest = hashlib.file_digest(generated, "sha256").hexdigest()
            step.update(bytes=binary.stat().st_size, sha256=digest)
            sidecar = out / "windowlights.bin"
            if name == "buildings" and sidecar.is_file():
                with sidecar.open("rb") as generated:
                    digest = hashlib.file_digest(generated, "sha256").hexdigest()
                step["sidecars"] = [{"name": sidecar.name, "bytes": sidecar.stat().st_size,
                                     "sha256": digest}]
        elif not limited:
            raise RuntimeError("generator succeeded without producing " + str(binary))
        report["steps"].append(step)
        if profile.exists() and args.profiler != "none":
            with (out / (name + "-hotspots.txt")).open("w", encoding="utf-8") as summary:
                stats = pstats.Stats(str(profile), stream=summary)
                stats.strip_dirs().sort_stats("cumulative").print_stats(40)
                stats.sort_stats("tottime").print_stats(40)
        if name == "buildings" and "relief" in args.steps and step["completed"]:
            target = tree / "3ds_port/romfs/voxel/buildings.bin"
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(binary, target)
        (out / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(name, "%.3f seconds" % elapsed, flush=True)
        if name == "buildings" and not step["completed"] and "relief" in args.steps:
            print("Skipping relief: the bounded building pass did not produce its input.", flush=True)
            break


if __name__ == "__main__":
    main()
