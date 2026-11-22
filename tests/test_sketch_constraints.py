"""Exercise the shared geometric solution used by native solids and STEP."""

import copy
import importlib.util
import json
import math
import unittest

from studio.sketch_constraints import solve_profile
from studio.solid_cad import Dimensions, rebuild


def rectangle():
    return {
        'id': 'plate',
        'kind': 'extrude',
        'height': 2,
        'points': [[0, 0], [20, 0], [19, 11], [-1, 9]],
        'constraints': [
            {'kind': 'fixed', 'point': 0, 'value': [0, 0]},
            {'kind': 'fixed', 'point': 1, 'value': ['w', 0]},
            {'kind': 'distance', 'points': [1, 2], 'value': 'h'},
            {'kind': 'perpendicular', 'edges': [0, 1]},
            {'kind': 'parallel', 'edges': [0, 2]},
            {'kind': 'equal', 'edges': [0, 2]},
        ],
    }


try:
    from studio import _relief_native as native
except ImportError:
    native = None


class SketchConstraintTests(unittest.TestCase):
    def test_dimension_driven_fully_constrained_rectangle(self):
        f = rectangle()
        original = copy.deepcopy(f)
        for w, h in ((20, 10), (32, 12)):
            d = Dimensions({'w': w, 'h': h})
            points, report = solve_profile(f, d)
            self.assertEqual((report['dof'], report['redundant']), (0, 0))
            for actual, expected in zip(points, [(0, 0), (w, 0), (w, h), (0, h)]):
                self.assertLess(math.dist(actual, expected), 1e-7)
            doc = {'version': 1, 'parameters': d.definitions, 'features': [f]}
            self.assertAlmostEqual(rebuild(json.loads(json.dumps(doc)))['volume'], 2 * w * h, places=5)
        self.assertEqual(f, original)

    def test_conflict_redundancy_and_remaining_degrees(self):
        f = rectangle()
        d = Dimensions({'w': 20, 'h': 10})
        f['constraints'].append({'kind': 'distance', 'points': [1, 2], 'value': 10})
        self.assertEqual(solve_profile(f, d)[1]['redundant'], 1)
        f['constraints'][-1]['value'] = 8
        with self.assertRaisesRegex(ValueError, 'Cannot satisfy sketch constraint'):
            solve_profile(f, d)
        f['constraints'] = [{'kind': 'fixed', 'point': 0, 'value': [0, 0]}]
        self.assertEqual(solve_profile(f, d)[1]['dof'], 6)
        f['points'][0][0] = 'w'
        with self.assertRaisesRegex(ValueError, 'contradicts another dimension'):
            solve_profile(f, d)

    def test_directed_angle_and_coincident_or_invalid_indices(self):
        f = rectangle()
        f['constraints'][3] = {'kind': 'angle', 'edges': [0, 1], 'value': 'angle'}
        for angle in (60, 135, -30):
            d = Dimensions({'w': 20, 'h': 10, 'angle': angle})
            points, result = solve_profile(f, d)
            self.assertEqual(result['dof'], 0)
            self.assertLess(
                math.dist(points[2], [20 + 10 * math.cos(math.radians(angle)), 10 * math.sin(math.radians(angle))]),
                1e-7,
            )
        for faulty in (
            {'kind': 'distance', 'points': [1, 1], 'value': 1},
            {'kind': 'parallel', 'edges': [-1, 2]},
            {'kind': 'equal', 'edges': [1, 9]},
        ):
            f['constraints'] = [faulty]
            with self.assertRaises(ValueError):
                solve_profile(f, d)

    @unittest.skipUnless(native is not None and hasattr(native, 'core_cam'), 'the compiled extension is not installed')
    def test_the_core_solver_agrees_with_the_numpy_reference_to_rounding(self):
        f = rectangle()
        cases = [({'w': 20, 'h': 10}, f['constraints'])]
        angled = copy.deepcopy(f['constraints'])
        angled[3] = {'kind': 'angle', 'edges': [0, 1], 'value': 'angle'}
        cases += [({'w': 20, 'h': 10, 'angle': angle}, angled) for angle in (60, 135, -30)]
        loose = [{'kind': 'fixed', 'point': 0, 'value': [0, 0]}, {'kind': 'equal', 'edges': [0, 1]}]
        cases.append(({'w': 20, 'h': 10}, loose))
        for parameters, constraints in cases:
            with self.subTest(parameters=parameters, constraints=constraints):
                feature = dict(f, constraints=constraints)
                core = solve_profile(feature, Dimensions(parameters), backend='native')
                reference = solve_profile(feature, Dimensions(parameters), backend='python')
                self.assertEqual(core[1], reference[1])
                for a, b in zip(core[0], reference[0], strict=True):
                    self.assertLess(math.dist(a, b), 1e-9)
        refused = dict(f, constraints=[*f['constraints'], {'kind': 'distance', 'points': [1, 2], 'value': 8}])
        for backend in ('native', 'python'):
            with self.assertRaisesRegex(ValueError, 'Cannot satisfy sketch constraint'):
                solve_profile(refused, Dimensions({'w': 20, 'h': 10}), backend=backend)
        with self.assertRaises(NotImplementedError):
            solve_profile(dict(f, constraints=[]), Dimensions({'w': 1, 'h': 1}), backend='native')

    @unittest.skipUnless(importlib.util.find_spec('OCP'), 'requires optional STEP runtime')
    def test_exact_step_follows_solved_constraints_and_fillets(self):
        from studio.cad_exchange import inspect_step, write_step

        f = rectangle()
        f['fillets'] = ['r'] * 4
        doc = {'version': 1, 'parameters': {'w': 24, 'h': 13, 'r': 2}, 'features': [f]}
        result = inspect_step(write_step(model=doc))
        self.assertAlmostEqual(result['volume'], (24 * 13 - (4 - math.pi) * 4) * 2, places=6)
