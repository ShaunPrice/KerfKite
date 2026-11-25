"""Relief roughing through `kerfkite-cam relief-roughing`, checked against the height field itself.

The passes are level by level from the stock top. The test checks them against the cutter's own contact (the
finishing calculation with the same cutter and allowance) and, separately, against the height field: a flat cutter
at any pass point must stay the allowance above every sample of the field under it.
"""

import math
import unittest

from kerfkite_core import CLI, call, generate

NOT_BUILT = 'kerfkite-cam is not built: set KERFKITE_CAM or build native/ into build/native'


def field(n=40):
    """A dome with a ridge and a pit, heights 0..1, row 0 at the front."""
    rows = []
    for j in range(n):
        row = []
        for i in range(n):
            x, y = (i / (n - 1) - 0.5) * 2, (j / (n - 1) - 0.5) * 2
            h = 0.55 + 0.3 * math.cos(3 * math.hypot(x, y)) + 0.12 * math.sin(7 * x) * math.cos(5 * y)
            h -= 0.35 * math.exp(-40 * ((x - 0.35) ** 2 + (y + 0.3) ** 2))
            row.append(max(0.0, min(1.0, h)))
        rows.append(row)
    return rows


class Base(unittest.TestCase):
    N = 40
    SETTINGS = dict(
        width=120, height=90, x=10, y=10, depth=8, tool_diameter=6, tool_shape='flat', stepover=2.5, allowance=0.4
    )

    def request(self, **changes):
        return {**self.SETTINGS, 'heightmap': field(self.N), **changes}


