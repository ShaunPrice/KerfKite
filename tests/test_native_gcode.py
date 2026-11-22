"""Studio's CAM through the native core must be the Python reference's to the byte: program text, numbers and
number types, every member of the job, and every refusal with its message."""

import json
import math
import os
import random
import struct
import unittest
from unittest.mock import patch

from studio import cam
from studio.cam import generate, job_json
from studio.cam_moves import CutRun, PackedSegments
from studio.rotary import generate_rotary, rotary_json, wrap_job

try:
    from studio import _relief_native as native
except ImportError:
    native = None

HAS_NATIVE = native is not None and hasattr(native, 'core_cam')


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


@unittest.skipUnless(native is not None, 'Optional C++ extension is not installed')
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


class Calls:
    """The core's CAM wrapped so that a test can see what it answered."""

    def __init__(self):
        self.answers = []

    def __call__(self, *args):
        self.answers.append(native.core_cam(*args))
        return self.answers[-1]


@unittest.skipUnless(HAS_NATIVE, 'Optional C++ extension is not installed')
class CoreParityTests(unittest.TestCase):
    def both(self, project, **options):
        reference = generate(project, backend='python', **options)
        core = generate(project, backend='native', **options)
        self.assertEqual(exact(core), exact(reference))
        return reference

    def test_every_mode_and_segment_form_matches_the_reference(self):
        for name, build in PROJECTS.items():
            for options in ({}, {'packed_segments': True}):
                with self.subTest(name, **options):
                    calls = Calls()
                    with patch.object(cam, '_core_cam', calls):
                        job = self.both(build(), **options)
                    self.assertTrue(calls.answers)
                    self.assertNotIn(None, calls.answers, 'the core was not given a plain job')
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
            for options in ({}, {'packed_segments': True}):
                with self.subTest(case=case, **options):
                    self.both(project, **options)

    def test_every_member_of_the_job_matches(self):
        square = [[20, 20], [60, 20], [60, 50], [20, 50], [20, 20]]
        jobs = {
            # The holes and the drill that the stock simulation reads, and the number of operations.
            'ordered drilling': base(
                tool_diameter=3,
                operations=[
                    dict(
                        operation='drill',
                        depth=1.5,
                        drill_peck=0.5,
                        paths=[[[10, 10], [10, 10]], [[30.5, 20], [30.5, 20]]],
                    ),
                    dict(operation='drill', depth=1, paths=[[[50, 25], [50, 25]]]),
                ],
            ),
            'drilling': base(operation='drill', tool_diameter=2, paths=[[[10, 10], [10, 10]]]),
            # The paths a ramp follows and their depths.
            'ramp': base(paths=[square], router_entry='ramp', tool_diameter=3),
            'pocket': base(paths=[square], operation='pocket', tool_diameter=3, stepover=1.2),
            'knife': base(mode='knife', paths=[square], knife_offset=0.25),
        }
        for name, project in jobs.items():
            with self.subTest(name):
                job = self.both(project)
                for key in ('drill_holes', 'simulation_tool', 'operations', 'prepared_paths', 'prepared_depths'):
                    if key in job:
                        self.assertIn(key, generate(project, backend='native'))


