#!/usr/bin/env python3
"""Compile pinned English Ruby/Sapphire task and RNG code for the 3DS ABI.

Requires a clean source-only checkout (bootstrap.py --game ruby), devkitARM
and a host C++ compiler. Outputs stay temporary; this does not build a game.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]
ARCH = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft", "-marm"]
CFLAGS = ["-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
          "-fsigned-char", "-fno-strict-aliasing"]
PREPROC_SOURCES = ["asm_file.cpp", "c_file.cpp", "charmap.cpp", "preproc.cpp",
                   "string_parser.cpp", "utf8.cpp", "io.cpp"]


def output(command, cwd=None):
    return subprocess.check_output([str(arg) for arg in command], cwd=cwd, text=True).strip()


def run(command, cwd=None, stdout=None):
    print("+ " + " ".join(str(arg) for arg in command), flush=True)
    subprocess.run([str(arg) for arg in command], cwd=cwd, stdout=stdout, check=True)


def check_source(tree: Path, repo: str, commit: str) -> None:
    if not tree.is_dir() or Path(output(["git", "rev-parse", "--show-toplevel"], tree)).resolve() != tree:
        raise ValueError("Prepare a separate source checkout with bootstrap.py --game ruby first.")
    if output(["git", "config", "--local", "--get", "remote.origin.url"], tree) != repo:
        raise ValueError("Source origin does not match upstream.lock.")
    if output(["git", "rev-parse", "HEAD"], tree) != commit:
        raise ValueError("Source commit does not match upstream.lock.")
    if output(["git", "status", "--porcelain", "--untracked-files=all"], tree):
        raise ValueError("Use a clean pinned source checkout; no local work will be reset.")


def compiler_path(name: str) -> Path:
    found = shutil.which(name)
    if found is None:
        raise ValueError("ARM compiler not found; set DEVKITARM or pass --cc.")
    compiler = Path(found).resolve()
    if output([compiler, "-dumpmachine"]) != "arm-none-eabi":
        raise ValueError("Use devkitARM's arm-none-eabi compiler, not a host or Linux ARM compiler.")
    return compiler


def check_objects(compiler: Path, objects: list[Path], directory: Path) -> None:
    # A relocatable link also catches duplicate definitions across real modules.
    combined = directory / "combined.o"
    run([compiler, *ARCH, "-nostdlib", "-r", *objects, "-o", combined])
    data = combined.read_bytes()
    if data[:6] != b"\x7fELF\x01\x01" or struct.unpack_from("<HH", data, 16) != (1, 40):
        raise ValueError("Expected a little-endian ELF32 ARM relocatable object.")
    nm = compiler.with_name("arm-none-eabi-nm.exe" if os.name == "nt" else "arm-none-eabi-nm")
    names = {line.split()[-1] for line in output([nm, "--defined-only", "--extern-only", combined]).splitlines()}
    required = {"ResetTasks", "CreateTask", "RunTasks", "DestroyTask", "GetTaskCount",
                "SetTaskFuncWithFollowupFunc", "SwitchTaskToFollowupFunc", "gTasks",
                "Random", "SeedRng", "gRngValue"}
    if not required <= names:
        raise ValueError("Missing compiled game symbols: " + ", ".join(sorted(required - names)))


def compile_checks(tree: Path, compiler: Path, host_cxx: str, games: list[str]) -> None:
    with tempfile.TemporaryDirectory(prefix="pokeruby-3ds-") as temporary:
        work = Path(temporary)
        preproc = work / ("preproc.exe" if os.name == "nt" else "preproc")
        run([host_cxx, "-std=c++11", "-O2", "-Wall", "-Wno-switch", "-Werror",
             *[tree / "tools/preproc" / src for src in PREPROC_SOURCES], "-o", preproc])
        for game in games:
            directory = work / game
            directory.mkdir()
            defines = ["-D" + game.upper() + "=1", "-DENGLISH=1", "-DREVISION=0", "-DDEBUG=0",
                       "-DMODERN=1", "-DPORTABLE", "-DPLATFORM_3DS", "-D__3DS__", "-DPORT_BRIDGE"]
            # -I would let the game's strings.h shadow newlib's <strings.h>.
            includes = ["-iquote", tree / "include"]
            objects = []
            for name in ("task", "random"):
                raw = directory / (name + ".i")
                translated = directory / (name + ".translated.i")
                obj = directory / (name + ".o")
                run([compiler, *ARCH, *CFLAGS, *defines, *includes, "-E",
                     tree / "src" / (name + ".c"), "-o", raw], cwd=tree)
                # Use the real upstream charmap converter, including task.c's text.
                with translated.open("w", encoding="utf-8") as destination:
                    run([preproc, raw, tree / "charmap.txt"], cwd=tree, stdout=destination)
                run([compiler, *ARCH, *CFLAGS, "-x", "cpp-output", "-c", translated, "-o", obj])
                objects.append(obj)
            abi = directory / "abi.o"
            run([compiler, *ARCH, *CFLAGS, *defines, *includes, "-c",
                 ROOT / "3ds_port/tests/pokeruby_abi_check.c", "-o", abi])
            check_objects(compiler, [*objects, abi], directory)
            print("PASS %s English Rev 0: real task/RNG objects, ARMv6K hard-float ABI and symbols" % game)
    print("Compile-only milestone: no executable, gameplay or hardware verification.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, default=ROOT / "build/upstream-ruby")
    parser.add_argument("--game", choices=["ruby", "sapphire", "both"], default="both")
    default_cc = str(Path(os.environ["DEVKITARM"]) / "bin/arm-none-eabi-gcc") if os.environ.get("DEVKITARM") else "arm-none-eabi-gcc"
    parser.add_argument("--cc", default=default_cc)
    parser.add_argument("--host-cxx", default="g++")
    args = parser.parse_args()
    try:
        compiler = compiler_path(args.cc)
        lock = tomllib.loads((ROOT / "upstream.lock").read_text(encoding="utf-8"))["pokeruby"]
        tree = args.tree.resolve()
        check_source(tree, lock["repository"], lock["commit"])
        compile_checks(tree, compiler, args.host_cxx,
                       ["ruby", "sapphire"] if args.game == "both" else [args.game])
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, "pokeruby 3DS compile check: %s\n" % exc)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
