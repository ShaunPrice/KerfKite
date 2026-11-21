"""Offsets, pockets and fills from studio.operations.prepare, on the native core's polygon kernel and on
the Shapely fallback.

Every expectation is worked out from the geometry of the job (areas, distances from the source contour,
numbers of rings and rows), so it holds for either code and does not depend on where a library happens to
place its vertices. The kernel's cases are skipped where the compiled extension is not installed.
"""

import math
import os
import unittest
from unittest import mock

from shapely.geometry import LinearRing, LineString, Point, Polygon

from studio import operations
from studio.cam import generate
from studio.operations import prepare

OUT = [[10, 10], [30, 10], [30, 30], [10, 30], [10, 10]]  # 20 by 20
HOLE = [[16, 17], [20, 17], [20, 21], [16, 21], [16, 17]]  # 4 by 4, with no edge on a row of the fills below
BACKENDS = ['shapely'] + (['kernel'] if operations.kernel_available() else [])
# 32 segments a quarter turn: a library that puts its corners on the arc is inside it by 1 - cos(step / 2),
# and one that puts them outside is outside by sec(0.53125 step) - 1. Either is under 0.04% of the radius.
ARC = 0.0004


def run(backend, project, paths):
    with mock.patch.dict(os.environ, KERFKITE_POLYGON_BACKEND=backend):
        return prepare(project, paths)


def area(path):
    return Polygon(path).area


def clockwise(path):
    return not LinearRing(path).is_ccw


