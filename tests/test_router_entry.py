import math
import os
import random
import unittest
from unittest import mock

from shapely.geometry import LineString, Point, Polygon
from studio import operations, router_entry
from studio.cam import generate
from studio.operations import region
from studio.simulation import simulate


def ring(x, y, side):
    return [[x, y], [x + side, y], [x + side, y + side], [x, y + side], [x, y]]


def project(**values):
    return dict(
        dict(
            mode='router',
            protocol='grbl',
            width=90,
            height=90,
            safe=3,
            depth=2,
            step=1,
            power=9000,
            feed=400,
            plunge=90,
            tool_diameter=4,
            stepover=2,
            router_entry='ramp',
            entry_angle=3,
            stock_thickness=4,
            paths=[ring(10, 10, 30)],
        ),
        **values,
    )


class RouterEntryTest(unittest.TestCase):
    def test_ramp_depth_changes_follow_contour_with_finishing_lap(self):
        job = generate(project())
        rim = LineString(ring(10, 10, 30))
        for s in job['segments']:
            xy = math.dist(s[:2], s[2:4])
            if not xy:
                self.assertGreaterEqual(s[4], -1)
            else:
                self.assertAlmostEqual(rim.distance(LineString([s[:2], s[2:4]])), 0)
                self.assertLessEqual(abs(s[6] - s[4]) / xy, math.tan(math.radians(3)) + 1e-8)
        for level in (-1, -2):
            floor = [s for s in job['segments'] if s[4] == s[6] == level]
            self.assertAlmostEqual(sum(math.dist(s[:2], s[2:4]) for s in floor), 120)
        self.assertIn('G1 X40.000 Y10.000 Z-0.250 F90.0', job['lines'])
        self.assertEqual(job['lines'][-2:], ['G0 Z3.000', 'M5'])

    def test_compensated_ramp_with_tabs_keeps_plateaus(self):
        job = generate(project(operation='outside', stock_thickness=2, tabs={'width': 4, 'height': 1, 'count': 2}))
        path = job['prepared_paths'][0]
        caps = job['prepared_depths'][0]
        raised = [
            LineString([a, b])
            for i, (a, b) in enumerate(zip(path, path[1:]))
            if caps[i] == caps[i + 1] == -1 and a != b
        ]
        self.assertTrue(raised)
        for s in job['segments']:
            if s[4] >= -1 - 1e-8 and s[6] >= -1 - 1e-8:
                continue
            edge = LineString([s[:2], s[2:4]])
            self.assertFalse(any(edge.intersection(t).length > 1e-5 for t in raised))

    def test_helix_and_leads_stay_inside_pocket_outside_island(self):
        outer = ring(10, 10, 40)
        island = ring(24, 24, 12)
        job = generate(project(operation='pocket', router_entry='helix', entry_radius=1.5, paths=[outer, island]))
        allowed = Polygon(outer, holes=[island]).buffer(-2, quad_segs=32).buffer(0.00001)
        falling = 0
        for s in job['segments']:
            d = math.dist(s[:2], s[2:4])
            if d > 1e-7:
                self.assertTrue(allowed.covers(LineString([s[:2], s[2:4]])))
                if s[4] < s[6]:
                    falling += 1
                    self.assertLessEqual((s[6] - s[4]) / d, math.tan(math.radians(3)) + 1e-7)
        self.assertGreater(falling, 36)
        stock = simulate(
            dict(segments=job['segments'], x=10, y=10, width=40, height=40, resolution=37, tool_diameter=4)
        )
        self.assertEqual(stock['heights'][18][18], 0)
        self.assertLess(min(min(row) for row in stock['heights']), -1.9)

    def test_ineligible_entries_cannot_fall_back_to_plunge(self):
        cases = [
            dict(paths=[[[10, 10], [20, 20]]]),
            dict(operation='pocket', router_entry='helix', entry_radius=3),
            dict(operation='pocket', router_entry='helix', entry_radius=1.8, paths=[ring(10, 10, 7)]),
            dict(router_entry='helix'),
            dict(operation='drill'),
            dict(tool_shape='ball'),
            dict(entry_angle=0),
            dict(stock_thickness=1),
            dict(paths=[ring(0.1, 10, 30)]),
            dict(path_depths=[[-2] * 5]),
        ]
        for case in cases:
            with self.subTest(case=case), self.assertRaises(ValueError):
                generate(project(**case))


