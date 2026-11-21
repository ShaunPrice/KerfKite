"""Probe levelling through the native core, checked against an independent calculation in Python."""

import math
import re
import unittest
from unittest import mock

from studio import leveling

HAS_CORE = leveling.available()
MOVE = re.compile(r'^G1 X(-?[\d.]+) Y(-?[\d.]+)(?: F([\d.]+))? Z(-?[\d.]+)$')


def bilinear(surface, x, y):
    """The surface height at (x, y): the same definition as the core, written separately."""
    fx = min(max((x - surface['x0']) / surface['dx'], 0), surface['columns'] - 1)
    fy = min(max((y - surface['y0']) / surface['dy'], 0), surface['rows'] - 1)
    c, r = min(int(fx), surface['columns'] - 2), min(int(fy), surface['rows'] - 2)
    tx, ty = fx - c, fy - r
    at = lambda row, column: surface['offsets'][row * surface['columns'] + column]
    return (at(r, c) * (1 - tx) + at(r, c + 1) * tx) * (1 - ty) + (at(r + 1, c) * (1 - tx) + at(r + 1, c + 1) * tx) * ty


def probed(plan, surface_z):
    return leveling.height_map(plan, heights=[surface_z(x, y) for x, y in plan['points']])


JOB = [
    'G21', 'G90', 'G17', 'G94', 'G54', 'M5', 'G0 Z5.000', 'M3 S12000', 'G4 P2',
    'G0 Z5.000', 'G0 X12.000 Y11.000', 'G1 Z-0.100 F100.0', 'G1 X47.500 Y11.000 F600.0', 'G1 X47.500 Y29.250 F600.0',
    'G1 X20.000 Y18.000 F600.0 Z-0.300', 'G0 Z5.000', 'M5',
]  # fmt: skip
REQUEST = {'bounds': [12, 11, 47.5, 29.25], 'width': 100, 'height': 60, 'spacing': 10}


@unittest.skipUnless(HAS_CORE, 'The compiled extension with the native core is not installed')
class ProbePlanTests(unittest.TestCase):
    def test_grid_covers_the_bounds_with_a_margin_inside_the_work_area(self):
        plan = leveling.probe_plan(REQUEST)
        grid = plan['grid']
        self.assertEqual((grid['x0'], grid['y0']), (10, 9))  # two millimetres of margin
        self.assertAlmostEqual(grid['x0'] + grid['dx'] * (grid['columns'] - 1), 49.5)
        self.assertAlmostEqual(grid['y0'] + grid['dy'] * (grid['rows'] - 1), 31.25)
        self.assertLessEqual(max(grid['dx'], grid['dy']), 10)
        self.assertEqual(len(plan['points']), grid['columns'] * grid['rows'])
        clipped = leveling.probe_plan({**REQUEST, 'bounds': [0, 0, 100, 60]})['grid']
        self.assertEqual((clipped['x0'], clipped['y0']), (0, 0))
        self.assertAlmostEqual(clipped['x0'] + clipped['dx'] * (clipped['columns'] - 1), 100)
        line = leveling.probe_plan({**REQUEST, 'bounds': [30, 20, 30, 20], 'margin': 0})['grid']
        self.assertEqual((line['columns'], line['rows']), (2, 2))  # a point still gets a surface around it

    def test_program_probes_each_point_once_back_and_forth(self):
        plan = leveling.probe_plan({**REQUEST, 'clearance': 3, 'depth': 0.5, 'feed': 40})
        grid, lines = plan['grid'], plan['lines']
        self.assertEqual(lines[:4], ['G21', 'G90', 'M5', 'G0 Z3.000'])  # the spindle is off before any move
        self.assertEqual(len(lines), 4 + 3 * len(plan['points']))
        for index, (x, y) in enumerate(plan['points']):
            self.assertEqual(
                lines[4 + 3 * index : 7 + 3 * index], [f'G0 X{x:.3f} Y{y:.3f}', 'G38.2 Z-0.500 F40.0', 'G0 Z3.000']
            )
        self.assertEqual(
            sorted(map(tuple, plan['order'])),
            sorted((c, r) for r in range(grid['rows']) for c in range(grid['columns'])),
        )
        first_row = [c for c, r in plan['order'] if r == 0]
        second_row = [c for c, r in plan['order'] if r == 1]
        self.assertEqual(first_row, sorted(first_row))
        self.assertEqual(second_row, sorted(second_row, reverse=True))

    def test_work_zero_can_be_set_where_the_first_point_makes_contact(self):
        plain = leveling.probe_plan(REQUEST)
        zeroed = leveling.probe_plan({**REQUEST, 'zero': True})
        plate = leveling.probe_plan({**REQUEST, 'zero': True, 'plate': 1.5})
        self.assertNotIn('G10', ' '.join(plain['lines']))
        self.assertEqual(zeroed['lines'][:8], plain['lines'][:6] + ['G10 L20 P1 Z0.000', 'G0 Z2.000'])
        self.assertEqual(zeroed['lines'][8:], plain['lines'][7:])  # only the first point sets zero
        self.assertEqual(plate['lines'][6], 'G10 L20 P1 Z1.500')
        for key in ('points', 'order', 'grid'):
            self.assertEqual(zeroed[key], plain[key])

    def test_rejections(self):
        for change, message in [
            ({'plate': 1.5}, 'applies only when work zero is set'),
            ({'zero': 1}, 'must be true or false'),
            ({'spacing': 1, 'bounds': [0, 0, 100, 60]}, 'exceeds 400 points'),
            ({'bounds': [0, 0, 101, 10]}, 'Bounds max X must be between'),
            ({'bounds': [5, 5, 4, 10]}, 'Bounds max X must be between'),
            ({'bounds': [0, 0, 1]}, 'cutting bounds'),
            ({'depth': 9}, 'Probe depth must be between'),
            ({'feed': True}, 'Probe feed must be a number'),
            ({'width': None}, 'Work width must be a number'),
        ]:
            with self.subTest(change):
                with self.assertRaisesRegex(ValueError, message):
                    leveling.probe_plan({**REQUEST, **change})