@unittest.skipUnless(HAS_NATIVE, 'Optional C++ extension is not installed')
class CrossingTests(unittest.TestCase):
    """Only what JSON carries unchanged is given to the core; the rest, and its message, stays the reference's."""

    def outcomes(self, project):
        found = []
        for backend in ('python', 'auto'):
            try:
                found.append(('ok', exact(generate(project, backend=backend))))
            except Exception as error:  # noqa: BLE001 - an int beyond the double range raises OverflowError
                found.append((type(error).__name__, str(error)))
        return found

    def test_values_json_would_change_stay_with_the_reference(self):
        class Float(float):
            pass

        good = [[1.0, 2.0], [3.0, 4.0]]
        for name, project in {
            'tuple path': base(paths=[tuple(good)]),
            'tuple point': base(paths=[[(1.0, 2.0), [3.0, 4.0]]]),
            'float subclass': base(paths=[[[Float(1.0), 2.0], [3.0, 4.0]]]),
            'not finite': base(paths=[[[math.nan, 2.0], [3.0, 4.0]]]),
            'infinite feed': base(feed=math.inf),
            'huge int': base(paths=[[[10**400, 2.0], [3.0, 4.0]]]),
            'key that is not text': {**base(), 7: 'seven'},
            'lone surrogate': base(protocol='\ud800'),
        }.items():
            with self.subTest(name):
                self.assertIsNone(native.core_cam('generate', project))
                self.assertIsNone(job_json(project))
                found = self.outcomes(project)
                self.assertEqual(found[0], found[1])
                with self.assertRaises(NotImplementedError):
                    generate(project, backend='native')

    def test_declined_input_gets_the_reference_message(self):
        cases = {
            'numeric text is accepted by the reference': [[['1.5', 2.0], [3.0, 4.0]]],
            'bool': [[[True, 2.0], [3.0, 4.0]]],
            'outside': [[[1.0, 2.0], [900.0, 4.0]]],
            'short': [[[1.0, 2.0]]],
            'three numbers': [[[1.0, 2.0, 3.0], [3.0, 4.0]]],
        }
        for name, paths in cases.items():
            with self.subTest(name):
                found = self.outcomes(base(paths=paths))
                self.assertEqual(found[0], found[1])

    def test_a_case_the_core_does_not_reproduce_is_answered_by_the_reference(self):
        numbers = base(feed='\uff16\uff10\uff10')  # 600 in full-width digits: the core declines such text
        with self.assertRaises(NotImplementedError):
            native.core_cam('generate', numbers)
        self.assertIsNone(job_json(numbers))
        found = self.outcomes(numbers)
        self.assertEqual(found[0], found[1])
        self.assertEqual(found[0][0], 'ok')

    def test_packed_rows_unpack_to_the_same_lists(self):
        job = generate(relief_project(), packed_segments=True, backend='native')
        self.assertIsInstance(job['segments'], PackedSegments)
        self.assertEqual(exact(job['segments'].to_list()), exact(list(job['segments'])))
        laser = generate(laser_project(), packed_segments=True, backend='native')
        self.assertEqual(exact(laser['segments'].to_list()), exact(list(laser['segments'])))
        self.assertIsNone(native.segment_rows(b'12345'))


class BoundsTests(unittest.TestCase):
    def test_bounds_keep_the_first_extreme_vertex_and_its_type(self):
        from studio.cam import _bounds

        paths = [[[0, 5.0], [10.0, 0]], [], [[0.0, 5], [10, 0.0], [3, 2]]]
        bounds = _bounds(paths)
        self.assertEqual(bounds, [0, 0, 10.0, 5.0])
        self.assertEqual([type(v) for v in bounds], [int, int, float, float])
        with self.assertRaises(ValueError):
            _bounds([[], []])


