import importlib.util
import json
import math
import struct
import unittest
from collections import Counter

from studio.cad_exchange import inspect_step, write_step
from studio.solid_cad import binary_stl, horizontal_section, rebuild


def transition(top='circle', base='rectangle'):
    return {
        'version': 1,
        'parameters': {'outlet': 18},
        'features': [
            {
                'id': 'inlet',
                'kind': 'loft',
                'baseShape': base,
                'baseWidth': 40,
                'baseDepth': 24,
                'topShape': top,
                'topWidth': 'outlet',
                'topDepth': 12,
                'height': 20,
                'offset': [4, 0],
            }
        ],
    }


class LoftTests(unittest.TestCase):
    def test_ruled_section_dimension_change_and_stl_watertightness(self):
        model = transition()
        for diameter in (18, 22):
            model = json.loads(json.dumps(model))
            model['parameters']['outlet'] = diameter
            rings = horizontal_section(model, 10)['profiles']
            self.assertEqual(len(rings), 1)
            x, y = zip(*rings[0])
            self.assertAlmostEqual(max(x) - min(x), (40 + diameter) / 2)
            self.assertAlmostEqual(max(y) - min(y), (24 + diameter) / 2)
            self.assertAlmostEqual((max(x) + min(x)) / 2, 2)
            mesh = rebuild(model)
            edges = Counter(tuple(sorted((t[i - 1], t[i]))) for t in mesh['triangles'] for i in range(3))
            self.assertEqual(set(edges.values()), {2})
            self.assertEqual(struct.unpack_from('<I', binary_stl(mesh), 80)[0], len(mesh['triangles']))

    def test_planar_transitions_rotations_and_booleans(self):
        model = transition('rectangle')
        expected = 20 * (40 * 24 + 4 * 29 * 18 + 18 * 12) / 6
        self.assertAlmostEqual(rebuild(model)['volume'], expected)
        model['features'].append(
            {'id': 'bore', 'kind': 'cylinder', 'op': 'cut', 'radius': 2, 'height': 22, 'origin': [0, 0, -1]}
        )
        self.assertAlmostEqual(rebuild(model)['volume'], expected - math.pi * 4 * 20, delta=0.6)
        model['features'][1]['op'] = 'intersect'
        self.assertAlmostEqual(rebuild(model)['volume'], math.pi * 4 * 20, delta=0.6)
        model['features'].pop()
        model['features'][0].update(rotation=[0, 0, 90], origin=[10, 20, 30])
        self.assertEqual(rebuild(model)['bounds'], [-2, 0, 30, 22, 40, 50])

    def test_invalid_dimension_shape_or_excessive_tessellation(self):
        for fields in (
            {'topShape': 'triangle'},
            {'baseWidth': -1},
            {'offset': [2]},
            {'height': 0},
            {'baseWidth': 100_000, 'baseDepth': 50_000, 'topWidth': 99_000},
        ):
            model = transition()
            model['features'][0].update(fields)
            with self.assertRaises(ValueError):
                rebuild(model)

    @unittest.skipUnless(importlib.util.find_spec('OCP'), 'requires optional STEP runtime')
    def test_exact_loft_step_matches_elliptical_volume_and_mixed_section(self):
        ellipse = transition('ellipse', 'ellipse')
        analytic = math.pi / 4 * 20 * (40 * 24 + 4 * 29 * 18 + 18 * 12) / 6
        exact = inspect_step(write_step(model=ellipse))
        self.assertEqual(exact['solids'], 1)
        self.assertAlmostEqual(exact['volume'], analytic, delta=0.001)
        mixed = transition()
        raw = write_step(model=mixed)
        self.assertIn(b'RATIONAL_B_SPLINE_SURFACE', raw)
        exact = inspect_step(raw)
        self.assertEqual(exact['solids'], 1)
        self.assertAlmostEqual(exact['volume'], rebuild(mixed)['volume'], delta=6)

        rectangular = transition('rectangle')
        volume = inspect_step(write_step(model=rectangular))['volume']
        rectangular['features'].append(
            {'id': 'bore', 'kind': 'cylinder', 'op': 'cut', 'radius': 2, 'height': 22, 'origin': [0, 0, -1]}
        )
        bored = inspect_step(write_step(model=rectangular))
        self.assertEqual(bored['solids'], 1)
        self.assertAlmostEqual(bored['volume'], volume - 20 * math.pi * 4, delta=0.002)
