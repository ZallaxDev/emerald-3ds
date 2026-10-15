#!/usr/bin/env python3
"""Semantic export regression tests; run with python3 this_file.py."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    'stereo', Path(__file__).resolve().parents[1] / 'scripts/gen_stereo_mons.py')
stereo = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stereo)


class AnatomyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'anatomy.json'
        self.frames = [[None] * 4096 for _ in range(2)]
        self.key = [-1] * 4096
        # Same colour on unrelated anatomy, and an eye/outline of another colour.
        for x, colour, depth in [(5, (2, 2, 2), 2), (6, (0, 0, 0), 2), (30, (2, 2, 2), 8)]:
            self.frames[0][10 * 64 + x] = colour
            self.key[10 * 64 + x] = depth
        # Limb moves far outside the old nearest-colour search and swaps sides.
        for x, colour in [(40, (2, 2, 2)), (41, (0, 0, 0)), (4, (2, 2, 2))]:
            self.frames[1][10 * 64 + x] = colour
        rows0, rows1 = [[] for _ in range(64)], [[] for _ in range(64)]
        rows0[10] = [[5, 7, 1], [30, 31, 2]]
        rows1[10] = [[40, 42, 1], [4, 5, 2]]
        self.doc = {'version': 1, 'size': [64, 64],
                    'parts': [{'id': 1, 'name': 'head'}, {'id': 2, 'name': 'blade'}],
                    'frames': [{'rows': rows0}, {'rows': rows1}]}

    def run_map(self, doc=None, **kwargs):
        self.path.write_text(json.dumps(self.doc if doc is None else doc))
        return stereo.anatomy_planes(self.path, self.frames, self.key, 10, **kwargs)

    def test_moving_anatomy_ignores_shared_colours_and_owns_eye(self):
        original = copy.deepcopy(self.frames)
        planes = self.run_map()
        self.assertEqual([planes[1][640 + x] for x in (40, 41, 4)], [2, 2, 8])
        self.assertEqual(self.frames, original)
        for frame, depth in zip(self.frames, planes):
            self.assertEqual([p is None for p in frame], [p == -1 for p in depth])
            packed = stereo.pack(depth)
            unpacked = [(byte >> shift) & 15 for byte in packed for shift in (0, 4)]
            self.assertEqual(unpacked, [15 if p < 0 else p for p in depth])

    def test_explicit_depth_and_median_are_shared_across_poses(self):
        self.doc['parts'][1]['depth'] = 0.5
        self.key[646] = 4
        planes = self.run_map()
        self.assertEqual(planes[0][645], 3)
        self.assertEqual(planes[1][680], 3)
        self.assertEqual(planes[0][670], 5)
        self.assertEqual(planes[1][644], 5)

    def test_invalid_annotations_fail_loudly(self):
        def mutate_gap(d): d['frames'][1]['rows'][10].pop()
        def mutate_overlap(d): d['frames'][0]['rows'][10].append([5, 6, 1])
        def mutate_background(d): d['frames'][0]['rows'][0].append([0, 1, 1])
        def mutate_unknown(d): d['frames'][0]['rows'][10][0][2] = 99
        def mutate_duplicate(d): d['parts'][1]['id'] = 1
        def mutate_name(d): d['parts'][1]['name'] = 'head'
        def mutate_rows(d): d['frames'][0]['rows'].pop()
        for mutate in (mutate_gap, mutate_overlap, mutate_background, mutate_unknown,
                       mutate_duplicate, mutate_name, mutate_rows):
            with self.subTest(case=mutate.__name__):
                doc = copy.deepcopy(self.doc)
                mutate(doc)
                with self.assertRaisesRegex(ValueError, 'anatomy.json'):
                    self.run_map(doc)
        for depth in (-0.1, 1.1, float('nan'), float('inf'), True, '0.5', None):
            with self.subTest(depth=depth):
                doc = copy.deepcopy(self.doc)
                doc['parts'][0]['depth'] = depth
                with self.assertRaisesRegex(ValueError, 'depth'):
                    self.run_map(doc)

    def test_part_without_key_depth_needs_explicit_depth(self):
        self.key = [-1] * 4096
        with self.assertRaisesRegex(ValueError, 'no authored key-frame depth'):
            self.run_map()
        for part in self.doc['parts']:
            part['depth'] = 0.4
        self.assertEqual(self.run_map()[1][644], 4)

    def test_source_digest_rejects_stale_art(self):
        source = Path(self.temp.name) / 'source.png'
        source.write_bytes(b'original source')
        self.doc['source_sha256'] = hashlib.sha256(source.read_bytes()).hexdigest()
        self.run_map(source_path=source)
        source.write_bytes(b'changed source')
        with self.assertRaisesRegex(ValueError, 'stale anatomy'):
            self.run_map(source_path=source)

    def test_build_without_sidecar_retains_legacy_transfer(self):
        args = SimpleNamespace(work=self.temp.name, sprites=self.temp.name,
                               root=self.temp.name, planes=10, preview=None)
        expected = [[-1] * 4096, [-1] * 4096]
        expected[0][645], expected[1][644] = 7, 3
        with patch.object(stereo.os.path, 'exists', side_effect=lambda p: not str(p).endswith('anatomy.json')), \
             patch.object(stereo, 'gif_frame0', return_value=((64, 64), self.frames[0])), \
             patch.object(stereo, 'key_planes', return_value=self.key), \
             patch.object(stereo, 'decomp_frames', return_value=self.frames), \
             patch.object(stereo, 'align', return_value=(0, 0, 1)), \
             patch.object(stereo, 'propagate', side_effect=expected) as legacy, \
             patch.object(stereo, 'anatomy_planes') as semantic:
            data, note = stereo.build(1, 'bulbasaur', args)
        self.assertEqual(data, b''.join(stereo.pack(p) for p in expected))
        self.assertEqual(legacy.call_count, 2)
        semantic.assert_not_called()
        self.assertIn('legacy colour transfer', note)

    def test_build_dispatches_sidecar_and_checks_source_hash(self):
        args = SimpleNamespace(work=self.temp.name, sprites=self.temp.name,
                               root=self.temp.name, planes=10, preview=None)
        sidecar = Path(args.work) / '1' / 'anatomy.json'
        sidecar.parent.mkdir()
        source = Path(args.root) / 'graphics/pokemon/bulbasaur/anim_front.png'
        source.parent.mkdir(parents=True)
        source.write_bytes(b'original art')
        self.doc['source_sha256'] = hashlib.sha256(source.read_bytes()).hexdigest()
        sidecar.write_text(json.dumps(self.doc))
        with patch.object(stereo.os.path, 'exists', return_value=True), \
             patch.object(stereo, 'gif_frame0', return_value=((64, 64), self.frames[0])), \
             patch.object(stereo, 'key_planes', return_value=self.key), \
             patch.object(stereo, 'decomp_frames', return_value=self.frames), \
             patch.object(stereo, 'align', return_value=(0, 0, 1)), \
             patch.object(stereo, 'propagate') as legacy:
            data, note = stereo.build(1, 'bulbasaur', args)
            self.assertEqual(data, b''.join(stereo.pack(p) for p in self.run_map()))
            self.assertIn('anatomy masks', note)
            source.write_bytes(b'changed art')
            with self.assertRaisesRegex(ValueError, 'stale anatomy'):
                stereo.build(1, 'bulbasaur', args)
            legacy.assert_not_called()

    def test_annotated_missing_input_fails_instead_of_skipping(self):
        args = SimpleNamespace(work=self.temp.name, sprites=self.temp.name,
                               root=self.temp.name, planes=10, preview=None)
        with patch.object(stereo.os.path, 'exists', side_effect=lambda p: str(p).endswith('anatomy.json')):
            with self.assertRaisesRegex(ValueError, 'missing'):
                stereo.build(1, 'bulbasaur', args)

    def boundary_fixture(self):
        mask = [None] * 4096
        for x in range(8, 20):
            mask[20 * 64 + x] = 4
        for x in (8, 9, 10):
            mask[27 * 64 + x] = 4
            mask[28 * 64 + x] = 1
        return [mask], {4: 8, 1: 3}

    def test_boundary_audit_exact_regression_and_no_mutation(self):
        labels, depths = self.boundary_fixture()
        original = copy.deepcopy((labels, depths))
        self.assertEqual(stereo.boundary_fragments(labels, depths), [{
            'frame': 0, 'part_id': 4, 'pixels': [[8, 27], [9, 27], [10, 27]],
            'neighbor_ids': [1]}])
        self.assertEqual((labels, depths), original)

    def test_boundary_keeps_whole_small_parts_and_diagonal_links(self):
        labels, depths = self.boundary_fixture()
        for x in range(8, 20):
            labels[0][20 * 64 + x] = None
        self.assertEqual(stereo.boundary_fragments(labels, depths), [])
        labels, depths = self.boundary_fixture()
        # Only diagonal links are needed to connect this tip to its parent.
        for y in range(21, 27):
            labels[0][y * 64 + 8 + (y % 2)] = 4
        self.assertEqual(stereo.boundary_fragments(labels, depths), [])

    def test_boundary_requires_different_depth_and_cardinal_transparency(self):
        labels, depths = self.boundary_fixture()
        self.assertEqual(stereo.boundary_fragments(labels, {4: 3, 1: 3}), [])
        for y in (26, 27, 28):
            for x in range(7, 12):
                if labels[0][y * 64 + x] is None:
                    labels[0][y * 64 + x] = 1
        self.assertEqual(stereo.boundary_fragments(labels, depths), [])

    def test_boundary_canvas_edge_is_transparency_without_wrapping(self):
        labels, depths = self.boundary_fixture()
        mask = labels[0]
        for x in (8, 9, 10):
            mask[27 * 64 + x] = None
        mask[0] = 4
        mask[1] = mask[64] = 1
        found = next(f for f in stereo.boundary_fragments(labels, depths) if f['part_id'] == 4)
        self.assertEqual(found['pixels'], [[0, 0]])
        self.assertEqual(found['neighbor_ids'], [1])

    def test_boundary_exact_review_and_stale_duplicate_rejection(self):
        labels, depths = self.boundary_fixture()
        def fail(message):
            raise ValueError(message)
        exception = {'frame': 0, 'part_id': 4,
                     'pixels': [[8, 27], [9, 27], [10, 27]],
                     'reason': 'Reviewed intentionally disconnected visible tip'}
        with self.assertRaisesRegex(ValueError, 'unreviewed.*8, 27'):
            stereo.validate_boundary_fragments(labels, depths, [], fail)
        stereo.validate_boundary_fragments(labels, depths, [exception], fail)
        with self.assertRaisesRegex(ValueError, 'duplicate boundary'):
            stereo.validate_boundary_fragments(labels, depths, [exception, exception], fail)
        stale = copy.deepcopy(exception)
        stale['pixels'].pop()
        with self.assertRaisesRegex(ValueError, 'stale boundary'):
            stereo.validate_boundary_fragments(labels, depths, [stale], fail)
        # Fewer export planes can merge depths; review still matches geometry.
        stereo.validate_boundary_fragments(labels, {4: 3, 1: 3}, [exception], fail)
        changed = copy.deepcopy(labels)
        changed[0][27 * 64 + 8] = 1
        with self.assertRaisesRegex(ValueError, 'stale boundary'):
            stereo.validate_boundary_fragments(changed, depths, [exception], fail)

    def test_anatomy_export_enforces_boundary_guard(self):
        labels, depths = self.boundary_fixture()
        self.frames = [[None if pid is None else (2, 2, 2) for pid in labels[0]]] * 2
        rows = [[] for _ in range(64)]
        for i, pid in enumerate(labels[0]):
            if pid is not None:
                rows[i // 64].append([i % 64, i % 64 + 1, pid])
        doc = {'version': 1, 'size': [64, 64],
               'parts': [{'id': pid, 'name': str(pid), 'depth': d / 9}
                         for pid, d in depths.items()],
               'frames': [{'rows': rows}, {'rows': rows}]}
        with self.assertRaisesRegex(ValueError, 'anatomy.json.*unreviewed'):
            self.run_map(doc)


if __name__ == '__main__':
    unittest.main()
