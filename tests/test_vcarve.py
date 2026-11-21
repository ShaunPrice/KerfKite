"""V-carving from studio.operations.prepare, on the native core (its own Voronoi diagram) and on the Shapely
fallback.

The safety property is measured with Shapely, which shares no code with the core: at every point of every
path the circle the V-bit cuts at that depth reaches no wall of the region, the point is in the region, and
the depth is within the depth of cut and the bit's radius. The carve is then compared with the Shapely
V-carve of the same samples, which it must follow to within the rounding of the two libraries.

The kernel's cases are skipped where the compiled extension is not installed.
validation/vcarve_check.py runs the same checks on thousands of shapes and prints the worst figures.
"""

import math
import os
import random
import unittest
from unittest import mock

import shapely
from shapely.geometry import LineString, Point

from studio import operations
from studio.cam import generate
from studio.operations import prepare, region

KERNEL = operations.kernel_available() and operations.kernel_vcarve_available()
BACKENDS = ['shapely'] + (['kernel'] if KERNEL else [])
GRID = 1e-6  # the kernel's grid, in millimetres
BAR = [[10, 10], [40, 10], [40, 18], [10, 18], [10, 10]]  # 30 by 8


def run(backend, project, paths):
    with mock.patch.dict(os.environ, KERFKITE_POLYGON_BACKEND=backend):
        return prepare(project, paths)


def carve(**values):
    return dict(
        dict(mode='router', operation='vcarve', v_resolution=0.5, v_angle=60, depth=2, tool_diameter=6), **values
    )


def letter(kind, x, y, w, h, s):
    """A block letter as closed contours: w wide, h tall, strokes s thick."""
    if kind == 'E':
        m = h / 2
        return [[[x, y], [x + w, y], [x + w, y + s], [x + s, y + s], [x + s, m + y - s / 2], [x + w * 0.7, m + y - s / 2], [x + w * 0.7, m + y + s / 2], [x + s, m + y + s / 2], [x + s, y + h - s], [x + w, y + h - s], [x + w, y + h], [x, y + h], [x, y]]]  # fmt: skip
    if kind == 'L':
        return [[[x, y], [x + w, y], [x + w, y + s], [x + s, y + s], [x + s, y + h], [x, y + h], [x, y]]]
    if kind == 'T':
        c = x + w / 2
        return [[[c - s / 2, y], [c + s / 2, y], [c + s / 2, y + h - s], [x + w, y + h - s], [x + w, y + h], [x, y + h], [x, y + h - s], [c - s / 2, y + h - s], [c - s / 2, y]]]  # fmt: skip
    if kind == 'O':  # an outline with a hole
        return [
            [[x, y], [x + w, y], [x + w, y + h], [x, y + h], [x, y]],
            [[x + s, y + s], [x + w - s, y + s], [x + w - s, y + h - s], [x + s, y + h - s], [x + s, y + s]],
        ]
    if kind == 'V':  # slanted strokes meeting at a point
        c = x + w / 2
        return [[[x, y + h], [c - s / 2, y], [c + s / 2, y], [x + w, y + h], [x + w - s, y + h], [c, y + s * 1.5], [x + s, y + h], [x, y + h]]]  # fmt: skip
    if kind == 'A':  # slanted strokes round a triangular hole
        c = x + w / 2
        return [
            [
                [x, y],
                [x + s, y],
                [c, y + h - 2 * s],
                [x + w - s, y],
                [x + w, y],
                [c + s / 2, y + h],
                [c - s / 2, y + h],
                [x, y],
            ],
        ]
    raise ValueError(kind)


