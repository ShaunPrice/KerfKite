import copy
import math
import struct
import unittest
from collections import Counter

from studio.solid_cad import binary_stl, horizontal_section, rebuild


def plate():
    return {'version': 1, 'parameters': {'width': '2 * half', 'half': 20, 'thickness': 6}, 'features': [
        {'id': 'base', 'kind': 'box', 'size': ['width', 30, 'thickness']},
        {'id': 'bore', 'kind': 'cylinder', 'op': 'cut', 'radius': 4, 'height': 'thickness + 2',
         'origin': ['half', 15, -1]},
    ]}


class SolidCadTests(unittest.TestCase):
    def test_forward_reference_changes_drive_boolean_mesh_and_export(self):
        model = plate()
        original = copy.deepcopy(model)
        part = rebuild(model)
        self.assertEqual(original, model)
        self.assertEqual([0, 0, 0, 40, 30, 6], part['bounds'])
        self.assertAlmostEqual(40*30*6 - math.pi*16*6, part['volume'], delta=.3)
        self.assertEqual(len(part['profiles']), 2)
        self.assertTrue(all(ring[0] == ring[-1] for ring in part['profiles']))
        edges = Counter(tuple(sorted((tri[i-1], tri[i]))) for tri in part['triangles'] for i in range(3))
        self.assertEqual(set(edges.values()), {2})
        stl = binary_stl(part)
        self.assertEqual(len(part['triangles']), struct.unpack_from('<I', stl, 80)[0])
        self.assertEqual(84 + len(part['triangles'])*50, len(stl))
        model['parameters']['half'] = 25
        updated = rebuild(model)
        self.assertEqual(updated['bounds'][3], 50)
        self.assertAlmostEqual(updated['volume'] - part['volume'], 1800)

    def test_sketch_extrusion_transform_and_feature_suppression(self):
        model = {'version': 1, 'features': [
            {'id': 'profile', 'kind': 'extrude', 'points': [[0,0],[0,3],[4,0]],
             'height': 5, 'rotation': [0,0,90], 'origin': [10,2,1]},
            {'id': 'suppressed', 'enabled': False, 'kind': 'sphere', 'radius': 20}]}
        part = rebuild(model)
        self.assertEqual(part['bounds'], [7,2,1,10,6,6])
        self.assertAlmostEqual(part['volume'], 30)

    def test_projection_omits_blind_pocket(self):
        model = plate()
        model['features'][1]['origin'][2] = 2
        self.assertEqual(len(rebuild(model)['profiles']), 1)

    def test_revolve_sector_sphere_frustum_and_intersection(self):
        model = {'version': 1, 'features': [
            {'id':'lathe','kind':'revolve','points':[[0,0],[4,0],[4,10],[0,10]],'angle':90}]}
        self.assertAlmostEqual(rebuild(model)['volume'], math.pi*40, delta=.12)
        model['features'] = [{'id':'ball','kind':'sphere','radius':10},
                             {'id':'clamp','kind':'box','size':[20,20,10],'origin':[-10,-10,0], 'op':'intersect'}]
        self.assertAlmostEqual(rebuild(model)['volume'], 2000*math.pi/3, delta=10)
        model['features'] = [{'id':'cone','kind':'cylinder','radius':3,'topRadius':0,'height':10}]
        self.assertAlmostEqual(rebuild(model)['volume'], 30*math.pi, delta=.1)

    def test_bad_histories_and_expressions_fail_atomically(self):
        invalid = [dict(parameters={'width':'width+1'}),dict(parameters={'width':'missing'}),
                   dict(parameters={'n':'1/0'}),dict(parameters={'n':'2**5'}),
                   dict(parameters={'n':'__import__("sys")'}),dict(parameters={'n':True}),
                   dict(parameters={'n':float('nan')}),dict(parameters={'n':'100000*100000'}),
                   dict(features=[{'id':'a','op':'cut','kind':'box','size':[2,2,2]}]),
                   dict(features=[{'id':'a','kind':'extrude','height':1,'points':[[0,0],[1,1],[0,1],[1,0]]}]),
                   dict(features=[{'id':'a','kind':'box','size':[0,2,2]}]),
                   dict(features=plate()['features']*2),dict(version=True)]
        for change in invalid:
            doc = {**plate(), **change}
            with self.subTest(change=change), self.assertRaises(ValueError):
                rebuild(doc)

    def test_sections_reveal_blind_pocket_only_above_its_floor(self):
        model = plate()
        model['features'][1]['origin'][2] = 2
        self.assertEqual(len(horizontal_section(model, 0)['profiles']), 1)
        section = horizontal_section(model, 'thickness / 2')
        self.assertEqual(section['z'], 3)
        self.assertEqual(len(section['profiles']), 2)
        self.assertTrue(all(r[0] == r[-1] for r in section['profiles']))
        for outside in [-1, 6]:
            with self.assertRaises(ValueError):
                horizontal_section(model, outside)

    def test_parameter_driven_linear_and_polar_cut_patterns(self):
        model = plate()
        bore = model['features'][1]
        bore['origin'] = [6,15,-1]
        bore['radius'] = 1
        bore['pattern'] = {'kind':'linear','count':'width / 10','step':[8,0,0]}
        self.assertEqual(len(rebuild(model)['profiles']), 5)
        bore['origin'] = [25,15,-1]
        bore['pattern'] = {'kind':'polar','count':4,'angle':90,'center':['half',15,0]}
        polar = rebuild(model)
        self.assertEqual(len(polar['profiles']), 5)
        self.assertAlmostEqual(polar['volume'], 7200-24*math.pi, delta=.1)
        bore['pattern']['count'] = 1.5
        with self.assertRaises(ValueError):
            rebuild(model)
