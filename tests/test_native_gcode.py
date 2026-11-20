"""The optional C++ move loops must reproduce the Python reference exactly: text, numbers and number types."""

import json
import math
import os
import random
import struct
import unittest
from unittest.mock import patch

from studio import cam, rotary
from studio.cam import generate
from studio.cam_moves import CutRun, PackedSegments
from studio.rotary import generate_rotary

try:
    from studio import _relief_native as native
except ImportError:
    native = None

HAS_NATIVE = native is not None and hasattr(native, 'gcode_moves')


def exact(value):
    """A canonical form that keeps the difference between 0 and 0.0, and between None and a number."""
    if isinstance(value, dict):
        return {key: exact(item) for key, item in value.items()}
    if isinstance(value, PackedSegments):
        return ['packed', [exact(row) for row in value]]
    if isinstance(value, CutRun):
        return ['run', value.start, value.stop, repr(value.feed), value.variable_z, value.laser]
    if isinstance(value, (list, tuple)):
        return [exact(item) for item in value]
    if isinstance(value, float):
        return 'f' + repr(value)
    return value


def wave(count, x0, y0, width, height, phase=0.0):
    return [
        [x0 + width * i / (count - 1), y0 + height * (0.5 + 0.5 * math.sin(phase + i * 0.37))] for i in range(count)
    ]


def base(**changes):
    project = dict(
        mode='router',
        protocol='grbl',
        width=300,
        height=200,
        feed=900,
        plunge=250,
        power=12000,
        safe=5,
        depth=2,
        step=1,
    )
    project['paths'] = [wave(40, 10, 10, 250, 60), wave(25, 20, 90, 200, 80, 1.3), [[5, 5], [5, 5.0625], [6.5, 5]]]
    project.update(changes)
    return project


def relief_project():
    paths = [wave(60, 10, 20 + 10 * row, 260, 6, row) for row in range(8)]
    depths = [[-3 * (0.5 + 0.5 * math.cos(0.21 * i + row)) for i in range(60)] for row in range(8)]
    return base(paths=paths, path_depths=depths, depth=3, step=1.5)


def laser_project(**changes):
    project = base(mode='laser', power=800, passes=2)
    project.pop('depth'), project.pop('step'), project.pop('plunge'), project.pop('safe')
    project.update(changes)
    return project


PROJECTS = {
    'router levels': base,
    'relief depths': relief_project,
    'laser two passes': laser_project,
    'laser leads and ramps': lambda: laser_project(
        laser_lead_in=2, laser_lead_out=1.5, laser_ramp_in=3, laser_ramp_out=2, passes=1
    ),
}


@unittest.skipUnless(HAS_NATIVE, 'Optional C++ extension is not installed')
class ExactNumberTextTests(unittest.TestCase):
    def values(self):
        rng = random.Random(20261121)
        for decimals in range(10):
            for k in range(-40, 40):
                tie = (2 * k + 1) / (2 * 10**decimals)
                yield from (tie, math.nextafter(tie, math.inf), math.nextafter(tie, -math.inf))
        for k in range(-512, 512):
            yield from (k / 16, k / 64, k / 1024)  # binary fractions are exact ties at three decimals
        for _ in range(20000):
            yield rng.uniform(-2000, 2000)
            yield rng.uniform(-1, 1) * 10 ** rng.randint(-12, 9)
            yield struct.unpack('<d', struct.pack('<Q', rng.getrandbits(64)))[0]
        yield from (0.0, -0.0, 5e-324, -5e-324, 0.5, 1.5, 2.5, -0.5, 0.0625, -0.0004, 1e15, 123456789012.3455)

    def test_fixed_text_matches_python_formatting(self):
        compared = 0
        for value in self.values():
            if not math.isfinite(value):
                continue
            for decimals in (0, 1, 3, 6, 9):
                text = native.fixed_text(value, decimals)
                if text is not None:  # None: outside the exact range, where the caller uses Python
                    compared += 1
                    self.assertEqual(text, format(value, f'.{decimals}f'), (value, decimals))
        self.assertGreater(compared, 250000)

    def test_rounding_matches_python_round_to_the_bit(self):
        for value in self.values():
            if not math.isfinite(value):
                continue
            for decimals in (0, 1, 3):
                result = native.rounded_value(value, decimals)
                if result is not None:
                    expected = float(round(value, decimals))
                    self.assertEqual(struct.pack('<d', result), struct.pack('<d', expected), (value, decimals))

    def test_out_of_range_values_are_declined_not_approximated(self):
        self.assertIsNone(native.fixed_text(1e308, 3))
        self.assertIsNone(native.fixed_text(1.9e10, 9))
        self.assertIsNone(native.fixed_text(float('nan'), 3))
        self.assertIsNone(native.fixed_text(float('inf'), 3))
        self.assertIsNone(native.fixed_text(1.0, 10))
        self.assertEqual(native.fixed_text(1.8e10, 9), format(1.8e10, '.9f'))


