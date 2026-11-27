"""Pen drawing: the programs for a pen lifted by the Z axis and for one lifted from the spindle or laser output.

The programs are followed here by a small interpreter of the test's own. For a Z pen it records where the pen is
down (Z below zero); for a servo pen, where the output last set is the pen-down value. Either way the lines the
pen draws must be the design's lines, the pen must be up for every travel, and the spindle must never be started
with a pen lifted by Z.
"""

import math
import sys
import time
import unittest
from pathlib import Path

from kerfkite_core import CLI, generate

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from validation import native_servers  # noqa: E402 - after the path of the checkout

try:
    STUDIO = native_servers.program('kerfkite-studio', None, 'KERFKITE_STUDIO')
except SystemExit:
    STUDIO = None

NOT_BUILT = 'kerfkite-cam is not built: set KERFKITE_CAM or build native/ into build/native'

PATHS = [[[10, 10], [50, 10], [50, 40]], [[20, 20], [30, 30]]]


def project(**changes):
    base = dict(
        mode='pen',
        protocol='grbl',
        width=200,
        height=150,
        feed=1500,
        plunge=600,
        power=1000,
        safe=3,
        depth=1,
        paths=PATHS,
    )
    base.update(changes)
    return base


def drawn(lines, down_value=None, up_value=None):
    """The strokes a program draws, as lists of points; checks the pen is up whenever it travels."""
    x = y = None
    z = None
    output = 0  # the S value of the spindle or laser output, 0 when it is off
    strokes = []
    for line in lines:
        words = line.split()
        if words[0] == 'M3':
            output = float(words[1][1:])
        elif words[0] == 'M5':
            output = 0
        if words[0] not in ('G0', 'G1'):
            continue
        at = {w[0]: float(w[1:]) for w in words[1:] if w[0] in 'XYZ'}
        nx, ny, nz = at.get('X', x), at.get('Y', y), at.get('Z', z)
        moves = nx != x or ny != y
        if down_value is None:
            down = z is not None and z < 0 and nz < 0
        else:
            assert output in (down_value, up_value), f'the holder is at neither position: {line}'
            down = output == down_value
        if moves and words[0] == 'G0':
            assert not down and (down_value is not None or (z is not None and z > 0)), (
                f'travel with the pen down: {line}'
            )
        if moves and words[0] == 'G1' and down:
            if strokes and strokes[-1][-1] == (x, y):
                strokes[-1].append((nx, ny))
            else:
                strokes.append([(x, y), (nx, ny)])
        x, y, z = nx, ny, nz
    return strokes


def as_points(paths):
    return [[tuple(float(v) for v in point) for point in path] for path in paths]


