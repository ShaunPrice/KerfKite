import copy
import math
import struct
import unittest
from collections import Counter
from unittest import mock

from studio import solid_cad
from studio.solid_cad import binary_stl, horizontal_section, model_stl, rebuild


def plate():
    return {
        'version': 1,
        'parameters': {'width': '2 * half', 'half': 20, 'thickness': 6},
        'features': [
            {'id': 'base', 'kind': 'box', 'size': ['width', 30, 'thickness']},
            {
                'id': 'bore',
                'kind': 'cylinder',
                'op': 'cut',
                'radius': 4,
                'height': 'thickness + 2',
                'origin': ['half', 15, -1],
            },
        ],
    }


def bored(count, *extra):
    """The plate's base with `count` separate bores cut by features of their own, then `extra` features."""
    bores = [
        {'id': f'bore{i}', 'kind': 'cylinder', 'op': 'cut', 'radius': 1.5, 'height': 8, 'origin': [5 + 7 * i, 15, -1]}
        for i in range(count)
    ]
    return {**plate(), 'features': [plate()['features'][0], *bores, *extra]}


def one_by_one(solid, run, kernel):
    """Each feature of a run on its own, as the history was built before runs were applied together."""
    for _, op, tool in run:
        solid = solid_cad._checked(solid_cad._one_step(solid, op, tool), kernel)
    return solid