class PolygonOperationTests(unittest.TestCase):
    def test_outside_offset_is_the_contour_grown_by_the_radius(self):
        source = LinearRing(OUT)
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                paths, depths, warnings = run(
                    backend, {'mode': 'router', 'operation': 'outside', 'tool_diameter': 2}, [OUT]
                )
                self.assertIsNone(depths)
                self.assertEqual(len(paths), 1)
                (path,) = paths
                self.assertEqual(path[0], path[-1])
                self.assertTrue(clockwise(path))  # outlines run clockwise, holes counter-clockwise
                # The square, a strip of the radius along each side, and a quarter disc at each corner.
                self.assertAlmostEqual(area(path), 400 + 4 * 20 * 1 + math.pi, delta=2 * math.pi * ARC + 1e-6)
                for point in path:
                    self.assertAlmostEqual(source.distance(Point(point)), 1, delta=ARC + 2e-6)
                self.assertEqual(Polygon(path).bounds, (9, 9, 31, 31))
                self.assertEqual(len(warnings), 1)
                self.assertIn('even-odd', warnings[0])

    def test_inside_offset_shrinks_the_outline_and_grows_the_hole(self):
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                paths, _, _ = run(backend, {'mode': 'router', 'operation': 'inside', 'tool_diameter': 2}, [OUT, HOLE])
                # One polygon: its hole first, counter-clockwise, then its outline, clockwise.
                self.assertEqual(len(paths), 2)
                hole, outline = paths
                self.assertFalse(clockwise(hole))
                self.assertTrue(clockwise(outline))
                self.assertAlmostEqual(area(outline), 18 * 18, delta=1e-6)  # square corners stay square going inward
                self.assertAlmostEqual(area(hole), 16 + 4 * 4 * 1 + math.pi, delta=2 * math.pi * ARC + 1e-6)
                self.assertEqual(len(outline), 5)

    def test_laser_offset_is_half_the_kerf_and_zero_kerf_is_the_contour(self):
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                paths, _, _ = run(backend, {'mode': 'laser', 'operation': 'inside', 'kerf': 0.5}, [OUT])
                self.assertAlmostEqual(area(paths[0]), 19.5 * 19.5, delta=1e-6)
                paths, _, _ = run(backend, {'mode': 'laser', 'operation': 'outside', 'kerf': 0}, [OUT])
                self.assertEqual(len(paths), 1)
                self.assertEqual(area(paths[0]), 400)
                self.assertEqual(sorted(map(tuple, paths[0][:-1])), sorted(map(tuple, OUT[:-1])))

    def test_separate_parts_are_cut_smallest_first(self):
        small = [[40, 10], [45, 10], [45, 15], [40, 15], [40, 10]]
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                paths, _, _ = run(backend, {'mode': 'laser', 'operation': 'inside', 'kerf': 0.2}, [OUT, HOLE, small])
                self.assertEqual(len(paths), 3)
                self.assertAlmostEqual(area(paths[0]), 4.8 * 4.8, delta=1e-6)  # the small square
                self.assertFalse(clockwise(paths[1]))  # then the larger polygon: hole, outline
                self.assertAlmostEqual(area(paths[2]), 19.8 * 19.8, delta=1e-6)

    def test_pocket_passes_are_one_stepover_apart_until_nothing_is_left(self):
        square = [[10, 10], [31, 10], [31, 31], [10, 31], [10, 10]]  # 21 by 21
        project = {'mode': 'router', 'operation': 'pocket', 'tool_diameter': 2, 'stepover': 1}
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                paths, _, warnings = run(backend, project, [square])
                # The first pass is a radius in, a square of side 19; each further pass is 2 narrower: 19, 17 ... 1.
                self.assertEqual(len(paths), 10)
                for index, path in enumerate(paths):
                    self.assertEqual(len(path), 5)
                    self.assertAlmostEqual(area(path), (19 - 2 * index) ** 2, delta=1e-6)
                    self.assertTrue(clockwise(path))
                self.assertEqual(
                    warnings,
                    [
                        'Concentric pocket clearing preserves islands. Narrow residual features may remain.',
                        'Plunge entry requires a centre-cutting tool.',
                    ],
                )
                self.assertEqual(len(run(backend, dict(project, router_entry='ramp'), [square])[2]), 1)

    def test_pocket_keeps_the_cutter_off_the_wall_and_the_island(self):
        project = {'mode': 'router', 'operation': 'pocket', 'tool_diameter': 2, 'stepover': 0.8}
        walls = [LinearRing(OUT), LinearRing(HOLE)]
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                paths, _, _ = run(backend, project, [OUT, HOLE])
                for path in paths:
                    for wall in walls:
                        self.assertGreaterEqual(wall.distance(LineString(path)), 1 - ARC - 2e-6)

    def test_fill_rows_cover_the_region_and_skip_the_hole(self):
        project = {'mode': 'laser', 'operation': 'fill', 'interval': 3}
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                paths, depths, warnings = run(backend, project, [OUT, HOLE])
                self.assertIsNone(depths)
                self.assertEqual(warnings, [])
                # Rows at y = 10, 13, 16, 19, 22, 25, 28 and the top edge, 30. The row at 19 crosses the hole and is
                # cut in two; the others run the whole width.
                self.assertEqual(len(paths), 9)
                self.assertEqual(sorted({path[0][1] for path in paths}), [10, 13, 16, 19, 22, 25, 28, 30])
                self.assertAlmostEqual(sum(math.dist(*path) for path in paths), 7 * 20 + 16, delta=1e-6)
                for path in paths:
                    self.assertEqual(len(path), 2)
                    self.assertEqual(path[0][1], path[1][1])
                # The beam sweeps to and fro: rows inside the region alternate direction. (A row along an edge is
                # left out of this: the Shapely code gives it the edge's direction.)
                inner = [path for path in paths if path[0][1] in (13, 16, 22, 25, 28)]
                for path in inner:
                    forward = path[0][1] in (16, 22, 28)  # even rows, counted from the row at 10
                    self.assertEqual(path[0][0] < path[1][0], forward)
                two = [path for path in paths if path[0][1] == 19]
                self.assertEqual(sorted(sorted(p[0] for p in path) for path in two), [[10, 16], [20, 30]])
                self.assertGreater(two[0][0][0], two[1][0][0])  # an odd row: the far piece first

    def test_crosshatch_adds_the_rows_a_quarter_turn_on(self):
        project = {'mode': 'laser', 'operation': 'fill', 'interval': 3, 'hatch_angle': 0, 'crosshatch': True}
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                paths, _, _ = run(backend, project, [OUT, HOLE])
                self.assertEqual(len(paths), 18)
                self.assertTrue(all(path[0][1] == path[1][1] for path in paths[:9]))
                self.assertTrue(all(abs(path[0][0] - path[1][0]) < 1e-9 for path in paths[9:]))
                self.assertAlmostEqual(sum(math.dist(*path) for path in paths), 2 * (7 * 20 + 16), delta=1e-6)

    def test_fill_at_an_angle_stays_in_the_region(self):
        project = {'mode': 'laser', 'operation': 'fill', 'interval': 0.7, 'hatch_angle': 33.3}
        region = Polygon(OUT, [HOLE])
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                paths, _, _ = run(backend, project, [OUT, HOLE])
                for path in paths:
                    self.assertLess(region.distance(LineString(path)), 1e-9)
                    self.assertLess(LineString(path).difference(region.buffer(1e-6)).length, 1e-9)
                # Rows 0.7 apart sweep the area once: their total length times the interval is the area, to within
                # one row's worth at the two ends of the sweep.
                total = sum(math.dist(*path) for path in paths)
                self.assertAlmostEqual(total * 0.7, region.area, delta=0.7 * 2 * 20 * math.sqrt(2))

    def test_rejections_keep_their_order(self):
        cases = [
            # A contour that cannot be used is reported before a setting that cannot.
            ({'mode': 'router', 'operation': 'inside', 'tool_diameter': 'wide'}, [OUT[:-1]], 'This operation needs closed contours'),
            ({'mode': 'router', 'operation': 'inside', 'tool_diameter': 'wide'}, [OUT], 'tool_diameter must be numeric'),
            ({'mode': 'router', 'operation': 'pocket', 'tool_diameter': 500}, [OUT], 'tool_diameter must be between 0.01 and 100'),
            ({'mode': 'laser', 'operation': 'pocket'}, [OUT, OUT], 'Contours cancel under even-odd fill'),
            ({'mode': 'laser', 'operation': 'pocket'}, [OUT], 'Pocketing requires router mode'),
            ({'mode': 'router', 'operation': 'pocket', 'tool_diameter': 2, 'stepover': 1.5}, [OUT], 'Pocket stepover cannot exceed cutter radius (50% diameter)'),
            ({'mode': 'router', 'operation': 'pocket', 'tool_diameter': 30, 'stepover': 1}, [OUT], 'Cutter cannot enter this pocket'),
            ({'mode': 'router', 'operation': 'inside', 'tool_diameter': 30}, [OUT], 'Tool does not fit inside these contours'),
            ({'mode': 'router', 'operation': 'fill'}, [OUT], 'Hatch fill requires laser mode'),
            ({'mode': 'laser', 'operation': 'fill', 'interval': 0.02}, [[[0, 0], [9, 0], [9, 500], [0, 500], [0, 0]]], 'Too many hatch rows; increase interval'),
            ({'mode': 'laser', 'operation': 'fill'}, [[[0, 0], [5, 5], [5, 0], [0, 5], [0, 0]]], 'Repair self-crossing or zero-area contours before CAM'),
            ({'mode': 'laser', 'operation': 'fill'}, [[[0, 0], [5, 5]], 'no'], 'Contours must be lists of finite XY points'),
            ({'mode': 'laser', 'operation': 'spiral'}, [OUT], 'Unsupported operation'),
            ({'mode': 'laser', 'operation': 'spiral'}, [OUT[:-1]], 'This operation needs closed contours'),
            ({'mode': 'laser', 'operation': 'vcarve'}, [OUT], 'V-carving requires router mode'),
        ]  # fmt: skip
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                for project, paths, message in cases:
                    with self.assertRaises(ValueError) as raised:
                        run(backend, project, paths)
                    self.assertEqual(str(raised.exception), message)

    def test_a_job_is_generated_from_either_code(self):
        base = dict(protocol='grbl', width=100, height=100, feed=600, power=800, paths=[OUT, HOLE])
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                with mock.patch.dict(os.environ, KERFKITE_POLYGON_BACKEND=backend):
                    job = generate(dict(base, mode='laser', operation='fill', interval=3))
                    self.assertEqual(len(job['prepared_paths']), 9)
                    self.assertAlmostEqual(job['cut_length'], 156, places=3)
                    job = generate(dict(base, mode='router', operation='inside', tool_diameter=2, depth=1, step=1))
                    self.assertEqual(job['bounds'], [11, 11, 29, 29])
                    with self.assertRaisesRegex(ValueError, 'Compensated X must be between 0 and 100'):
                        generate(dict(base, mode='router', operation='outside', tool_diameter=2, paths=[[[0.5, 20], [30, 20], [30, 40], [0.5, 40], [0.5, 20]]]))  # fmt: skip


