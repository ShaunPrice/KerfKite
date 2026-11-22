import base64
import math
import random
import unittest
from unittest import mock

from studio import relief
from studio.relief import prepare_relief


def sample(**changes):
    data = dict(
        heightmap=[[0.5] * 11 for _ in range(11)],
        width=10,
        height=10,
        depth=2,
        x=0,
        y=0,
        tool_diameter=2,
        tool_shape='ball',
        stepover=1,
    )
    data.update(changes)
    return data


class ReliefTests(unittest.TestCase):
    def assert_contact_safe(self, result, surface):
        radius = result['surface']['tool_diameter'] / 2
        ball = result['surface']['tool_shape'] == 'ball'
        # Probe the swept tool at fractional positions, not only path vertices.
        for path, depths in zip(result['paths'], result['path_depths']):
            for a, b, za, zb in zip(path, path[1:], depths, depths[1:]):
                for t in (0, 0.27, 0.65, 1):
                    px, py = a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])
                    z = za + t * (zb - za)
                    for radial in (0, 0.35, 0.7, 1):
                        r = radius * radial
                        sag = radius - math.sqrt(max(0, radius * radius - r * r)) if ball else 0
                        for angle in range(0, 360, 45):
                            sx = px + r * math.cos(math.radians(angle))
                            sy = py + r * math.sin(math.radians(angle))
                            if 0 <= sx <= 10 and 0 <= sy <= 10:
                                self.assertGreaterEqual(z + sag + 1e-10, surface(sx, sy))

    def test_plane_and_allowance_are_exact_for_both_cutters(self):
        for shape in ('flat', 'ball'):
            result = prepare_relief(sample(tool_shape=shape, allowance=0.2))
            self.assertTrue(all(z == -0.8 for row in result['path_depths'] for z in row))
            self.assertEqual(result['paths'][0][0], [0, 0])
            self.assertEqual(result['paths'][1][0], [10, 1])
            self.assert_contact_safe(result, lambda x, y: -1)
        top = prepare_relief(sample(heightmap=[[1, 1], [1, 1]], allowance=0.2))
        self.assertTrue(all(z == 0 for row in top['path_depths'] for z in row))

    def test_analytic_ramp_swept_cutter_is_above_surface(self):
        matrix = [[i / 10 for i in range(11)] for _ in range(11)]
        for shape in ('flat', 'ball'):
            result = prepare_relief(sample(heightmap=matrix, tool_shape=shape))
            self.assert_contact_safe(result, lambda x, y: x / 5 - 2)
            self.assertTrue(any(z < -0.5 for row in result['path_depths'] for z in row))

    def test_analytic_dome_swept_cutter_is_above_surface(self):
        def height(x, y):
            return 1 - ((x - 5) ** 2 + (y - 5) ** 2) / 50

        matrix = [[height(i, j) for i in range(11)] for j in range(11)]
        for shape in ('flat', 'ball'):
            result = prepare_relief(sample(heightmap=matrix, tool_shape=shape))
            self.assert_contact_safe(result, lambda x, y: (height(x, y) - 1) * 2)

    def test_peak_between_raster_rows_is_not_ignored(self):
        matrix = [[0] * 11 for _ in range(11)]
        matrix[5][5] = 1
        result = prepare_relief(sample(heightmap=matrix, stepover=2, tool_shape='flat'))
        for path, depths in zip(result['paths'], result['path_depths']):
            if path[0][1] in (4, 6):
                for point, z in zip(path, depths):
                    if 4 <= point[0] <= 6:
                        self.assertEqual(z, 0)

    def test_offsets_and_matching_lengths(self):
        result = prepare_relief(sample(x=20, y=30))
        self.assertEqual(result['paths'][0][0], [20, 30])
        self.assertEqual(len(result['paths']), len(result['path_depths']))
        for path, depths in zip(result['paths'], result['path_depths']):
            self.assertEqual(len(path), len(depths))
            self.assertTrue(all(-2 <= z <= 0 for z in depths))
        self.assertEqual(result['surface']['sample_spacing'], [1, 1])
        self.assertTrue(result['warnings'])

    def test_invalid_inputs_and_work_limits(self):
        cases = [
            dict(width=0),
            dict(height=float('nan')),
            dict(depth=True),
            dict(tool_shape='vbit'),
            dict(tool_diameter=0),
            dict(stepover=3),
            dict(stepover=0),
            dict(stepover=1e-320),
            dict(allowance=-1),
            dict(allowance=3),
            dict(heightmap=[[0, 1], [0]]),
            dict(heightmap=[[False, 1], [0, 1]]),
            dict(heightmap=[[0, float('inf')], [0, 1]]),
            dict(heightmap=[[0, -1], [0, 1]]),
            dict(heightmap=[[0, 1]]),
            dict(stepover=0.0001),
        ]
        for change in cases:
            with self.subTest(change=list(change)), self.assertRaises(ValueError):
                prepare_relief(sample(**change))
        with self.assertRaises(ValueError):
            prepare_relief(None)


def rgb(pixels):
    """heightmap_rgb and heightmap_size for rows of (r, g, b), the top row first, as a canvas gives them."""
    data = bytes(v for row in pixels for pixel in row for v in pixel)
    return dict(heightmap_rgb=base64.b64encode(data).decode(), heightmap_size=[len(pixels[0]), len(pixels)])


