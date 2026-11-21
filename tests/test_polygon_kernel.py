"""The polygon kernel of the native core, through `kerfkite-cam polygon`: properties that need no oracle.

A correct answer is valid; the areas of the set operations satisfy their identities; a region united with
itself is itself; an offset contains, or is contained in, its source, and out and back in still contains
it; hatch rows lie in the region and their lengths times the spacing give its area. Exact expectations for
small cases are in native/tests/polygon_selftest.cpp, and validation/polygon_kernel_check.py compares the
kernel with Shapely.

Delaunay triangulations are checked exactly in Python's whole numbers (no point inside any triangle's
circle, the hull complete, every point present, Euler's formula), Voronoi edges against Shapely's, and the
distance to a boundary against Shapely's.

The tool is taken from KERFKITE_CAM, or from build/native as the build instructions leave it; without it
the tests are skipped.
"""

import json
import math
import os
import random
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def find_cli():
    named = os.environ.get('KERFKITE_CAM')
    places = [Path(named)] if named else []
    places += [ROOT / 'build/native/kerfkite-cam', ROOT / 'build/native/Release/kerfkite-cam.exe']
    return next((str(place) for place in places if place.is_file()), None)


CLI = find_cli()
STEP = (math.pi / 2) / 32
OUTER = 1 / math.cos(0.53125 * STEP) - 1  # how far outside the true arc a round corner may lie


def ask(requests):
    """The kernel's answers to several requests, each {'ok': ...} or {'error': message}."""
    with tempfile.TemporaryDirectory() as folder:
        cases = Path(folder) / 'cases.jsonl'
        cases.write_text(''.join(json.dumps(request) + '\n' for request in requests))
        done = subprocess.run([CLI, 'polygon-batch', str(cases)], capture_output=True, text=True, check=True)
    return [json.loads(row) for row in done.stdout.splitlines()]


def blob(rng, cx, cy, radius, count):
    """A simple ring round a centre, on the kernel's grid of a millionth of a millimetre."""
    angles = sorted(rng.uniform(0, 2 * math.pi) for _ in range(count))
    return [
        [round(cx + r * math.cos(a), 6), round(cy + r * math.sin(a), 6)]
        for a in angles
        for r in [radius * rng.uniform(0.5, 1)]
    ]


def rings(answer):
    return [p['shell'] for p in answer['polygons']] + [hole for p in answer['polygons'] for hole in p['holes']]


def length(answer):
    """The length of boundary of an answer."""
    return sum(math.dist(a, b) for ring in rings(answer) for a, b in zip(ring, ring[1:] + ring[:1]))