@unittest.skipUnless(HAS_NATIVE, 'Optional C++ extension is not installed')
class MoveLoopParityTests(unittest.TestCase):
    def both(self, project, **options):
        reference = generate(project, backend='python', **options)
        accelerated = generate(project, backend='native', **options)
        self.assertEqual(exact(accelerated), exact(reference))
        return reference

    def test_every_mode_and_segment_form_matches_the_reference(self):
        for name, build in PROJECTS.items():
            for options in ({}, {'packed_segments': True}, {'_rotary_blocks': True}):
                with self.subTest(name, **options):
                    results = []
                    loop = cam._native_moves

                    def recording(*args, results=results, loop=loop):
                        results.append(loop(*args))
                        return results[-1]

                    with patch.object(cam, '_native_moves', recording):
                        job = self.both(build(), **options)
                    self.assertTrue(results)
                    self.assertNotIn(None, results, 'the C++ loop declined a plain job')
                    self.assertGreater(job['cut_length'], 0)

    def test_list_rows_keep_the_reference_number_types(self):
        laser = generate(laser_project(), backend='native')['segments'][0]
        self.assertIs(type(laser[4]), int)  # a laser move has no Z: the reference stores integer zero
        self.assertIs(type(laser[6]), int)
        self.assertIs(type(laser[5]), float)
        router = generate(base(), backend='native')['segments'][0]
        self.assertIs(type(router[4]), float)
        self.assertIsNone(router[5])
        self.assertEqual(json.dumps(generate(base(), backend='native')), json.dumps(generate(base(), backend='python')))

    def test_perforated_and_bridged_laser_cuts_with_integer_zero_power_match(self):
        for pattern in ('perforate', 'bridges'):
            with self.subTest(pattern):
                closed = [[[20, 20], [120, 20], [120, 90], [20, 90], [20, 20]]]
                project = laser_project(passes=1, laser_pattern=pattern, laser_gap=1.5, laser_dash=4, paths=closed)
                reference = generate(project, backend='python')
                # Beam-off intervals carry integer zero power in the reference; the row types must survive.
                self.assertIn(int, {type(row[5]) for row in reference['segments']})
                self.assertEqual(exact(generate(project, backend='native')), exact(reference))

    def test_random_jobs_match_the_reference(self):
        rng = random.Random(7)
        for case in range(25):
            paths, depths = [], []
            for _ in range(rng.randint(1, 6)):
                count = rng.randint(2, 40)
                path = [[rng.uniform(0, 300), rng.uniform(0, 200)] for _ in range(count)]
                if rng.random() < 0.3:
                    path[rng.randrange(1, count)] = list(path[0])  # a repeated point: a zero-length move
                paths.append(path)
                depths.append([-rng.uniform(0, 4) for _ in range(count)])
            project = base(paths=paths, depth=4, step=rng.choice([0.5, 1, 4]), feed=rng.uniform(100, 3000))
            if case % 2:
                project['path_depths'] = depths
            for options in ({}, {'packed_segments': True}, {'_rotary_blocks': True}):
                with self.subTest(case=case, **options):
                    self.both(project, **options)

    def test_inputs_outside_the_plain_case_fall_back_to_the_reference(self):
        class Float(float):
            pass

        # The C++ loop accepts exact floats and ints only. Called directly with a float subclass it declines.
        paths = [[[Float(1.0), 2.0], [3.0, 4.0]]]
        self.assertIsNone(native.gcode_moves(paths, None, [-1.0], False, False, False, 5.0, 100.0, 300.0))
        self.assertIsNone(native.gcode_moves([[[1.0, 2.0]]], None, [-1.0], False, False, False, 5.0, 100.0, 300.0))
        self.assertIsNone(
            native.gcode_moves([[[1.0, 2.0], [math.inf, 4.0]]], None, [-1.0], False, False, False, 5.0, 100.0, 300.0)
        )
        self.assertIsNone(native.gcode_moves(paths[:0], None, [-1.0], False, False, False, 5.0, 100.0, 300.0))
        accepted = native.gcode_moves([[[1.0, 2.0], [3.0, 4.0]]], None, [-1.0], False, False, False, 5.0, 100.0, 300.0)
        self.assertEqual(
            accepted[0].split('\n'),
            ['G0 Z5.000', 'G0 X1.000 Y2.000', 'G1 Z-1.000 F100.0', 'G1 X3.000 Y4.000 F300.0', 'G0 Z5.000'],
        )
        # A declined loop leaves the job to Python, with the same result.
        with patch.object(cam, '_native_moves', return_value=None):
            self.assertEqual(exact(generate(base(), backend='native')), exact(generate(base(), backend='python')))


