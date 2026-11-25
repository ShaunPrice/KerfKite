"""A V-carved inlay, checked by the shapes the two carves leave: the plug must fit the pocket wall to wall.

The pocket is an ordinary V-carve of a shape. The plug is the V-carve of everything round the mirrored shape
inside a border, with `v_start_depth`: the outline is cut that far below the top of the plug's stock. The test
follows the V-bit's point through both programs and works out, at places across the walls, how deep each carve
leaves the stock. Turned over and pushed in, the plug's wall must lie on the pocket's wall: nowhere inside it
(the plug would not go in) and nowhere far from it (a gap would show).
"""

import math
import unittest

from kerfkite_core import CLI, generate

NOT_BUILT = 'kerfkite-cam is not built: set KERFKITE_CAM or build native/ into build/native'

ANGLE, RESOLUTION = 60, 0.2
SLOPE = math.tan(math.radians(ANGLE / 2))
# An arrow: sharp points, and corners that turn into the shape.
SHAPE = [[30, 30], [50, 30], [50, 45], [62, 45], [40, 68], [18, 45], [30, 45], [30, 30]]


def tips(lines):
    """The V-bit's point along every cutting move, a sample every 0.1 mm: (x, y, depth below the stock top)."""
    out = []
    x = y = z = None
    for line in lines:
        words = line.split()
        if words[0] not in ('G0', 'G1'):
            continue
        at = {w[0]: float(w[1:]) for w in words[1:] if w[0] in 'XYZ'}
        nx, ny, nz = at.get('X', x), at.get('Y', y), at.get('Z', z)
        if words[0] == 'G1' and ('X' in at or 'Y' in at) and x is not None and z is not None:
            pieces = max(1, math.ceil(math.dist((x, y), (nx, ny)) / 0.1))
            for k in range(pieces + 1):
                t = k / pieces
                out.append((x + (nx - x) * t, y + (ny - y) * t, -(z + (nz - z) * t)))
        x, y, z = nx, ny, nz
    return out


def carved(points, x, y):
    """How deep the cone of the bit leaves the stock at a place: the deepest any point of the bit reaches there."""
    return max(0.0, max(depth - math.hypot(px - x, py - y) / SLOPE for px, py, depth in points))


def project(**changes):
    base = dict(
        mode='router', protocol='grbl', width=200, height=120, feed=900, plunge=200, power=12000, safe=5,
        operation='vcarve', v_angle=ANGLE, v_resolution=RESOLUTION, tool_diameter=12.7,
    )  # fmt: skip
    base.update(changes)
    return base


@unittest.skipUnless(CLI, NOT_BUILT)
class VInlayTests(unittest.TestCase):
    def test_a_start_depth_lowers_the_whole_carve_and_nothing_else(self):
        plain = generate(project(paths=[SHAPE], depth=2, step=2))
        lowered = generate(project(paths=[SHAPE], depth=5, step=5, v_start_depth=3))
        a, b = tips(plain['lines']), tips(lowered['lines'])
        self.assertEqual(len(a), len(b))
        for (ax, ay, az), (bx, by, bz) in zip(a, b):
            self.assertEqual((ax, ay), (bx, by))
            self.assertAlmostEqual(bz, az + 3, 3)
        self.assertEqual(generate(project(paths=[SHAPE], depth=2, step=2, v_start_depth=0))['lines'], plain['lines'])

    def test_the_plug_fits_the_pocket_wall_to_wall(self):
        depth, start, above = 4.0, 3.0, 1.5
        # What the joinery panel works out: the outline of the plug is carved sampling ÷ slope deeper than the plug
        # goes in, because the pocket's carve stops one sampling step short of its wall. (The plug's own wall is
        # cut exactly: the bit's point runs along the outline at the start depth.)
        plug_start = start + RESOLUTION / SLOPE
        pocket = tips(generate(project(paths=[SHAPE], depth=depth, step=depth))['lines'])
        low, high = min(p[0] for p in SHAPE), max(p[0] for p in SHAPE)
        mirror = lambda x: low + high - x
        mirrored = [[mirror(x), y] for x, y in reversed(SHAPE)]
        border = [[low - 8, 22], [high + 8, 22], [high + 8, 76], [low - 8, 76], [low - 8, 22]]
        plug_job = generate(
            project(
                paths=[border, mirrored], depth=plug_start + above, step=plug_start + above, v_start_depth=plug_start
            )
        )
        plug = tips(plug_job['lines'])
        self.assertAlmostEqual(min(d for _, _, d in plug), plug_start, 2)  # the outline itself, at the walls
        self.assertAlmostEqual(max(d for _, _, d in plug), plug_start + above, 3)
        # Places across the wall at the middle of each edge and near each corner, from 0.3 to 1.8 mm inside.
        checked, worst_gap, worst_clash = 0, 0.0, 0.0
        for k in range(len(SHAPE) - 1):
            (ax, ay), (bx, by) = SHAPE[k], SHAPE[k + 1]
            length = math.dist((ax, ay), (bx, by))
            nx, ny = -(by - ay) / length, (bx - ax) / length  # to the left of a counter-clockwise edge: inside
            for along in (0.2, 0.5, 0.8):
                for inside in (0.3, 0.8, 1.3, 1.8):
                    x, y = ax + (bx - ax) * along + nx * inside, ay + (by - ay) * along + ny * inside
                    p = carved(pocket, x, y)
                    q = carved(plug, mirror(x), y)
                    if not 0.2 < p < depth - 0.2:
                        continue  # not on the pocket's sloping wall here (near a corner that turns into the shape)
                    # The plug is turned over and seats where its walls meet the pocket's. Where the pocket is p
                    # deep and the plug's stock is carved q deep, the plug's wall is start - q below the surface
                    # when the plug is in by `start`: the walls coincide when p + q equals `start`.
                    gap = p + q - start  # more than 0: a gap between the walls; less: the plug is too fat here
                    worst_gap, worst_clash = max(worst_gap, gap), max(worst_clash, -gap)
                    checked += 1
        self.assertGreater(checked, 40)
        # In depth, 0.1 mm is 0.06 mm across a 60 degree wall.
        self.assertLess(worst_clash, 0.12, 'the plug is fatter than the pocket somewhere: it would not go in')
        self.assertLess(worst_gap, 0.12, 'the plug is thinner than the pocket somewhere: a gap would show')

    def test_requests_are_checked(self):
        with self.assertRaisesRegex(ValueError, 'must be more than the V start depth'):
            generate(project(paths=[SHAPE], depth=3, step=3, v_start_depth=3))
        with self.assertRaisesRegex(ValueError, 'too narrow to carve from that start depth'):
            generate(project(paths=[SHAPE], depth=5, step=5, v_start_depth=4, tool_diameter=4))
        with self.assertRaisesRegex(ValueError, 'v_start_depth must be between 0 and 50'):
            generate(project(paths=[SHAPE], depth=5, step=5, v_start_depth=-1))


if __name__ == '__main__':
    unittest.main()
