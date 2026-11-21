import math
import random
import unittest
from shapely.geometry import LineString, Point
from studio.operations import KNIFE_REPEAT, knife, knife_points, prepare, region
from studio.cam import generate

OUT = [[10, 10], [30, 10], [30, 30], [10, 30], [10, 10]]
HOLE = [[18, 18], [22, 18], [22, 22], [18, 22], [18, 18]]


class OperationTests(unittest.TestCase):
    def test_outside_offset_hole_shrinks_and_cut_edge_matches(self):
        p, _, _ = prepare({'mode': 'router', 'operation': 'outside', 'tool_diameter': 2}, [OUT, HOLE])
        g = region(p)
        self.assertEqual(g.bounds, (9, 9, 31, 31))
        self.assertFalse(g.contains(Point(20, 20)))
        self.assertTrue(g.contains(Point(18.5, 20)))

    def test_pocket_keeps_tool_disk_in_region_and_island(self):
        p, _, _ = prepare({'mode': 'router', 'operation': 'pocket', 'tool_diameter': 2, 'stepover': 0.8}, [OUT, HOLE])
        g = region([OUT, HOLE])
        for path in p:
            self.assertLess(LineString(path).buffer(0.999).difference(g).area, 1e-8)

    def test_fill_excludes_holes(self):
        p, _, _ = prepare({'mode': 'laser', 'operation': 'fill', 'interval': 0.5}, [OUT, HOLE])
        g = region([OUT, HOLE])
        for path in p:
            self.assertTrue(g.covers(LineString(path)))

    def test_knife_corner_swivel_and_no_energy(self):
        job = generate(
            dict(
                mode='knife',
                width=100,
                height=100,
                feed=100,
                power=1000,
                paths=[OUT],
                knife_offset=0.5,
                knife_lead=1,
                depth=0.1,
            )
        )
        self.assertFalse(any(l.startswith(('M3', 'M4')) for l in job['lines']))
        self.assertGreater(len(job['prepared_paths'][0]), 10)

    def test_vcarve_depths_bounded(self):
        p, z, _ = prepare(
            {'mode': 'router', 'operation': 'vcarve', 'tool_diameter': 6, 'depth': 2, 'v_resolution': 1}, [OUT]
        )
        self.assertEqual(len(p), len(z))
        self.assertTrue(all(-2 <= v <= 0 for q in z for v in q))

    def test_compensated_bounds_rejected(self):
        with self.assertRaises(ValueError):
            generate(
                dict(
                    mode='router',
                    operation='outside',
                    tool_diameter=4,
                    width=30,
                    height=30,
                    feed=100,
                    power=1000,
                    paths=[OUT],
                )
            )


def knife_before(paths, offset, lead):
    """studio.operations.knife as it was before repeats were compared with the last point kept."""
    result = []
    for path in paths:
        p = [path[0]] + [b for a, b in zip(path, path[1:]) if math.dist(a, b) > 1e-7]
        if len(p) < 2:
            continue
        vectors = [((b[0] - a[0]) / math.dist(a, b), (b[1] - a[1]) / math.dist(a, b)) for a, b in zip(p, p[1:])]
        ux, uy = vectors[0]
        q = [
            [p[0][0] + (offset - lead) * ux, p[0][1] + (offset - lead) * uy],
            [p[0][0] + offset * ux, p[0][1] + offset * uy],
        ]
        for i, (vx, vy) in enumerate(vectors):
            x, y = p[i + 1]
            q.append([x + offset * vx, y + offset * vy])
            next_v = vectors[i + 1] if i + 1 < len(vectors) else vectors[0] if p[-1] == p[0] else None
            if next_v:
                a = math.atan2(vy, vx)
                b = math.atan2(next_v[1], next_v[0])
                delta = (b - a + math.pi) % (2 * math.pi) - math.pi
                n = max(1, math.ceil(abs(delta) / math.radians(5)))
                q.extend(
                    [
                        [x + offset * math.cos(a + delta * j / n), y + offset * math.sin(a + delta * j / n)]
                        for j in range(1, n + 1)
                    ]
                )
        result.append(q)
    return result


def knife_job(paths, **settings):
    return generate(
        dict(dict(mode='knife', width=100, height=100, feed=100, power=1000, depth=0.1), paths=paths, **settings)
    )


