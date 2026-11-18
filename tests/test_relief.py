import math
import unittest

from studio.relief import prepare_relief


def sample(**changes):
    data = dict(heightmap=[[0.5]*11 for _ in range(11)], width=10, height=10,
                depth=2, x=0, y=0, tool_diameter=2, tool_shape='ball', stepover=1)
    data.update(changes)
    return data


class ReliefTests(unittest.TestCase):
    def assert_contact_safe(self, result, surface):
        radius = result['surface']['tool_diameter']/2
        ball = result['surface']['tool_shape'] == 'ball'
        # Probe the swept tool at fractional positions, not only path vertices.
        for path, depths in zip(result['paths'], result['path_depths']):
            for a, b, za, zb in zip(path, path[1:], depths, depths[1:]):
                for t in (0, .27, .65, 1):
                    px, py = a[0]+t*(b[0]-a[0]), a[1]+t*(b[1]-a[1])
                    z = za+t*(zb-za)
                    for radial in (0, .35, .7, 1):
                        r = radius*radial
                        sag = radius-math.sqrt(max(0, radius*radius-r*r)) if ball else 0
                        for angle in range(0, 360, 45):
                            sx = px+r*math.cos(math.radians(angle))
                            sy = py+r*math.sin(math.radians(angle))
                            if 0 <= sx <= 10 and 0 <= sy <= 10:
                                self.assertGreaterEqual(z+sag+1e-10, surface(sx, sy))

    def test_plane_and_allowance_are_exact_for_both_cutters(self):
        for shape in ('flat', 'ball'):
            result = prepare_relief(sample(tool_shape=shape, allowance=.2))
            self.assertTrue(all(z == -.8 for row in result['path_depths'] for z in row))
            self.assertEqual(result['paths'][0][0], [0, 0])
            self.assertEqual(result['paths'][1][0], [10, 1])
            self.assert_contact_safe(result, lambda x, y: -1)
        top = prepare_relief(sample(heightmap=[[1, 1], [1, 1]], allowance=.2))
        self.assertTrue(all(z == 0 for row in top['path_depths'] for z in row))

    def test_analytic_ramp_swept_cutter_is_above_surface(self):
        matrix = [[i/10 for i in range(11)] for _ in range(11)]
        for shape in ('flat', 'ball'):
            result = prepare_relief(sample(heightmap=matrix, tool_shape=shape))
            self.assert_contact_safe(result, lambda x, y: x/5-2)
            self.assertTrue(any(z < -.5 for row in result['path_depths'] for z in row))

    def test_analytic_dome_swept_cutter_is_above_surface(self):
        def height(x, y):
            return 1-((x-5)**2+(y-5)**2)/50
        matrix = [[height(i, j) for i in range(11)] for j in range(11)]
        for shape in ('flat', 'ball'):
            result = prepare_relief(sample(heightmap=matrix, tool_shape=shape))
            self.assert_contact_safe(result, lambda x, y: (height(x, y)-1)*2)

    def test_peak_between_raster_rows_is_not_ignored(self):
        matrix = [[0]*11 for _ in range(11)]
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
        cases = [dict(width=0), dict(height=float('nan')), dict(depth=True),
                 dict(tool_shape='vbit'), dict(tool_diameter=0), dict(stepover=3),
                 dict(stepover=0), dict(stepover=1e-320), dict(allowance=-1), dict(allowance=3),
                 dict(heightmap=[[0, 1], [0]]), dict(heightmap=[[False, 1], [0, 1]]),
                 dict(heightmap=[[0, float('inf')], [0, 1]]),
                 dict(heightmap=[[0, -1], [0, 1]]), dict(heightmap=[[0, 1]]),
                 dict(heightmap=[[0, 1]]*257), dict(stepover=.0001),
                 dict(heightmap=[[0]*256 for _ in range(256)], tool_diameter=100)]
        for change in cases:
            with self.subTest(change=list(change)), self.assertRaises(ValueError):
                prepare_relief(sample(**change))
        with self.assertRaises(ValueError):
            prepare_relief(None)


if __name__ == '__main__':
    unittest.main()
