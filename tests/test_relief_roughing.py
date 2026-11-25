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
