"""Check the native core's V-carving against Shapely, which shares no code with it.

    python setup.py build_ext --inplace
    python validation/vcarve_check.py --cases 4000

The shapes are those of validation/native_core_parity.py (vcarve_cases): words of block letters, shapes with
holes, thin strokes, flat areas, round shapes and stars. Each one that can be carved is carved through
studio.operations.prepare, as Studio does it, and three things are measured.

1. Safety, with Shapely as the oracle. At every point of every path the circle the V-bit cuts at that depth,
   of radius |z| tan(angle / 2), must reach no wall: it may be no wider than the distance from the point to
   the region's boundary less the sampling interval (the kernel's allowance), with two steps of the kernel's
   grid allowed for rounding. The point must lie in the region, |z| must be within the depth of cut and the
   cut radius within the tool's. The middle of every move is measured as well, since the cutter moves in a
   straight line between points. The least room left between a cut and a wall is printed.

2. The same medial axis. The Shapely V-carve (studio.operations.sampled_medial_axis, the fallback for an
   installation without the extension) is run on the samples the kernel took along the outlines. The
   Hausdorff distance between the two sets of paths and the difference of their total
   lengths are then only the rounding of the kernel's grid and of the two libraries, except where samples
   lie almost on one circle (the middle of a round shape), where a Voronoi vertex moves far for a small
   change in the samples. The worst of each is printed.

3. The fallback as shipped. Left to itself the Shapely code starts sampling each outline where GEOS starts
   the ring, so its spokes stand elsewhere along the walls, and a piece that barely cuts in one carve may
   not cut at all in the other. Compared are the places where either carve cuts a circle at least one
   sampling interval in radius: how far each is from the nearest path of the other carve. For a shape with
   no corner turning inwards that is under a sampling interval and a half. Round a corner that turns
   inwards the method itself leaves gaps: the two samples beside such a corner own wedges that widen with
   the distance from it, so the paths there stand further apart than the interval in either carve, and
   the two carves put them in different places. The distances are printed, with the differences in length,
   the numbers of paths and the travel between them.

A failure of 1 stops the run with the case. 2 fails beyond 2% of the sampling interval or a ten-thousandth
of the length, and 3 beyond the bounds given at the end of main().
"""

import argparse
import math
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import shapely  # noqa: E402
from shapely.geometry import Point  # noqa: E402

GRID = 1e-6
# The Shapely carve measures the distance to the wall one point at a time, so the two comparisons with it
# are made for carves of up to this many points; safety is measured for every carve.
COMPARED = 12000


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


def hausdorff(a, b):
    """The Hausdorff distance between two carves, measured at their points, which stand at most one
    sampling interval apart along every path."""
    return max(b.furthest(a.points), a.furthest(b.points))


def carvable(count, seed):
    """The generated projects that are plain V-carves with settings the reference accepts."""
    from native_core_parity import vcarve_cases
    from studio import operations

    for project in vcarve_cases(count, seed):
        if project.get('operation') != 'vcarve' or project.get('mode') != 'router' or 'operations' in project:
            continue
        try:
            settings = operations._vcarve_settings(project, 'router')
            g = operations.region(project['paths'])
        except (ValueError, TypeError, AttributeError):
            continue
        yield project, settings, g


