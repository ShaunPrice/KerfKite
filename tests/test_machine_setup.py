"""Machine setup on the workstation server's simulator: zeroing on a touch plate, writing a setting, macros.

The simulator's touch plate lies on a corner of the stock, at a place the zeroing program is not told: its left
face 12.5 mm to the left of where the cutter starts, its front face 9.5 mm in front and its top 7.3 mm below. The test follows the program's own moves
and the controller's probe reports, works out where work zero ended up, and compares it with that corner.

The server is taken from KERFKITE_STUDIO or build/native; without it the tests are skipped.
"""

import json
import re
import sys
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from validation import native_servers  # noqa: E402 - after the path of the checkout

try:
    STUDIO = native_servers.program('kerfkite-studio', None, 'KERFKITE_STUDIO')
except SystemExit:
    STUDIO = None

FACES = (-12.5, -9.5)  # the plate's left and front faces, from the cutter's start
TOP = -7.3  # the plate's top, from the cutter's start


@unittest.skipUnless(STUDIO, 'kerfkite-studio is not built: build native/ into build/native')
class SetupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.running = native_servers.studio(STUDIO)
        cls.server = cls.running.__enter__()

    @classmethod
    def tearDownClass(cls):
        cls.running.__exit__(None, None, None)

    def setUp(self):
        self.post('api/disconnect', {})
        self.post('api/connect', {'port': 'simulator'})
        self.idle()

    def post(self, route, payload):
        return self.server.post(route, payload)

    def refused(self, route, payload, *fragments):
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.post(route, payload)
        self.assertEqual(caught.exception.code, 400)
        message = json.loads(caught.exception.read())['error']
        for fragment in fragments:
            self.assertIn(fragment, message)
        return message

    def idle(self):
        for _ in range(400):
            status = self.server.status()
            if not status['active'] and status['state'] in ('Idle', 'Fault'):
                return status
            time.sleep(0.02)
        self.fail('the simulator did not come to rest')

    def follow(self, lines, reports):
        """Where the program leaves the machine and its work offsets, from its own moves and the probe contacts."""
        machine, offset, contacts = [0.0, 0.0, 0.0], [0.0, 0.0, 0.0], iter(reports)
        for line in lines:
            words = dict((w[0], float(w[1:])) for w in line.split()[1:] if w[0] in 'XYZ')
            if line.startswith('G38.2'):
                contact = [
                    float(v) for v in re.fullmatch(r'\[PRB:([-\d.]+),([-\d.]+),([-\d.]+):1\]', next(contacts)).groups()
                ]
                (axis,) = words
                machine['XYZ'.index(axis)] = contact['XYZ'.index(axis)]
            elif line.startswith('G0'):
                for axis, value in words.items():
                    machine['XYZ'.index(axis)] += value  # the program is relative throughout
            elif line.startswith('G10'):
                for axis, value in words.items():
                    offset['XYZ'.index(axis)] = machine['XYZ'.index(axis)] - value
        self.assertIsNone(next(contacts, None))
        return machine, offset

    def reports(self):
        """The probe contacts of the program that last ran, as the controller reported them (to 0.001 mm)."""
        status = self.server.status()
        rows = [row for row in status['log'] if row.startswith('[PRB:')]
        return rows[len(rows) - status['probe_count'] :] if status['probe_count'] else []

    def test_the_setup_routes_say_what_the_controller_can_do(self):
        with urllib.request.urlopen(self.server.url + 'api/setup', timeout=10) as response:  # noqa: S310 - loopback
            self.assertEqual(
                json.loads(response.read()), {'touch_plate': True, 'settings': True, 'macros': True, 'protocol': 'grbl'}
            )

    def test_z_alone_puts_work_zero_the_plate_thickness_below_its_top(self):
        answer = self.post('api/setup/probe', {'confirmed': True, 'axes': 'z', 'thickness': 12.7})
        self.assertEqual(answer['lines'][:3], ['G21', 'G91', 'G38.2 Z-15.000 F150.0'])
        self.assertEqual(answer['lines'][-1], 'G90')
        status = self.idle()
        self.assertIsNone(status['error'])
        reports = self.reports()
        self.assertEqual(len(reports), 2)  # a fast touch and a slow one
        machine, offset = self.follow(answer['lines'], reports)
        self.assertAlmostEqual(offset[2], TOP - 12.7, delta=0.0011)  # work Z zero is the stock under the plate
        self.assertAlmostEqual(status['position']['z'], machine[2] - offset[2], delta=0.0011)
        self.assertAlmostEqual(status['position']['z'], 12.7 + 5, 6)  # retracted 5 mm above the plate
        self.assertEqual((status['position']['x'], status['position']['y']), (0, 0))

    def test_a_corner_plate_puts_work_zero_on_the_corner_of_the_stock(self):
        for wall, diameter in ((3, 3.175), (0, 6.35), (10, 1)):
            with self.subTest(wall=wall, diameter=diameter):
                self.setUp()
                request = {'confirmed': True, 'axes': 'xyz', 'thickness': 10, 'wall': wall, 'tool_diameter': diameter}
                answer = self.post('api/setup/probe', request)
                status = self.idle()
                self.assertIsNone(status['error'], status['log'][-5:])
                reports = self.reports()
                self.assertEqual(len(reports), 6)
                machine, offset = self.follow(answer['lines'], reports)
                # Work zero is the corner of the stock and its surface, whatever the wall and the cutter.
                # (The controller reports a contact to 0.001 mm, so that is as near as the test can follow it.)
                # The corner of the stock is the plate's wall inside its faces.
                self.assertAlmostEqual(offset[0], FACES[0] + wall, delta=0.0011)
                self.assertAlmostEqual(offset[1], FACES[1] + wall, delta=0.0011)
                self.assertAlmostEqual(offset[2], TOP - 10, delta=0.0011)
                for axis, at in zip('xyz', machine):
                    self.assertAlmostEqual(status['position'][axis], at - offset['xyz'.index(axis)], delta=0.0011)
                # The cutter ends 10 mm inside the stock on both axes, above the plate.
                self.assertAlmostEqual(status['position']['x'], 10, 6)
                self.assertAlmostEqual(status['position']['y'], 10, 6)
                self.assertAlmostEqual(status['position']['z'], 15, 6)

    def test_a_probe_that_finds_nothing_raises_the_alarm(self):
        # The plate is 7.3 mm below the cutter: a travel of 5 mm ends in the air.
        self.post('api/setup/probe', {'confirmed': True, 'axes': 'z', 'thickness': 10, 'travel': 5})
        status = self.idle()
        self.assertEqual(status['error'], 'ALARM:5')
        self.assertEqual(status['state'], 'Fault')

    def test_probe_requests_are_checked(self):
        good = {'confirmed': True, 'axes': 'xyz', 'thickness': 10, 'wall': 3, 'tool_diameter': 3.175}
        self.refused('api/setup/probe', {**good, 'confirmed': False}, 'Confirm that the plate is on the stock')
        self.refused('api/setup/probe', {**good, 'axes': 'xy'}, 'Probe Z only, or X, Y and Z')
        self.refused('api/setup/probe', {**good, 'thickness': 0}, 'Plate thickness')
        self.refused('api/setup/probe', {**good, 'thickness': None}, 'Plate thickness')
        self.refused('api/setup/probe', {k: v for k, v in good.items() if k != 'tool_diameter'}, 'Cutter diameter')
        self.refused('api/setup/probe', {**good, 'wall': -1}, 'Plate wall')
        self.refused(
            'api/setup/probe',
            {**good, 'feed': 20, 'slow_feed': 10, 'travel': 30},
            'Each probe must finish within 10 seconds',
        )
        self.refused('api/setup/probe', {**good, 'feed': 100, 'inset': 40}, 'Each probe must finish within 10 seconds')
        self.refused('api/setup/probe', {**good, 'feed': 30, 'slow_feed': 40}, 'must not be faster than the first')
        self.assertEqual(self.reports(), [])
        # Not while a job runs, and not on a controller without a probe report.
        self.post('api/setup/macro', {'confirmed': True, 'lines': ['G4 P0'] * 100})
        self.refused('api/setup/probe', good, 'Controller must be idle')
        self.idle()

    def test_a_setting_is_written_and_read_back(self):
        answer = self.post('api/setup/setting', {'confirmed': True, 'key': '100', 'value': 80.125})
        self.assertEqual(answer['line'], '$100=80.125')
        self.assertEqual(self.idle()['settings']['100'], 80.125)
        self.assertEqual(self.post('api/setup/setting', {'confirmed': True, 'key': 21, 'value': 1})['line'], '$21=1')
        self.assertEqual(self.idle()['settings']['21'], 1)
        self.assertEqual(
            self.post('api/setup/setting', {'confirmed': True, 'key': '110', 'value': 2500.0})['line'], '$110=2500'
        )
        self.idle()
        self.refused('api/setup/setting', {'key': '100', 'value': 80}, 'Confirm the change')
        for key in ('', 'x', '1000', '1 0', None, 1.5):
            self.refused('api/setup/setting', {'confirmed': True, 'key': key, 'value': 1}, "Give the setting's number")
        for value in ('80', None, True, 1e9):
            self.refused('api/setup/setting', {'confirmed': True, 'key': '100', 'value': value}, 'must be a number')

    def test_a_macro_runs_plain_g_code_and_nothing_else(self):
        answer = self.post(
            'api/setup/macro', {'confirmed': True, 'lines': ['g90 ; absolute', '', 'G0 X12.5 Y-3', '  G0 Z4  ']}
        )
        self.assertEqual(answer['lines'], 3)
        status = self.idle()
        self.assertEqual((status['position']['x'], status['position']['y'], status['position']['z']), (12.5, -3, 4))
        self.assertEqual(status['log'][-3:], ['G90', 'G0 X12.5 Y-3', 'G0 Z4'])
        self.refused('api/setup/macro', {'lines': ['G0 X1']}, 'Confirm that the machine is clear')
        self.refused('api/setup/macro', {'confirmed': True, 'lines': []}, '1 to 100 lines')
        self.refused('api/setup/macro', {'confirmed': True, 'lines': ['G0 X1'] * 101}, '1 to 100 lines')
        self.refused('api/setup/macro', {'confirmed': True, 'lines': ['; only a note']}, 'no G-code in it')
        self.refused('api/setup/macro', {'confirmed': True, 'lines': [5]}, 'Each macro line is text')
        for bad in ('$100=5', '$H', 'G0 X1\nG0 X2', '%', 'G0 X1 (comment)', 'G1 X#1', '!'):
            self.refused('api/setup/macro', {'confirmed': True, 'lines': [bad]}, 'plain G-code words only')
        self.refused('api/setup/macro', {'confirmed': True, 'lines': ['12 G0']}, 'starts with a G-code word')
        self.refused('api/setup/macro', {'confirmed': True, 'lines': ['G0 X' + '1' * 80]}, 'at most 78 characters')

    def test_setup_needs_a_connection_and_the_session_token(self):
        self.post('api/disconnect', {})
        self.refused('api/setup/macro', {'confirmed': True, 'lines': ['G0 X1']}, 'Controller is not ready')
        request = urllib.request.Request(  # noqa: S310 - the loopback server under test
            self.server.url + 'api/setup/macro',
            data=b'{"confirmed": true, "lines": ["G0 X1"]}',
            headers={'Content-Type': 'application/json'},
        )
        with self.assertRaises(urllib.error.HTTPError) as caught:
            urllib.request.urlopen(request, timeout=10)  # noqa: S310 - the loopback server under test
        self.assertEqual(caught.exception.code, 403)