@unittest.skipUnless(CLI, 'kerfkite-cam is not built: set KERFKITE_CAM or build native/ into build/native')
class PolygonKernel(unittest.TestCase):
    def cases(self, count, seed):
        rng = random.Random(seed)
        for _ in range(count):
            size = 10 ** rng.uniform(-3, 2)  # a micrometre to a hundred millimetres
            a = [blob(rng, rng.uniform(-size, size), rng.uniform(-size, size), size, rng.randint(3, 24))]
            b = [blob(rng, rng.uniform(-size, size), rng.uniform(-size, size), size, rng.randint(3, 24))]
            if rng.random() < 0.4:
                a.append(blob(rng, a[0][0][0], a[0][0][1], size / 3, rng.randint(3, 9)))
            yield size, a, b

    def test_set_operations_are_valid_and_their_areas_agree(self):
        cases = list(self.cases(120, 1))
        operations = ('union', 'intersection', 'difference', 'xor')
        requests = [{'op': op, 'a': a, 'b': b, 'check': True} for _, a, b in cases for op in operations]
        requests += [{'op': 'normalise', 'rings': side, 'check': True} for _, a, b in cases for side in (a, b)]
        answers = ask(requests)
        for index in range(len(cases)):
            union, both, less, either = (answers[index * 4 + k]['ok'] for k in range(4))
            first, second = (answers[len(cases) * 4 + index * 2 + k]['ok'] for k in range(2))
            for answer in (union, both, less, either, first, second):
                self.assertTrue(answer['valid'], answer.get('reason'))
            # Each area is that of a region whose crossings were rounded to the grid on their own: they
            # agree to a few millionths of a millimetre along the boundaries involved.
            slack = (length(first) + length(second)) * 6e-6 + 1e-11
            self.assertAlmostEqual(union['area'] + both['area'], first['area'] + second['area'], delta=slack)
            self.assertAlmostEqual(either['area'], union['area'] - both['area'], delta=slack)
            self.assertAlmostEqual(less['area'], first['area'] - both['area'], delta=slack)
            self.assertGreaterEqual(union['area'] + slack, max(first['area'], second['area']))

    def test_a_region_united_or_intersected_with_itself_is_itself(self):
        cases = list(self.cases(60, 2))
        regions = [answer['ok'] for answer in ask([{'op': 'union', 'a': a, 'b': b} for _, a, b in cases])]
        again = []
        for region in regions:
            again += [{'op': op, 'a': rings(region), 'b': rings(region)} for op in ('union', 'intersection')]
        for index, region in enumerate(regions):
            for answer in ask(again[index * 2 : index * 2 + 2]):
                self.assertEqual(answer['ok']['polygons'], region['polygons'])
        emptied = ask([{'op': op, 'a': rings(r), 'b': rings(r)} for r in regions for op in ('difference', 'xor')])
        self.assertTrue(all(answer['ok']['polygons'] == [] for answer in emptied))

    def test_an_offset_is_valid_nested_with_its_source_and_survives_the_way_back(self):
        cases = list(self.cases(60, 3))
        sources = [answer['ok'] for answer in ask([{'op': 'union', 'a': a, 'b': b} for _, a, b in cases])]
        requests = []
        for (size, _, _), source in zip(cases, sources):
            distance = round(size * 0.07, 6)
            requests += [
                {'op': 'offset', 'rings': rings(source), 'distance': d, 'check': True} for d in (distance, -distance)
            ]
        moved = [answer['ok'] for answer in ask(requests)]
        checks = []
        for index, ((size, _, _), source) in enumerate(zip(cases, sources)):
            distance = round(size * 0.07, 6)
            grown, shrunk = moved[index * 2], moved[index * 2 + 1]
            self.assertTrue(grown['valid'] and shrunk['valid'])
            # Grown: larger by at least the distance along the boundary, less what concave corners lose.
            self.assertGreater(grown['area'], source['area'])
            self.assertLess(shrunk['area'], source['area'])
            # Nothing of the source lies outside the grown region, nothing of the shrunk one outside the source.
            checks.append({'op': 'difference', 'a': rings(source), 'b': rings(grown)})
            checks.append({'op': 'difference', 'a': rings(shrunk), 'b': rings(source)})
            # Out and back in: the source, to the arc allowance (round corners are drawn outside the arc).
            checks.append({'op': 'offset', 'rings': rings(grown), 'distance': -distance})
        results = ask(checks)
        for index, ((size, _, _), source) in enumerate(zip(cases, sources)):
            distance = round(size * 0.07, 6)
            outside_grown, outside_source, back = (results[index * 3 + k]['ok'] for k in range(3))
            self.assertEqual(outside_grown['polygons'], [])
            self.assertEqual(outside_source['polygons'], [])
            allowance = length(source) * (distance * OUTER + 8e-6) + 1e-11
            self.assertGreaterEqual(back['area'] + allowance, source['area'])

    def test_a_disc_grown_and_shrunk_has_the_areas_of_its_circles(self):
        # A disc of radius 5 drawn by the kernel, grown by 2 and shrunk by 2: corners lie outside each true
        # circle by at most OUTER of its radius, so each area is that of its circle and at most 2 OUTER more.
        disc = ask([{'op': 'artwork', 'shapes': [{'type': 'disc', 'centre': [1, 2], 'diameter': 10}]}])[0]['ok']
        grown, shrunk = (
            answer['ok']
            for answer in ask([{'op': 'offset', 'rings': rings(disc), 'distance': d, 'check': True} for d in (2, -2)])
        )
        for answer, radius in ((disc, 5), (grown, 7)):
            self.assertGreater(answer['area'], math.pi * radius**2)
            self.assertLess(answer['area'], math.pi * radius**2 * (1 + 2.5 * OUTER))
        # Shrinking a drawn disc moves its straight edges in: the result is the same polygon, smaller.
        self.assertAlmostEqual(shrunk['area'], disc['area'] * (3 / 5) ** 2, delta=1e-4)
        self.assertTrue(grown['valid'] and shrunk['valid'])
        self.assertEqual(len(shrunk['polygons'][0]['shell']), len(disc['polygons'][0]['shell']))

    def test_hatch_rows_lie_in_the_region_and_cover_it(self):
        rng = random.Random(5)
        outline = blob(rng, 50, 40, 30, 40)
        hole = blob(rng, 50, 40, 8, 12)
        region = ask([{'op': 'normalise', 'rings': [outline, hole]}])[0]['ok']
        spacing = 0.05
        ys = [5 + spacing * (i + 0.5) for i in range(int(70 / spacing))]
        rows = [[[0, round(y, 6)], [100, round(y, 6)]] for y in ys]
        answer = ask([{'op': 'clip-lines', 'rings': [outline, hole], 'lines': rows}])[0]['ok']
        covered = 0
        middles = []
        for row, parts in zip(rows, answer['segments']):
            last = -math.inf
            for (x0, y0), (x1, y1) in parts:
                self.assertEqual((y0, y1), (row[0][1], row[0][1]))
                self.assertLess(last, x0)  # in order along the row, and apart
                self.assertLess(x0, x1)
                last = x1
                covered += x1 - x0
                middles.append([round((x0 + x1) / 2, 6), y0])
        # The rows are strips: their lengths times the spacing are the area, to a strip along the boundary.
        self.assertAlmostEqual(covered * spacing, region['area'], delta=length(region) * spacing)
        where = ask([{'op': 'locate', 'rings': [outline, hole], 'points': middles}])[0]['ok']['where']
        self.assertTrue(all(place != 'outside' for place in where))

    def test_artwork_draws_dark_and_clear_shapes_in_order(self):
        shapes = [
            {'type': 'rectangle', 'centre': [5, 5], 'width': 10, 'height': 10},
            {'type': 'disc', 'centre': [5, 5], 'diameter': 4, 'polarity': 'clear'},
            {'type': 'stroke', 'points': [[0, 5], [10, 5]], 'width': 1},
        ]
        image = ask([{'op': 'artwork', 'shapes': shapes, 'offsets': [0.5], 'check': True}])[0]['ok']
        # The square, less the disc but for the bar across it, with the bar's round ends beyond the sides.
        hole = math.pi * 4 - (2 * math.asin(0.25) * 4 + 2 * 0.5 * math.sqrt(4 - 0.25))
        expected = 100 - hole + math.pi * 0.25
        self.assertTrue(image['valid'])
        self.assertAlmostEqual(image['area'], expected, delta=0.01)
        self.assertEqual(sum(len(p['holes']) for p in image['polygons']), 2)
        # The same shapes with the clear disc first: it removes nothing.
        first = ask([{'op': 'artwork', 'shapes': [shapes[1], shapes[0], shapes[2]]}])[0]['ok']
        self.assertAlmostEqual(first['area'], 100 + math.pi * 0.25, delta=0.01)
        contour = image['contours'][0]
        self.assertTrue(contour['valid'])
        self.assertGreater(contour['area'], image['area'])

    def test_contours_are_refused_as_the_application_refuses_them(self):
        square = [[0, 0], [10, 0], [10, 10], [0, 10], [0, 0]]
        answers = ask(
            [
                {'op': 'compose', 'paths': [square[:-1]]},
                {'op': 'compose', 'paths': [[[0, 0], [10, 10], [10, 0], [0, 10], [0, 0]]]},
                {'op': 'compose', 'paths': [square, square]},
                {'op': 'compose', 'paths': [square, [[2, 2], [4, 2], [4, 4], [2, 4], [2, 2]]]},
                {'op': 'offset', 'rings': [square], 'distance': float('1e9')},
                {'op': 'nothing'},
            ]
        )
        self.assertEqual(answers[0], {'error': 'This operation needs closed contours'})
        self.assertEqual(answers[1], {'error': 'Repair self-crossing or zero-area contours before CAM'})
        self.assertEqual(answers[2], {'error': 'Contours cancel under even-odd fill'})
        self.assertEqual(answers[3]['ok']['area'], 96)
        self.assertIn('beyond the range', answers[4]['error'])
        self.assertEqual(answers[5], {'error': 'Unknown polygon operation: nothing'})


