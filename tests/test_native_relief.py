"""Native parity is exercised when the optional extension is built."""

import math
import random
import unittest
from unittest.mock import patch

from studio import relief


def sample(**changes):
    result = dict(
        heightmap=[[0.5] * 11 for _ in range(11)],
        width=10,
        height=10,
        depth=2,
        x=3,
        y=4,
        tool_diameter=2,
        tool_shape='ball',
        stepover=1,
    )
    result.update(changes)
    return result


class ReliefFallbackTests(unittest.TestCase):
    def test_python_fallback_and_explicit_backend(self):
        with patch.object(relief, '_relief_native', None):
            result = relief.prepare_relief(sample())
            self.assertEqual(result['surface']['backend'], 'python')
            with self.assertRaises(ValueError):
                relief.prepare_relief(sample(), backend='native')
        with self.assertRaises(ValueError):
            relief.prepare_relief(sample(), backend='unknown')

    def test_input_validation_is_shared(self):
        for backend in ('auto', 'python'):
            for changes in (
                {'stepover': 0},
                {'heightmap': [[True, 0], [0, 0]]},
                {'tool_diameter': float('nan')},
                {'allowance': 3},
            ):
                with self.subTest(backend=backend, changes=changes), self.assertRaises(ValueError):
                    relief.prepare_relief(sample(**changes), backend=backend)


@unittest.skipIf(relief._relief_native is None, 'Optional C++ extension is not installed')
class NativeReliefTests(unittest.TestCase):
    def test_full_path_and_depth_parity(self):
        rng = random.Random(4567)
        maps = [
            [[0.5] * 11 for _ in range(11)],
            [[i / 10 for i in range(11)] for _ in range(11)],
            [[1 - ((i - 5) ** 2 + (j - 5) ** 2) / 50 for i in range(11)] for j in range(11)],
            [[rng.random() for _ in range(17)] for _ in range(13)],
        ]
        maps.append([[0] * 11 for _ in range(11)])
        maps[-1][5][5] = 1
        for matrix in maps:
            for shape in ('flat', 'ball'):
                for allowance in (0, 0.17):
                    data = sample(heightmap=matrix, tool_shape=shape, allowance=allowance, stepover=0.73)
                    py = relief.prepare_relief(data, backend='python')
                    cpp = relief.prepare_relief(data, backend='native')
                    self.assertEqual(cpp['surface']['backend'], 'cpp')
                    self.assertEqual(cpp['paths'], py['paths'])
                    self.assertEqual(cpp['warnings'], py['warnings'])
                    for a, b in zip(cpp['path_depths'], py['path_depths']):
                        self.assertEqual(len(a), len(b))
                        for x, y in zip(a, b):
                            self.assertAlmostEqual(x, y, delta=1e-9)
                    cpp['surface'].pop('backend')
                    py['surface'].pop('backend')
                    self.assertEqual(cpp['surface'], py['surface'])
        self.assertEqual(relief.prepare_relief(sample())['surface']['backend'], 'cpp')

    def test_native_boundary_validates_values_shapes_and_caps(self):
        valid = [[[[-1.0] * 10 for _ in range(10)], 10.0, 10.0, 2.0, 2.0, 0.0, 10, 10, 'ball']]
        base = valid[0]
        self.assertEqual(len(relief._relief_native.contact(*base)), 11)
        for index, value in [
            (0, []),
            (0, [[0], [0, 0]]),
            (0, [[True]]),
            (0, [[float('nan')]]),
            (0, [[1.0]]),
            (0, [[-3.0]]),
            (1, True),
            (1, float('inf')),
            (2, 0),
            (3, -1),
            (4, 0),
            (5, 3),
            (6, 10.5),
            (6, True),
            (6, 100000),
            (7, -1),
            (8, 'other'),
            (8, 'ball\0other'),
            (8, None),
        ]:
            args = base.copy()
            args[index] = value
            with self.subTest(index=index, value=value), self.assertRaises(ValueError):
                relief._relief_native.contact(*args)
        args = [[[0.0] * 255 for _ in range(255)], 10.0, 10.0, 2.0, 100.0, 0.0, 255, 255, 'flat']
        with self.assertRaises(ValueError):
            relief._relief_native.contact(*args)


if __name__ == '__main__':
    unittest.main()