def shapes(rng, count):
    """Text-like shapes, shapes with holes, thin strokes and flat areas, with settings that fit them."""
    for index in range(count):
        kind = index % 8
        x, y = rng.uniform(5, 20), rng.uniform(5, 20)
        resolution = rng.choice([0.2, 0.3, 0.5, 0.5, 1])
        if kind < 3:  # a word of block letters
            h, s = rng.uniform(6, 14), rng.uniform(0.6, 3)
            w = h * rng.uniform(0.5, 0.8)
            paths = []
            for i in range(rng.randrange(1, 4)):
                paths += letter(rng.choice('ELTOVA'), x + i * (w + 1.5), y, w, h, min(s, w / 3.2))
        elif kind == 3:  # a frame: a flat area with an island
            w, h = rng.uniform(8, 25), rng.uniform(8, 20)
            paths = letter('O', x, y, w, h, min(w, h) * rng.uniform(0.1, 0.4))
        elif kind == 4:  # a thin stroke at any slope, as thin as the allowance or thinner
            length, thick, turn = rng.uniform(8, 30), rng.uniform(0.3, 2.5), rng.uniform(0, math.pi / 2)
            ux, uy = math.cos(turn), math.sin(turn)
            paths = [[[x, y], [x + length * ux, y + length * uy], [x + length * ux - thick * uy, y + length * uy + thick * ux], [x - thick * uy, y + thick * ux], [x, y]]]  # fmt: skip
            paths = [[[px + 3, py] for px, py in paths[0]]]
        elif kind == 5:  # a flat area
            w, h = rng.uniform(10, 30), rng.uniform(10, 25)
            paths = [[[x, y], [x + w, y], [x + w, y + h], [x, y + h], [x, y]]]
            resolution = rng.choice([0.5, 1])
        elif kind == 6:  # a round shape: many short sides
            radius, sides = rng.uniform(3, 10), rng.choice([7, 24, 90])
            points = [[x + radius + radius * math.cos(2 * math.pi * i / sides), y + radius + radius * math.sin(2 * math.pi * i / sides)] for i in range(sides)]  # fmt: skip
            paths = [points + [points[0]]]
        else:  # a star: sharp and reflex corners
            n = rng.randrange(5, 11)
            angles = sorted(rng.uniform(0, 2 * math.pi) for _ in range(n))
            points = [[x + 10 + 9 * r * math.cos(a), y + 10 + 9 * r * math.sin(a)] for a, r in ((a, rng.uniform(0.3, 1)) for a in angles)]  # fmt: skip
            paths = [points + [points[0]]]
        if rng.random() < 0.5:
            paths = [[[round(px, 3), round(py, 3)] for px, py in path] for path in paths]
        settings = carve(
            v_resolution=resolution,
            v_angle=rng.choice([20, 30, 60, 60, 90, 120, 150]),
            depth=rng.choice([0.3, 1, 2, 5, 20]),
            tool_diameter=rng.choice([0.5, 3, 6, 6, 12, 40]),
        )
        try:
            region(paths)
        except ValueError:
            continue  # a shape that happens to cross itself
        yield settings, paths


class Carve:
    """The moves of a carve, indexed to find the nearest to a point."""

    def __init__(self, paths):
        self.points = [point for path in paths for point in path]
        self.moves = shapely.linestrings([[a, b] for path in paths for a, b in zip(path, path[1:])])
        self.tree = shapely.STRtree(self.moves)
        self.length = float(shapely.length(self.moves).sum())

    def furthest(self, points):
        """The greatest distance from any of the points to the nearest move."""
        if not points:
            return 0.0
        return float(self.tree.query_nearest(shapely.points(points), return_distance=True, all_matches=False)[1].max())


def measure(case, paths, depths, settings, source, slack):
    """Check every point and every move of a V-carve against the region; the least room left to a wall."""
    g = region(source)
    resolution, radius, depth = settings['v_resolution'], settings['tool_diameter'] / 2, settings['depth']
    slope = math.tan(math.radians(settings['v_angle'] / 2))
    case.assertEqual(len(paths), len(depths))
    xy = [point for path in paths for point in path]
    z = [value for row in depths for value in row]
    case.assertEqual(len(xy), len(z))
    case.assertTrue(all(len(path) >= 2 for path in paths))
    points = shapely.points(xy)
    wall = shapely.distance(points, g.boundary)
    inside = shapely.contains(g, points)
    room = math.inf
    for (x, y), value, away, within in zip(xy, z, wall, inside):
        cut = -value * slope
        case.assertLessEqual(value, 0)
        case.assertGreaterEqual(value, -depth)
        case.assertLessEqual(cut, radius * (1 + 1e-12))
        if value < 0:
            case.assertTrue(within, (x, y))
            # The cut is narrower than the room to the nearest wall by the sampling interval.
            case.assertLessEqual(cut, away - resolution + slack, (x, y, value))
        else:
            case.assertTrue(within or away <= slack, (x, y))
        room = min(room, away - cut)
    # Between two points the cutter moves in a straight line: the middle of each move is measured too.
    middles, cuts, longest = [], [], 0
    for path, row in zip(paths, depths):
        for (ax, ay), (bx, by), za, zb in zip(path, path[1:], row, row[1:]):
            middles.append(((ax + bx) / 2, (ay + by) / 2))
            cuts.append(-(za + zb) / 2 * slope)
            longest = max(longest, math.dist((ax, ay), (bx, by)))
    case.assertLessEqual(longest, resolution + slack)
    for away, cut in zip(shapely.distance(shapely.points(middles), g.boundary), cuts):
        if cut > 0:
            case.assertLessEqual(cut, away)
            room = min(room, away - cut)
    return room