class BackendChoiceTests(unittest.TestCase):
    def test_backend_setting(self):
        with mock.patch.dict(os.environ, KERFKITE_POLYGON_BACKEND='fast'):
            with self.assertRaisesRegex(ValueError, 'KERFKITE_POLYGON_BACKEND must be auto, kernel or shapely'):
                operations.polygon_backend()
        with mock.patch.dict(os.environ, KERFKITE_POLYGON_BACKEND='shapely'):
            self.assertEqual(operations.polygon_backend(), 'shapely')
        with mock.patch.object(operations, '_kernel_installed', False):
            self.assertEqual(operations.polygon_backend(), 'shapely')
            with mock.patch.dict(os.environ, KERFKITE_POLYGON_BACKEND='kernel'):
                with self.assertRaisesRegex(ValueError, 'The polygon kernel is not installed'):
                    prepare({'mode': 'laser', 'operation': 'fill'}, [OUT])

    def test_an_extension_without_the_operations_is_not_the_kernel(self):
        def old(command, text):
            raise ValueError('Unknown command: ' + command)

        with mock.patch.object(operations, '_kernel_installed', None), mock.patch.object(operations, '_core_call', old):
            self.assertFalse(operations.kernel_available())
        with (
            mock.patch.object(operations, '_kernel_installed', None),
            mock.patch.object(operations, '_core_call', None),
        ):
            self.assertFalse(operations.kernel_available())


