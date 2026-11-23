"""The native core's relief must be the Python reference's to the bit, and refuse what it refuses."""

import base64
import json
import random
import sys
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


def exact(value):
    """A form that tells 0 from 0.0 and -0.0 from 0.0."""
    return json.loads(json.dumps(value), parse_float=lambda text: ('f', text), parse_int=lambda text: ('i', text))


def without_backend(result):
    """The result without what says where it was computed: the same design gives the same surface anywhere."""
    result = dict(result, surface=dict(result['surface']))
    result.pop('device')
    result['surface'].pop('backend')
    return result


class ReliefFallbackTests(unittest.TestCase):
    def test_python_fallback_and_explicit_backend(self):
        with patch.object(relief, '_relief_native', None):
            result = relief.prepare_relief(sample())
            self.assertEqual(result['surface']['backend'], 'python')
            self.assertEqual(result['device'], 'python')
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


@unittest.skipUnless(relief._native('auto'), 'Optional C++ extension is not installed')
class NativeReliefTests(unittest.TestCase):
    def both(self, data):
        """The core's answer and the reference's with the core's limits, as ('ok', result) or ('error', message)."""
        found = []
        for backend in ('native', 'python'):
            try:
                result = relief.prepare_relief(data, backend=backend, _native_limits=True)
                found.append(('ok', exact(without_backend(result))))
            except ValueError as error:
                found.append(('error', str(error)))
        self.assertEqual(found[0], found[1])
        return found[0]

    def test_paths_and_depths_are_the_reference_to_the_bit(self):
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
                    with self.subTest(shape=shape, allowance=allowance):
                        status, _ = self.both(
                            sample(heightmap=matrix, tool_shape=shape, allowance=allowance, stepover=0.73)
                        )
                        self.assertEqual(status, 'ok')
        result = relief.prepare_relief(sample())
        self.assertEqual(result['surface']['backend'], 'cpp')
        self.assertIn(result['device'], ('cpu', 'gpu'))

    def test_image_heights_are_the_reference_to_the_bit(self):
        rng = random.Random(8)
        for columns, rows in ((2, 2), (7, 3), (16, 9)):
            raw = bytes(rng.randrange(256) for _ in range(columns * rows * 3))
            data = sample(heightmap=None, heightmap_rgb=base64.b64encode(raw).decode(), heightmap_size=[columns, rows])
            del data['heightmap']
            with self.subTest(columns=columns, rows=rows):
                self.assertEqual(self.both(data)[0], 'ok')

    def test_refusals_are_the_reference_refusals(self):
        image = base64.b64encode(bytes(12)).decode()
        cases = {
            'no settings': {},
            'text width': sample(width='10'),
            'negative x': sample(x=-1),
            'wide stepover': sample(stepover=3),
            'deep allowance': sample(allowance=3),
            'unknown cutter': sample(tool_shape='v'),
            'both maps': sample(heightmap_rgb=image, heightmap_size=[2, 2]),
            'one row': sample(heightmap=[[0, 1]]),
            'ragged': sample(heightmap=[[0, 1], [1]]),
            'above one': sample(heightmap=[[0, 2], [1, 0]]),
            'a flag': sample(heightmap=[[0, True], [1, 0]]),
            'too many vertices': sample(stepover=0.01, tool_diameter=0.02),
            'float size': dict(sample(heightmap=None), heightmap_rgb=image, heightmap_size=[2.0, 2]),
            'map too large': dict(sample(heightmap=None), heightmap_rgb=image, heightmap_size=[1025, 2]),
            'short image': dict(sample(heightmap=None), heightmap_rgb=image[:-4], heightmap_size=[2, 2]),
            'padding inside': dict(sample(heightmap=None), heightmap_rgb='AA=A' + image[4:], heightmap_size=[2, 2]),
            'not base64': dict(sample(heightmap=None), heightmap_rgb='!' * 16, heightmap_size=[2, 2]),
            'not ascii': dict(sample(heightmap=None), heightmap_rgb='é' * 16, heightmap_size=[2, 2]),
        }
        for name, data in cases.items():
            with self.subTest(name):
                self.assertEqual(self.both(data)[0], 'error')

    def test_padding_after_whole_groups_is_refused(self):
        # Some Python releases read "...=" after whole groups (3.11.18, 3.12.3); 3.12.10 and 3.13 refuse it, and so
        # does the core. (Within the length the image allows; longer text is refused before it is read.)
        raw = base64.b64encode(bytes(range(6))).decode()
        data = dict(sample(heightmap=None), heightmap_rgb=raw + '==', heightmap_size=[2, 2])
        del data['heightmap']
        with self.assertRaises(ValueError):
            relief.prepare_relief(data, backend='native')
        if sys.version_info < (3, 12, 10):
            return
        try:
            expected = (
                'ok',
                exact(without_backend(relief.prepare_relief(data, backend='python', _native_limits=True))),
            )
        except ValueError as error:
            expected = ('error', str(error))
        try:
            found = ('ok', exact(without_backend(relief.prepare_relief(data, _native_limits=True))))
        except ValueError as error:
            found = ('error', str(error))
        self.assertEqual(found, expected)

    def test_the_core_takes_the_larger_maps(self):
        matrix = [[(i * j) % 7 / 7 for i in range(300)] for j in range(300)]
        data = sample(heightmap=matrix, width=60, height=60, stepover=1.5, tool_diameter=3)
        with self.assertRaisesRegex(ValueError, 'larger maps need the compiled extension'):
            relief.prepare_relief(data, backend='python')
        result = relief.prepare_relief(data)
        self.assertEqual(result['surface']['rows'], 300)
        self.assertEqual(relief.map_limit(), 1024)


if __name__ == '__main__':
    unittest.main()
