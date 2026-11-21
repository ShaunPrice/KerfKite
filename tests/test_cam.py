import unittest
from studio.cam import generate


def sample(**kw):
    return dict(
        mode='router',
        width=100,
        height=100,
        feed=600,
        power=1000,
        safe=5,
        depth=1.2,
        step=0.5,
        plunge=100,
        passes=1,
        paths=[[[10, 10], [20, 10], [20, 20]]],
        **kw,
    )


class CamTests(unittest.TestCase):
    def test_router_retracts_before_xy_and_limits_depth(self):
        job = generate(sample())
        lines = job['lines']
        self.assertEqual(
            [s for s in lines if s.startswith('G1 Z')], ['G1 Z-0.500 F100.0', 'G1 Z-1.000 F100.0', 'G1 Z-1.200 F100.0']
        )
        for i, line in enumerate(lines):
            if line.startswith('G0 X'):
                self.assertEqual(lines[i - 1], 'G0 Z5.000')
        self.assertEqual(lines[-2:], ['G0 Z5.000', 'M5'])

    def test_laser_off_before_travel_no_z(self):
        p = sample()
        p.update(mode='laser', passes=2)
        lines = generate(p)['lines']
        self.assertFalse(any('Z' in x for x in lines))
        self.assertEqual(lines.count('M4 S1000'), 2)
        for i, line in enumerate(lines):
            if line.startswith('G0 X'):
                self.assertEqual(lines[i - 1], 'M5')

    def test_reject_invalid_geometry_and_parameters(self):
        for key, value in [
            ('width', float('nan')),
            ('feed', 0),
            ('step', 0),
            ('passes', 1.5),
            ('power', 'M3'),
            ('paths', [[[0, 0], [101, 0]]]),
            ('paths', [[[0, 0], [float('inf'), 0]]]),
        ]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                p = sample()
                p[key] = value
                generate(p)

    def test_expansion_limit(self):
        p = sample()
        p.update(depth=50, step=0.01, paths=[[[i, 0] for i in range(100)]])
        with self.assertRaises(ValueError):
            generate(p)

    def test_laser_leads_and_ramps_include_envelope(self):
        p = sample()
        p.update(
            mode='laser',
            paths=[[[10, 10], [30, 10]]],
            lead_in=2,
            lead_out=3,
            ramp_in=4,
            ramp_out=5,
            start_power=0,
            end_power=10,
        )
        job = generate(p)
        self.assertEqual(job['bounds'], [8.0, 10.0, 33.0, 10.0])
        self.assertEqual(job['cut_length'], 25)
        self.assertIn('G0 X8.000 Y10.000', job['lines'])
        self.assertIn('M4 S0', job['lines'])
        powers = [s[5] for s in job['segments']]
        self.assertLess(powers[0], 100)
        self.assertEqual(max(powers), 1000)
        self.assertLess(powers[-1], 200)
        self.assertTrue(all(0 <= v <= 1000 for v in powers))
        self.assertTrue(all(' S' in s for s in job['lines'] if s.startswith('G1 ')))
        self.assertEqual(job['lines'][-1], 'M5')

    def test_laser_rejects_outside_leads_and_overlapping_ramps(self):
        for changes in ({'lead_in': 20}, {'lead_out': 100}, {'ramp_in': 15, 'ramp_out': 15}, {'start_power': 101}):
            p = sample()
            p.update(mode='laser', paths=[[[10, 10], [30, 10]]], **changes)
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                generate(p)

    def test_laser_duplicate_points_do_not_break_leads(self):
        p = sample()
        p.update(mode='laser', paths=[[[10, 10], [10, 10], [20, 10], [20, 10]]], lead_in=2)
        self.assertEqual(generate(p)['cut_length'], 12)
        p['paths'] = [[[10, 10], [10, 10]]]
        with self.assertRaises(ValueError):
            generate(p)
