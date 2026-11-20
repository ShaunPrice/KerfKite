import unittest
from collections import Counter
from studio.solid_text import extrude


class SolidTextTests(unittest.TestCase):
    def test_watertight_extrusion_with_hole(self):
        r = extrude(
            {
                'paths': [[[0, 0], [10, 0], [10, 10], [0, 10], [0, 0]], [[3, 3], [7, 3], [7, 7], [3, 7], [3, 3]]],
                'height': 2,
            }
        )
        vertices = [tuple(map(float, l.split()[1:])) for l in r['stl'].splitlines() if l.startswith('vertex')]
        edges = Counter()
        volume = 0
        for i in range(0, len(vertices), 3):
            a, b, c = vertices[i : i + 3]
            for u, v in [(a, b), (b, c), (c, a)]:
                edges[tuple(sorted((u, v)))] += 1
            volume += (
                a[0] * (b[1] * c[2] - b[2] * c[1])
                - a[1] * (b[0] * c[2] - b[2] * c[0])
                + a[2] * (b[0] * c[1] - b[1] * c[0])
            ) / 6
        self.assertEqual(set(edges.values()), {2})
        self.assertAlmostEqual(volume, 168)