@unittest.skipUnless(HAS_CORE, 'The compiled extension with the native core is not installed')
class HeightMapTests(unittest.TestCase):
    def setUp(self):
        self.plan = leveling.probe_plan(REQUEST)
        self.surface_z = lambda x, y: -41.2 + 0.004 * x - 0.006 * y + 0.00003 * (x - 30) ** 2

    def test_offsets_are_relative_to_the_reference_and_placed_by_grid_position(self):
        surface = probed(self.plan, self.surface_z)
        grid = self.plan['grid']
        zero = self.surface_z(*self.plan['points'][0])
        for r in range(grid['rows']):
            for c in range(grid['columns']):
                x, y = grid['x0'] + grid['dx'] * c, grid['y0'] + grid['dy'] * r
                self.assertAlmostEqual(
                    surface['offsets'][r * grid['columns'] + c], self.surface_z(x, y) - zero, places=9
                )
        other = leveling.height_map(
            self.plan, heights=[self.surface_z(x, y) for x, y in self.plan['points']], reference=5
        )
        shift = surface['offsets'][0] - other['offsets'][0]
        self.assertTrue(
            all(abs(a - b - shift) < 1e-12 for a, b in zip(surface['offsets'], other['offsets'], strict=True))
        )
        self.assertIn(0.0, other['offsets'])

    def test_controller_reports_are_read(self):
        reports = [f'[PRB:{x:.3f},{y:.3f},{self.surface_z(x, y):.3f}:1]' for x, y in self.plan['points']]
        surface = leveling.height_map(self.plan, reports=reports)
        exact = probed(self.plan, lambda x, y: round(self.surface_z(x, y), 3))
        self.assertEqual(surface, exact)
        four_axis = [report.replace(':1]', ',0.000:1]') for report in reports]
        self.assertEqual(leveling.height_map(self.plan, reports=four_axis), exact)

    def test_rejections(self):
        heights = [self.surface_z(x, y) for x, y in self.plan['points']]
        reports = [f'[PRB:{x:.3f},{y:.3f},{z:.3f}:1]' for (x, y), z in zip(self.plan['points'], heights, strict=True)]
        cases = [
            (dict(heights=heights[:-1]), 'One probe height is needed'),
            (dict(heights=heights[:-1] + [heights[-1] + 6]), 'more than the 5.0 mm limit'),
            (dict(heights=heights, limit=0.01), 'more than the 0.01 mm limit'),
            (dict(heights=heights, reports=reports), 'not both'),
            (
                dict(reports=reports[:3] + ['[PRB:1.000,2.000,-3.000:0]'] + reports[4:]),
                'did not make contact at point 4',
            ),
            (dict(reports=reports[:3] + ['ok'] + reports[4:]), 'Probe report 4 is not'),
            (dict(reports=reports[:3] + ['[PRB:1.000,2.000:1]'] + reports[4:]), 'Probe report 4 has no Z'),
            (dict(heights=heights, reference=len(heights)), 'Reference point must be between'),
        ]
        for arguments, message in cases:
            with self.subTest(message):
                with self.assertRaisesRegex(ValueError, message):
                    leveling.height_map(self.plan, **arguments)
        twice = {**self.plan, 'order': [self.plan['order'][0]] * len(heights)}
        with self.assertRaisesRegex(ValueError, 'names a grid point twice'):
            leveling.height_map(twice, heights=heights)


