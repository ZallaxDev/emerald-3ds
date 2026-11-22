#!/usr/bin/env python3
"""Check the production terrace minimum-cut function against the previous algorithm.

This uses genuine production AST functions and the exact original traversal,
independent of the ROM and devkitARM. It exercises residual cycles, parallel
arcs, epsilon boundary capacities, and convex-label problems.
"""
import ast
import random
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "voxel_terraces.py"


def reference_source_side(n, arcs, s, t):
    """The nodes on the source's side of a minimum s-t cut (Dinic's
    blocking flows) of a graph of `n` nodes and arcs (u, v, capacity)."""
    to, cap, nxt, head = [], [], [], [-1] * n
    for u, v, w in arcs:
        to += [v, u]
        cap += [w, 0.0]
        nxt += [head[u], head[v]]
        head[u], head[v] = len(to) - 2, len(to) - 1
    eps = 1e-9
    while True:
        level = [-1] * n
        level[s] = 0
        queue = [s]
        for u in queue:
            e = head[u]
            while e >= 0:
                if cap[e] > eps and level[to[e]] < 0:
                    level[to[e]] = level[u] + 1
                    queue.append(to[e])
                e = nxt[e]
        if level[t] < 0:
            break
        it = head[:]
        while True:
            # one path along the levels, as deep as it goes
            path, u = [], s
            while u != t:
                e = it[u]
                while e >= 0 and not (cap[e] > eps and level[to[e]] == level[u] + 1):
                    e = nxt[e]
                it[u] = e
                if e < 0:
                    if not path:
                        break
                    level[u] = -1           # a dead end: never again this phase
                    e = path.pop()
                    u = to[e ^ 1]
                    it[u] = nxt[it[u]]
                    continue
                path.append(e)
                u = to[e]
            if u != t:
                break
            push = min(cap[e] for e in path)
            for e in path:
                cap[e] -= push
                cap[e ^ 1] += push
    seen = [False] * n
    seen[s] = True
    queue = [s]
    for u in queue:
        e = head[u]
        while e >= 0:
            if cap[e] > eps and not seen[to[e]]:
                seen[to[e]] = True
                queue.append(to[e])
            e = nxt[e]
    return seen


def production(name, namespace):
    tree = ast.parse(SCRIPT.read_text(encoding="utf-8"), str(SCRIPT))
    node = next(x for x in tree.body if isinstance(x, ast.FunctionDef) and x.name == name)
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(SCRIPT), "exec"), namespace)
    return namespace[name]


def test_gc_threshold_restore():
    """The tuning is bounded to a solve, even after a failure."""
    import gc
    from unittest.mock import patch
    source = production("_solve_gc_tuned", {"gc": gc, "solve": lambda *a, **k: "ok"})
    original = gc.get_threshold()
    assert source("layout", None, None) == "ok"
    assert gc.get_threshold() == original
    observed = []

    def observe(*a, **k):
        observed.append(gc.get_threshold())
        raise RuntimeError("synthetic solver failure")

    broken = production("_solve_gc_tuned", {"gc": gc, "solve": observe})
    try:
        broken("layout", None, None)
    except RuntimeError as exc:
        assert str(exc) == "synthetic solver failure"
    else:
        raise AssertionError("expected synthetic failure")
    assert observed and observed[0][0] >= 3000
    assert gc.get_threshold() == original


def main():
    test_gc_threshold_restore()
    rng = random.Random(0x303702)
    cut = production("_source_side", {})
    # Include graphs with zero edges, cycles, repeated edges, and tiny flows.
    capacities = (0.0, 1e-12, 0.5e-9, 1e-9, 1.5e-9, 1e-6,
                  0.1, 0.5, 1.0, 2.0, 3.3, 10.0, 1000.0)
    for i in range(2000):
        n = rng.randrange(2, 38)
        arcs = [(rng.randrange(n), rng.randrange(n), rng.choice(capacities))
                for _ in range(rng.randrange(0, n * 5))]
        # Parallel arcs and a direct source-sink edge stress residual order.
        if i % 4 == 0:
            arcs += [(0, n - 1, 1.0), (0, n - 1, 2.0),
                     (n - 1, 0, 0.25)]
        want = reference_source_side(n, arcs, 0, n - 1)
        got = cut(n, arcs, 0, n - 1)
        assert got == want, ("residual min-cut", i, n, arcs, want, got)

    # Test the actual convex-label caller with both implementations; this
    # compares the chosen integer labels, not just reachable graph vertices.
    script = ast.parse(SCRIPT.read_text(encoding="utf-8"), str(SCRIPT))
    node = next(x for x in script.body if isinstance(x, ast.FunctionDef) and x.name == "_convex_labels")
    source = compile(ast.Module(body=[node], type_ignores=[]), str(SCRIPT), "exec")
    old_ns = {"_source_side": reference_source_side}
    new_ns = {"_source_side": cut}
    exec(source, old_ns)
    exec(source, new_ns)
    weights = (None, 0.0, 0.1, 1.0, 3.0, 11.0)
    for i in range(160):
        n, k = rng.randrange(1, 16), rng.randrange(2, 9)
        unary = [[rng.uniform(0, 25) for _ in range(k)] for _ in range(n)]
        hinges = [(rng.randrange(n), rng.randrange(n), rng.randrange(-2, 4),
                   rng.choice(weights)) for _ in range(rng.randrange(n * 3))]
        want = old_ns["_convex_labels"](k, unary, hinges)
        got = new_ns["_convex_labels"](k, unary, hinges)
        assert got == want, ("convex labels", i, k, hinges, want, got)
    print("PASS exact mincut reachability (2000 graphs), convex labels (160 problems), GC threshold restoration")


if __name__ == "__main__":
    main()