@unittest.skipUnless(CLI, NOT_BUILT)
class RoughingTests(Base):
    def check(self, step, **changes):
        request = self.request(**changes)
        contact = call('relief', request)
        rough = call('relief-roughing', {**request, 'rough_step': step})
        # The contact at every raster vertex, by its place.
        allowed = {}
        for path, depths in zip(contact['paths'], contact['path_depths']):
            for (x, y), z in zip(path, depths):
                allowed[(round(x, 6), round(y, 6))] = z
        lowest = min(allowed.values())
        self.assertEqual(rough['lowest'], lowest)
        self.assertEqual(rough['levels'], math.ceil(-lowest / step - 1e-9))
        self.assertEqual(rough['surface'], contact['surface'])
        reached = {}
        level, floor_seen = 1, 0.0
        for path, depths in zip(rough['paths'], rough['path_depths']):
            self.assertEqual(len(path), len(depths))
            self.assertGreaterEqual(len(path), 2)
            self.assertEqual(len({round(y, 6) for _, y in path}), 1)  # along one raster
            low = min(depths)
            # Passes come level by level from the top, and none is deeper than its level.
            while low < -step * level - 1e-9:
                level += 1
            self.assertGreaterEqual(low, max(-step * level, lowest) - 1e-9)
            self.assertGreaterEqual(low, floor_seen - step - 1e-9)
            floor_seen = min(floor_seen, low)
            for (x, y), z in zip(path, depths):
                key = (round(x, 6), round(y, 6))
                self.assertIn(key, allowed)
                self.assertGreaterEqual(z, allowed[key] - 1e-9, 'a pass goes below the cutter contact')
                self.assertLessEqual(z, 1e-9)
                reached[key] = min(reached.get(key, 0.0), z)
            # Between the points kept, a pass is straight: it must clear the contact at the vertices left out.
            xs = sorted(k[0] for k in allowed if k[1] == round(path[0][1], 6))
            for (ax, _), az, (bx, _), bz in zip(path, depths, path[1:], depths[1:]):
                for x in xs:
                    if min(ax, bx) + 1e-5 < x < max(ax, bx) - 1e-5:
                        t = (x - ax) / (bx - ax)
                        self.assertGreaterEqual(az + t * (bz - az), allowed[(x, round(path[0][1], 6))] - 1e-9)
                        reached[(x, round(path[0][1], 6))] = min(
                            reached.get((x, round(path[0][1], 6)), 0.0), az + t * (bz - az)
                        )
        self.assertEqual(level, rough['levels'])
        # Roughing is complete: wherever the cutter may go below the top, some pass took it to its contact.
        for key, z in allowed.items():
            if z < -1e-9:
                self.assertAlmostEqual(reached.get(key, 0.0), z, 9, f'stock left at {key}')
        return request, rough

    def test_passes_reach_the_contact_level_by_level_and_never_below_it(self):
        for step in (2, 3.5, 0.75, 8, 20):
            with self.subTest(step=step):
                self.check(step)
        self.check(2, tool_shape='ball', allowance=0)
        self.check(1.5, stepover=6, tool_diameter=6)

    def test_a_flat_cutter_stays_the_allowance_above_the_field(self):
        # Independently of the core's contact: every sample of the field under the cutter's footprint.
        request, rough = self.check(2)
        heights, n = request['heightmap'], self.N
        dx, dy = request['width'] / (n - 1), request['height'] / (n - 1)
        radius, allowance, depth = request['tool_diameter'] / 2, request['allowance'], request['depth']
        checked = 0
        for path, depths in zip(rough['paths'], rough['path_depths']):
            for (x, y), z in zip(path, depths):
                cx, cy = x - request['x'], y - request['y']
                for j in range(max(0, int((cy - radius) / dy)), min(n - 1, int((cy + radius) / dy) + 1) + 1):
                    for i in range(max(0, int((cx - radius) / dx)), min(n - 1, int((cx + radius) / dx) + 1) + 1):
                        if math.hypot(i * dx - cx, j * dy - cy) <= radius:
                            surface = (heights[j][i] - 1) * depth
                            self.assertGreaterEqual(z, min(0.0, surface + allowance) - 1e-9)
                            checked += 1
        self.assertGreater(checked, 5000)

    def test_the_passes_generate_as_a_router_job(self):
        request, rough = self.check(2)
        project = dict(
            mode='router', protocol='grbl', width=200, height=150, feed=900, plunge=200, power=12000, safe=5, depth=8, step=8,
            tool_diameter=6, paths=rough['paths'], path_depths=rough['path_depths'],
        )  # fmt: skip
        job = generate(project)
        deepest = min(float(word[1:]) for line in job['lines'] for word in line.split() if word.startswith('Z'))
        self.assertAlmostEqual(deepest, rough['lowest'], 3)
        self.assertEqual(job['bounds'], [10, 10, 130, 100])

    def test_requests_are_checked(self):
        request = self.request()
        for step in (None, 0, 0.01, 101, '2', True):
            with self.assertRaisesRegex(ValueError, 'Roughing step down must be between 0.05 and 100'):
                call('relief-roughing', {**request, 'rough_step': step})
        with self.assertRaisesRegex(ValueError, 'more than 400 roughing levels'):
            call('relief-roughing', {**request, 'depth': 50, 'rough_step': 0.05})
        with self.assertRaisesRegex(ValueError, 'nothing to rough'):
            call('relief-roughing', {**request, 'heightmap': [[1.0] * 8 for _ in range(8)], 'rough_step': 2})
        # The relief's own refusals pass through.
        with self.assertRaisesRegex(ValueError, 'stepover must not exceed tool_diameter'):
            call('relief-roughing', {**request, 'stepover': 9, 'rough_step': 2})
        with self.assertRaisesRegex(ValueError, 'Relief input must be an object'):
            call('relief-roughing', [])

    def test_the_answer_is_the_same_every_time(self):
        request = {**self.request(), 'rough_step': 2}
        self.assertEqual(call('relief-roughing', request), call('relief-roughing', request))


if __name__ == '__main__':
    unittest.main()


def under(request, n, cx, cy, radius):
    """The highest the field needs the cutter's flat end to be at a centre, from the samples under it."""
    heights = request['heightmap']
    dx, dy = request['width'] / (n - 1), request['height'] / (n - 1)
    need = -request['depth'] - 1
    for j in range(max(0, int((cy - radius) / dy)), min(n - 1, int((cy + radius) / dy) + 1) + 1):
        for i in range(max(0, int((cx - radius) / dx)), min(n - 1, int((cx + radius) / dx) + 1) + 1):
            if math.hypot(i * dx - cx, j * dy - cy) <= radius:
                need = max(need, min(0.0, (heights[j][i] - 1) * request['depth'] + request['allowance']))
    return need


