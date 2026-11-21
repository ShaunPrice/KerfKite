import math
import unittest

from shapely.geometry import LineString, Polygon
from studio.cam import generate
from studio.simulation import simulate


def ring(x, y, side):
    return [[x, y], [x + side, y], [x + side, y + side], [x, y + side], [x, y]]


def project(**values):
    return dict(
        dict(
            mode='router',
            protocol='grbl',
            width=90,
            height=90,
            safe=3,
            depth=2,
            step=1,
            power=9000,
            feed=400,
            plunge=90,
            tool_diameter=4,
            stepover=2,
            router_entry='ramp',
            entry_angle=3,
            stock_thickness=4,
            paths=[ring(10, 10, 30)],
        ),
        **values,
    )


class RouterEntryTest(unittest.TestCase):
    def test_ramp_depth_changes_follow_contour_with_finishing_lap(self):
        job = generate(project())
        rim = LineString(ring(10, 10, 30))
        for s in job['segments']:
            xy = math.dist(s[:2], s[2:4])
            if not xy:
                self.assertGreaterEqual(s[4], -1)
            else:
                self.assertAlmostEqual(rim.distance(LineString([s[:2], s[2:4]])), 0)
                self.assertLessEqual(abs(s[6] - s[4]) / xy, math.tan(math.radians(3)) + 1e-8)
        for level in (-1, -2):
            floor = [s for s in job['segments'] if s[4] == s[6] == level]
            self.assertAlmostEqual(sum(math.dist(s[:2], s[2:4]) for s in floor), 120)
        self.assertIn('G1 X40.000 Y10.000 Z-0.250 F90.0', job['lines'])
        self.assertEqual(job['lines'][-2:], ['G0 Z3.000', 'M5'])

    def test_compensated_ramp_with_tabs_keeps_plateaus(self):
        job = generate(project(operation='outside', stock_thickness=2, tabs={'width': 4, 'height': 1, 'count': 2}))
        path = job['prepared_paths'][0]
        caps = job['prepared_depths'][0]
        raised = [
            LineString([a, b])
            for i, (a, b) in enumerate(zip(path, path[1:]))
            if caps[i] == caps[i + 1] == -1 and a != b
        ]
        self.assertTrue(raised)
        for s in job['segments']:
            if s[4] >= -1 - 1e-8 and s[6] >= -1 - 1e-8:
                continue
            edge = LineString([s[:2], s[2:4]])
            self.assertFalse(any(edge.intersection(t).length > 1e-5 for t in raised))

    def test_helix_and_leads_stay_inside_pocket_outside_island(self):
        outer = ring(10, 10, 40)
        island = ring(24, 24, 12)
        job = generate(project(operation='pocket', router_entry='helix', entry_radius=1.5, paths=[outer, island]))
        allowed = Polygon(outer, holes=[island]).buffer(-2, quad_segs=32).buffer(0.00001)
        falling = 0
        for s in job['segments']:
            d = math.dist(s[:2], s[2:4])
            if d > 1e-7:
                self.assertTrue(allowed.covers(LineString([s[:2], s[2:4]])))
                if s[4] < s[6]:
                    falling += 1
                    self.assertLessEqual((s[6] - s[4]) / d, math.tan(math.radians(3)) + 1e-7)
        self.assertGreater(falling, 36)
        stock = simulate(
            dict(segments=job['segments'], x=10, y=10, width=40, height=40, resolution=37, tool_diameter=4)
        )
        self.assertEqual(stock['heights'][18][18], 0)
        self.assertLess(min(min(row) for row in stock['heights']), -1.9)

    def test_ineligible_entries_cannot_fall_back_to_plunge(self):
        cases = [
            dict(paths=[[[10, 10], [20, 20]]]),
            dict(operation='pocket', router_entry='helix', entry_radius=3),
            dict(operation='pocket', router_entry='helix', entry_radius=1.8, paths=[ring(10, 10, 7)]),
            dict(router_entry='helix'),
            dict(operation='drill'),
            dict(tool_shape='ball'),
            dict(entry_angle=0),
            dict(stock_thickness=1),
            dict(paths=[ring(0.1, 10, 30)]),
            dict(path_depths=[[-2] * 5]),
        ]
        for case in cases:
            with self.subTest(case=case), self.assertRaises(ValueError):
                generate(project(**case))