@unittest.skipUnless(HAS_NATIVE, 'Optional C++ extension is not installed')
class PlainInputTests(unittest.TestCase):
    """The C++ input checks accept only the plain case and leave every other input, and its message, to Python."""

    def test_plain_points_are_copied_as_floats(self):
        source = [[[1, 2.5], [3.0, 4]], [[0, 0], [10.0, 20.0], [5, 5]]]
        clean = native.plain_points(source, 10.0, 20.0, 100)
        self.assertEqual(clean, source)
        self.assertEqual({type(v) for path in clean for point in path for v in point}, {float})
        self.assertIs(clean[0][0][1], source[0][0][1])  # float(x) is x for an exact float
        self.assertIsNot(clean[0], source[0])

    def test_anything_else_is_declined(self):
        good = [[1.0, 2.0], [3.0, 4.0]]
        for name, paths in {
            'not a list': ([tuple(good)]),
            'tuple point': [[(1.0, 2.0), [3.0, 4.0]]],
            'one point': [[[1.0, 2.0]]],
            'three numbers': [[[1.0, 2.0, 3.0], [3.0, 4.0]]],
            'bool': [[[True, 2.0], [3.0, 4.0]]],
            'numeric text': [[['1.0', 2.0], [3.0, 4.0]]],
            'outside the work area': [[[1.0, 2.0], [10.5, 4.0]]],
            'negative': [[[-0.1, 2.0], [3.0, 4.0]]],
            'not finite': [[[math.nan, 2.0], [3.0, 4.0]]],
            'huge int': [[[10**400, 2.0], [3.0, 4.0]]],
        }.items():
            with self.subTest(name):
                self.assertIsNone(native.plain_points(paths, 10.0, 20.0, 100))
        self.assertIsNone(native.plain_points([good, good], 10.0, 20.0, 3))  # more points than the limit
        self.assertIsNone(native.plain_points((good,), 10.0, 20.0, 100))

    def test_declined_input_gets_the_reference_message(self):
        cases = {
            'numeric text is accepted by the reference': [[['1.5', 2.0], [3.0, 4.0]]],
            'bool': [[[True, 2.0], [3.0, 4.0]]],
            'outside': [[[1.0, 2.0], [900.0, 4.0]]],
            'short': [[[1.0, 2.0]]],
        }
        for name, paths in cases.items():
            with self.subTest(name):
                outcomes = []
                for backend in ('python', 'native'):
                    try:
                        outcomes.append(('ok', exact(generate(base(paths=paths), backend=backend))))
                    except ValueError as error:
                        outcomes.append(('error', str(error)))
                self.assertEqual(outcomes[0], outcomes[1])

    def test_plain_depths(self):
        paths = [[[0.0, 0.0], [1.0, 1.0]], [[0.0, 0.0], [1.0, 1.0], [2.0, 2.0]]]
        depths = [[-1, -0.5], [0, -50.0, -2.25]]
        clean = native.plain_depths(depths, paths)
        self.assertEqual(clean, depths)
        self.assertEqual({type(v) for row in clean for v in row}, {float})
        for name, bad in {
            'above the surface': [[-1, 0.5], [0, -1, -2]],
            'too deep': [[-1, -50.5], [0, -1, -2]],
            'wrong length': [[-1], [0, -1, -2]],
            'missing path': [[-1, -0.5]],
            'bool': [[-1, False], [0, -1, -2]],
            'tuple row': [(-1, -0.5), [0, -1, -2]],
        }.items():
            with self.subTest(name):
                self.assertIsNone(native.plain_depths(bad, paths))

    def test_packed_rows_unpack_to_the_same_lists(self):
        job = generate(relief_project(), packed_segments=True, backend='python')
        self.assertEqual(exact(job['segments'].to_list()), exact(list(job['segments'])))
        laser = generate(laser_project(), packed_segments=True, backend='python')
        self.assertEqual(exact(laser['segments'].to_list()), exact(list(laser['segments'])))
        self.assertIsNone(native.segment_rows(b'12345'))