def safety(paths, depths, g, settings, slack):
    """The least room between a cut and a wall, at the points and in the middle of the moves, each as a
    multiple of the sampling interval; or the first point that breaks the rule."""
    resolution, radius, depth = settings['resolution'], settings['radius'], settings['max_depth']
    slope = math.tan(math.radians(settings['angle'] / 2))
    xy = [point for path in paths for point in path]
    z = [value for row in depths for value in row]
    assert len(paths) == len(depths) and len(xy) == len(z) and all(len(path) >= 2 for path in paths)
    points = shapely.points(xy)
    wall = shapely.distance(points, g.boundary)
    inside = shapely.contains(g, points)
    room = between = math.inf
    for (x, y), value, away, within in zip(xy, z, wall, inside):
        cut = -value * slope
        if not -depth <= value <= 0 or cut > radius * (1 + 1e-12):
            return None, None, f'depth {value} at {(x, y)} is beyond the depth of cut or the tool'
        if value < 0 and (not within or cut > away - resolution + slack):
            return None, None, f'the bit cuts {cut} wide at {(x, y)}, {away} from the wall'
        if value == 0 and not within and away > slack:
            return None, None, f'the point {(x, y)} is outside the region'
        if value < 0:
            room = min(room, (away - cut) / resolution)
    middles, cuts = [], []
    for path, row in zip(paths, depths):
        for (ax, ay), (bx, by), za, zb in zip(path, path[1:], row, row[1:]):
            if math.dist((ax, ay), (bx, by)) > resolution + slack:
                return None, None, f'a move from {(ax, ay)} is longer than the sampling interval'
            middles.append(((ax + bx) / 2, (ay + by) / 2))
            cuts.append(-(za + zb) / 2 * slope)
    for (x, y), away, cut in zip(middles, shapely.distance(shapely.points(middles), g.boundary), cuts):
        if cut > 0 and cut > away:
            return None, None, f'the bit cuts {cut} wide in the middle of a move at {(x, y)}, {away} from the wall'
        if cut > 0:
            between = min(between, (away - cut) / resolution)
    return room, between, None


def deep_points(paths, depths, settings):
    """The points where a carve cuts a circle at least one sampling interval in radius."""
    least = settings['resolution'] / math.tan(math.radians(settings['angle'] / 2))
    return [point for path, row in zip(paths, depths) for point, value in zip(path, row) if -value >= least]


