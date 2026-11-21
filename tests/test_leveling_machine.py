"""Probe levelling on the simulator and through the workstation server."""

import re
import time
import unittest

from studio import leveling
from studio.cam import generate
from studio.controller import Controller
from studio.server import create_app
from test_cam import sample

HAS_CORE = leveling.available()
ENGRAVING = dict(depth=0.1, step=0.1, stock_thickness=1.6, paths=[[[10, 10], [60, 10], [60, 40], [10, 10]]])


def finished(controller, seconds=10):
    deadline = time.monotonic() + seconds
    while controller.status()['active'] and time.monotonic() < deadline:
        time.sleep(0.005)
    return controller.status()


class SimulatorProbeTests(unittest.TestCase):
    def setUp(self):
        self.ctl = Controller()
        self.addCleanup(self.ctl.disconnect)
        self.ctl.connect('simulator')

    def test_probe_stops_on_the_surface_and_reports_it(self):
        self.ctl.simulated_surface = lambda x, y: -0.3 + 0.01 * x
        self.ctl.start(['G0 Z2.000', 'G0 X10.000 Y5.000', 'G38.2 Z-1.000 F25.0', 'G0 Z2.000', 'G0 X20.000 Y5.000', 'G38.2 Z-1.000 F25.0'])  # fmt: skip
        status = finished(self.ctl)
        self.assertEqual((status['state'], status['probe_count'], status['error']), ('Idle', 2, None))
        self.assertEqual(self.ctl.probe_reports(), ['[PRB:10.000,5.000,-0.200:1]', '[PRB:20.000,5.000,-0.100:1]'])
        self.assertAlmostEqual(status['position']['z'], -0.1)
        self.assertIn('[PRB:20.000,5.000,-0.100:1]', status['log'])

    def test_probe_without_contact_raises_the_controller_alarm(self):
        self.ctl.simulated_surface = lambda x, y: -3.0
        self.ctl.start(['G0 Z2.000', 'G38.2 Z-1.000 F25.0', 'G0 Z2.000'])
        status = finished(self.ctl)
        self.assertEqual((status['state'], status['error'], status['probe_count']), ('Fault', 'ALARM:5', 0))
        self.ctl.command('reset')
        self.assertEqual(self.ctl.status()['state'], 'Idle')

    def test_setting_work_zero_moves_the_offset_and_reports_stay_in_machine_coordinates(self):
        self.ctl.simulated_surface = lambda x, y: -0.4
        self.ctl.start(['G0 Z2.000', 'G38.2 Z-1.000 F25.0', 'G10 L20 P1 Z0.000', 'G0 Z2.000', 'G0 X5.000 Y0.000', 'G38.2 Z-1.000 F25.0'])  # fmt: skip
        status = finished(self.ctl)
        self.assertEqual(self.ctl.probe_reports(), ['[PRB:0.000,0.000,-0.400:1]', '[PRB:5.000,0.000,-0.400:1]'])
        self.assertAlmostEqual(status['position']['z'], 0.0)  # the surface is now work Z zero
        self.ctl.start(['M5'])
        self.assertEqual(self.ctl.probe_reports(), [])  # reports belong to one job
        finished(self.ctl)
        self.ctl.command('zero')
        finished(self.ctl)
        self.assertEqual([self.ctl.status()['position'][axis] for axis in 'xyz'], [0.0, 0.0, 0.0])


class ControllerProbeReportTests(unittest.TestCase):
    def test_reports_from_a_controller_are_kept_for_the_running_job(self):
        c = Controller()
        c.connected = c.ready = True
        c.state = 'Idle'
        c.last_status = time.monotonic()
        c._receive('[PRB:0.000,0.000,-1.000:1]')  # nothing is running: not part of any job
        c.start(['G38.2 Z-1.000 F25.0', 'G0 Z2.000'])
        c.pending = c.queue.popleft()
        c._receive('[PRB:1.000,2.000,-3.456:1]')
        c._receive('ok')
        self.assertEqual(c.probe_reports(), ['[PRB:1.000,2.000,-3.456:1]'])
        self.assertEqual(c.status()['probe_count'], 1)
        self.assertIsNone(c.error)