@unittest.skipUnless(CLI, NOT_BUILT)
class PenTests(unittest.TestCase):
    def test_a_z_pen_presses_draws_and_lifts_with_the_spindle_off(self):
        job = generate(project())
        self.assertEqual(job['mode'], 'pen')
        self.assertEqual(
            job['lines'],
            [
                'G21', 'G90', 'G17', 'G94', 'G54', 'M5', 'G0 Z3.000',
                'G0 Z3.000', 'G0 X10.000 Y10.000', 'G1 Z-1.000 F600.0', 'G1 X50.000 Y10.000 F1500.0', 'G1 X50.000 Y40.000 F1500.0', 'G0 Z3.000',
                'G0 Z3.000', 'G0 X20.000 Y20.000', 'G1 Z-1.000 F600.0', 'G1 X30.000 Y30.000 F1500.0', 'G0 Z3.000',
                'M5',
            ],
        )  # fmt: skip
        self.assertFalse(any(line.startswith(('M3', 'M4')) for line in job['lines']))
        self.assertEqual(drawn(job['lines']), as_points(PATHS))
        self.assertAlmostEqual(job['cut_length'], 40 + 30 + math.hypot(10, 10), 2)
        self.assertTrue(any('spindle stays off' in warning for warning in job['warnings']))

    def test_a_servo_pen_is_worked_from_the_output_and_never_moves_z(self):
        job = generate(project(pen_lift='servo'))
        self.assertEqual(
            job['lines'],
            [
                'G21', 'G90', 'G17', 'G94', 'G54', 'M5', 'M5', 'G4 P0.200',
                'G0 X10.000 Y10.000', 'M3 S1000', 'G4 P0.200', 'G1 X50.000 Y10.000 F1500.0', 'G1 X50.000 Y40.000 F1500.0', 'M5', 'G4 P0.200',
                'G0 X20.000 Y20.000', 'M3 S1000', 'G4 P0.200', 'G1 X30.000 Y30.000 F1500.0', 'M5', 'G4 P0.200',
                'M5',
            ],
        )  # fmt: skip
        self.assertFalse(any(' Z' in line or line.startswith('M4') for line in job['lines']))
        self.assertEqual(drawn(job['lines'], 1000, 0), as_points(PATHS))
        self.assertTrue(any('Z is not moved' in warning for warning in job['warnings']))

    def test_a_holder_with_an_up_value_is_never_switched_off_between_strokes(self):
        job = generate(project(pen_lift='servo', pen_up_power=300, pen_dwell=0.35))
        self.assertEqual(drawn(job['lines'], 1000, 300), as_points(PATHS))
        body = job['lines'][6:]
        self.assertEqual(body[0], 'M3 S300')
        self.assertNotIn('M5', body)  # only the M5 of the start-up lines, before the holder is raised
        self.assertEqual(body[-2:], ['M3 S300', 'G4 P0.350'])
        # No pause at all when it is set to 0.
        self.assertFalse(
            any(line.startswith('G4') for line in generate(project(pen_lift='servo', pen_dwell=0))['lines'])
        )

    def test_the_pause_is_written_in_the_controllers_units(self):
        marlin = generate(project(pen_lift='servo', protocol='marlin'))['lines']
        self.assertIn('G4 P200', marlin)  # Marlin's G4 P is milliseconds
        self.assertNotIn('G4 P0.200', marlin)
        with self.assertRaisesRegex(ValueError, 'Select configured dwell units'):
            generate(project(pen_lift='servo', protocol='mach3'))
        self.assertIn(
            'G4 P200', generate(project(pen_lift='servo', protocol='mach3', dwell_units='milliseconds'))['lines']
        )
        self.assertIn(
            'G4 P0.200', generate(project(pen_lift='servo', protocol='mach3', dwell_units='seconds'))['lines']
        )
        self.assertIn('G4 P0.200', generate(project(pen_lift='servo', protocol='linuxcnc'))['lines'])
        # An RS274 controller's program ends with M30, after the pen is up.
        self.assertEqual(generate(project(protocol='linuxcnc'))['lines'][-3:], ['G0 Z3.000', 'M5', 'M30'])

    def test_going_over_the_lines_again_repeats_them_at_the_same_height(self):
        once, twice = generate(project()), generate(project(passes=2))
        self.assertEqual(drawn(twice['lines']), as_points(PATHS) * 2)
        self.assertAlmostEqual(twice['cut_length'], 2 * once['cut_length'], 2)
        heights = {line for line in twice['lines'] if line.startswith('G1 Z')}
        self.assertEqual(heights, {'G1 Z-1.000 F600.0'})
        # The step down of a cutter means nothing to a pen.
        self.assertEqual(generate(project(step=0.25, depth=1))['lines'], once['lines'])

    def test_a_broad_nib_draws_each_stroke_side_by_side_along_its_edge(self):
        line = [[20, 20], [60, 20]]
        job = generate(project(paths=[line], pen_nib=1.2, pen_nib_angle=90, pen_tip=0.4))
        strokes = drawn(job['lines'])
        # 1.2 mm of nib with a 0.4 mm line: four strokes, 0.4 mm apart, across the nib's edge (here straight up).
        self.assertEqual(
            [sorted(stroke) for stroke in strokes], [[(20.0, y), (60.0, y)] for y in (19.4, 19.8, 20.2, 20.6)]
        )
        # There and back, to save travel.
        self.assertEqual([stroke[0][0] for stroke in strokes], [20.0, 60.0, 20.0, 60.0])
        # A stroke along the nib's edge is thin: its copies lie on one line. One across it is as wide as the nib.
        for angle, direction, expected in (
            (0, (40, 0), 0.0),
            (0, (0, 40), 1.2),
            (45, (40, 40), 0.0),
            (45, (40, -40), 1.2),
        ):
            path = [[80, 80], [80 + direction[0], 80 + direction[1]]]
            strokes = drawn(generate(project(paths=[path], pen_nib=1.2, pen_nib_angle=angle, pen_tip=0.4))['lines'])
            length = math.hypot(*direction)
            across = [(-direction[1] * (s[0][0] - 80) + direction[0] * (s[0][1] - 80)) / length for s in strokes]
            self.assertAlmostEqual(max(across) - min(across), expected, 2, f'nib at {angle}, stroke {direction}')
        # A pen line as wide as the nib still draws both edges; no nib is the plain line.
        self.assertEqual(len(drawn(generate(project(paths=[line], pen_nib=0.4, pen_tip=0.4))['lines'])), 2)
        self.assertEqual(generate(project(pen_nib=0))['lines'], generate(project())['lines'])
        with self.assertRaisesRegex(ValueError, 'outside the work area'):
            generate(project(paths=[[[0.2, 10], [50, 10]]], pen_nib=2, pen_nib_angle=0))

    def test_a_pen_fills_a_shape_with_hatching(self):
        square = [[10, 10], [30, 10], [30, 20], [10, 20], [10, 10]]
        job = generate(project(paths=[square], operation='fill', interval=2, hatch_angle=0))
        strokes = drawn(job['lines'])
        self.assertEqual(sorted({stroke[0][1] for stroke in strokes}), [10.0, 12.0, 14.0, 16.0, 18.0, 20.0])
        for stroke in strokes:
            self.assertEqual(sorted(point[0] for point in stroke), [10.0, 30.0])
        # A router still may not.
        with self.assertRaisesRegex(ValueError, 'Hatch fill requires laser mode'):
            generate(project(mode='router', paths=[square], operation='fill'))

    def test_a_z_pen_can_vary_its_pressure_along_a_stroke(self):
        job = generate(project(paths=[[[10, 10], [30, 10], [50, 10]]], path_depths=[[-0.2, -1.5, -0.2]]))
        self.assertIn('G1 X30.000 Y10.000 F600.0 Z-1.500', job['lines'])
        with self.assertRaisesRegex(ValueError, 'needs the Z axis to lift the pen'):
            generate(project(pen_lift='servo', paths=[[[10, 10], [30, 10]]], path_depths=[[-0.2, -1.0]]))

    def test_what_a_pen_cannot_do_is_refused(self):
        for operation in ('pocket', 'inside', 'outside', 'vcarve', 'drill'):
            with self.assertRaisesRegex(ValueError, 'A pen draws lines or hatch fill'):
                generate(project(operation=operation))
        with self.assertRaisesRegex(ValueError, 'Pen lift must be z or servo'):
            generate(project(pen_lift='magnet'))
        with self.assertRaisesRegex(ValueError, 'Holding tabs require router'):
            generate(project(tabs={'count': 2}))
        with self.assertRaisesRegex(ValueError, 'Pen pause must be between 0 and 5'):
            generate(project(pen_lift='servo', pen_dwell=9))
        with self.assertRaisesRegex(ValueError, 'Nib width must be between 0 and 20'):
            generate(project(pen_nib=50))
        with self.assertRaisesRegex(ValueError, 'Choose router, laser, knife or pen'):
            generate(project(mode='plasma'))

    def test_an_ordered_job_draws_each_object_with_its_own_settings(self):
        operations = [
            {'paths': [[[10, 10], [50, 10]]]},
            {'paths': [[[10, 30], [30, 30], [30, 40], [10, 40], [10, 30]]], 'operation': 'fill', 'interval': 5},
            {'paths': [[[60, 60], [90, 60]]], 'pen_nib': 0.8, 'pen_nib_angle': 90},
        ]
        job = generate({**project(paths=[]), 'operations': operations})
        strokes = drawn(job['lines'])
        self.assertEqual(strokes[0], [(10.0, 10.0), (50.0, 10.0)])
        self.assertEqual(len(strokes), 1 + 3 + 3)
        self.assertFalse(any(line.startswith(('M3', 'M4')) for line in job['lines']))