class SolidCadTests(unittest.TestCase):
    def test_forward_reference_changes_drive_boolean_mesh_and_export(self):
        model = plate()
        original = copy.deepcopy(model)
        part = rebuild(model)
        self.assertEqual(original, model)
        self.assertEqual([0, 0, 0, 40, 30, 6], part['bounds'])
        self.assertAlmostEqual(40 * 30 * 6 - math.pi * 16 * 6, part['volume'], delta=0.3)
        self.assertEqual(len(part['profiles']), 2)
        self.assertTrue(all(ring[0] == ring[-1] for ring in part['profiles']))
        edges = Counter(tuple(sorted((tri[i - 1], tri[i]))) for tri in part['triangles'] for i in range(3))
        self.assertEqual(set(edges.values()), {2})
        stl = binary_stl(part)
        self.assertEqual(len(part['triangles']), struct.unpack_from('<I', stl, 80)[0])
        self.assertEqual(84 + len(part['triangles']) * 50, len(stl))
        model['parameters']['half'] = 25
        updated = rebuild(model)
        self.assertEqual(updated['bounds'][3], 50)
        self.assertAlmostEqual(updated['volume'] - part['volume'], 1800)

    def test_direct_model_export_preserves_facet_order_normals_and_coordinates(self):
        # A sphere spans several batches of the STL writer.
        model = {'version': 1, 'features': [{'id': 'ball', 'kind': 'sphere', 'radius': 4}]}
        part = rebuild(model)
        self.assertGreater(len(part['triangles']), 2048)
        self.assertEqual(model_stl(model), binary_stl(part))

    def test_sketch_extrusion_transform_and_feature_suppression(self):
        model = {
            'version': 1,
            'features': [
                {
                    'id': 'profile',
                    'kind': 'extrude',
                    'points': [[0, 0], [0, 3], [4, 0]],
                    'height': 5,
                    'rotation': [0, 0, 90],
                    'origin': [10, 2, 1],
                },
                {'id': 'suppressed', 'enabled': False, 'kind': 'sphere', 'radius': 20},
            ],
        }
        part = rebuild(model)
        self.assertEqual(part['bounds'], [7, 2, 1, 10, 6, 6])
        self.assertAlmostEqual(part['volume'], 30)

    def test_projection_omits_blind_pocket(self):
        model = plate()
        model['features'][1]['origin'][2] = 2
        self.assertEqual(len(rebuild(model)['profiles']), 1)

    def test_revolve_sector_sphere_frustum_and_intersection(self):
        model = {
            'version': 1,
            'features': [{'id': 'lathe', 'kind': 'revolve', 'points': [[0, 0], [4, 0], [4, 10], [0, 10]], 'angle': 90}],
        }
        self.assertAlmostEqual(rebuild(model)['volume'], math.pi * 40, delta=0.12)
        model['features'] = [
            {'id': 'ball', 'kind': 'sphere', 'radius': 10},
            {'id': 'clamp', 'kind': 'box', 'size': [20, 20, 10], 'origin': [-10, -10, 0], 'op': 'intersect'},
        ]
        self.assertAlmostEqual(rebuild(model)['volume'], 2000 * math.pi / 3, delta=10)
        model['features'] = [{'id': 'cone', 'kind': 'cylinder', 'radius': 3, 'topRadius': 0, 'height': 10}]
        self.assertAlmostEqual(rebuild(model)['volume'], 30 * math.pi, delta=0.1)

    def test_runs_of_one_operation_give_the_solid_of_feature_after_feature(self):
        boss = {'id': 'boss', 'kind': 'cylinder', 'radius': 3, 'height': 4, 'origin': [20, 5, 5]}
        cap = {'id': 'cap', 'kind': 'box', 'size': [6, 6, 2], 'origin': [17, 2, 8]}
        trim = {'id': 'trim', 'kind': 'box', 'op': 'intersect', 'size': [38, 28, 9], 'origin': [1, 1, -1]}
        model = bored(5, boss, cap, trim)
        together = rebuild(model)
        with mock.patch.object(solid_cad, '_apply_run', one_by_one):
            apart = rebuild(model)
            apart_solid = solid_cad._build(model)[0]
        self.assertEqual(together['bounds'], apart['bounds'])
        self.assertAlmostEqual(together['volume'], apart['volume'], delta=1e-9 * apart['volume'])
        self.assertEqual(len(together['profiles']), len(apart['profiles']))
        # The same set of points: nothing of substance is in one and not in the other (the kernel finds nothing;
        # a floating-point library may keep slivers of no measurable volume).
        together_solid = solid_cad._build(model)[0]
        self.assertLess((together_solid - apart_solid).volume(), 1e-6)
        self.assertLess((apart_solid - together_solid).volume(), 1e-6)
        # The trim keeps the plate within 1..39 x 1..29 and the boss below 8 mm; the cap lies above it.
        expected = 38 * 28 * 6 - 5 * math.pi * 1.5**2 * 6 + math.pi * 3**2 * 2
        self.assertAlmostEqual(together['volume'], expected, delta=0.5)

    def test_a_run_names_the_feature_that_fails_and_errors_keep_their_order(self):
        everything = {'id': 'everything', 'kind': 'box', 'op': 'cut', 'size': [60, 50, 20], 'origin': [-5, -5, -5]}
        bad = {'id': 'bad', 'kind': 'box', 'op': 'cut', 'size': [0, 2, 2]}
        for model, named in (
            (bored(3, everything), 'everything: The operation produced an empty or invalid solid'),
            (bored(2, everything, bad), 'everything: The operation produced an empty or invalid solid'),
            (bored(3, bad), 'bad: '),
        ):
            with self.subTest(named=named), self.assertRaises(ValueError) as caught:
                rebuild(model)
            self.assertTrue(str(caught.exception).startswith(named), str(caught.exception))
        model = bored(3)
        model['features'][2]['enabled'] = False
        self.assertEqual(len(rebuild(model)['profiles']), 3)

    def test_bad_histories_and_expressions_fail_atomically(self):
        invalid = [
            dict(parameters={'width': 'width+1'}),
            dict(parameters={'width': 'missing'}),
            dict(parameters={'n': '1/0'}),
            dict(parameters={'n': '2**5'}),
            dict(parameters={'n': '__import__("sys")'}),
            dict(parameters={'n': True}),
            dict(parameters={'n': float('nan')}),
            dict(parameters={'n': '100000*100000'}),
            dict(features=[{'id': 'a', 'op': 'cut', 'kind': 'box', 'size': [2, 2, 2]}]),
            dict(features=[{'id': 'a', 'kind': 'extrude', 'height': 1, 'points': [[0, 0], [1, 1], [0, 1], [1, 0]]}]),
            dict(features=[{'id': 'a', 'kind': 'box', 'size': [0, 2, 2]}]),
            dict(features=plate()['features'] * 2),
            dict(version=True),
        ]
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
        bore['origin'] = [6, 15, -1]
        bore['radius'] = 1
        bore['pattern'] = {'kind': 'linear', 'count': 'width / 10', 'step': [8, 0, 0]}
        self.assertEqual(len(rebuild(model)['profiles']), 5)
        bore['origin'] = [25, 15, -1]
        bore['pattern'] = {'kind': 'polar', 'count': 4, 'angle': 90, 'center': ['half', 15, 0]}
        polar = rebuild(model)
        self.assertEqual(len(polar['profiles']), 5)
        self.assertAlmostEqual(polar['volume'], 7200 - 24 * math.pi, delta=0.1)
        bore['pattern']['count'] = 1.5
        with self.assertRaises(ValueError):
            rebuild(model)