@unittest.skipUnless(CLI, NOT_BUILT)
class AdaptiveTests(Base):
    """Adaptive clearing, followed here on a grid of the test's own: where the cutter goes and what it takes."""

    def rough(self, step=2, **changes):
        request = self.request(**changes)
        return request, call('relief-roughing', {**request, 'rough_step': step, 'rough_strategy': 'adaptive'})

    def walk(self, request, rough, spacing):
        """Every path as points no more than `spacing` apart: (x, y, z, level or None on the way down, length, first, the path's level)."""
        for path, depths in zip(rough['paths'], rough['path_depths']):
            floor = min(depths)
            previous = None
            for (x, y), z in zip(path, depths):
                x, y = x - request['x'], y - request['y']
                if previous is None:
                    yield x, y, z, floor if z == floor else None, 0.0, True, floor
                else:
                    px, py, pz = previous
                    span = math.hypot(x - px, y - py)
                    pieces = max(1, math.ceil(span / spacing))
                    for k in range(1, pieces + 1):
                        t = k / pieces
                        level = floor if z == floor and pz == floor else None
                        yield (
                            px + (x - px) * t,
                            py + (y - py) * t,
                            pz + (z - pz) * t,
                            level,
                            span / pieces,
                            False,
                            floor,
                        )
                previous = (x, y, z)

    def test_the_cutter_stays_the_allowance_above_the_field_all_along_every_move(self):
        request, rough = self.rough()
        radius = request['tool_diameter'] / 2
        self.assertEqual(rough['strategy'], 'adaptive')
        self.assertEqual(rough['levels'], math.ceil(-rough['lowest'] / 2 - 1e-9))
        checked = 0
        for x, y, z, _, _, _, _ in self.walk(request, rough, 0.4):
            self.assertTrue(-1e-9 <= x <= request['width'] + 1e-9 and -1e-9 <= y <= request['height'] + 1e-9)
            self.assertLessEqual(z, 1e-9)
            self.assertGreaterEqual(z, rough['lowest'] - 1e-9)
            self.assertGreaterEqual(
                z, under(request, self.N, x, y, radius) - 1e-9, f'the cutter gouges at {x:.2f}, {y:.2f}'
            )
            checked += 1
        self.assertGreater(checked, 10000)

    def test_passes_are_level_by_level_and_enter_down_a_helix_or_straight(self):
        request, rough = self.rough()
        floors = [min(depths) for depths in rough['path_depths']]
        self.assertEqual(floors, sorted(floors, reverse=True))  # from the top down
        levels = sorted(set(floors), reverse=True)
        self.assertEqual(len(levels), rough['levels'])
        for k, level in enumerate(levels):
            self.assertAlmostEqual(level, max(-2.0 * (k + 1), rough['lowest']), 9)
        helixes = 0
        for depths in rough['path_depths']:
            floor = min(depths)
            down = [z for z in depths if z != floor]
            if down:
                helixes += 1
                # Down from the level above, never rising, and nothing but the floor afterwards.
                self.assertAlmostEqual(depths[0], min(0.0, floor + 2), 9)
                self.assertEqual(down, sorted(down, reverse=True))
                self.assertEqual(depths[: len(down)], down)
        self.assertEqual(helixes, rough['helix_entries'])
        self.assertGreaterEqual(rough['helix_entries'], 1)

    def measure(self, request, rough, cell=0.5):
        """Cutting on each level, on the test's own cells: the width of cut along the way and what is cleared."""
        radius = request['tool_diameter'] / 2
        reach = math.ceil(radius / cell) + 1
        disc = [
            (i, j)
            for j in range(-reach, reach)
            for i in range(-reach, reach)
            if math.hypot((i + 0.5) * cell, (j + 0.5) * cell) <= radius
        ]
        columns, rows = round(request['width'] / cell), round(request['height'] / cell)
        gone = {}
        widths = []  # (width of cut, length) for each stretch of level cutting
        run = [0.0, 0.0]
        for x, y, _, level, length, first, floor in self.walk(request, rough, cell):
            # What a helix takes on its way down belongs to the level it ends on.
            cleared = gone.setdefault(floor, set())
            ci, cj = round(x / cell), round(y / cell)
            new = 0
            for i, j in disc:
                key = (ci + i, cj + j)
                if 0 <= key[0] < columns and 0 <= key[1] < rows and key not in cleared:
                    cleared.add(key)
                    new += 1
            if first:
                run = [0.0, 0.0]
            elif level is not None and length > 0:
                # The width of cut over each stretch of about a cutter radius: single cells are too coarse a measure.
                run[0] += new * cell * cell
                run[1] += length
                if run[1] >= radius:
                    widths.append((run[0] / run[1], run[1]))
                    run = [0.0, 0.0]
        return widths, gone

    def test_the_width_of_cut_keeps_near_the_engagement(self):
        for engagement in (2.4, 1.2):
            with self.subTest(engagement=engagement):
                request, rough = self.rough(stepover=engagement)
                self.assertEqual(rough['engagement'], engagement)
                widths, _ = self.measure(request, rough)
                total = sum(length for _, length in widths)
                mean = sum(width * length for width, length in widths) / total
                self.assertLess(mean, engagement)
                self.assertGreater(mean, 0.5 * engagement)  # and it is not idling either
                over = sum(length for width, length in widths if width > 1.5 * engagement)
                self.assertLess(over / total, 0.02, 'too much of the cutting is well over the engagement')
                # Nowhere is the cutter buried: the widest stretch is well short of its diameter.
                self.assertLess(max(width for width, _ in widths), 0.75 * request['tool_diameter'])
                self.assertLess(rough['forced_length'], 0.01 * rough['cut_length'])

    def test_what_the_cutter_can_reach_on_a_level_is_cleared(self):
        request, rough = self.rough()
        radius, cell = request['tool_diameter'] / 2, 1.0
        _, gone = self.measure(request, rough, cell)
        reach = math.ceil(radius / cell) + 1
        disc = [
            (i, j)
            for j in range(-reach, reach)
            for i in range(-reach, reach)
            if math.hypot((i + 0.5) * cell, (j + 0.5) * cell) <= radius
        ]
        columns, rows = round(request['width'] / cell), round(request['height'] / cell)
        # The core bounds the field by the highest corner of each of the map's cells, so it keeps the cutter a map
        # cell further from a wall than the samples alone would: the test allows that, the wall's extra and its own cell.
        n = self.N
        margin = radius + rough['wall_extra'] + math.hypot(request['width'], request['height']) / (n - 1) + cell
        checked = 0
        for level in sorted(gone):
            # Where a cutter that much larger than the real one may stand at this level, by the field itself.
            reachable = set()
            for j in range(rows + 1):
                for i in range(columns + 1):
                    if under(request, self.N, i * cell, j * cell, margin) <= level + 1e-9:
                        for a, b in disc:
                            if 0 <= i + a < columns and 0 <= j + b < rows:
                                reachable.add((i + a, j + b))
            if not reachable:
                continue  # the lowest level may be reachable only nearer the walls than the test allows
            checked += 1
            missed = reachable - gone[level]
            self.assertLess(len(missed) / len(reachable), 0.03, f'stock left on the level at {level}')
        self.assertGreaterEqual(checked, 2)

    def test_the_passes_generate_as_a_router_job(self):
        request, rough = self.rough()
        project = dict(
            mode='router', protocol='grbl', width=200, height=150, feed=900, plunge=200, power=12000, safe=5, depth=8, step=8,
            tool_diameter=6, paths=rough['paths'], path_depths=rough['path_depths'],
        )  # fmt: skip
        job = generate(project)
        deepest = min(float(word[1:]) for line in job['lines'] for word in line.split() if word.startswith('Z'))
        self.assertAlmostEqual(deepest, rough['lowest'], 3)

    def test_requests_are_checked(self):
        request = self.request()
        with self.assertRaisesRegex(ValueError, 'rough_strategy must be raster or adaptive'):
            call('relief-roughing', {**request, 'rough_step': 2, 'rough_strategy': 'spiral'})
        with self.assertRaisesRegex(ValueError, 'needs a flat cutter'):
            call('relief-roughing', {**request, 'rough_step': 2, 'rough_strategy': 'adaptive', 'tool_shape': 'ball'})
        with self.assertRaisesRegex(ValueError, 'between 8% and 100% of the cutter diameter'):
            call('relief-roughing', {**request, 'rough_step': 2, 'rough_strategy': 'adaptive', 'stepover': 0.3})
        with self.assertRaisesRegex(ValueError, 'nothing to rough'):
            call(
                'relief-roughing',
                {**request, 'heightmap': [[1.0] * 8 for _ in range(8)], 'rough_step': 2, 'rough_strategy': 'adaptive'},
            )
        # A strategy named raster is the level passes, as before.
        self.assertEqual(
            call('relief-roughing', {**request, 'rough_step': 2, 'rough_strategy': 'raster'})['paths'],
            call('relief-roughing', {**request, 'rough_step': 2})['paths'],
        )