@unittest.skipUnless(HAS_CORE, 'The compiled extension with the native core is not installed')
class LevelledRunTests(unittest.TestCase):
    def setUp(self):
        self.ctl = Controller()
        self.addCleanup(self.ctl.disconnect)
        self.client = create_app(self.ctl).test_client()
        self.headers = {'X-Studio-Token': self.client.get('/api/session').json['token']}
        self.project = {**sample(), **ENGRAVING}

    def post(self, path, data=None):
        return self.client.post(path, json=data or {}, headers=self.headers)

    def probe(self, **settings):
        settings = {'spacing': 30, **settings}  # a coarse grid keeps the simulated probing short
        result = self.post('/api/level/probe', {'confirmed': True, 'project': self.project, **settings})
        self.assertEqual(result.status_code, 200, result.json)
        finished(self.ctl)
        return result.json

    def test_probe_collect_and_run_follow_the_simulated_surface(self):
        self.post('/api/connect', {'port': 'simulator'})
        surface_z = self.ctl.simulated_surface
        started = self.probe(spacing=10)
        points = started['plan']['points']
        self.assertEqual(started['surface'], None)
        self.assertEqual(self.client.get('/api/level').json['plan']['probed'], len(points))
        collected = self.post('/api/level/collect').json
        surface = collected['surface']
        self.assertEqual(len(surface['offsets']), len(points))
        self.assertEqual(surface['offsets'][0], 0.0)  # the first point became work Z zero
        self.assertAlmostEqual(self.ctl.status()['position']['z'], 2.0)  # left at the clearance height
        zero = surface_z(*points[0])
        for (x, y), (column, row) in zip(points, self.ctl_order(started), strict=True):
            index = row * surface['columns'] + column
            self.assertAlmostEqual(surface['offsets'][index], round(surface_z(x, y), 3) - round(zero, 3), places=9)

        program = self.post('/api/level/program', {'project': self.project}).json['lines']
        plain = generate(self.project)['lines']
        self.assertGreater(len(program), len(plain))
        cuts = [re.fullmatch(r'G1 X(\S+) Y(\S+) F\S+ Z(\S+)', line) for line in program]
        cuts = [tuple(map(float, cut.groups())) for cut in cuts if cut]
        self.assertGreater(len(cuts), 40)
        for x, y, z in cuts:  # each piece is at the programmed depth below the surface at that place
            self.assertAlmostEqual(z, -0.1 + surface_z(x, y) - zero, delta=0.004)

        result = self.post('/api/run', {'confirmed': True, 'project': self.project, 'leveled': True})
        self.assertEqual((result.status_code, result.json['lines']), (200, len(program)))
        status = finished(self.ctl)
        self.assertEqual((status['state'], status['error'], status['progress']), ('Idle', None, 100))
        unlevelled = self.post('/api/run', {'confirmed': True, 'project': self.project})
        self.assertEqual(unlevelled.json['lines'], len(plain))

    def ctl_order(self, started):
        grid = started['plan']['grid']
        order = []
        for row in range(grid['rows']):
            columns = range(grid['columns'])
            order += [(column, row) for column in (reversed(columns) if row % 2 else columns)]
        return order

    def test_levelled_run_needs_a_probed_surface(self):
        self.post('/api/connect', {'port': 'simulator'})
        result = self.post('/api/run', {'confirmed': True, 'project': self.project, 'leveled': True})
        self.assertEqual(result.status_code, 400)
        self.assertIn('Probe the stock surface', result.json['error'])
        self.assertIn('Probe the stock surface first', self.post('/api/level/collect').json['error'])
        self.assertIn('true or false', self.post('/api/run', {'confirmed': True, 'project': self.project, 'leveled': 1}).json['error'])  # fmt: skip

    def test_probing_is_refused_without_confirmation_or_for_other_jobs(self):
        self.post('/api/connect', {'port': 'simulator'})
        cases = [
            ({'project': self.project}, 'Confirm that the probe is connected'),
            ({'confirmed': True, 'project': {**self.project, 'mode': 'laser'}}, 'applies to router jobs'),
            ({'confirmed': True, 'project': self.project, 'rotary': {}}, 'does not apply to rotary'),
            ({'confirmed': True, 'project': {**self.project, 'protocol': 'marlin'}}, 'Grbl-family'),
            ({'confirmed': True, 'project': self.project, 'feed': 1}, 'within 10 seconds'),
            ({'confirmed': True, 'project': self.project, 'spacing': 0.5}, 'Probe spacing must be between'),
            ({'confirmed': True}, 'Supply a design project'),
        ]
        for body, message in cases:
            with self.subTest(message):
                result = self.post('/api/level/probe', body)
                self.assertEqual(result.status_code, 400)
                self.assertIn(message, result.json['error'])
        self.assertFalse(self.ctl.status()['active'])
        self.assertIsNone(self.client.get('/api/level').json['plan'])
        self.post('/api/disconnect')
        self.assertEqual(self.post('/api/level/probe', {'confirmed': True, 'project': self.project}).status_code, 400)

    def test_an_incomplete_or_uneven_probing_is_not_accepted(self):
        self.post('/api/connect', {'port': 'simulator'})
        self.ctl.simulated_surface = lambda x, y: -0.2 if x < 40 else -5.0  # the stock ends part of the way across
        self.post('/api/level/probe', {'confirmed': True, 'project': self.project})
        status = finished(self.ctl)
        self.assertEqual(status['error'], 'ALARM:5')
        self.assertIn('Probing did not finish: ALARM:5', self.post('/api/level/collect').json['error'])
        self.post('/api/command', {'action': 'reset'})
        self.assertIsNone(self.client.get('/api/level').json['plan'])  # a reset forgets the plan

        self.ctl.simulated_surface = lambda x, y: 0.02 * x
        self.probe()
        self.assertIn('more than the 0.5 mm limit', self.post('/api/level/collect', {'limit': 0.5}).json['error'])
        self.assertIsNone(self.client.get('/api/level').json['surface'])
        self.assertEqual(self.post('/api/level/collect').status_code, 200)

    def test_the_surface_is_forgotten_when_work_zero_or_the_tool_may_have_changed(self):
        for change in (
            lambda: self.post('/api/command', {'action': 'zero'}),
            lambda: self.post('/api/command', {'action': 'home'}),
            lambda: self.post('/api/head', {'mode': 'router', 'confirmed': True}),
            lambda: self.post('/api/level/clear'),
            lambda: self.post('/api/disconnect'),
            lambda: self.post('/api/connect', {'port': 'simulator'}),
        ):
            self.post('/api/connect', {'port': 'simulator'})
            self.probe()
            self.assertIsNotNone(self.post('/api/level/collect').json['surface'])
            self.post('/api/command', {'action': 'jog', 'axis': 'X', 'distance': 1, 'feed': 100})
            finished(self.ctl)
            self.assertIsNotNone(self.client.get('/api/level').json['surface'])  # a jog keeps it
            self.assertEqual(change().status_code, 200)
            state = self.client.get('/api/level').json
            self.assertEqual((state['plan'], state['surface']), (None, None))


if __name__ == '__main__':
    unittest.main()