def travel(paths):
    return sum(math.dist(a[-1], b[0]) for a, b in zip(paths, paths[1:]))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cases', type=int, default=4000, help='generated projects to draw from')
    parser.add_argument('--seed', type=int, default=93)
    args = parser.parse_args()

    from studio import operations

    os.environ.pop('KERFKITE_POLYGON_BACKEND', None)
    if not (operations.kernel_available() and operations.kernel_vcarve_available()):
        raise SystemExit('The compiled extension with V-carving is not installed: python setup.py build_ext --inplace')

    carved = empty = 0
    room = between = math.inf
    same = dict(hausdorff=0.0, relative=0.0, length=0.0, depth=0.0, compared=0)
    apart, convex_apart, lengths = [], [], []
    shipped = dict(kernel_paths=0, fallback_paths=0, kernel_travel=0.0, fallback_travel=0.0, cut=0.0, fallback_cut=0.0)
    failures = []
    for project, settings, g in carvable(args.cases, args.seed):
        paths = project['paths']
        try:
            ours, depths, _ = operations.prepare(project, paths)
        except ValueError as error:
            if str(error) not in ('Operation produced no toolpaths', 'Job exceeds 200,000 cutting segments', 'V-carve outline too complex; increase sampling interval', 'Per-vertex depth contours require the line operation'):  # fmt: skip
                failures.append((project, str(error)))
            empty += 1
            continue
        carved += 1
        resolution = settings['resolution']
        at_points, in_moves, broken = safety(ours, depths, g, settings, 2 * GRID)
        if broken:
            raise SystemExit(f'UNSAFE: {broken}\n{project}')
        room, between = min(room, at_points), min(between, in_moves)

        if sum(map(len, ours)) > COMPARED:
            continue
        # 2. The Shapely carve of the same samples.
        samples = operations.kernel_vcarve(paths, samples=True, **settings)[2]
        theirs, heights = operations.sampled_medial_axis(g, [Point(sample) for sample in samples], **settings)
        if not theirs:
            failures.append((project, 'the Shapely carve of the same samples is empty'))
            continue
        a, b = Carve(ours), Carve(theirs)
        distance = hausdorff(a, b)
        same['hausdorff'] = max(same['hausdorff'], distance)
        same['relative'] = max(same['relative'], distance / resolution)
        same['length'] = max(same['length'], abs(a.length - b.length) / b.length)
        same['depth'] = max(same['depth'], abs(min(map(min, depths)) - min(map(min, heights))))
        same['compared'] += 1
        if distance > 0.02 * resolution or abs(a.length - b.length) > 1e-4 * b.length + 0.01:
            failures.append((project, f'differs from the Shapely carve of the same samples: {distance} mm apart, lengths {a.length} and {b.length}'))  # fmt: skip

        # 3. The fallback as it ships.
        os.environ['KERFKITE_POLYGON_BACKEND'] = 'shapely'
        try:
            fallback, fallen, _ = operations.prepare(project, paths)
        except ValueError:
            fallback = None
        finally:
            os.environ.pop('KERFKITE_POLYGON_BACKEND', None)
        if fallback is None:
            continue
        c = Carve(fallback)
        deep, fallen_deep = deep_points(ours, depths, settings), deep_points(fallback, fallen, settings)
        if deep or fallen_deep:
            gap = max(c.furthest(deep), a.furthest(fallen_deep)) / resolution
            apart.append(gap)
            # A shape with no corner turning inwards: its walls are sampled evenly as seen from every path.
            if g.geom_type == 'Polygon' and not g.interiors and g.convex_hull.area - g.area < 1e-9 * g.area:
                convex_apart.append(gap)
        lengths.append(abs(a.length - c.length) / c.length)
        shipped['kernel_paths'] += len(ours)
        shipped['fallback_paths'] += len(fallback)
        shipped['kernel_travel'] += travel(ours)
        shipped['fallback_travel'] += travel(fallback)
        shipped['cut'] += a.length
        shipped['fallback_cut'] += c.length

    apart.sort(), convex_apart.sort(), lengths.sort()
    middle = lambda values: values[len(values) // 2] if values else 0.0
    high = lambda values: values[len(values) * 95 // 100] if values else 0.0
    last = lambda values: values[-1] if values else 0.0
    print(f'{carved} shapes carved, {empty} with nothing to carve or beyond a limit')
    print(
        '1. safe: at every point and in the middle of every move the cut stays inside the outline; least room between a cut'
        f' and a wall {room:.4f} sampling intervals at a point and {between:.4f} in the middle of a move'
    )
    print(
        f'2. the Shapely carve of the same samples, {same["compared"]} shapes: paths at most {same["hausdorff"]:.9f} mm apart'
        f' ({same["relative"]:.2e} of the sampling interval), total length within {same["length"]:.2e}, deepest cut within'
        f' {same["depth"]:.2e} mm'
    )
    print(
        f'3. the fallback as shipped, {len(lengths)} shapes, {len(apart)} of them with deep cuts: a deep cut is from the'
        f' other carve at most {middle(apart):.2f} sampling intervals in the middle shape, {high(apart):.2f} in 95% of shapes'
        f' and {last(apart):.2f} in the worst;'
        f' for the {len(convex_apart)} shapes with no corner turning inwards, {last(convex_apart):.2f} in the worst.'
        f' Total length differs by {middle(lengths) * 100:.2f}% in the middle shape, {high(lengths) * 100:.1f}% in 95% and'
        f' {last(lengths) * 100:.1f}% in the worst; over all shapes {shipped["cut"]:.0f} mm against {shipped["fallback_cut"]:.0f} mm.'
        f' {shipped["kernel_paths"]} paths against {shipped["fallback_paths"]}, and {shipped["kernel_travel"]:.0f} mm of'
        f' travel between paths against {shipped["fallback_travel"]:.0f} mm'
    )
    if last(convex_apart) > 1.5:
        failures.append(({}, f'a deep cut in a shape with no corner turning inwards is {last(convex_apart)} sampling intervals from the fallback carve'))  # fmt: skip
    if shipped['fallback_cut'] and abs(shipped['cut'] - shipped['fallback_cut']) > 0.02 * shipped['fallback_cut']:
        failures.append(({}, 'the total length of all carves differs from the fallback by more than 2%'))
    if failures:
        for project, reason in failures[:5]:
            print('FAIL', reason, '\n  ', str(project)[:400])
        raise SystemExit(f'{len(failures)} shapes outside the bounds.')
    print('PASS')


if __name__ == '__main__':
    main()