class KnifeRepeatTests(unittest.TestCase):
    """A drag-knife path with moves of no length, or that returns to a point through steps too short to keep."""

    def test_steps_too_short_to_keep_that_return_to_a_point_do_not_divide_by_zero(self):
        # Each step out is under 0.1 nm, so the old filter dropped both points and kept the return to the
        # start: a move of no length, and a ZeroDivisionError. Out and back is 0.18 nm each way, which is
        # further than the tolerance, so both moves are now kept and the knife turns round between them.
        back = [[20, 20], [20 + 9e-8, 20], [20 + 1.8e-7, 20], [20, 20]]
        self.assertEqual(knife_points(back), [[20, 20], [20 + 1.8e-7, 20], [20, 20]])
        job = knife_job([back + [[30, 25]]])
        for x, y in job['prepared_paths'][0]:
            self.assertTrue(math.isfinite(x) and math.isfinite(y))
        self.assertEqual(job['lines'], knife_job([[[20, 20], [20 + 1.8e-7, 20], [20, 20], [30, 25]]])['lines'])
        self.assertGreater(len(knife([back], 0.25, 1)[0]), 70)  # two half turns, 36 steps each
        # An excursion that stays within the tolerance of the point kept is not a move at all.
        near = [[20, 20], [20 + 6e-8, 20], [20 + 6e-8, 20 + 6e-8], [20, 20]]
        self.assertEqual(knife_job([near + [[30, 25]]])['lines'], knife_job([[[20, 20], [30, 25]]])['lines'])
        with self.assertRaisesRegex(ValueError, 'Knife paths must contain nonzero travel'):
            knife_job([near])

    def test_every_move_kept_is_longer_than_the_tolerance_and_the_path_stays_within_it(self):
        rng = random.Random(41)
        for _ in range(300):
            x, y = rng.uniform(20, 80), rng.uniform(20, 80)
            path = [[x, y]]
            for _ in range(rng.randrange(1, 40)):
                step = rng.choice([0, 3e-8, 6e-8, 9.9e-8, 1.01e-7, 2e-7, 0.5, 3])
                turn = rng.uniform(0, 2 * math.pi)
                x, y = x + step * math.cos(turn), y + step * math.sin(turn)
                path.append([x, y] if rng.random() < 0.9 else list(path[rng.randrange(len(path))]))
            kept = knife_points(path)
            self.assertEqual(kept[0], path[0])
            for a, b in zip(kept, kept[1:]):
                self.assertGreater(math.dist(a, b), KNIFE_REPEAT)
            # Every drawn point is within the tolerance of a kept point, in order along the path.
            at = 0
            for point in path:
                if at + 1 < len(kept) and point == kept[at + 1]:
                    at += 1
                self.assertLessEqual(math.dist(point, kept[at]), KNIFE_REPEAT)
            if len(kept) > 1:
                for q in knife([path], 0.3, 1.5):
                    self.assertTrue(all(math.isfinite(v) for point in q for v in point))

    def test_paths_without_steps_shorter_than_the_tolerance_are_cut_as_before(self):
        # Exact repeats and ordinary moves only: the pivot path is the earlier one to the last bit.
        rng = random.Random(7)
        compared = 0
        for _ in range(400):
            path = [[round(rng.uniform(10, 90), rng.randrange(0, 5)), round(rng.uniform(10, 90), rng.randrange(0, 5))]]
            for _ in range(rng.randrange(1, 12)):
                kind = rng.random()
                if kind < 0.25:
                    path.append(list(path[-1]))  # a move of no length
                elif kind < 0.35:
                    path.append(list(path[0]))  # back to the start, closing the path
                else:
                    path.append([round(rng.uniform(10, 90), rng.randrange(0, 5)), round(rng.uniform(10, 90), 3)])
            if any(0 < math.dist(a, b) <= 1e-7 for a, b in zip(path, path[1:])):
                continue
            offset = rng.choice([0.25, 0.5, 2])
            before = knife_before([path], offset, 2)
            if not before:
                with self.assertRaises(ValueError):
                    knife([path], offset, 2)
                continue
            now = knife([path], offset, 2)
            self.assertEqual(
                [[v.hex() for point in q for v in point] for q in now],
                [[float(v).hex() for point in q for v in point] for q in before],
            )
            compared += 1
        self.assertGreater(compared, 300)

    def test_a_path_that_is_a_single_point_is_refused_with_a_clear_message(self):
        for paths in ([[[5, 5], [5, 5]]], [[[10, 10], [20, 10]], [[5, 5], [5, 5], [5 + 5e-8, 5]]]):
            with self.assertRaisesRegex(ValueError, '^Knife paths must contain nonzero travel$'):
                knife_job(paths)

    def test_many_short_steps_are_followed_and_not_dropped(self):
        # 40 steps of 0.05 nm: each is too short to keep, together they are 2 nm of travel.
        path = [[30 + 5e-8 * i, 30] for i in range(41)]
        kept = knife_points(path)
        self.assertGreater(len(kept), 10)
        self.assertLessEqual(math.dist(kept[-1], path[-1]), KNIFE_REPEAT)
        self.assertEqual(len(knife([path], 0.25, 1)), 1)


class OperationRegressionTests(unittest.TestCase):
    def test_pocket_clears_accessible_centre(self):
        from shapely.ops import unary_union

        square = [[10, 10], [31, 10], [31, 31], [10, 31], [10, 10]]
        p, _, _ = prepare({'mode': 'router', 'operation': 'pocket', 'tool_diameter': 2, 'stepover': 1}, [square])
        swept = unary_union([LineString(path).buffer(1, quad_segs=32) for path in p])
        accessible = region([square]).buffer(-1).buffer(1, quad_segs=32)
        self.assertLess(accessible.difference(swept).area, 0.001)
        with self.assertRaises(ValueError):
            prepare({'mode': 'router', 'operation': 'pocket', 'tool_diameter': 2, 'stepover': 2}, [square])

    def test_ordered_job_cannot_silently_change_cutters(self):
        base = dict(mode='router', protocol='grbl', width=100, height=100, feed=100, power=1000)
        ops = [
            dict(paths=[OUT], operation='pocket', tool_diameter=2, stepover=0.5),
            dict(paths=[OUT], operation='outside', tool_diameter=4),
        ]
        with self.assertRaises(ValueError):
            generate({**base, 'operations': ops})

    def test_raster_powers_keep_overscan_off(self):
        job = generate(
            dict(
                mode='laser',
                width=100,
                height=100,
                feed=1000,
                power=1000,
                paths=[[[1, 1], [2, 1], [3, 1], [4, 1]]],
                path_powers=[[0, 0, 750, 0]],
            )
        )
        self.assertIn('M4 S0', job['lines'])
        self.assertIn('G1 X3.000 Y1.000 F1000.0 S750', job['lines'])
        self.assertIn('G1 X4.000 Y1.000 F1000.0 S0', job['lines'])