def grid(point):
    """A point in whole nanometres, as the kernel holds it."""
    return (round(point[0] * 1e6), round(point[1] * 1e6))


def side(a, b, c):
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def in_circle(a, b, c, d):
    """Positive when d is inside the circle through a, b and c, which run counter-clockwise: exact, in
    Python's whole numbers."""
    ax, ay, bx, by, cx, cy = a[0] - d[0], a[1] - d[1], b[0] - d[0], b[1] - d[1], c[0] - d[0], c[1] - d[1]
    return (
        (ax * ax + ay * ay) * (bx * cy - by * cx)
        + (bx * bx + by * by) * (cx * ay - cy * ax)
        + (cx * cx + cy * cy) * (ax * by - ay * bx)
    )


def point_sets(rng, count):
    """Points of the kinds a V-carve meets: scattered, on lattices, on one line, on one circle, repeated."""
    on_circle = [
        (sx * x, sy * y)
        for a, b in ((16, 63), (25, 60), (33, 56), (39, 52), (0, 65), (63, 16), (60, 25), (56, 33), (52, 39))
        for x, y in ((a, b), (b, a))
        for sx in (1, -1)
        for sy in (1, -1)
    ]
    for index in range(count):
        kind, n = index % 7, rng.randrange(1, 70)
        if kind == 0:
            yield [[rng.uniform(0, 50), rng.uniform(0, 50)] for _ in range(n)]
        elif kind == 1:  # a lattice, with repeats
            yield [[rng.randrange(0, 6) * 0.5, rng.randrange(0, 6) * 0.5] for _ in range(n)]
        elif kind == 2:  # on one line
            dx, dy = rng.choice([(0.5, 0), (0, 0.25), (0.25, 0.75), (-0.3, 0.1)])
            yield [[10 + k * dx, 10 + k * dy] for k in rng.sample(range(-200, 200), n)]
        elif kind == 3:  # on one circle, with and without its centre
            yield [[x * 0.001, y * 0.001] for x, y in rng.sample(on_circle, min(n, len(on_circle)))] + (
                [[0, 0]] if index % 2 else []
            )
        elif kind == 4:  # the samples of a rectangle's outline
            w, h = rng.randrange(1, 9), rng.randrange(1, 9)
            yield (
                [[i * 0.5, 0] for i in range(2 * w + 1)]
                + [[i * 0.5, h] for i in range(2 * w + 1)]
                + [[0, j * 0.5] for j in range(2 * h + 1)]
                + [[w, j * 0.5] for j in range(2 * h + 1)]
            )
        elif kind == 5:  # a few nanometres apart
            yield [[rng.randrange(-3, 4) * 1e-6, rng.randrange(-3, 4) * 1e-6] for _ in range(n)]
        else:  # far apart and close together at once
            yield [[rng.choice([0, 1, 2, 1e-6, 2e-6, 100]), rng.choice([0, 1, 5e-6, 100, -100])] for _ in range(n)]


