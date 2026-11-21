import math
import unittest

from studio.cam import generate
from studio.simulation import simulate
from studio.tool_changes import plan_tool_changes


def circle(x, y, r):
    samples = [[x + r * math.cos(i * math.pi / 16), y + r * math.sin(i * math.pi / 16)] for i in range(32)]
    return samples + [samples[0]]


def project(**values):
    return dict(
        dict(
            mode='router',
            protocol='grbl',
            operation='drill',
            depth=2.5,
            safe=3,
            power=9000,
            feed=100,
            plunge=80,
            width=100,
            height=100,
            tool_diameter=4,
            paths=[circle(20, 20, 2), [[40, 30], [40, 30]]],
        ),
        **values,
    )


class DrillTest(unittest.TestCase):
    def test_single_and_peck_clear_each_hole_before_xy(self):
        for peck, expected in [(0, [-2.5]), (1, [-1, -2, -2.5])]:
            job = generate(project(drill_peck=peck))
            self.assertEqual(len(job['drill_holes']), 2)
            self.assertEqual(job['simulation_tool'], dict(diameter=4, shape='drill', angle=118))
            self.assertEqual([s[4] for s in job['segments']], expected * 2)
            self.assertTrue(all(s[6] == 3 and s[:2] == s[2:4] for s in job['segments']))
            lines = job['lines']
            self.assertLess(lines.index('G0 Z3.000'), lines.index('G0 X20.000 Y20.000'))
            for i, l in enumerate(lines):
                if l.startswith('G1 '):
                    self.assertTrue(l.endswith(' F80.0'))
                    self.assertEqual(lines[i + 1], 'G0 Z3.000')
            self.assertEqual(lines[-1], 'M5')
            self.assertEqual(job['cut_length'], 2 * sum(3 - z for z in expected))

    def test_exact_peck_multiple_stops_at_target_once(self):
        job = generate(project(depth=2.1, drill_peck=0.3))
        self.assertEqual(job['lines'].count('G1 Z-2.100 F80.0'), 2)
        self.assertEqual(len(job['segments']), 14)

    def test_pure_vertical_conical_stock_contact(self):
        job = generate(project(paths=[circle(20, 20, 2)], drill_peck=1))
        result = simulate(
            dict(
                segments=job['segments'],
                resolution=17,
                x=18,
                y=18,
                width=4,
                height=4,
                tool_shape='drill',
                tool_diameter=4,
                v_angle=118,
            )
        )
        self.assertAlmostEqual(result['heights'][8][8], -2.5)
        self.assertAlmostEqual(result['heights'][8][12], -2.5 + 1 / math.tan(math.radians(59)))
        self.assertEqual(result['heights'][0][0], 0)

    def test_reject_ambiguous_geometry_and_wrong_tools(self):
        invalid = [
            dict(paths=[[[10, 10], [20, 20]]]),
            dict(paths=[circle(20, 20, 4)]),
            dict(paths=[circle(1, 1, 2)]),
            dict(tool_shape='v'),
            dict(mode='knife'),
            dict(drill_peck=0.001),
            dict(path_depths=[[0, 0]]),
            dict(drill_point_angle=175),
        ]
        for change in invalid:
            with self.subTest(change=change), self.assertRaises(ValueError):
                generate(project(**change))
        with self.assertRaisesRegex(ValueError, 'drill bit is for drilling'):
            generate(project(operation='line', tool_shape='drill'))

    def test_ordered_drill_stages_and_protocol_export(self):
        raw = project(protocol='linuxcnc')
        p = dict(raw, operations=[dict(tool_id=2, paths=raw['paths'])])
        output = generate(p)
        self.assertEqual(output['simulation_tool']['shape'], 'drill')
        self.assertEqual(len(output['drill_holes']), 2)
        self.assertEqual(output['lines'][-1], 'M30')
        plan = plan_tool_changes(p)
        self.assertEqual(plan['stages'][0]['tool']['shape'], 'drill')
        self.assertEqual(plan['stages'][0]['tool']['drill_point_angle'], 118)