@unittest.skipUnless(STUDIO, 'kerfkite-studio is not built: build native/ into build/native')
class PenMachineTests(unittest.TestCase):
    """A pen job through the workstation server and its simulated controller."""

    @classmethod
    def setUpClass(cls):
        cls.running = native_servers.studio(STUDIO)
        cls.server = cls.running.__enter__()
        cls.server.post('api/disconnect', {})
        cls.server.post('api/connect', {'port': 'simulator'})

    @classmethod
    def tearDownClass(cls):
        cls.running.__exit__(None, None, None)

    def idle(self):
        for _ in range(300):
            status = self.server.status()
            if not status['active'] and status['state'] == 'Idle':
                return status
            time.sleep(0.02)
        self.fail(f'the controller did not come to rest: {status["state"]}')

    def test_the_head_can_be_set_to_a_pen_which_is_spindle_mode(self):
        self.idle()
        self.server.post('api/head', {'mode': 'pen', 'confirmed': True})
        status = self.idle()
        self.assertTrue(any('$32=0' in row for row in status['log']), status['log'][-6:])

    def test_a_pen_job_runs_and_leaves_the_pen_lifted(self):
        self.idle()
        request = {**project(stock_thickness=3), 'confirmed': True}
        answer = self.server.post('api/run', {'project': request, 'confirmed': True})
        self.assertEqual(answer['lines'], len(generate(project())['lines']))
        status = self.idle()
        self.assertIsNone(status['error'])
        position = status['position']
        height = position['z'] if isinstance(position, dict) else position[2]
        self.assertAlmostEqual(height, 3, 6)
        self.assertFalse(any(row.lstrip('> ').startswith(('M3', 'M4')) for row in status['log']))


if __name__ == '__main__':
    unittest.main()