# The helix into a pocket is placed by the native core's polygon kernel where the compiled extension is
# installed, and by Shapely where it is not. Both are checked here against the pocket as drawn, with Shapely
# used only to measure distances: nothing below depends on where either code puts its vertices.
BACKENDS = ['shapely'] + (['kernel'] if operations.kernel_available() and operations.kernel_entries_available() else [])
# How much nearer a wall than its radius the cutter's centre may be measured. The kernel's coordinates are
# whole millionths of a millimetre, so a contour and a pass may each be half a step from where they were
# drawn; its round offsets lie outside the true arc, so they give nothing away. Shapely's round offsets lie
# inside the true arc, and it straightens a finely drawn curve a little before offsetting it: up to 0.2% of
# the offset between them.
SLACK = {'kernel': lambda bit: 2e-6, 'shapely': lambda bit: 0.002 * bit + 1e-7}


def pocket_shapes(seed, count):
    """Pockets as lists of closed contours: plain and odd shapes, with and without islands, on and off any grid."""
    rng = random.Random(seed)

    def polygon(cx, cy, size, kind):
        if kind == 0:  # a rectangle, axis-aligned
            w, h = size * rng.uniform(0.6, 1), size * rng.uniform(0.6, 1)
            points = [[cx - w, cy - h], [cx + w, cy - h], [cx + w, cy + h], [cx - w, cy + h]]
        elif kind == 1:  # a regular polygon at any angle, up to a fine circle
            n, turn = rng.choice([3, 5, 6, 8, 24, 90]), rng.uniform(0, 7)
            points = [
                [cx + size * math.cos(turn + 2 * math.pi * i / n), cy + size * math.sin(turn + 2 * math.pi * i / n)]
                for i in range(n)
            ]
        elif kind == 2:  # star-shaped, with sharp and reflex corners
            angles = sorted(rng.uniform(0, 2 * math.pi) for _ in range(rng.randrange(5, 12)))
            points = [
                [cx + size * r * math.cos(a), cy + size * r * math.sin(a)]
                for a, r in ((a, rng.uniform(0.45, 1)) for a in angles)
            ]
        elif kind == 3:  # an L, whose leg may be too narrow for the helix
            leg = size * rng.choice([0.3, 0.5, 0.8])
            points = [[cx - size, cy - size], [cx + size, cy - size], [cx + size, cy - size + leg], [cx - size + leg, cy - size + leg], [cx - size + leg, cy + size], [cx - size, cy + size]]  # fmt: skip
        else:  # two lobes joined by a neck
            neck = size * rng.choice([0.15, 0.3, 0.5])
            points = [[cx - size, cy - size / 2], [cx - neck, cy - size / 2], [cx - neck, cy - neck], [cx + neck, cy - neck], [cx + neck, cy - size / 2], [cx + size, cy - size / 2], [cx + size, cy + size / 2], [cx + neck, cy + size / 2], [cx + neck, cy + neck], [cx - neck, cy + neck], [cx - neck, cy + size / 2], [cx - size, cy + size / 2]]  # fmt: skip
        if rng.random() < 0.3:
            points.reverse()
        if rng.random() < 0.4:
            points = [[round(x, 2), round(y, 2)] for x, y in points]
        return points + [list(points[0])]

    for _ in range(count):
        size = rng.uniform(5, 12)
        cx, cy = rng.uniform(25, 65), rng.uniform(25, 65)
        paths = [polygon(cx, cy, size, rng.randrange(5))]
        islands = rng.choice([0, 0, 1, 1, 2])
        if islands == 1:  # an island in the middle
            paths.append(polygon(cx, cy, size * rng.uniform(0.1, 0.35), rng.randrange(3)))
        elif islands == 2:  # a second pocket beside the first, which may be too small for the helix
            paths.append(polygon(cx + size + 14, cy, rng.uniform(1.2, 4), rng.randrange(2)))
        yield paths


