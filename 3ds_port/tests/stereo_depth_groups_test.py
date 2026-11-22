#!/usr/bin/env python3
import itertools
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from stereo_depth_groups import cluster_depths, smart_pixels, pack, unpack, clean_fragments


def fixture():
    # Both poses carry the same head with noisy source depth, and a far tail.
    pixels = [15]*8192
    frames = []
    for f in range(2):
        pixels[f*4096:f*4096+4] = [1, 4, 8, 2]
        rows = [[] for _ in range(64)]
        rows[0] = [[0,3,1],[3,4,2]]
        frames.append({'rows': rows})
    doc = {'version':1, 'size':[64,64], 'parts':[
        {'id':1,'name':'whole head, including eyes','depth':.8},
        {'id':2,'name':'tail','depth':.2}], 'frames':frames}
    return pack(pixels), doc


class SmartTest(unittest.TestCase):
    def test_coherence_across_pixels_and_poses(self):
        src, doc = fixture()
        result, report = smart_pixels(src, 10, 4, doc)
        pixels = unpack(result)
        for f in range(2):
            self.assertEqual(pixels[f*4096:f*4096+4], [3,3,3,0])
        self.assertEqual([p==15 for p in unpack(src)], [p==15 for p in pixels])
        self.assertTrue(report['semantic_cleanup'])
        self.assertEqual(report['parts'][0]['source_levels'], 3)

    def test_same_surface_and_equal_depth(self):
        src, doc = fixture()
        for p in doc['parts']: p['collapse_group'] = 'one surface'
        result, report = smart_pixels(src, 10, 4, doc)
        self.assertEqual(set(unpack(result)) - {15}, {2})
        self.assertEqual(report['groups_used'], 1)
        mapping, _ = cluster_depths([(1,.4,1),(2,.4,30),(3,.8,1)], 4)
        self.assertEqual(mapping[1],mapping[2])
        self.assertLess(mapping[1],mapping[3])

    def test_closest_groups_and_global_optimum(self):
        items = [(i,d,w) for i,(d,w) in enumerate(zip([.1,.12,.4,.42,.7,.95],[1,3,1,2,1,1]))]
        m, _ = cluster_depths(items, 4)
        self.assertEqual([m[i] for i in range(6)], [0,0,1,1,2,3])
        def cost(groups):
            total = 0
            for group in groups:
                mean=sum(d*w for _,d,w in group)/sum(w for _,_,w in group)
                total+=sum(w*(d-mean)**2 for _,d,w in group)
            return total
        achieved=cost([[item for item in items if m[item[0]]==g] for g in range(4)])
        for cuts in itertools.combinations(range(1,6),3):
            bounds=(0,)+cuts+(6,)
            self.assertLessEqual(achieved, cost([items[a:b] for a,b in zip(bounds,bounds[1:])])+1e-12)

    def test_fallback_is_not_semantic_and_preserves_sparse_depth(self):
        data = pack([3,4]+[15]*4094)
        result, report = smart_pixels(data,10,4)
        self.assertFalse(report['semantic_cleanup'])
        self.assertEqual(unpack(result)[:2],[0,3])

    def test_fragment_cleanup_preserves_art_and_large_or_detached_parts(self):
        pixels = [15]*8192
        for f in range(2):
            for y in range(8,16):
                for x in range(8,16): pixels[f*4096+y*64+x] = 1
        pixels[10*64+10] = 2  # Shallow island inside a body.
        pixels[12*64+12] = 3  # Two depth steps: do not guess.
        pixels[30*64+30] = 2  # Detached detail: do not bridge transparency.
        pixels[4096+10*64+10] = 0
        result, changes = clean_fragments(pixels)
        self.assertEqual(result[10*64+10],1)
        self.assertEqual(result[4096+10*64+10],1)
        self.assertEqual(result[12*64+12],3)
        self.assertEqual(result[30*64+30],2)
        self.assertEqual([p==15 for p in pixels],[p==15 for p in result])
        self.assertEqual(sum(len(c['pixels']) for c in changes),2)
        self.assertEqual(pixels[10*64+10],2)  # Input untouched.

    def test_cleanup_does_not_override_authored_parts(self):
        src,doc=fixture()
        self.assertEqual(smart_pixels(src,10,4,doc),smart_pixels(src,10,4,doc,cleanup=True))
        result,report=smart_pixels(src,10,4,cleanup=True)
        self.assertFalse(report['semantic_cleanup'])
        self.assertTrue(report['review_required'])
        self.assertEqual(report['method'],'automatic-spatial-draft')

    def test_bad_masks_fail_instead_of_silently_reusing_noise(self):
        for bad in ['gap','overlap','background','unused','frames','depth']:
            src,doc=fixture()
            if bad=='gap':doc['frames'][0]['rows'][0]=[[0,2,1],[3,4,2]]
            if bad=='overlap':doc['frames'][0]['rows'][0].append([0,1,2])
            if bad=='background':doc['frames'][0]['rows'][1]=[[0,1,2]]
            if bad=='unused':doc['parts'].append({'id':3,'name':'missing','depth':.5})
            if bad=='frames':doc['frames'].pop()
            if bad=='depth':doc['parts'][0]['depth']=float('nan')
            with self.subTest(bad=bad), self.assertRaises(ValueError):smart_pixels(src,10,4,doc)

if __name__=='__main__':unittest.main()