class VCarveSafetyTests(unittest.TestCase):
    def test_the_bit_never_cuts_outside_the_outline(self):
        for backend in BACKENDS:
            rng = random.Random(2027)
            carved = 0
            for settings, source in shapes(rng, 160 if backend == 'kernel' else 40):
                with self.subTest(backend=backend, settings=settings, source=source):
                    try:
                        paths, depths, warnings = run(backend, settings, source)
                    except ValueError as error:
                        self.assertEqual(str(error), 'Operation produced no toolpaths')
                        continue
                    carved += 1
                    self.assertEqual(warnings, [operations.VCARVE_WARNING])
                    room = measure(self, paths, depths, settings, source, 2 * GRID if backend == 'kernel' else 1e-9)
                    self.assertGreaterEqual(room, 0)
            self.assertGreater(carved, 25)

    def test_depth_follows_the_width_the_bit_angle_the_depth_of_cut_and_the_tool(self):
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                # The middle of the bar is 4 mm from its sides: 3.5 mm of cut radius after the allowance.
                deepest = lambda **values: min(min(row) for row in run(backend, carve(**values), [BAR])[1])
                self.assertAlmostEqual(deepest(v_angle=90, depth=6, tool_diameter=12), -3.5, places=9)
                self.assertAlmostEqual(
                    deepest(v_angle=120, depth=6, tool_diameter=12), -3.5 / math.tan(math.radians(60)), places=9
                )
                self.assertEqual(deepest(v_angle=90, depth=2, tool_diameter=12), -2)
                self.assertAlmostEqual(deepest(v_angle=90, depth=6, tool_diameter=5), -2.5, places=9)
                self.assertAlmostEqual(deepest(v_angle=90, depth=6, tool_diameter=12, v_resolution=1), -3, places=9)

    def test_refusals_are_the_same_on_either_backend(self):
        crossing = [[10, 10], [30, 30], [30, 10], [10, 30], [10, 10]]
        many = [
            [[2 + i, 2 + i], [88 - i, 2 + i], [88 - i, 88 - i], [2 + i, 88 - i], [2 + i, 2 + i]] for i in range(0, 8, 2)
        ]
        cases = [
            (dict(mode='laser'), [BAR], 'V-carving requires router mode'),
            (dict(mode='laser'), [crossing], 'Repair self-crossing or zero-area contours before CAM'),
            (dict(v_resolution=0.05), [crossing], 'Repair self-crossing or zero-area contours before CAM'),
            (dict(mode='laser', v_resolution=0.05), [BAR], 'V-carving requires router mode'),
            (dict(v_resolution=0.05, v_angle=5), [BAR], 'v_resolution must be between 0.1 and 5'),
            (dict(v_resolution='fine'), [BAR], 'v_resolution must be numeric'),
            (dict(v_angle=171, depth=0), [BAR], 'v_angle must be between 10 and 170'),
            (dict(depth=51, tool_diameter=0), [BAR], 'depth must be between 0.01 and 50'),
            (dict(tool_diameter=0.05), [BAR], 'tool_diameter must be between 0.1 and 100'),
            (dict(v_resolution=0.1), many, 'V-carve outline too complex; increase sampling interval'),
            (dict(), [[[10, 10], [40, 10], [40, 10.8], [10, 10.8], [10, 10]]], 'Operation produced no toolpaths'),
            (dict(), [BAR[:-1]], 'This operation needs closed contours'),
            (dict(path_depths=[[-1] * 5]), [BAR], 'Per-vertex depth contours require the line operation'),
        ]
        for backend in BACKENDS:
            for values, paths, message in cases:
                with self.subTest(backend=backend, values=values):
                    with self.assertRaisesRegex(ValueError, '^' + message + '$'):
                        run(backend, carve(**values), paths)

    def test_a_job_is_generated_with_a_depth_at_every_move(self):
        for backend in BACKENDS:
            with self.subTest(backend=backend), mock.patch.dict(os.environ, KERFKITE_POLYGON_BACKEND=backend):
                job = generate(
                    dict(carve(v_angle=90, depth=6, step=2, tool_diameter=12), protocol='grbl', width=90, height=90, feed=400, power=9000, plunge=90, paths=[BAR])
                )  # fmt: skip
                self.assertEqual(job['warnings'][1], operations.VCARVE_WARNING)
                self.assertEqual(len(job['prepared_paths']), len(job['prepared_depths']))
                cutting = [line for line in job['lines'] if line.startswith('G1 X')]
                self.assertTrue(cutting and all(' Z' in line and ' F90.0' in line for line in cutting))
                # The bar allows 3.5 mm, which two levels of 2 mm reach: every path is cut at each level.
                self.assertAlmostEqual(min(segment[4] for segment in job['segments']), -3.5, places=3)
                self.assertEqual(
                    sum(line == 'G0 Z5.000' for line in job['lines']), 1 + 2 * 2 * len(job['prepared_paths'])
                )