class BackendSelectionTests(unittest.TestCase):
    def test_backend_names_are_validated(self):
        with self.assertRaisesRegex(ValueError, 'auto, python or native'):
            generate(base(), backend='fast')
        with self.assertRaisesRegex(ValueError, 'auto, python or native'):
            generate_rotary(base(), dict(diameter=100, wrap_axis='X', postprocessor='grblhal'), backend='fast')

    def test_missing_extension_uses_python_unless_native_is_demanded(self):
        with patch.object(cam, '_native_moves', None), patch.object(rotary, '_native_run', None):
            self.assertEqual(exact(generate(base())), exact(generate(base(), backend='python')))
            with self.assertRaisesRegex(ValueError, 'not installed'):
                generate(base(), backend='native')
            with self.assertRaisesRegex(ValueError, 'not installed'):
                generate_rotary(base(), dict(diameter=100, wrap_axis='X', postprocessor='grblhal'), backend='native')

    @unittest.skipUnless(HAS_NATIVE, 'Optional C++ extension is not installed')
    def test_environment_switch_selects_the_python_loops(self):
        options = dict(diameter=100, wrap_axis='X', postprocessor='grblhal')
        with patch.object(cam, '_native_moves', wraps=cam._native_moves) as moves:
            with patch.object(rotary, '_native_run', wraps=rotary._native_run) as runs:
                with patch.dict(os.environ, {'KERFKITE_GCODE_BACKEND': 'python'}):
                    generate(base())
                    generate_rotary(base(), options)
                self.assertFalse(moves.called or runs.called)
                with patch.dict(os.environ, {'KERFKITE_GCODE_BACKEND': ''}):
                    generate_rotary(base(), options)
                self.assertTrue(moves.called and runs.called)


