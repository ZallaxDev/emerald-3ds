"""Run the port's voxel generators on the tree rebuilt from the ROM.

The generator scripts ship with the release (payload/voxelgen/, the same files
as 3ds_port/scripts in the source tree), with the rebuilt tree as their
repository root. Their outputs must match the CRCs the release was built with;
anything else would be a different game data set than the executable expects.

The steps, their order, the copy of the scripts into the tree and the output
checks are shared. Only the way one step is run differs:

* `run_step_subprocess` runs each script in its own process (desktop and the
  frozen Windows builder): no state can carry over between scripts. Independent
  steps run side by side. Relief waits for buildings, whose model placements it
  reads when deriving the terrain under each model;
* `run_step_inprocess` runs each script in this interpreter (the web builder:
  Pyodide has no processes). It gives every script a fresh copy of its sibling
  modules, its own argv, working directory and import path, and puts them all
  back afterwards, so a script sees the same clean start as in a new process.

Both run the scripts' own `main()` through their `__main__` guard, so the
command-line scripts keep working unchanged and no generator logic is copied.
"""

from __future__ import annotations

import concurrent.futures
import contextlib
import io
import os
import runpy
import shutil
import subprocess
import sys
import time
import zlib
from pathlib import Path

from .errors import BuilderError

# (script, arguments, output path relative to the tree) in the order the
# generators depend on each other.
STEPS = [
    ("gen_voxel_regions.py", [], "3ds_port/romfs/voxel/regions.bin"),
    ("gen_voxel_sign_masks.py", [], "3ds_port/romfs/voxel/signposts.bin"),
    ("gen_voxel_buildings.py", ["--output", "3ds_port/romfs/voxel/buildings.bin"],
     "3ds_port/romfs/voxel/buildings.bin"),
    ("gen_voxel_relief.py", ["--output", "3ds_port/romfs/voxel/relief.bin"], "3ds_port/romfs/voxel/relief.bin"),
    # not voxel data, but made the same way: the intro's leaves scene, widened
    ("gen_intro_margins.py", ["--output", "3ds_port/romfs/stage/leaves.bin"], "3ds_port/romfs/stage/leaves.bin"),
]

# What each step makes, for the progress line.
LABELS = {
    "gen_voxel_regions.py": "map regions",
    "gen_voxel_sign_masks.py": "signposts",
    "gen_voxel_relief.py": "terrain relief",
    "gen_voxel_buildings.py": "buildings",
    "gen_intro_margins.py": "intro scene",
}

RUNNERS = ("subprocess", "inprocess")


def script_command(script: Path, args: list[str]) -> list[str]:
    """How to run one bundled script: through this very executable when frozen."""
    if getattr(sys, "frozen", False):
        exe = Path(sys.executable)
        console = exe.with_name("emerald3ds-builder-cli" + exe.suffix)
        return [str(console if console.exists() else exe), "--run-script", str(script)] + args
    return [sys.executable, "-B", "-m", "emerald3ds_builder", "--run-script", str(script)] + args