def cutter_points(job):
    """Every position of the cutter's centre in a job, as the geometry it sweeps."""
    for s in job['segments']:
        yield Point(s[:2]) if s[:2] == s[2:4] else LineString([s[:2], s[2:4]])


class HelixPlacementTests(unittest.TestCase):
    def generate(self, backend, **settings):
        with mock.patch.dict(os.environ, KERFKITE_POLYGON_BACKEND=backend):
            return generate(project(operation='pocket', router_entry='helix', **settings))

    def entries(self, backend, source, paths, bit, radius):
        with mock.patch.dict(os.environ, KERFKITE_POLYGON_BACKEND=backend):
            return router_entry._pocket_entries(source, paths, bit, radius)

    def test_the_cutter_is_never_nearer_a_wall_or_island_than_its_radius(self):
        for backend in BACKENDS:
            cut = helices = 0
            rng = random.Random(3)
            for paths in pocket_shapes(17, 140):
                diameter = rng.choice([1, 1.5, 2, 3, 3.175, 4])
                radius = rng.choice([0.05, 0.2, 0.5, 0.75, 1, 1.5, 2])
                settings = dict(
                    paths=paths, tool_diameter=diameter, stepover=diameter * 0.4, entry_radius=radius, depth=1, step=0.5
                )
                try:
                    job = self.generate(backend, **settings)
                except ValueError:
                    continue  # no room for this cutter or helix: the refusals have their own tests
                cut += 1
                bit = diameter / 2
                stock = region(paths)
                wall = stock.boundary
                slack = SLACK[backend](bit)
                # The whole job: the helices, the feeds across to each pass and the passes themselves.
                for swept in cutter_points(job):
                    self.assertTrue(stock.contains(swept), (backend, settings))
                    self.assertGreaterEqual(wall.distance(swept), bit - slack, (backend, settings))
                # Each helix on its own: its circle clears every wall by the cutter's radius with no slack at
                # all, and the feed from it to the pass stays where the cutter may be.
                passes = job['prepared_paths']
                placed = self.entries(backend, paths, passes, bit, radius)
                self.assertEqual(len(placed), len(passes))
                for (cx, cy, angle), contour in zip(placed, passes):
                    helices += 1
                    self.assertTrue(stock.contains(Point(cx, cy)))
                    self.assertGreaterEqual(wall.distance(Point(cx, cy)), bit + radius)
                    start = (cx + radius * math.cos(angle), cy + radius * math.sin(angle))
                    if math.dist(start, contour[0]) > 0:
                        self.assertGreaterEqual(wall.distance(LineString([start, contour[0]])), bit - slack)
                    # The helix ends where it started, and the program visits that point.
                    self.assertTrue(
                        any(math.dist(s[2:4], start) < 1e-9 and s[4] < 0 for s in job['segments']), (backend, settings)
                    )
            self.assertGreater(cut, 60, backend)
            self.assertGreater(helices, 300, backend)

    def test_a_helix_enters_a_pocket_whose_walls_are_not_on_the_kernel_grid(self):
        # The kernel's passes are on a grid of a millionth of a millimetre. Their seams used to be checked
        # against a Shapely offset to a hundredth of that, which refused most pockets at an angle.
        turn = 0.3
        square = [
            [45 + 14 * math.cos(turn + i * math.pi / 2), 45 + 14 * math.sin(turn + i * math.pi / 2)] for i in range(4)
        ]
        island = [[45 + 3 * math.cos(1 + i * math.pi / 3), 45 + 3 * math.sin(1 + i * math.pi / 3)] for i in range(6)]
        for backend in BACKENDS:
            job = self.generate(backend, paths=[square + [square[0]], island + [island[0]]], entry_radius=1.5)
            self.assertGreater(sum(1 for s in job['segments'] if s[4] < s[6]), 36, backend)

    def test_the_helix_descends_no_more_steeply_than_the_entry_angle(self):
        for backend in BACKENDS:
            job = self.generate(backend, entry_radius=1.5, entry_angle=5, paths=[ring(10, 10, 40), ring(24, 24, 12)])
            # From the stock's surface down, every fall is along the helix: nothing plunges straight into
            # material. (The move from clearance height down to the last level's depth is through air.)
            falling = [s for s in job['segments'] if s[4] < s[6] <= 0]
            self.assertGreater(len(falling), 36)
            for s in falling:
                run = math.dist(s[:2], s[2:4])
                self.assertGreater(run, 0)
                self.assertLessEqual((s[6] - s[4]) / run, math.tan(math.radians(5)) + 1e-7)

    def test_refusals_are_the_same_on_either_backend(self):
        room = 'The pocket has insufficient room for the cutter and helix radius'
        feed = (
            'No clear helix and connecting feed to this pocket contour; reduce the helix radius or use a contour ramp'
        )
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                # The 4 mm cutter fits a 7 mm pocket, a helix of radius 1.8 round it does not.
                with self.assertRaisesRegex(ValueError, '^' + room + '$'):
                    self.generate(backend, entry_radius=1.8, paths=[ring(10, 10, 7)])
                # The helix fits the large pocket only, and no straight feed leads from there into the small one.
                with self.assertRaisesRegex(ValueError, '^' + feed + '$'):
                    self.generate(backend, entry_radius=1.8, paths=[ring(10, 10, 30), ring(50, 10, 7)])
                with self.assertRaisesRegex(ValueError, '^Helix radius cannot exceed the cutter radius'):
                    self.generate(backend, entry_radius=2.5)

    @unittest.skipUnless('kernel' in BACKENDS, 'the compiled extension is not installed')
    def test_studio_asks_the_native_core_and_uses_its_answer_unchanged(self):
        source = [ring(10, 10, 40), ring(24, 24, 12)]
        job = self.generate('kernel', entry_radius=1.5, paths=source)
        passes = job['prepared_paths']
        placed = operations.kernel_pocket_entries(source, [contour[0] for contour in passes], 2, 1.5)
        self.assertEqual(placed, self.entries('kernel', source, passes, 2, 1.5))
        self.assertTrue(all(len(entry) == 3 and all(type(v) is float for v in entry) for entry in placed))
        # The first move of each helix starts on its circle.
        cx, cy, angle = placed[0]
        self.assertIn(f'G0 X{cx + 1.5 * math.cos(angle):.3f} Y{cy + 1.5 * math.sin(angle):.3f}', job['lines'])
        # A centre is a seam where the seam is itself clear, and otherwise a point of the region kept for
        # centres: either way a whole number of millionths of a millimetre or a double the kernel worked out
        # without the maths library, so it is the same text on every platform.
        self.assertEqual(placed[1][:2], (13.5024, 13.5024))
        with self.assertRaisesRegex(ValueError, 'Contours must be lists of finite XY points'):
            operations.kernel_pocket_entries(source, [[1, math.nan]], 2, 1.5)

    def test_the_shapely_fallback_is_used_without_the_extension(self):
        source = [ring(10, 10, 40), ring(24, 24, 12)]
        absent = [
            mock.patch.object(operations, name, None)
            for name in ('_core_call', '_kernel_installed', '_entries_installed')
        ]
        # The backend is named as well, which keeps this job out of the comparison with the native core.
        with absent[0], absent[1], absent[2], mock.patch.dict(os.environ, KERFKITE_POLYGON_BACKEND='shapely'):
            self.assertFalse(operations.kernel_entries_available())
            with mock.patch.object(router_entry, 'kernel_pocket_entries', side_effect=AssertionError):
                job = generate(project(operation='pocket', router_entry='helix', entry_radius=1.5, paths=source))
        self.assertGreater(sum(1 for s in job['segments'] if s[4] < s[6] <= 0), 36)