def luminance(pixel):
    r, g, b = pixel
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255


class ReliefMapLimitTests(unittest.TestCase):
    def test_python_contact_keeps_small_maps_and_says_why(self):
        with self.assertRaisesRegex(ValueError, r'2\.\.256 rows; larger maps need the compiled extension'):
            prepare_relief(sample(heightmap=[[0, 1]] * 257), backend='python')
        with self.assertRaisesRegex(ValueError, r'2\.\.256 columns; larger maps need the compiled extension'):
            prepare_relief(sample(heightmap=[[0] * 300] * 3), backend='python')
        with self.assertRaisesRegex(ValueError, r'2\.\.256 rows$'):
            prepare_relief(sample(heightmap=[[0, 1]] * 1025), backend='python')
        with self.assertRaisesRegex(ValueError, 'too large'):
            prepare_relief(sample(heightmap=[[0] * 256 for _ in range(256)], tool_diameter=100), backend='python')
        with self.assertRaisesRegex(ValueError, r'each 2\.\.256; larger maps need the compiled extension'):
            prepare_relief(sample(heightmap=None, **rgb([[(0, 0, 0)] * 300] * 2)), backend='python')

    @unittest.skipUnless(relief._native('auto'), 'Optional C++ extension is not installed')
    def test_compiled_contact_takes_larger_maps_within_bounds(self):
        tall = prepare_relief(sample(heightmap=[[0, 1]] * 257))
        self.assertEqual(tall['surface']['backend'], 'cpp')
        self.assertEqual(tall['surface']['rows'], 257)
        with self.assertRaisesRegex(ValueError, r'2\.\.1024 rows$'):
            prepare_relief(sample(heightmap=[[0, 1]] * 1025))
        black = sample(
            heightmap=None, **rgb([[(0, 0, 0)] * 1024] * 1024), tool_diameter=100, width=100, height=100, stepover=2
        )
        with self.assertRaisesRegex(ValueError, 'too large'):
            prepare_relief(black)
        with self.assertRaisesRegex(ValueError, 'each 2..1024'):
            prepare_relief(sample(heightmap=None, **rgb([[(0, 0, 0)] * 1025] * 2)))

    def test_the_editor_is_told_the_largest_map(self):
        from studio.device import DeviceController
        from studio.server import create_app

        device = DeviceController()
        self.addCleanup(device.disconnect)
        session = create_app(device).test_client().get('/api/session').json
        self.assertEqual(session['relief_map'], relief.map_limit())
        self.assertEqual(relief.map_limit(), 1024 if relief._native('auto') else 256)
        with mock.patch.object(relief, '_relief_native', None):
            self.assertEqual(relief.map_limit(), 256)
            self.assertEqual(relief.gpu_status(), 'no compiled extension')

    def test_a_result_says_where_its_contact_was_computed(self):
        data = sample(heightmap=[[0, 0.5, 1]] * 3)
        self.assertEqual(prepare_relief(data, backend='python')['device'], 'python')
        if relief._native('auto'):
            native = prepare_relief(data)
            self.assertIn(native['device'], ('cpu', 'gpu'))
            self.assertNotIn('device', native['surface'])
            self.assertIsInstance(relief.gpu_status(), str)

    def test_rgb_bytes_give_what_the_same_numbers_give(self):
        rng = random.Random(77)  # noqa: S311 - reproducible test data
        pixels = [[(rng.randrange(256), rng.randrange(256), rng.randrange(256)) for _ in range(23)] for _ in range(17)]
        pixels[0][0] = (255, 255, 255)
        pixels[5][7] = (0, 0, 0)
        # The editor sends the bottom image row first in heightmap.
        numbers = [[luminance(p) for p in row] for row in reversed(pixels)]
        backends = ['python'] + (['native'] if relief._native('auto') else [])
        for backend in backends:
            for shape in ('ball', 'flat'):
                with self.subTest(backend=backend, shape=shape):
                    settings = dict(width=30, height=20, stepover=0.4, tool_shape=shape, allowance=0.05)
                    expected = prepare_relief(sample(heightmap=numbers, **settings), backend=backend)
                    got = prepare_relief(sample(heightmap=None, **rgb(pixels), **settings), backend=backend)
                    self.assertEqual(got, expected)

    def test_rgb_input_is_checked(self):
        good = rgb([[(0, 0, 0)] * 4] * 3)
        for change in (
            dict(heightmap_size=[4, 2]),
            dict(heightmap_size=[4]),
            dict(heightmap_size=[True, 3]),
            dict(heightmap_size=[1, 3]),
            dict(heightmap_rgb='not base64!'),
            dict(heightmap_rgb=good['heightmap_rgb'][:-4]),
            dict(heightmap_rgb=7),
            dict(heightmap=[[0, 1], [0, 1]]),
        ):
            data = {**sample(), **good, **change}
            if 'heightmap' not in change:
                data.pop('heightmap')
            with self.subTest(change=change), self.assertRaises(ValueError):
                prepare_relief(data)


if __name__ == '__main__':
    unittest.main()