def run_step_subprocess(script: Path, args: list[str], cwd: Path) -> None:
    env = dict(os.environ)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    here = Path(__file__).resolve().parent.parent
    env["PYTHONPATH"] = str(here) + os.pathsep + env.get("PYTHONPATH", "")
    result = subprocess.run(script_command(script, args), cwd=str(cwd), env=env, capture_output=True, text=True,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode != 0:
        raise BuilderError("A voxel data generator failed (%s)." % script.name,
                           (result.stderr or result.stdout)[-1500:], code="generator_failed")


def run_step_inprocess(script: Path, args: list[str], cwd: Path) -> None:
    scripts_dir = script.parent
    siblings = {p.stem for p in scripts_dir.glob("*.py")}
    saved_argv, saved_path, saved_cwd = sys.argv[:], sys.path[:], os.getcwd()
    saved_modules = {name: sys.modules.pop(name) for name in list(sys.modules) if name in siblings}
    saved_bytecode = sys.dont_write_bytecode
    output = io.StringIO()
    failure = None
    try:
        sys.argv = [str(script)] + list(args)
        sys.path.insert(0, str(scripts_dir))
        sys.dont_write_bytecode = True
        os.chdir(str(cwd))
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            runpy.run_path(str(script), run_name="__main__")
    except SystemExit as exc:
        if exc.code not in (None, 0):
            failure = str(exc.code)
    except MemoryError:
        raise
    except Exception as exc:  # the generator's own crash, reported like a failed process
        failure = "%s: %s" % (type(exc).__name__, exc)
    finally:
        os.chdir(saved_cwd)
        sys.argv = saved_argv
        sys.path[:] = saved_path
        sys.dont_write_bytecode = saved_bytecode
        for name in [n for n in sys.modules if n in siblings]:
            del sys.modules[name]
        sys.modules.update(saved_modules)
    if failure is not None:
        raise BuilderError("A voxel data generator failed (%s)." % script.name,
                           (output.getvalue() + "\n" + failure)[-1500:], code="generator_failed")


def run_generators(tree: Path, voxelgen: Path, expected: list[dict], progress=None,
                   runner: str = "subprocess", timings: list | None = None) -> dict[str, bytes]:
    """Run the steps and check their outputs.

    progress(fraction, detail) is told when each step starts and how long it
    took; `timings`, when given, receives (step, seconds) for every step and
    for the output check ("check"). The coordinator measures, not the scripts:
    their own output is captured."""
    if runner not in RUNNERS:
        raise ValueError("unknown generator runner %r" % runner)
    run_step = run_step_inprocess if runner == "inprocess" else run_step_subprocess
    if not voxelgen.is_dir():
        raise BuilderError("The release is incomplete: payload/voxelgen is missing.", code="payload_incomplete")
    port = tree / "3ds_port"
    shutil.copytree(voxelgen, port, dirs_exist_ok=True)
    (port / "romfs" / "voxel").mkdir(parents=True, exist_ok=True)
    steps = [s for s in STEPS if (port / "scripts" / s[0]).exists()]

    def label(i: int) -> str:
        return "%s (%d/%d)" % (LABELS.get(steps[i][0], steps[i][0]), i + 1, len(steps))

    def timed(i: int) -> float:
        script, args, _ = steps[i]
        start = time.perf_counter()
        run_step(port / "scripts" / script,
                 [a if not a.startswith("3ds_port/") else str(tree / a) for a in args], port)
        return time.perf_counter() - start

    took: dict[int, float] = {}
    if runner == "subprocess" and len(steps) > 1:
        if progress:
            progress(0.0, ", ".join(LABELS.get(s[0], s[0]) for s in steps))
        workers = max(1, min(len(steps), os.cpu_count() or 1))
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            futures = {}
            running = {}

            def after_buildings(i, prerequisite):
                if prerequisite is not None:
                    prerequisite.result()
                return timed(i)

            for i, (script, _, _) in enumerate(steps):
                prerequisite = (futures.get("gen_voxel_buildings.py")
                                if script == "gen_voxel_relief.py" else None)
                future = pool.submit(after_buildings, i, prerequisite)
                futures[script] = future
                running[future] = i
            failed: dict[int, BaseException] = {}
            for future in concurrent.futures.as_completed(running):
                i = running[future]
                try:
                    took[i] = future.result()
                except BaseException as exc:  # reported below, the first step's first
                    failed[i] = exc
                    continue
                if progress:
                    progress(len(took) / len(steps), "%s, done in %.1f s" % (label(i), took[i]))
        if failed:
            raise failed[min(failed)]
    else:
        for i in range(len(steps)):
            if progress:
                progress(i / len(steps), label(i))
            took[i] = timed(i)
            if progress:
                progress((i + 1) / len(steps), "%s, done in %.1f s" % (label(i), took[i]))
    if timings is not None:
        timings.extend((steps[i][0], took[i]) for i in range(len(steps)))
    start = time.perf_counter()
    outputs: dict[str, bytes] = {}
    for item in expected:
        rel = item["path"]
        path = port / "romfs" / rel
        if not path.exists():
            raise BuilderError("A voxel data file was not produced: %s" % rel, code="generator_output_missing")
        data = path.read_bytes()
        if len(data) != item["size"] or zlib.crc32(data) & 0xFFFFFFFF != item["crc"]:
            raise BuilderError("A voxel data file does not match this release: %s" % rel,
                               "The generated file differs from the one the game was built with.",
                               code="generator_output_mismatch")
        outputs[rel] = data
    if timings is not None:
        timings.append(("check", time.perf_counter() - start))
    return outputs
