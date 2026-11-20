import unittest

from studio.cam import generate
from studio.fixture_collision import check_fixtures
from studio.protocols import PROTOCOLS


def setup(lines=None, start=None):
    return {
        'start': start or [0, 0, 4],
        'lines': lines or ['G21', 'G90', 'G0 X100'],
        'tool': {'diameter': 2, 'stickout': 4, 'holder_diameter': 12, 'holder_length': 15},
        'fixtures': [{'name': 'Bridge clamp', 'min': [30, 1, 7], 'max': [32, 3, 10]}],
    }


def generated(protocol='grbl', mode='router'):
    return generate(
        {
            'mode': mode,
            'protocol': protocol,
            'width': 100,
            'height': 80,
            'power': 8000,
            'depth': 1,
            'feed': 500,
            'step': 1,
            'safe': 4,
            'dwell_units': 'seconds',
            'paths': [[[5, 20], [10, 20]], [[50, 20], [55, 20]]],
        }
    )['lines']


class FixtureCollisionTests(unittest.TestCase):
    def test_actual_postprocessors_and_rapid_between_contours(self):
        for protocol, profile in PROTOCOLS.items():
            for mode in profile.modes:
                with self.subTest(protocol=protocol, mode=mode):
                    lines = generated(protocol, mode)
                    payload = setup(lines)
                    clamp = payload['fixtures'][0]
                    clamp.update(min=[30, 21, 7], max=[32, 23, 10])
                    result = check_fixtures(payload)
                    self.assertTrue(result['collision'])
                    first = result['events'][0]
                    self.assertEqual(first['component'], 'holder')
                    self.assertEqual(lines[first['line'] - 1].split()[0], 'G0')
                    self.assertAlmostEqual(first['position'][0], 30 - 35**0.5, places=4)
                    clamp.update(min=[30, 40, 7], max=[32, 42, 10])
                    self.assertFalse(check_fixtures(payload)['collision'])

    def test_diagonal_z_and_xy_must_overlap_at_same_time(self):
        payload = setup(['G21', 'G90', 'G1 X100 Z100 F100'], [0, 0, 0])
        payload['fixtures'][0].update(min=[80, -1, 4], max=[81, 1, 5])
        self.assertFalse(check_fixtures(payload)['collision'])
        payload['fixtures'][0].update(min=[80, -1, 80], max=[81, 1, 81])
        self.assertTrue(check_fixtures(payload)['collision'])

    def test_submillimetre_block_and_corner_rounding(self):
        payload = setup(['G21', 'G90', 'G1 X100 F500'], [0, 0, 0])
        payload['fixtures'][0].update(min=[33.003, 0.99999, 0], max=[33.004, 1.1, 0.01])
        event = check_fixtures(payload)['events'][0]
        self.assertEqual(event['component'], 'tool')
        self.assertAlmostEqual(event['position'][0], 33.003 - (1 - 0.99999**2) ** 0.5, places=4)
        payload['lines'] = ['G21', 'G90', 'G1 X32.1']
        self.assertFalse(check_fixtures(payload)['collision'])

    def test_tangent_vertical_plunge_and_report_cap(self):
        payload = setup(['G21', 'G90'] + ['G1 Z0', 'G0 Z20'] * 110, [29, 1, 20])
        result = check_fixtures(payload)
        self.assertGreater(result['contact_count'], 100)
        self.assertEqual(len(result['events']), 100)
        self.assertEqual(result['events'][0]['position'], [29, 1, 10])
        self.assertEqual(result['events'][0]['component'], 'tool')
        self.assertEqual(result['moves_checked'], 220)

    def test_initial_position_checked(self):
        payload = setup(['G21', 'G90', 'G0 Z20'], [30, 2, 8])
        self.assertEqual(check_fixtures(payload)['events'][0]['position'], [30, 2, 8])

    def test_unsupported_program_fails_whole_check_even_after_collision(self):
        for command in (
            'G2 X0 Y0 I2 J0',
            'G91',
            'G20',
            'G55',
            'G92 X10',
            'G0 A360',
            'X100',
            'G0 X1 X2',
            'G0 ZNaN',
            'G0 X1.2.3',
            'M6 T2',
            'M30 X0',
        ):
            with self.subTest(command=command), self.assertRaisesRegex(ValueError, 'line 4'):
                check_fixtures(setup(['G21', 'G90', 'G0 X100', command]))
        for lines in (['G0 X0'], ['G21', 'G1 X10'], ['G21', 'G90'], ['G21', 'G90', 'M30', 'G0 X1']):
            with self.subTest(lines=lines), self.assertRaises(ValueError):
                check_fixtures(setup(lines))

    def test_validate_measured_setup(self):
        for attribute, bad in (('start', [0, None, 0]), ('fixtures', []), ('tool', []), ('fixtures', [{'name': 1}])):
            with self.subTest(attribute=attribute), self.assertRaises(ValueError):
                check_fixtures({**setup(), attribute: bad})
        payload = setup()
        payload['fixtures'][0]['max'] = [30, 3, 10]
        with self.assertRaisesRegex(ValueError, 'positive size'):
            check_fixtures(payload)
