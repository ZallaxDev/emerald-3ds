#!/usr/bin/env python3
"""Asset-free regression for release-compatible relief solver rounding.

Load only the solver's AST: importing the generator requires private assets.
Run directly with Python 3.11 and Python 3.12+ (no third-party dependencies).
"""
import ast
from pathlib import Path
import unittest


def load_solver():
    script = Path(__file__).resolve().parents[1] / 'scripts' / 'gen_voxel_relief.py'
    tree = ast.parse(script.read_text(encoding='utf-8'))
    node = next(n for n in tree.body if isinstance(n, ast.FunctionDef)
                and n.name == '_gauss_seidel')
    namespace = {}
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(script), 'exec'), namespace)
    return namespace['_gauss_seidel']


solve = load_solver()


class ReliefRoundingTests(unittest.TestCase):
    def test_numerator_keeps_left_to_right_rounding(self):
        pairs = {(0, 1): (0, 1), (0, 2): (0, 1), (0, 3): (0, 1)}
        actual = solve(4, pairs, {1: 1e16, 2: 1.0, 3: -1e16}, iterations=1)
        self.assertEqual(actual[0], 0.0)

    def test_denominator_keeps_left_to_right_rounding(self):
        pairs = {(0, i): (0, 0.01) for i in range(1, 11)}
        fixed = {i: float(i == 1) for i in range(1, 11)}
        weight = 0
        for _ in range(10):
            weight += 0.01
        self.assertEqual(solve(11, pairs, fixed, iterations=1)[0], 0.01 / weight)

    def test_convergence_and_fixed_nodes(self):
        actual = solve(3, {(0, 1): (16, 1), (1, 2): (16, 1)}, {0: 0})
        self.assertEqual(actual[0], 0)
        self.assertAlmostEqual(actual[1], -16, delta=0.001)
        self.assertAlmostEqual(actual[2], -32, delta=0.001)

    def test_isolated_nodes(self):
        self.assertEqual(solve(3, {}, {1: 2.5}), [0.0, 2.5, 0.0])


if __name__ == '__main__':
    unittest.main()
