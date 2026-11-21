import math
import unittest

from studio.cam import generate


def project(**changes):
    return dict(
        dict(
            mode='laser', width=100, height=100, power=800, feed=600, paths=[[[10, 10], [14, 10], [14, 16], [26, 16]]]
        ),
        **changes,
    )


def off_length(job):
    return sum(math.dist(s[:2], s[2:4]) for s in job['segments'] if s[5] == 0)


class LaserPatternTest(unittest.TestCase):
    def test_perforation_phase_across_corner_and_leads(self):
        job = generate(project(laser_pattern='perforate', laser_dash=3, laser_gap=2, lead_in=2, lead_out=3, passes=2))
        self.assertAlmostEqual(off_length(job), 16)
        self.assertIn('G1 X14.000 Y10.000 F600.0 S0', job['lines'])
        self.assertIn('G1 X14.000 Y11.000 F600.0 S0', job['lines'])
        self.assertIn('G1 X29.000 Y16.000 F600.0 S800', job['lines'])
        self.assertEqual(job['lines'].count('G0 X8.000 Y10.000'), 2)

    def test_bridges_on_compensated_profile_and_mid_ramp(self):
        shape = [[[20, 20], [40, 20], [40, 40], [20, 40], [20, 20]]]
        for operation in ['line', 'outside']:
            job = generate(
                project(
                    paths=shape,
                    laser_pattern='bridges',
                    laser_gap=2,
                    laser_bridge_count=4,
                    operation=operation,
                    kerf=0.2,
                    ramp_in=15,
                    start_power=20,
                )
            )
            self.assertAlmostEqual(off_length(job), 8)
            self.assertTrue(any(0 < s[5] < 800 for s in job['segments']))

    def test_quarter_circle_both_sides_and_bounds(self):
        for shape, side in [('arc-left', 1), ('arc-right', -1)]:
            job = generate(
                project(paths=[[[30, 30], [60, 30]]], lead_shape=shape, lead_in=5 * math.pi, lead_out=5 * math.pi)
            )
            moves = job['segments']
            self.assertAlmostEqual(moves[0][0], 20)
            self.assertAlmostEqual(moves[0][1], 30 + 10 * side)
            self.assertAlmostEqual(moves[-1][2], 70)
            self.assertAlmostEqual(moves[-1][3], 30 + 10 * side)
            self.assertAlmostEqual(job['cut_length'], 30 + 10 * math.pi, delta=0.04)
            self.assertGreater(len(moves), 18)
        with self.assertRaisesRegex(ValueError, 'outside'):
            generate(project(paths=[[[2, 30], [60, 30]]], lead_shape='arc-right', lead_in=8))

    def test_reject_invalid_raster_bridge_and_oversized_patterns(self):
        for args in [
            dict(laser_pattern='bridges'),
            dict(laser_pattern='bridges', laser_bridge_count=1.5),
            dict(laser_pattern='bridges', paths=[[[1, 1], [2, 1], [2, 2], [1, 1]]], laser_gap=5),
            dict(laser_pattern='perforate', path_powers=[[0, 100, 100, 0]]),
            dict(laser_pattern='perforate', laser_gap=0),
            dict(laser_pattern='unknown'),
            dict(lead_shape='unknown'),
            dict(
                laser_pattern='perforate',
                width=2000,
                height=2000,
                laser_gap=0.01,
                laser_dash=0.01,
                paths=[[[0, 0], [2000, 2000]]],
            ),
        ]:
            with self.subTest(args=args), self.assertRaises(ValueError):
                generate(project(**args))