@unittest.skipUnless(HAS_CORE, 'The compiled extension with the native core is not installed')
class LevelProgramTests(unittest.TestCase):
    def setUp(self):
        self.plan = leveling.probe_plan(REQUEST)
        self.surface = probed(self.plan, lambda x, y: 0.004 * x - 0.006 * y + 0.00003 * (x - 30) ** 2)

    def moves(self, lines):
        """Cutting moves as (x, y, z), with the programmed depth carried alongside by position."""
        return [
            tuple(float(v) for i, v in enumerate(MOVE.match(line).groups()) if i != 2)
            for line in lines
            if MOVE.match(line)
        ]

    def test_every_other_line_is_kept_in_place(self):
        out = leveling.level(JOB, self.surface, max_segment=1)['lines']
        kept = [line for line in out if not line.startswith('G1')]
        self.assertEqual(kept, [line for line in JOB if not line.startswith('G1')])
        self.assertEqual(out[: JOB.index('G1 Z-0.100 F100.0')], JOB[: JOB.index('G1 Z-0.100 F100.0')])

    def test_cuts_are_split_and_each_piece_follows_the_surface(self):
        for segment in (0.4, 1, 3.3):
            with self.subTest(segment):
                out = leveling.level(JOB, self.surface, max_segment=segment)['lines']
                plunge = next(line for line in out if line.startswith('G1 Z'))
                self.assertEqual(plunge, f'G1 Z{-0.1 + bilinear(self.surface, 12, 11):.3f} F100.0')
                x, y, depth = 12.0, 11.0, -0.1
                position = [x, y]
                index = out.index(plunge) + 1
                for target_x, target_y, target_depth in ((47.5, 11.0, -0.1), (47.5, 29.25, -0.1), (20.0, 18.0, -0.3)):
                    length = math.dist((x, y), (target_x, target_y))
                    pieces = max(1, math.ceil(length / segment))
                    for i in range(1, pieces + 1):
                        cx, cy, cz = (float(v) for v in MOVE.match(out[index]).group(1, 2, 4))
                        t = i / pieces
                        want_x, want_y = x + (target_x - x) * t, y + (target_y - y) * t
                        want_z = depth + (target_depth - depth) * t + bilinear(self.surface, want_x, want_y)
                        self.assertAlmostEqual(cx, want_x, delta=0.00051)
                        self.assertAlmostEqual(cy, want_y, delta=0.00051)
                        self.assertAlmostEqual(cz, want_z, delta=0.00051)
                        self.assertLessEqual(math.dist(position, (cx, cy)), segment + 0.0011)
                        self.assertIn(' F600.0 ', out[index])
                        position = [cx, cy]
                        index += 1
                    self.assertEqual(position, [target_x, target_y])  # the programmed end point is reached exactly
                    x, y, depth = target_x, target_y, target_depth
                self.assertEqual(out[index], 'G0 Z5.000')

    def test_a_flat_surface_changes_no_depth(self):
        flat = {**self.surface, 'offsets': [0.0] * len(self.surface['offsets'])}
        out = leveling.level(JOB, flat, max_segment=2)['lines']
        moves = self.moves(out)
        level_pieces = math.ceil(35.5 / 2) + math.ceil(18.25 / 2)  # the two cuts at a constant depth
        self.assertEqual({z for _, _, z in moves[:level_pieces]}, {-0.1})
        self.assertEqual(moves[-1], (20.0, 18.0, -0.3))  # the ramp still ends at its programmed depth
        self.assertIn('G1 Z-0.100 F100.0', out)

    def test_default_segment_follows_the_grid_spacing(self):
        result = leveling.level(JOB, self.surface)
        spacing = min(self.surface['dx'], self.surface['dy'])
        self.assertEqual(result['max_segment'], min(max(spacing / 4, 0.25), 2.0))
        self.assertEqual(result['highest'], max(self.surface['offsets']))

    def test_programs_that_cannot_be_followed_safely_are_refused(self):
        start = ['G21', 'G90', 'G0 Z5.000', 'G0 X12.000 Y11.000', 'G1 Z-0.100 F100.0']
        cases = [
            (start + ['G2 X20.000 Y11.000 I4 J0 F600'], 'only straight moves'),
            (['G91'] + start, 'only straight moves'),
            (['G20'] + start, 'only straight moves'),
            (start + ['G1 X20.000 Y11.000 F600 S500'], 'not laser power'),
            (start + ['M4 S500'], 'only straight moves'),
            (start + ['G1 X60.000 Y11.000 F600'], 'outside the probed area'),
            (['G0 Z5.000', 'G0 X2.000 Y2.000', 'G1 Z-0.1 F100'], 'outside the probed area'),
            (['G1 Z-0.1 F100'], 'a plunge before any position'),
            (['G0 X12.000 Y11.000', 'G1 X20.000 Y11.000 F600'], 'a rapid before any height|before the start position'),
            (['G0 Z0.050', 'G0 X12.000 Y11.000'], 'not clear of the highest probed point'),
            (start + ['G0 X20.000 Y11.000'], 'not clear of the highest probed point'),
            (start + ['G1 X20.000 Y11.000 F600 A3'], 'unexpected word'),
            (start + ['hello'], 'not understood'),
            (start + ['G38.2 Z-1 F20'], 'only straight moves'),
        ]
        for program, message in cases:
            with self.subTest(program[-1]):
                with self.assertRaisesRegex(ValueError, message):
                    leveling.level(program, self.surface)
        with self.assertRaisesRegex(ValueError, 'Levelling segment length must be between'):
            leveling.level(JOB, self.surface, max_segment=0.01)
        with self.assertRaisesRegex(ValueError, 'one offset for each grid point'):
            leveling.level(JOB, {**self.surface, 'offsets': self.surface['offsets'][:-1]})
        with self.assertRaisesRegex(ValueError, 'Height offset must be between'):
            leveling.level(JOB, {**self.surface, 'offsets': [11.0] + self.surface['offsets'][1:]})

    def test_generated_router_job_levels_from_end_to_end(self):
        from studio.cam import generate

        project = dict(
            mode='router', protocol='grbl', width=100, height=60, feed=600, power=12000, depth=0.1, step=0.1,
            paths=[[[12, 11], [47.5, 11], [47.5, 29.25], [12, 11]]],
        )  # fmt: skip
        job = generate(project)
        plan = leveling.probe_plan({'bounds': job['bounds'], 'width': 100, 'height': 60, 'spacing': 6})
        surface = probed(plan, lambda x, y: 0.01 * x - 0.004 * y)
        out = leveling.level(job['lines'], surface)['lines']
        zero = 0.01 * plan['points'][0][0] - 0.004 * plan['points'][0][1]
        for x, y, z in self.moves(out):
            self.assertAlmostEqual(z, -0.1 + 0.01 * x - 0.004 * y - zero, delta=0.0011)  # a plane is followed exactly
        self.assertEqual(out[-2:], job['lines'][-2:])


class MissingCoreTests(unittest.TestCase):
    def test_without_the_extension_the_request_is_refused_plainly(self):
        with mock.patch.object(leveling, '_core_call', None):
            self.assertFalse(leveling.available())
            with self.assertRaisesRegex(ValueError, 'needs the compiled KerfKite extension'):
                leveling.probe_plan(REQUEST)

    @unittest.skipUnless(HAS_CORE, 'The compiled extension with the native core is not installed')
    def test_values_json_cannot_carry_are_refused(self):
        with self.assertRaisesRegex(ValueError, 'cannot be sent'):
            leveling.probe_plan({**REQUEST, 'spacing': math.nan})
        with self.assertRaisesRegex(ValueError, 'must be an object'):
            leveling.height_map(None, heights=[0])


if __name__ == '__main__':
    unittest.main()