if __name__ == '__main__':
    unittest.main()


@unittest.skipUnless(STUDIO, 'kerfkite-studio is not built: build native/ into build/native')
class MarlinSetupTests(unittest.TestCase):
    """The same tools on a Marlin controller, which sets work zero with G92 and keeps its settings under M codes.

    The simulated Marlin's plate is where the Grbl simulator's is. Marlin does not report a probe's contact, so
    the test goes by the position the simulator holds, which /api/setup gives for the simulator only.
    """

    @classmethod
    def setUpClass(cls):
        cls.running = native_servers.studio(STUDIO)
        cls.server = cls.running.__enter__()

    @classmethod
    def tearDownClass(cls):
        cls.running.__exit__(None, None, None)

    def setUp(self):
        self.post('api/disconnect', {})
        self.post('api/connect', {'port': 'simulator', 'protocol': 'marlin'})
        self.idle()

    post = SetupTests.post
    refused = SetupTests.refused
    idle = SetupTests.idle

    def setup(self):
        with urllib.request.urlopen(self.server.url + 'api/setup', timeout=10) as response:  # noqa: S310 - loopback
            return json.loads(response.read())

    def test_the_setup_routes_report_marlin_and_its_settings(self):
        answer = self.setup()
        self.assertEqual((answer['touch_plate'], answer['settings'], answer['protocol']), (True, True, 'marlin'))
        self.assertEqual(answer['report']['settings']['M92'], {'X': 80, 'Y': 80, 'Z': 400})
        self.assertEqual(answer['report']['position'], [0, 0, 0])
        # The status the editor and the reference comparisons read is as it was.
        status = self.server.status()
        self.assertEqual((status['settings'], status['position'], status['readback']), ({}, None, {}))

    def test_z_alone_sets_zero_with_g92(self):
        answer = self.post('api/setup/probe', {'confirmed': True, 'axes': 'z', 'thickness': 12.7})
        self.assertEqual(
            answer['lines'],
            [
                'G21',
                'G91',
                'G38.2 Z-15.000 F150.0',
                'G0 Z1.000',
                'G38.2 Z-2.000 F40.0',
                'G92 Z12.700',
                'G0 Z5.000',
                'G90',
            ],
        )
        self.assertFalse(any(line.startswith('G10') for line in answer['lines']))
        status = self.idle()
        self.assertIsNone(status['error'], status['log'][-5:])
        # The cutter touched the plate 7.3 mm down, was told that is Z 12.7, and retracted 5 mm.
        self.assertEqual(self.setup()['report']['position'], [0, 0, 17.7])

    def test_a_corner_plate_puts_zero_on_the_corner_of_the_stock(self):
        for wall, diameter in ((3, 3.175), (0, 6.35), (10, 1)):
            with self.subTest(wall=wall, diameter=diameter):
                self.setUp()
                request = {'confirmed': True, 'axes': 'xyz', 'thickness': 10, 'wall': wall, 'tool_diameter': diameter}
                answer = self.post('api/setup/probe', request)
                status = self.idle()
                self.assertIsNone(status['error'], status['log'][-5:])
                self.assertEqual(sum(line.startswith('G92') for line in answer['lines']), 3)
                # The cutter ends 10 mm inside the stock on both axes and 15 mm above it, in the new coordinates:
                # so work zero is the corner of the stock at its surface, whatever the wall and the cutter.
                x, y, z = self.setup()['report']['position']
                self.assertAlmostEqual(x, 10, delta=0.0011)
                self.assertAlmostEqual(y, 10, delta=0.0011)
                self.assertAlmostEqual(z, 15, delta=0.0011)
                # Followed independently from the program: every relative move from the start, against where the
                # plate is. The cutter started 12.5 - wall left of the stock's edge and 9.5 - wall in front.
                moved = [0.0, 0.0]
                for line in answer['lines']:
                    for word in line.split()[1:]:
                        if line.startswith('G0') and word[0] in 'XY':
                            moved['XY'.index(word[0])] += float(word[1:])
                # The probing moves themselves end at the faces: left face less the cutter's radius.
                touch = [FACES[0] - diameter / 2, FACES[1] - diameter / 2]
                for k, face in enumerate(FACES):
                    edge = face + wall  # the stock's edge, from the cutter's start
                    out = -(15 + diameter / 2 + 5)  # the first move out past the face
                    after = touch[k] - 2 + (wall + diameter / 2 + 12)  # back off 2, then in over the stock
                    self.assertAlmostEqual(after - edge, 10, delta=0.0011)
                    self.assertLess(out, touch[k])
                self.assertEqual(len(moved), 2)

    def test_a_probe_that_finds_nothing_stops_the_program_before_zero_is_set(self):
        self.post('api/setup/probe', {'confirmed': True, 'axes': 'z', 'thickness': 10, 'travel': 5})
        status = self.idle()
        self.assertEqual(status['state'], 'Fault')
        self.assertIn('Failed to reach target', status['error'])
        probe = max(i for i, row in enumerate(status['log']) if 'G38.2' in row)
        self.assertFalse(any('G92' in row for row in status['log'][probe:]))

    def test_a_setting_is_written_read_back_and_stored_only_when_asked(self):
        answer = self.post('api/setup/setting', {'confirmed': True, 'code': 'M92', 'axis': 'X', 'value': 80.125})
        self.assertEqual(answer['line'], 'M92 X80.125')
        status = self.idle()
        self.assertIsNone(status['error'])
        self.assertEqual(self.setup()['report']['settings']['M92'], {'X': 80.125, 'Y': 80, 'Z': 400})
        self.assertFalse(any('M500' in row for row in status['log']))
        self.assertEqual(self.post('api/setup/setting', {'confirmed': True, 'store': True})['line'], 'M500')
        self.assertIn('echo:Settings Stored', self.idle()['log'])
        self.assertEqual(
            self.post('api/setup/setting', {'confirmed': True, 'code': 'M203', 'axis': 'Z', 'value': 8})['line'],
            'M203 Z8',
        )
        self.idle()
        self.assertEqual(self.setup()['report']['settings']['M203']['Z'], 8)
        self.refused('api/setup/setting', {'code': 'M92', 'axis': 'X', 'value': 80}, 'Confirm the change')
        for code in ('M500', 'M502', '$100', 'G92', None, 92):
            self.refused(
                'api/setup/setting', {'confirmed': True, 'code': code, 'axis': 'X', 'value': 80}, 'Choose the setting'
            )
        for axis in ('E', 'x', '', None):
            self.refused(
                'api/setup/setting', {'confirmed': True, 'code': 'M92', 'axis': axis, 'value': 80}, 'Choose the axis'
            )
        for value in (0, -1, '80', None, True, 1e9, 0.0001):
            self.refused(
                'api/setup/setting', {'confirmed': True, 'code': 'M92', 'axis': 'X', 'value': value}, "setting's value"
            )
        # A numbered Grbl setting means nothing here.
        self.refused('api/setup/setting', {'confirmed': True, 'key': '100', 'value': 80}, 'Choose the setting')
