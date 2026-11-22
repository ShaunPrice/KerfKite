import json
import os
import unittest
from unittest.mock import patch

from studio import cam
from studio.cam import generate
from studio.fixture_collision import check_fixtures, fixtures_json
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


def exact(value):
    """A form that tells 0 from 0.0 and -0.0 from 0.0."""
    return json.loads(json.dumps(value), parse_float=lambda text: ('f', text), parse_int=lambda text: ('i', text))


def answer(payload, backend):
    try:
        return 'ok', exact(check_fixtures(payload, backend=backend))
    except ValueError as error:
        return 'error', str(error)


@unittest.skipIf(cam._core_cam is None, 'Build the extension to compare the native core with the reference')
class NativeFixtureTests(unittest.TestCase):
    """The native core's checks against this module's Python, the reference: every member, with its type."""

    def setUp(self):
        environment = patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        for key in ('KERFKITE_GCODE_BACKEND', 'KERFKITE_POLYGON_BACKEND'):
            os.environ.pop(key, None)

    def test_checks_and_refusals_are_the_reference(self):
        many = {**setup(generated()), 'fixtures': [
            {'name': f'Clamp {i}', 'min': [5 + 9 * i, 18, -2], 'max': [8 + 9 * i, 22, 1]} for i in range(10)
        ]}  # fmt: skip
        cases = [
            setup(),
            setup(generated()),
            setup(generated('linuxcnc')),
            many,
            setup(['G21', 'G90', 'G0 X-0 Y.5', 'g1 x31 y2 z8 f300', 'G4 P1', 'M30']),
            setup(['G21', 'G90', 'G0 X31 Y2', 'G0 Z0']),
            setup(['G21', 'G90', 'G0 X1 X2']),
            setup(['G21', 'G0 X1']),
            setup(['G21', 'G90', 'M30', 'G0 X1']),
            setup(['G21', 'G90', 'G0 X' + '9' * 300]),
            {**setup(), 'fixtures': [{'name': ' \u3000 ', 'min': [0, 0, 0], 'max': [1, 1, 1]}]},
            {**setup(), 'fixtures': [{'name': 'caf\u00e9', 'min': [30, 1, 7], 'max': [32, 3, True]}]},
            {**setup(), 'tool': {'diameter': 2, 'stickout': 4, 'holder_diameter': 12}},
            {**setup(), 'start': [0, 0]},
        ]
        for case in cases:
            with self.subTest(case=json.dumps(case)[:120]):
                self.assertEqual(answer(case, 'native'), answer(case, 'python'))
        # Many contacts: the report keeps the first hundred, in the order of each move's instants.
        self.assertEqual(
            len(check_fixtures(many, backend='native')['events']), len(check_fixtures(many, backend='python')['events'])
        )

    def test_text_that_is_not_ascii_is_the_reference_s(self):
        payload = setup(['G21', 'G90', 'G0 X\u0661'])
        with self.assertRaises(NotImplementedError):
            check_fixtures(payload, backend='native')
        self.assertEqual(answer(payload, 'auto'), answer(payload, 'python'))

    def test_route_sends_the_core_text(self):
        from studio.server import create_app

        client = create_app().test_client()
        headers = {'X-Studio-Token': client.get('/api/session').get_json()['token']}
        response = client.post('/api/fixture-collision', json=setup(), headers=headers)
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.data, fixtures_json(setup()))
        self.assertEqual(exact(response.get_json()), exact(check_fixtures(setup(), backend='python')))
        refused = client.post('/api/fixture-collision', json={**setup(), 'fixtures': []}, headers=headers)
        self.assertEqual(refused.status_code, 400)
        self.assertIn('fixture boxes', refused.get_json()['error'])


if __name__ == '__main__':
    unittest.main()