@unittest.skipUnless(CLI, 'kerfkite-cam is not built: set KERFKITE_CAM or build native/ into build/native')
class VoronoiDiagrams(unittest.TestCase):
    def test_triangulations_are_delaunay_and_complete(self):
        sets = list(point_sets(random.Random(5), 210))
        for points, answer in zip(sets, ask([{'op': 'delaunay', 'points': points} for points in sets])):
            done = answer['ok']
            at = [grid(point) for point in done['points']]
            triangles, edges = done['triangles'], done['edges']
            self.assertEqual(at, sorted(set(grid(point) for point in points)))
            for triangle in triangles:
                a, b, c = (at[i] for i in triangle)
                self.assertGreater(side(a, b, c), 0)
                # No point is inside the circle through the corners of any triangle.
                self.assertFalse([d for d in at if in_circle(a, b, c, d) > 0])
            n = len(at)
            if n < 2:
                self.assertEqual((triangles, edges), ([], []))
                continue
            # Euler's formula, counting the face outside, and every point in some edge.
            self.assertEqual(n - len(edges) + len(triangles) + 1, 2)
            self.assertEqual({i for edge in edges for i in edge[:2]}, set(range(n)))
            if all(side(at[0], at[1], point) == 0 for point in at):
                self.assertEqual(
                    (len(triangles), [edge[:2] for edge in edges]), (0, [[i, i + 1] for i in range(n - 1)])
                )
                continue
            hull = [edge for edge in edges if (edge[2] is None) != (edge[3] is None)]
            for a, b, left, right in hull:
                turns = [side(at[a], at[b], point) for point in at]
                self.assertTrue(all(t >= 0 for t in turns) if left is not None else all(t <= 0 for t in turns))
            # With h points on the hull a complete triangulation has 2n - 2 - h triangles and 3n - 3 - h edges.
            self.assertEqual((len(triangles), len(edges)), (2 * n - 2 - len(hull), 3 * n - 3 - len(hull)))
            self.assertFalse([edge for edge in edges if edge[2] is None and edge[3] is None])

    def test_voronoi_edges_are_those_shapely_finds(self):
        from shapely.geometry import MultiPoint, box
        from shapely.ops import voronoi_diagram
        import shapely

        rng = random.Random(7)
        sets = [
            [[round(rng.uniform(0, 40), 6), round(rng.uniform(0, 30), 6)] for _ in range(rng.randrange(3, 120))]
            for _ in range(40)
        ]
        frame = [[-5, -5], [45, 35]]
        answers = ask([{'op': 'voronoi', 'points': points, 'box': frame} for points in sets])
        for points, answer in zip(sets, answers):
            ours = shapely.linestrings([edge['ends'] for edge in answer['ok']['edges']])
            theirs = voronoi_diagram(MultiPoint(points), envelope=box(-60, -60, 100, 90), edges=True).intersection(
                box(-5, -5, 45, 35)
            )
            theirs = shapely.get_parts(theirs)
            theirs = theirs[shapely.length(theirs) > 1e-5]
            # The same edges: the same total length, and every end of one diagram on an edge of the other.
            self.assertAlmostEqual(float(shapely.length(ours).sum()), float(shapely.length(theirs).sum()), delta=1e-4)
            for some, others in ((ours, theirs), (theirs, ours)):
                ends = shapely.points(shapely.get_coordinates(some))
                self.assertLess(
                    float(
                        shapely.STRtree(others).query_nearest(ends, return_distance=True, all_matches=False)[1].max()
                    ),
                    1e-5,
                )
            # Each edge lies between its two sites: its ends are equally far from both, to the rounding.
            at = answer['ok']['points']
            for edge in answer['ok']['edges']:
                for end in edge['ends']:
                    near, far = (math.dist(end, at[i]) for i in edge['sites'])
                    self.assertAlmostEqual(near, far, delta=3e-6)

    def test_degenerate_points_give_the_diagram_without_edges_of_no_length(self):
        square = [[0, 0], [10, 0], [10, 10], [0, 10]]
        line = [[0, 0], [4, 4], [10, 10], [4, 4]]
        answers = ask(
            [
                {'op': 'voronoi', 'points': square, 'box': [[-5, -5], [15, 15]]},
                {'op': 'voronoi', 'points': line, 'box': [[0, 0], [10, 10]]},
                {'op': 'voronoi', 'points': [[1, 1]], 'box': [[0, 0], [10, 10]]},
                {'op': 'voronoi', 'points': [[0, 0], [3e-6, 0], [0, 3e-6]], 'box': [[-1, -1], [1, 1]]},
                {'op': 'voronoi', 'points': [[0, 0], [1e9, 0]], 'box': [[-1, -1], [1, 1]]},
            ]
        )
        # Four corners on one circle: four edges from the centre, and nothing between the two triangles.
        ends = [edge['ends'] for edge in answers[0]['ok']['edges']]
        self.assertEqual(len(ends), 4)
        self.assertTrue(all([5.0, 5.0] in pair for pair in ends))
        # Three points on a line, one given twice: two parallel bisectors.
        self.assertEqual(
            [edge['ends'] for edge in answers[1]['ok']['edges']], [[[0.0, 4.0], [4.0, 0.0]], [[4.0, 10.0], [10.0, 4.0]]]
        )
        self.assertEqual(answers[2]['ok']['edges'], [])
        # A centre between grid points is rounded to the nearest, halves upwards: (1.5, 1.5) nm becomes (2, 2).
        self.assertTrue(all([2e-6, 2e-6] in edge['ends'] for edge in answers[3]['ok']['edges']))
        self.assertIn('beyond the range', answers[4]['error'])

    def test_the_distance_to_a_boundary_is_shapelys(self):
        from shapely.geometry import Point, Polygon

        rng = random.Random(9)
        requests, shapes = [], []
        for _ in range(30):
            shell, hole = blob(rng, 20, 20, 15, rng.randint(5, 60)), blob(rng, 20, 20, 3, rng.randint(3, 9))
            points = [[round(rng.uniform(0, 40), 6), round(rng.uniform(0, 40), 6)] for _ in range(60)]
            requests.append({'op': 'distance', 'rings': [shell, hole], 'points': points})
            shapes.append((Polygon(shell, [hole]), points))
        for (shape, points), answer in zip(shapes, ask(requests)):
            for point, distance in zip(points, answer['ok']['distances']):
                self.assertAlmostEqual(distance, Point(point).distance(shape.boundary), delta=1e-9)


if __name__ == '__main__':
    unittest.main()