@unittest.skipUnless(KERNEL, 'the compiled extension is not installed')
class VCarveKernelTests(unittest.TestCase):
    def test_the_carve_follows_the_shapely_medial_axis_of_the_same_samples(self):
        # The Shapely code is given the samples the kernel took, so the two diagrams differ only by the two
        # libraries' rounding: by nanometres, except where samples lie almost on one circle and a Voronoi
        # vertex moves far for a small change in them.
        rng = random.Random(41)
        compared = 0
        for settings, source in shapes(rng, 120):
            with self.subTest(settings=settings, source=source):
                try:
                    paths, depths, _ = run('kernel', settings, source)
                except ValueError:
                    continue
                request = operations._vcarve_settings(settings, 'router')
                samples = operations.kernel_vcarve(source, samples=True, **request)[2]
                reference, heights = operations.sampled_medial_axis(
                    region(source), [Point(sample) for sample in samples], **request
                )
                ours, theirs = Carve(paths), Carve(reference)
                self.assertLess(
                    max(theirs.furthest(ours.points), ours.furthest(theirs.points)), 0.001 * settings['v_resolution']
                )
                self.assertLess(abs(ours.length - theirs.length), 1e-5 * theirs.length + 1e-4)
                self.assertAlmostEqual(min(min(row) for row in depths), min(min(row) for row in heights), places=4)
                self.assertLessEqual(len(paths), len(reference))
                compared += 1
        self.assertGreater(compared, 80)

    def test_the_carve_stays_near_the_shapely_fallback_as_shipped(self):
        # The fallback starts its samples wherever GEOS starts each ring, so its spokes stand elsewhere along
        # the walls. Where either carve cuts a circle of at least the sampling interval in radius, the other
        # has a path within an interval and a half, for a shape with no corner turning inwards. Round such a
        # corner the method leaves gaps wider than the interval in either carve, and the two differ by more.
        rng = random.Random(43)
        ours_length = theirs_length = 0
        convex = 0
        for settings, source in shapes(rng, 80):
            with self.subTest(settings=settings, source=source):
                try:
                    paths, depths, _ = run('kernel', settings, source)
                    reference, heights, _ = run('shapely', settings, source)
                except ValueError:
                    continue
                ours, theirs = Carve(paths), Carve(reference)
                least = settings['v_resolution'] / math.tan(math.radians(settings['v_angle'] / 2))
                deep = lambda rows, zs: [
                    point for path, row in zip(rows, zs) for point, value in zip(path, row) if -value >= least
                ]
                gap = (
                    max(theirs.furthest(deep(paths, depths)), ours.furthest(deep(reference, heights)))
                    / settings['v_resolution']
                )
                g = region(source)
                if g.geom_type == 'Polygon' and not g.interiors and g.convex_hull.area - g.area < 1e-9 * g.area:
                    convex += 1
                    self.assertLess(gap, 1.5)
                self.assertLess(gap, 8)
                ours_length += ours.length
                theirs_length += theirs.length
        self.assertGreater(convex, 10)
        self.assertLess(abs(ours_length - theirs_length), 0.02 * theirs_length)

    def test_pieces_are_joined_and_ordered_for_short_travel(self):
        settings = carve(v_resolution=0.3)
        source = [contour for i in range(4) for contour in letter('ELTO'[i], 5 + i * 11, 5, 8, 12, 1.6)]
        paths, depths, _ = run('kernel', settings, source)
        reference, _, _ = run('shapely', settings, source)
        self.assertLess(len(paths), 0.6 * len(reference))
        travel = lambda rows: sum(math.dist(a[-1], b[0]) for a, b in zip(rows, rows[1:]))
        self.assertLess(travel(paths), 0.1 * travel(reference))
        # No move is made twice.
        moves = [tuple(sorted((tuple(a), tuple(b)))) for path in paths for a, b in zip(path, path[1:])]
        self.assertEqual(len(moves), len(set(moves)))
        # The stated order: the first path starts at the smallest end of all, and each next path starts at
        # the end, of all the paths still to come, that is nearest to where the last one finished.
        ends = [tuple(path[0]) for path in paths] + [tuple(path[-1]) for path in paths]
        self.assertEqual(tuple(paths[0][0]), min(ends))
        for index in range(len(paths) - 1):
            here = paths[index][-1]
            later = [point for path in paths[index + 1 :] for point in (path[0], path[-1])]
            self.assertLessEqual(
                math.dist(here, paths[index + 1][0]), min(math.dist(here, point) for point in later) + 1e-12
            )
        # Where a path ends, no single other path ends too unless a third passes or ends there: two pieces
        # alone at a junction are always joined.
        counts = {}
        for path in paths:
            for point in (path[0], path[-1]) if path[0] != path[-1] else ():
                counts[tuple(point)] = counts.get(tuple(point), 0) + 1
        passing = {tuple(point) for path in paths for point in path[1:-1]}
        self.assertFalse([point for point, count in counts.items() if count == 2 and point not in passing])

    def test_every_coordinate_is_on_the_kernel_grid_and_the_result_repeats(self):
        settings, source = carve(v_resolution=0.3, v_angle=45), letter('A', 7.3, 9.1, 14, 16, 2.2)
        paths, depths, _ = run('kernel', settings, source)
        for value in (value for path in paths for point in path for value in point):
            self.assertEqual(value, round(value * 1e6) / 1e6)
        self.assertEqual((paths, depths), run('kernel', settings, source)[:2])
        # The contours in another order, and each started elsewhere, are the same region and the same carve.
        turned = [path[2:-1] + path[:3] for path in reversed(source)]
        self.assertEqual((paths, depths), run('kernel', settings, turned)[:2])

    def test_studio_asks_the_native_core_and_uses_its_answer_unchanged(self):
        answer = '{"paths": [[[1.0, 2.0], [3.0, 4.0]]], "depths": [[-0.0, -1.25]]}'
        with mock.patch.object(operations, '_core_call', return_value=answer) as core:
            paths, depths, warnings = run('kernel', carve(v_angle=75, depth=3, tool_diameter=8), [BAR])
        self.assertEqual((paths, depths), ([[[1.0, 2.0], [3.0, 4.0]]], [[-0.0, -1.25]]))
        self.assertEqual(warnings, [operations.VCARVE_WARNING])
        command, request = core.call_args.args
        self.assertEqual(command, 'operation')
        self.assertIn('"op": "vcarve"', request)
        for part in ('"resolution": 0.5', '"angle": 75.0', '"depth": 3.0', '"radius": 4.0'):
            self.assertIn(part, request)

    def test_an_extension_without_v_carving_falls_back_to_shapely(self):
        with mock.patch.object(operations, '_vcarve_installed', False):
            paths, depths, _ = run('kernel', carve(), [BAR])
        self.assertEqual((paths, depths), run('shapely', carve(), [BAR])[:2])


class VCarveFallbackTests(unittest.TestCase):
    def test_the_shapely_fallback_is_used_without_the_extension(self):
        with (
            mock.patch.object(operations, '_kernel_installed', False),
            mock.patch.object(operations, '_core_call', None),
        ):
            paths, depths, warnings = prepare(carve(), [BAR])
        self.assertEqual(warnings, [operations.VCARVE_WARNING])
        self.assertEqual((paths, depths), run('shapely', carve(), [BAR])[:2])
        # One path for each piece of the diagram, as before the kernel.
        self.assertTrue(all(LineString(path).length <= 0.5 * len(path) for path in paths))
        self.assertTrue(all(Point(path[0]).distance(region([BAR])) < 1e-9 for path in paths))


if __name__ == '__main__':
    unittest.main()