@unittest.skipUnless(operations.kernel_available(), 'the compiled extension is not installed')
class KernelOperationTests(unittest.TestCase):
    def test_coordinates_are_whole_millionths_of_a_millimetre(self):
        # The core answers n / 1e6 for a whole number n, and JSON carries that double unchanged.
        star = [[50 + (20 if i % 2 else 9) * math.cos(i * math.pi / 7), 50 + (20 if i % 2 else 9) * math.sin(i * math.pi / 7)] for i in range(14)]  # fmt: skip
        star.append(star[0])
        for project in (
            {'mode': 'router', 'operation': 'outside', 'tool_diameter': 3.175},
            {'mode': 'router', 'operation': 'pocket', 'tool_diameter': 3.175, 'stepover': 1.2},
            {'mode': 'laser', 'operation': 'fill', 'interval': 0.37, 'hatch_angle': 17.5, 'crosshatch': True},
        ):
            paths, _, _ = run('kernel', project, [star])
            self.assertTrue(paths)
            for path in paths:
                for point in path:
                    for value in point:
                        self.assertIs(type(value), float)
                        self.assertEqual(value, round(value * 1e6) / 1e6)

    def test_the_tool_is_never_nearer_than_its_radius(self):
        # Arcs are drawn from outside: every vertex and every edge of the path is at least the radius away.
        star = [[50 + (20 if i % 2 else 9) * math.cos(i * math.pi / 7), 50 + (20 if i % 2 else 9) * math.sin(i * math.pi / 7)] for i in range(14)]  # fmt: skip
        star.append(star[0])
        source = LinearRing(star)
        for operation in ('outside', 'inside'):
            paths, _, _ = run('kernel', {'mode': 'router', 'operation': operation, 'tool_diameter': 3}, [star])
            for path in paths:
                self.assertGreaterEqual(source.distance(LinearRing(path)), 1.5 - 2e-6)
                for point in path:
                    self.assertLessEqual(source.distance(Point(point)), 1.5 * (1 + ARC) + 2e-6)

    def test_kernel_and_fallback_cut_the_same_region(self):
        star = [[50 + (20 if i % 2 else 9) * math.cos(i * math.pi / 7), 50 + (20 if i % 2 else 9) * math.sin(i * math.pi / 7)] for i in range(14)]  # fmt: skip
        star.append(star[0])
        project = {'mode': 'router', 'operation': 'pocket', 'tool_diameter': 3, 'stepover': 1.5}
        mine, theirs = run('kernel', project, [star])[0], run('shapely', project, [star])[0]
        # The star's inward corners are 9 from its centre, so there is room for passes 1.5, 3, 4.5, 6 and 7.5
        # in from the edge, and the pass 9 in is the centre point alone: five passes. (GEOS's arcs, drawn from
        # inside, leave a speck of a millionth of a square millimetre there as a sixth; a pass that small is
        # the only difference allowed in the number.)
        self.assertEqual(len(mine), 5)
        self.assertTrue(all(Polygon(extra).area < 1e-4 for extra in theirs[len(mine) :]))
        for a, b in zip(mine, theirs):
            # Corners outside the arc on one side and inside it on the other: under 0.08% of the distance apart.
            self.assertLess(Polygon(a).symmetric_difference(Polygon(b)).area, 2 * ARC * 20 * LinearRing(b).length)
            self.assertLess(LinearRing(a).hausdorff_distance(LinearRing(b)), 2 * ARC * 20 + 1e-5)
        # The kernel does not draw every arc again at every pass, so its passes have fewer vertices.
        self.assertLessEqual(sum(map(len, mine)), sum(map(len, theirs)))

    def test_a_hatch_piece_too_short_to_write_is_left_out(self):
        # A speck half a micrometre across lies on the row at y = 5. The program writes three decimals, so a
        # stroke that short cannot be cut: the kernel's operation leaves it out, the Shapely code kept it.
        speck = [[20, 4.99975], [20.0005, 4.99975], [20.0005, 5.00025], [20, 5.00025], [20, 4.99975]]
        block = [[0, 0], [10, 0], [10, 10], [0, 10], [0, 0]]
        project = {'mode': 'laser', 'operation': 'fill', 'interval': 5}
        mine, theirs = run('kernel', project, [block, speck])[0], run('shapely', project, [block, speck])[0]
        self.assertEqual(len(mine), 3)
        self.assertTrue(all(math.dist(*path) == 10 for path in mine))
        self.assertEqual(len(theirs), 4)
        with self.assertRaisesRegex(ValueError, 'Operation produced no toolpaths'):
            run('kernel', project, [speck])

    def test_a_contour_closed_to_within_the_tolerance_is_the_closed_contour(self):
        # The last point may miss the first by up to a millionth of a millimetre. It is then the first point,
        # not a further vertex, wherever the miss falls on the kernel's grid.
        project = {'mode': 'router', 'operation': 'inside', 'tool_diameter': 2}
        exact = run('kernel', project, [OUT])[0]
        for dx, dy in ((4e-7, -5e-7), (-4e-7, 5e-7), (6e-7, 6e-7), (0, -9e-7), (1e-9, 0)):
            nearly = OUT[:-1] + [[OUT[0][0] + dx, OUT[0][1] + dy]]
            self.assertEqual(run('kernel', project, [nearly])[0], exact)
        with self.assertRaisesRegex(ValueError, 'This operation needs closed contours'):
            run('kernel', project, [OUT[:-1] + [[OUT[0][0] + 2e-6, OUT[0][1]]]])

    def test_quarter_turns_are_exact(self):
        # The rows of a fill at 90, 180, 270 and 360 degrees lie exactly on whole coordinates.
        block = [[0, 0], [10, 0], [10, 4], [0, 4], [0, 0]]
        for angle, axis, count in ((90, 0, 6), (180, 1, 3), (270, 0, 6), (360, 1, 3), (-90, 0, 6)):
            paths, _, _ = run(
                'kernel', {'mode': 'laser', 'operation': 'fill', 'interval': 2, 'hatch_angle': angle}, [block]
            )
            self.assertEqual(len(paths), count)
            for path in paths:
                self.assertEqual(path[0][axis], path[1][axis])
                self.assertEqual(path[0][axis] % 2, 0)


if __name__ == '__main__':
    unittest.main()
