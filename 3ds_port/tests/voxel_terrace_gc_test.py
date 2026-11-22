#!/usr/bin/env python3
"""Verify scoped GC tuning for relief terrace generation without ROM inputs.

Extract the production functions from the source AST, so testing does not
require importing the ROM-dependent voxel graphics modules.
"""
import ast
import gc
from pathlib import Path
from types import SimpleNamespace

SOURCE = Path(__file__).resolve().parents[1] / "scripts" / "voxel_terraces.py"
TREE = ast.parse(SOURCE.read_text(encoding="utf-8"), str(SOURCE))


def production(name, namespace):
    node = next(node for node in TREE.body
                if isinstance(node, ast.FunctionDef) and node.name == name)
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(SOURCE), "exec"), namespace)
    return namespace[name]


def test_restoration_and_result():
    original = gc.get_threshold()
    observed = []

    def solve(lid, reference, *, topology):
        observed.append((lid, reference, topology, gc.get_threshold()))
        return {"result": lid}

    tuned = production("_solve_gc_tuned", {"gc": gc, "solve": solve})
    assert tuned("TEST_MAP", object(), "topology") == {"result": "TEST_MAP"}
    assert len(observed) == 1
    assert observed[0][3][0] >= max(3000, original[0])
    assert observed[0][3][1:] == original[1:]
    assert gc.get_threshold() == original

    def broken(*args, **kwargs):
        observed.append(gc.get_threshold())
        raise RuntimeError("synthetic solver failure")

    failed = production("_solve_gc_tuned", {"gc": gc, "solve": broken})
    try:
        failed("TEST_MAP", None, None)
    except RuntimeError as exc:
        assert str(exc) == "synthetic solver failure"
    else:
        raise AssertionError("expected synthetic failure")
    assert observed[-1][0] >= 3000
    assert gc.get_threshold() == original


def test_existing_higher_threshold():
    initial = gc.get_threshold()
    try:
        gc.set_threshold(5000, initial[1], initial[2])
        visited = []
        def observe(*args, **kwargs):
            visited.append(gc.get_threshold())
            return None
        tuned = production("_solve_gc_tuned", {"gc": gc, "solve": observe})
        assert tuned("map", None, None) is None
        assert visited == [(5000, initial[1], initial[2])]
        assert gc.get_threshold() == (5000, initial[1], initial[2])
    finally:
        gc.set_threshold(*initial)


def test_serial_and_worker_paths():
    seen = []
    def tuned(lid, reference, topology):
        seen.append((lid, reference, topology))
        return {"cells": SimpleNamespace(members=[(lid, 0, 0)])}
    globals_ = {"_solve_gc_tuned": tuned,
                "gr": SimpleNamespace(map_topology=lambda: "topology")}
    serial = production("_serial_solves", globals_)
    sols = list(serial(["A", "A", "B"], "reference"))
    assert len(sols) == 2
    assert seen == [("A", "reference", "topology"), ("B", "reference", "topology")]
    globals_["_worker_reference"] = "worker reference"
    globals_["_worker_topology"] = "worker topology"
    worker = production("_solve_in_worker", globals_)
    assert worker("C")["cells"].members == [("C", 0, 0)]
    assert seen[-1] == ("C", "worker reference", "worker topology")


if __name__ == "__main__":
    test_restoration_and_result()
    test_existing_higher_threshold()
    test_serial_and_worker_paths()
    print("PASS: GC threshold scope, restoration, and serial/worker wiring")