@unittest.skipUnless(HAS_NATIVE, 'Optional C++ extension is not installed')
class RotaryRunParityTests(unittest.TestCase):
    def outcome(self, project, options, backend):
        try:
            return 'ok', exact(generate_rotary(project, options, backend=backend))
        except ValueError as error:
            return 'error', str(error)

    def both(self, project, options):
        reference = self.outcome(project, options, 'python')
        self.assertEqual(self.outcome(project, options, 'native'), reference)
        return reference

    def test_option_combinations_match_the_reference(self):
        projects = {'router': base(), 'relief': relief_project(), 'laser': laser_project(passes=1)}
        for name, project in projects.items():
            for wrap in 'XY':
                for reference in ('surface', 'cut'):
                    for post in ('xpro-v5', 'grblhal', 'linuxcnc'):
                        if post == 'linuxcnc' and name == 'laser':
                            continue
                        for direction in (1, -1):
                            options = dict(
                                diameter=80,
                                wrap_axis=wrap,
                                postprocessor=post,
                                feed_reference=reference,
                                direction=direction,
                                angle_offset=12.5,
                                fixture=dict(axial_min=-1, axial_max=400, min_radius=5),
                            )
                            source = dict(project, protocol='linuxcnc') if post == 'linuxcnc' else project
                            with self.subTest(name, wrap=wrap, reference=reference, post=post, direction=direction):
                                with patch.object(rotary, '_native_run', wraps=rotary._native_run) as run:
                                    status, _ = self.both(source, options)
                                self.assertEqual(status, 'ok')
                                self.assertTrue(run.called)

    def test_conditions_the_translator_declines_give_the_reference_error(self):
        plain = dict(diameter=80, wrap_axis='X', postprocessor='grblhal')
        # Each job starts inside its limits and breaks them part-way along a cutting run, so the C++ translator
        # is entered and must hand the run back for the reference to raise.
        deepening = base(paths=[wave(30, 10, 20, 200, 10)], path_depths=[[-0.1 - 0.12 * i for i in range(30)]], depth=4)
        leaving = base(paths=[[[10, 20], [30, 20], [90, 20]]])
        turning = base(paths=[[[1, 20], [3, 20], [60, 20]]])
        cases = {
            'fixture axial window': (
                base(paths=[[[10, 20], [10, 30], [10, 90]]]),
                dict(plain, fixture=dict(axial_min=0, axial_max=40, min_radius=1)),
            ),
            'fixture core': (deepening, dict(plain, fixture=dict(axial_min=-1, axial_max=400, min_radius=38))),
            'cut through the centre': (deepening, dict(plain, diameter=5)),
            'more than 1000 revolutions': (turning, dict(plain, angle_offset=359990)),
            'outside the wrap window is still legal': (leaving, plain),
        }
        for name, (project, options) in cases.items():
            with self.subTest(name):
                results = []
                translator = rotary._native_run

                def recording(*args, results=results, translator=translator):
                    results.append(translator(*args))
                    return results[-1]

                with patch.object(rotary, '_native_run', recording):
                    status, message = self.both(project, options)
                if name.endswith('legal'):
                    self.assertEqual(status, 'ok')
                    self.assertNotIn(None, results)
                else:
                    self.assertEqual(status, 'error', name)
                    self.assertTrue(message)
                    self.assertIn(None, results, 'the translator was not the one to meet the limit')

    def test_zero_length_moves_and_seam_snap_match(self):
        circumference = math.pi * 80
        seam = [
            [circumference, 20],
            [circumference, 20],
            [circumference + 0.0004, 60],
            [2 * circumference - 0.0003, 80],
        ]
        project = base(width=600, paths=[seam, [[10, 10], [10, 10], [30, 10]]])
        status, _ = self.both(project, dict(diameter=80, wrap_axis='X', postprocessor='grblhal'))
        self.assertEqual(status, 'ok')

    def test_translator_changes_nothing_when_it_declines(self):
        job = generate(base(), _rotary_blocks=True, backend='python')
        run = next(line for line in job['lines'] if isinstance(line, CutRun))
        arguments = [
            run.segments.buffer(), run.start, run.stop, round(run.feed, 1), run.variable_z, False, True,
            math.pi * 80, 0.0, 1.0, 40.0, False, 0.0, 0.0, 0.0, 'AXZ', 10.0, 10.0, -1.0, False, False, 0.0,
            math.inf, -math.inf, math.inf, -math.inf, math.inf, -math.inf, rotary._cut_length, 40.0, 'surface',
        ]  # fmt: skip
        self.assertIsNotNone(native.rotary_run(*arguments))
        unknown_start = list(arguments)
        unknown_start[16] = None
        self.assertIsNone(native.rotary_run(*unknown_start))
        tiny = list(arguments)
        tiny[10] = 0.5  # radius smaller than the cut depth
        self.assertIsNone(native.rotary_run(*tiny))
        failing = list(arguments)
        failing[28] = lambda *args: (_ for _ in ()).throw(RuntimeError('length failed'))
        self.assertIsNone(native.rotary_run(*failing))


if __name__ == '__main__':
    unittest.main()