class BackendSelectionTests(unittest.TestCase):
    def test_backend_names_are_validated(self):
        with self.assertRaisesRegex(ValueError, 'auto, python or native'):
            generate(base(), backend='fast')
        with self.assertRaisesRegex(ValueError, 'auto, python or native'):
            generate_rotary(base(), dict(diameter=100, wrap_axis='X', postprocessor='grblhal'), backend='fast')

    def test_missing_extension_uses_python_unless_native_is_demanded(self):
        options = dict(diameter=100, wrap_axis='X', postprocessor='grblhal')
        with patch.object(cam, '_core_cam', None), patch.object(cam, '_core_cam_text', None):
            self.assertEqual(exact(generate(base())), exact(generate(base(), backend='python')))
            self.assertIsNone(job_json(base()))
            self.assertIsNone(rotary_json(base(), options))
            with self.assertRaisesRegex(ValueError, 'not installed'):
                generate(base(), backend='native')
            with self.assertRaisesRegex(ValueError, 'not installed'):
                generate_rotary(base(), options, backend='native')

    @unittest.skipUnless(HAS_NATIVE, 'Optional C++ extension is not installed')
    def test_environment_switches_select_the_python_cam(self):
        options = dict(diameter=100, wrap_axis='X', postprocessor='grblhal')
        for switch in ({'KERFKITE_GCODE_BACKEND': 'python'}, {'KERFKITE_POLYGON_BACKEND': 'shapely'}):
            with self.subTest(**switch):
                calls = Calls()
                with patch.object(cam, '_core_cam', calls), patch.dict(os.environ, switch):
                    generate(base())
                    generate_rotary(base(), options)
                    self.assertIsNone(job_json(base()))
                self.assertFalse(calls.answers)
        calls = Calls()
        with patch.object(cam, '_core_cam', calls), patch.dict(os.environ, {'KERFKITE_GCODE_BACKEND': ''}):
            generate(base())
            generate_rotary(base(), options)
        self.assertEqual(len(calls.answers), 2)

    @unittest.skipUnless(HAS_NATIVE, 'Optional C++ extension is not installed')
    @patch.dict(os.environ, {'KERFKITE_GCODE_BACKEND': '', 'KERFKITE_POLYGON_BACKEND': ''})
    def test_json_text_is_the_job(self):
        for name, build in PROJECTS.items():
            with self.subTest(name):
                project = build()
                self.assertEqual(json.loads(job_json(project)), json.loads(json.dumps(generate(project))))
        options = dict(diameter=80, wrap_axis='Y', postprocessor='grblhal', feed_reference='cut')
        self.assertEqual(
            json.loads(rotary_json(base(), options)), json.loads(json.dumps(generate_rotary(base(), options)))
        )
        with self.assertRaisesRegex(ValueError, 'Choose router, laser or knife'):
            job_json(base(mode='plasma'))


@unittest.skipUnless(HAS_NATIVE, 'Optional C++ extension is not installed')
class RotaryParityTests(unittest.TestCase):
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
                                calls = Calls()
                                with patch.object(cam, '_core_cam', calls):
                                    status, _ = self.both(source, options)
                                self.assertEqual(status, 'ok')
                                self.assertTrue(calls.answers)

    def test_limits_give_the_reference_error(self):
        plain = dict(diameter=80, wrap_axis='X', postprocessor='grblhal')
        # Each job starts inside its limits and breaks them part-way along a cutting run.
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
                status, message = self.both(project, options)
                self.assertEqual(status, 'ok' if name.endswith('legal') else 'error', name)

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

    def test_given_programs_match_the_reference(self):
        planar = json.loads(json.dumps(generate(base(), backend='python')))
        options = dict(diameter=80, wrap_axis='X', postprocessor='grblhal')
        given = {
            'as generated': planar['lines'],
            'a comment': planar['lines'][:3] + ['(note)'] + planar['lines'][3:],
            'a word twice': planar['lines'][:12] + ['G1 X1 X2'] + planar['lines'][12:],
            'no spindle off': planar['lines'][:-1],
            'odd spacing': ['  ' + line + '\t' for line in planar['lines']],
        }
        for name, lines in given.items():
            with self.subTest(name):
                job = dict(planar, lines=lines, note='kept')
                outcomes = []
                for backend in ('python', 'native'):
                    try:
                        outcomes.append(('ok', exact(wrap_job(job, options, backend=backend))))
                    except ValueError as error:
                        outcomes.append(('error', str(error)))
                self.assertEqual(outcomes[0], outcomes[1])
        # Text that is not ASCII is the reference's alone: Python splits words on any Unicode space.
        foreign = dict(planar, lines=planar['lines'][:12] + ['G1 X10\u00a0Y20 F900'] + planar['lines'][12:])
        with self.assertRaises(NotImplementedError):
            wrap_job(foreign, options, backend='native')
        self.assertEqual(exact(wrap_job(foreign, options)), exact(wrap_job(foreign, options, backend='python')))


if __name__ == '__main__':
    unittest.main()
