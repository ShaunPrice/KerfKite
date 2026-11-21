"""Check KerfKite's own polygon kernel against Shapely (GEOS), by tolerance.

    cmake -S native -B build/native -DKERFKITE_WITH_GEOS=OFF && cmake --build build/native
    python validation/polygon_kernel_check.py --cli build/native/kerfkite-cam

The kernel (native/core/include/kerfkite/polygon.hpp) works on a grid of one nanometre with exact
arithmetic; GEOS works in double precision. The two never agree to the byte and are not meant to. Every
case is generated on the grid, and GEOS is given the grid coordinates themselves (whole numbers, which
doubles hold exactly), so both see the same input. Results are compared by

* the area of their symmetric difference, against the length of boundary times the tolerance, and
* the Hausdorff distance between their boundaries, against the tolerance.

The tolerance comes from what the kernel documents, not from what happens to pass: for Boolean results a
few grid units (a crossing is rounded to the grid and the edges are bent through it), and for offsets the
arc allowance of each library as well (the kernel draws arcs outside the true arc, GEOS inside it).
A difference thinner than the tolerance everywhere, such as a sliver one library keeps and the other
loses, is accepted and counted in the `thin` column; anything else is a failure and is printed with its
request.

Independently of GEOS, each result must be valid, the areas must satisfy the identities of the set
operations, a result united with itself must come back unchanged, and every vertex of an offset must be
at the offset distance from the source boundary.

Groups (--only): boolean, normalise, compose, offset, pocket, clip, artwork, cam. `cam` runs
studio.operations (inside, outside, pocket, fill) and compares its tool paths with the same steps made
from kernel calls. --dump writes every request and answer; --replay sends the requests of such a file to
another build and compares the answers byte for byte.
"""

import argparse
from fractions import Fraction
import functools
import json
import math
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import shapely  # noqa: E402
from shapely import affinity  # noqa: E402
from shapely.geometry import LinearRing, LineString, MultiPolygon, Point, Polygon  # noqa: E402
from shapely.ops import polygonize, unary_union  # noqa: E402

UNITS = 1e6  # grid units in a millimetre
# Boolean results, in grid units. A crossing is rounded by at most 0.71 units, and an edge bent through
# one rounded point can be bent again by another. The worst distance seen is printed beside this limit.
BOOLEAN_TOLERANCE = 3.0
SEGMENTS = 32
STEP = (math.pi / 2) / SEGMENTS
# Offsets by d: the kernel's corners lie outside the true arc by up to OUTER * d (ArcOptions in
# polygon.hpp), and GEOS's chords inside it by up to INNER * d.
OUTER = 1 / math.cos(0.53125 * STEP) - 1
INNER = 1 - math.cos(STEP / 2)
# What rounding adds to an offset, in grid units: the shifted edges and corners are rounded to the grid
# (0.71) and the result is then resolved like any Boolean result.
OFFSET_GRID = 4.0


class Kernel:
    """Requests to `kerfkite-cam polygon-batch`, many at a time."""

    def __init__(self, cli, dump=None):
        self.cli = str(cli)
        self.dump = open(dump, 'w') if dump else None
        self.seconds = 0.0
        self.requests = 0
        self.rounds = 0  # the most rounds of splitting the kernel has needed for any request

    def answers(self, requests):
        with tempfile.NamedTemporaryFile('w', suffix='.jsonl', delete=False) as file:
            for request in requests:
                file.write(json.dumps(request) + '\n')
        started = time.perf_counter()
        try:
            done = subprocess.run([self.cli, 'polygon-batch', file.name], capture_output=True, text=True)
        finally:
            os.unlink(file.name)
        self.seconds += time.perf_counter() - started
        if done.returncode != 0:
            raise SystemExit(f'kerfkite-cam failed ({done.returncode}): {done.stderr[-3000:]}')
        rows = done.stdout.splitlines()
        if len(rows) != len(requests):
            raise SystemExit('kerfkite-cam did not answer every request')
        self.requests += len(requests)
        return rows

    def run(self, requests):
        if not requests:
            return []
        rows = self.answers(requests + [{'op': 'statistics'}])
        self.rounds = max(self.rounds, json.loads(rows.pop())['ok']['rounds'])
        if self.dump:
            for request, row in zip(requests, rows):
                self.dump.write(json.dumps(request) + '\n' + row + '\n')
        return [json.loads(row) for row in rows]


class Report:
    def __init__(self):
        self.groups = {}
        self.failures = []
        self.strays = {}

    def group(self, name):
        blank = {'cases': 0, 'thin': 0, 'refused': 0, 'area': 0.0, 'distance': 0.0, 'limit': 0.0, 'stray': 0.0}
        return self.groups.setdefault(name, blank)

    def fail(self, name, what, request):
        self.failures.append((name, what, request))
        if len(self.failures) <= 10:
            print(f'FAIL [{name}] {what}\n     {json.dumps(request)[:1200]}')


# ---- Geometry helpers --------------------------------------------------------------------------------


def to_mm(rings):
    return [[[x / UNITS, y / UNITS] for x, y in ring] for ring in rings]


def on_grid(ring):
    """A ring of the kernel's answer, which is in millimetres, back in grid units."""
    return [(round(x * UNITS), round(y * UNITS)) for x, y in ring]


def region_of(answer, grid=True):
    """The kernel's answer as a Shapely geometry, in grid units or in millimetres."""
    convert = on_grid if grid else (lambda ring: ring)
    polygons = [Polygon(convert(p['shell']), [convert(h) for h in p['holes']]) for p in answer['polygons']]
    return MultiPolygon(polygons) if polygons else Polygon()


def rings_of(answer):
    return [p['shell'] for p in answer['polygons']] + [h for p in answer['polygons'] for h in p['holes']]


def winding(point, ring):
    """How many times a ring winds round a point that is not on it."""
    x, y = point
    total = 0
    for (ax, ay), (bx, by) in zip(ring, ring[1:] + ring[:1]):
        if (ay <= y) != (by <= y):
            side = (bx - ax) * (y - ay) - (by - ay) * (x - ax)
            if by > ay and side > 0:
                total += 1
            elif by <= ay and side < 0:
                total -= 1
    return total


def filled(count, rule):
    return count % 2 == 1 if rule == 'evenodd' else count != 0 if rule == 'nonzero' else count > 0


def oracle_region(rings, rule='evenodd'):
    """The region of any rings under a fill rule, through GEOS: node the lines, form the faces, and keep
    each face by the winding number at a point inside it."""
    rings = [ring for ring in rings if len(ring) >= 3]
    lines = [LineString(ring + ring[:1]) for ring in rings]
    if not lines:
        return Polygon()
    keep = []
    for face in polygonize(unary_union(lines)):
        at = face.representative_point()
        if filled(sum(winding((at.x, at.y), ring) for ring in rings), rule):
            keep.append(face)
    return unary_union(keep) if keep else Polygon()


def areal(geometry):
    """The part of a geometry that has area: GEOS answers a touch with points and lines as well."""
    if geometry.geom_type in ('Polygon', 'MultiPolygon'):
        return geometry
    parts = [part for part in getattr(geometry, 'geoms', []) if part.geom_type in ('Polygon', 'MultiPolygon')]
    return unary_union(parts) if parts else Polygon()


def boundary_length(*geometries):
    return sum(g.boundary.length for g in geometries if not g.is_empty)


def thin(geometry, tolerance):
    """Whether a region is nowhere wider than the tolerance."""
    return geometry.is_empty or geometry.buffer(-tolerance / 2).is_empty


def exactly_inside(point, rings, rule):
    """Whether a point is inside rings under a fill rule, in exact arithmetic."""
    x, y = Fraction(point.x), Fraction(point.y)
    total = 0
    for ring in rings:
        for (ax, ay), (bx, by) in zip(ring, ring[1:] + ring[:1]):
            if (ay <= y) != (by <= y):
                side = (bx - ax) * (y - ay) - (by - ay) * (x - ax)
                total += 1 if by > ay and side > 0 else -1 if by <= ay and side < 0 else 0
    return filled(total, rule)


def compare(report, name, request, mine, oracle, tolerance, truth=None):
    """Records how far the kernel's region is from the oracle's; False for a failure.

    `truth`, when given, says exactly whether a point belongs to the region asked for. GEOS's own overlay
    is at times wrong about slivers a few units wide between nearly coincident edges, so before a
    difference from it is called a failure, a point of each part of the difference is put to the truth:
    if the kernel is right about every one, the case is counted under `GEOS overlay` instead."""
    group = report.group(name)
    group['cases'] += 1
    group['limit'] = max(group['limit'], tolerance)
    oracle = areal(oracle)
    if not mine.is_valid:
        report.fail(name, 'GEOS calls the result invalid: ' + shapely.is_valid_reason(mine), request)
        return False
    if mine.is_empty and oracle.is_empty:
        return True
    if not mine.is_empty and not oracle.is_empty:
        # How far the kernel's boundary strays from GEOS's, whatever GEOS has that the kernel has not.
        coords = shapely.get_coordinates(mine.boundary.segmentize(max(mine.boundary.length / 400, 1e-9)))
        stray = float(shapely.distance(shapely.points(coords), oracle.boundary).max())
        group['stray'] = max(group['stray'], stray)
        report.strays.setdefault(name, []).append(stray)
    difference = mine.symmetric_difference(oracle)
    area = difference.area
    allowed = boundary_length(mine, oracle) * tolerance
    distance = math.inf
    if not mine.is_empty and not oracle.is_empty:
        distance = shapely.hausdorff_distance(mine.boundary, oracle.boundary, densify=0.25)
    if area <= allowed and distance <= tolerance:
        group['area'] = max(group['area'], area / allowed if allowed else 0)
        group['distance'] = max(group['distance'], distance)
        return True
    if thin(difference, tolerance):
        group['thin'] += 1
        return True
    if truth is not None:
        parts = [part for part in getattr(difference, 'geoms', [difference]) if not thin(part, tolerance)]
        points = [part.representative_point() for part in parts if part.geom_type == 'Polygon']
        own = [ring.coords[:-1] for polygon in mine.geoms for ring in [polygon.exterior, *polygon.interiors]]
        own = [[(int(x), int(y)) for x, y in ring] for ring in own]
        if points and all(exactly_inside(point, own, 'nonzero') == truth(point) for point in points):
            report.group('GEOS overlay')['cases'] += 1
            return True
    what = f'area {area:.4g} (allowed {allowed:.4g}), Hausdorff {distance:.4g} (allowed {tolerance:.4g})'
    report.fail(name, 'differs from GEOS: ' + what, request)
    return False


# ---- Generators: rings of [x, y] in grid units -------------------------------------------------------


def star(rng, cx, cy, radius, count, rough=0.6):
    """A simple ring round (cx, cy)."""
    ring = []
    for angle in sorted(rng.uniform(0, 2 * math.pi) for _ in range(count)):
        r = radius * rng.uniform(1 - rough, 1)
        point = [round(cx + r * math.cos(angle)), round(cy + r * math.sin(angle))]
        if not ring or ring[-1] != point:
            ring.append(point)
    return ring[:-1] if len(ring) > 1 and ring[0] == ring[-1] else ring


def random_scale(rng):
    """A feature size in grid units, from tens of nanometres to metres."""
    return 10 ** rng.uniform(1.3, 9.5)


def blobs(rng, scale=None, most=3):
    """Random simple polygons, some with holes, some overlapping, somewhere in the work area."""
    scale = scale or random_scale(rng)
    centre_x = rng.choice([0, 0, rng.uniform(-9e9, 9e9)])
    centre_y = rng.choice([0, 0, rng.uniform(-9e9, 9e9)])
    rings = []
    for _ in range(rng.randint(1, most)):
        cx = centre_x + rng.uniform(-scale, scale)
        cy = centre_y + rng.uniform(-scale, scale)
        radius = scale * rng.uniform(0.2, 1)
        rings.append(star(rng, cx, cy, radius, rng.randint(3, 40)))
        if rng.random() < 0.4:
            rings.append(star(rng, cx, cy, radius * rng.uniform(0.05, 0.35), rng.randint(3, 16), 0.3))
    return [ring for ring in rings if len(ring) >= 3]


def boxes(rng, most=6):
    """Rectangles on a coarse lattice: shared edges, corners that touch, exact fits."""
    pitch = rng.choice([1, 2, 1000, 250000, 1000000])
    rings = []
    for _ in range(rng.randint(1, most)):
        x0, y0 = rng.randint(-6, 6), rng.randint(-6, 6)
        w, h = rng.randint(1, 6), rng.randint(1, 6)
        ring = [[x0, y0], [x0 + w, y0], [x0 + w, y0 + h], [x0, y0 + h]]
        if w > 1 and rng.random() < 0.3:  # extra vertices along the sides
            middle = x0 + w // 2
            ring = [[x0, y0], [middle, y0], [x0 + w, y0], [x0 + w, y0 + h], [middle, y0 + h], [x0, y0 + h]]
        if rng.random() < 0.5:
            ring.reverse()
        rings.append([[x * pitch, y * pitch] for x, y in ring])
    return rings


def lattice(rng, most=3):
    """Polygons whose vertices are a few grid units apart: every crossing rounds onto something."""
    size = rng.choice([3, 5, 8, 20, 60])
    rings = []
    for _ in range(rng.randint(1, most)):
        ring = []
        for _ in range(rng.randint(3, 9)):
            point = [rng.randint(0, size), rng.randint(0, size)]
            if not ring or ring[-1] != point:
                ring.append(point)
        if len(ring) >= 3 and ring[0] != ring[-1]:
            rings.append(ring)
    return rings


def tangle(rng, most=2):
    """Rings that cross themselves."""
    scale = random_scale(rng)
    rings = []
    for _ in range(rng.randint(1, most)):
        ring = [
            [round(rng.uniform(-scale, scale)), round(rng.uniform(-scale, scale))] for _ in range(rng.randint(3, 14))
        ]
        ring = [p for i, p in enumerate(ring) if p != ring[i - 1]]
        if len(ring) >= 3:
            rings.append(ring)
    return rings


def square(x, y, side):
    return [[x, y], [x + side, y], [x + side, y + side], [x, y + side]]


def awkward(rng):
    """Constructions that are hard for a clipper: slivers, exact fits, touching rings, collinear runs,
    repeated points, near-misses by a unit or two, coordinates at the edge of the work area."""
    s = rng.choice([1, 7, 1000, 123457, 10000000])

    def j():
        return rng.randint(-2, 2) if rng.random() < 0.5 else 0

    kind = rng.randrange(13)
    if kind == 0:  # two squares sharing an edge, or nearly
        return [
            square(0, 0, 10 * s),
            [[10 * s + j(), j()], [20 * s, 0], [20 * s, 10 * s], [10 * s + j(), 10 * s + j()]],
        ]
    if kind == 1:  # a slot and the bar that exactly fits it
        slot = [[0, 0], [30 * s, 0], [30 * s, 30 * s], [20 * s, 30 * s], [20 * s, 10 * s], [10 * s, 10 * s]]
        slot += [[10 * s, 30 * s], [0, 30 * s]]
        return [slot, [[10 * s, 10 * s + j()], [20 * s, 10 * s + j()], [20 * s, 40 * s], [10 * s, 40 * s]]]
    if kind == 2:  # squares touching at a corner
        return [
            square(0, 0, 10 * s),
            [[10 * s + j(), 10 * s + j()], [20 * s, 10 * s], [20 * s, 20 * s], [10 * s, 20 * s]],
        ]
    if kind == 3:  # a sliver a few units high across a square
        sliver = [[-10 * s, 50 * s], [110 * s, 50 * s + rng.randint(1, 3)], [110 * s, 50 * s - rng.randint(0, 3)]]
        return [square(0, 0, 100 * s), sliver]
    if kind == 4:  # many bars through nearly one point
        centre = 50 * s
        rings = []
        for i in range(rng.randint(3, 8)):
            angle = math.pi * i / 8 + 0.01
            dx, dy = round(60 * s * math.cos(angle)), round(60 * s * math.sin(angle))
            nx, ny = round(-math.sin(angle) * max(2, s // 3)), round(math.cos(angle) * max(2, s // 3))
            bar = [[centre - dx + j(), centre - dy], [centre + dx, centre + dy + j()]]
            rings.append(bar + [[centre + dx + nx, centre + dy + ny], [centre - dx + nx, centre - dy + ny]])
        return rings
    if kind == 5:  # collinear runs and repeated vertices
        run = [[0, 0], [5 * s, 0], [5 * s, 0], [10 * s, 0], [20 * s, 0], [20 * s, 10 * s], [10 * s, 10 * s]]
        return [run + [[0, 10 * s], [0, 5 * s]], [[5 * s, 0], [15 * s, 0], [15 * s, -5 * s], [5 * s, -5 * s]]]
    if kind == 6:  # a hole that touches its outline at a vertex
        return [square(0, 0, 20 * s), [[0, 10 * s], [5 * s, 5 * s], [10 * s, 10 * s], [5 * s, 15 * s]]]
    if kind == 7:  # a comb against a comb
        rings = []
        for i in range(rng.randint(2, 6)):
            rings.append([[2 * i * s, 0], [2 * i * s + s, 0], [2 * i * s + s, 20 * s], [2 * i * s, 20 * s]])
            rings.append([[0, 2 * i * s + j()], [20 * s, 2 * i * s], [20 * s, 2 * i * s + s], [0, 2 * i * s + s + j()]])
        return rings
    if kind == 8:  # the same ring twice, and once more the other way
        ring = star(rng, 0, 0, 20 * s, 9)
        return [ring, list(ring), list(reversed(ring))][: rng.randint(2, 3)]
    if kind == 9:  # nested squares with coincident sides
        return [
            square(0, 0, 30 * s),
            square(0, 0, 20 * s),
            [[10 * s, 0], [30 * s, 0], [30 * s, 10 * s], [10 * s, 10 * s]],
        ]
    if kind == 10:  # long thin triangles fanned from one vertex
        return [[[0, 0], [1000 * s, i * 2 + j()], [1000 * s, i * 2 + 1]] for i in range(rng.randint(2, 7))]
    if kind == 11:  # a ring that touches itself at a vertex: two triangles tip to tip
        return [[[0, 0], [10 * s, 0], [5 * s, 5 * s + j()], [10 * s, 10 * s], [0, 10 * s], [5 * s, 5 * s]]]
    edge = 10000000000  # ten metres from the origin, the application's limit
    return [
        square(edge - 10 * s, edge - 10 * s, 10 * s),
        [[-edge, -edge], [-edge + 7 * s, -edge + j()], [-edge + 3 * s, -edge + 9 * s]],
        [[edge - 15 * s, edge - 5 * s], [edge - 5 * s, edge - 15 * s], [edge - 5 * s + 1, edge - 5 * s]],
    ]


def any_rings(rng):
    roll = rng.random()
    if roll < 0.35:
        return blobs(rng)
    if roll < 0.5:
        return boxes(rng)
    if roll < 0.65:
        return lattice(rng)
    if roll < 0.8:
        return tangle(rng)
    return awkward(rng)


def extent(rings):
    xs = [x for ring in rings for x, _ in ring] or [0]
    ys = [y for ring in rings for _, y in ring] or [0]
    return min(xs), min(ys), max(xs), max(ys)


# ---- Groups ------------------------------------------------------------------------------------------

OPERATIONS = ['union', 'intersection', 'difference', 'xor']


def check_boolean(kernel, report, rng, count):
    """Union, intersection, difference and symmetric difference, and the identities between their areas."""
    cases = []
    for _ in range(count):
        a, b = any_rings(rng), any_rings(rng)
        if rng.random() < 0.3:  # the second set is the first moved by a few units: everything nearly coincides
            b = [[[x + rng.randint(-3, 3), y + rng.randint(-3, 3)] for x, y in ring] for ring in a]
            b = [[p for i, p in enumerate(ring) if p != ring[i - 1]] for ring in b]
        cases.append((a, b, rng.choice(['evenodd', 'nonzero'])))
    requests = []
    for a, b, fill in cases:
        requests += [{'op': op, 'a': to_mm(a), 'b': to_mm(b), 'fill': fill, 'check': True} for op in OPERATIONS]
    requests += [{'op': 'normalise', 'rings': to_mm(side), 'fill': fill} for a, b, fill in cases for side in (a, b)]
    answers = kernel.run(requests)
    sides = answers[len(cases) * 4 :]
    for index, (a, b, fill) in enumerate(cases):
        left, right = oracle_region(a, fill), oracle_region(b, fill)

        def within(point, a=a, b=b, fill=fill):
            return exactly_inside(point, a, fill), exactly_inside(point, b, fill)

        truths = {
            'union': lambda p: any(within(p)),
            'intersection': lambda p: all(within(p)),
            'difference': lambda p: within(p) == (True, False),
            'xor': lambda p: within(p)[0] != within(p)[1],
        }
        results = {}
        for k, op in enumerate(OPERATIONS):
            request, answer = requests[index * 4 + k], answers[index * 4 + k]
            if 'ok' not in answer:
                report.fail(op, 'refused: ' + str(answer), request)
                continue
            if not answer['ok']['valid']:
                report.fail(op, 'the kernel calls its own result invalid: ' + answer['ok'].get('reason', ''), request)
            results[op] = answer['ok']
            oracle = getattr(left, 'symmetric_difference' if op == 'xor' else op)(right)
            compare(report, op, request, region_of(answer['ok']), oracle, BOOLEAN_TOLERANCE, truths[op])
        if len(results) == 4:
            # |A or B| + |A and B| = |A| + |B|, |A xor B| = |A or B| - |A and B|, |A - B| = |A| - |A and B|.
            # Each term is rounded separately, so they agree to the boundary length times the tolerance.
            area_a, area_b = sides[index * 2]['ok']['area'], sides[index * 2 + 1]['ok']['area']
            union, both, xor = (results[op]['area'] for op in ('union', 'intersection', 'xor'))
            slack = (boundary_length(left, right) * BOOLEAN_TOLERANCE * 2 + 1) / UNITS**2
            errors = (
                abs(union + both - area_a - area_b),
                abs(xor - (union - both)),
                abs(results['difference']['area'] - (area_a - both)),
            )
            group = report.group('area identities')
            group['cases'] += 1
            group['area'] = max(group['area'], max(errors) / slack)
            if max(errors) > slack:
                report.fail(
                    'area identities', f'off by {max(errors):.3e} mm2 (allowed {slack:.3e})', requests[index * 4]
                )


def check_normalise(kernel, report, rng, count):
    """The region of arbitrary rings under each fill rule; a result united with itself is itself."""
    cases = [(any_rings(rng), rng.choice(['evenodd', 'nonzero', 'positive'])) for _ in range(count)]
    requests = [{'op': 'normalise', 'rings': to_mm(rings), 'fill': fill, 'check': True} for rings, fill in cases]
    answers = kernel.run(requests)
    again = []
    for (rings, fill), request, answer in zip(cases, requests, answers):
        if 'ok' not in answer or not answer['ok']['valid']:
            report.fail('normalise', 'refused or invalid: ' + json.dumps(answer)[:300], request)
            again.append(None)
            continue
        truth = functools.partial(exactly_inside, rings=rings, rule=fill)
        compare(
            report, 'normalise', request, region_of(answer['ok']), oracle_region(rings, fill), BOOLEAN_TOLERANCE, truth
        )
        flat = rings_of(answer['ok'])
        again.append({'op': 'union', 'a': flat, 'b': flat, 'fill': 'nonzero'})
    repeats = iter(kernel.run([r for r in again if r]))
    for request, answer in zip(again, answers):
        if not request:
            continue
        group = report.group('A united with A')
        group['cases'] += 1
        # A result is already resolved, so uniting it with itself gives it back vertex for vertex.
        if next(repeats).get('ok', {}).get('polygons') != answer['ok']['polygons']:
            report.fail('A united with A', 'is not A', request)


def near_degenerate(closed, scale=1.0):
    """Whether a contour's validity, or a region's emptiness, turns on less than the tolerance: there the
    grid and double precision may properly disagree. Coordinates are in units of `scale` grid units."""
    tolerance = BOOLEAN_TOLERANCE / scale
    region = Polygon()
    for path in closed:
        ring = Polygon(path)
        if not ring.is_valid or ring.area < 4e-9 * (UNITS / scale) ** 2:
            return True
        inner = ring.buffer(-tolerance)
        if inner.geom_type != 'Polygon' or inner.is_empty:
            return True
        region = region.symmetric_difference(ring)
    return thin(areal(region), 4 * tolerance)


def check_compose(kernel, report, rng, count):
    """studio.operations.region: the same region, or the same refusal with the same message."""
    from studio import operations

    cases = []
    for _ in range(count):
        roll = rng.random()
        makers = ((0.5, blobs), (0.7, boxes), (0.8, lattice), (0.9, tangle), (1.0, awkward))
        rings = next(make for limit, make in makers if roll < limit)(rng)
        cases.append([ring + ring[:1] for ring in rings])
    requests = [{'op': 'compose', 'paths': to_mm(closed), 'check': True} for closed in cases]
    for closed, request, answer in zip(cases, requests, kernel.run(requests)):
        group = report.group('compose')
        try:
            reference = operations.region(request['paths'])
        except ValueError as error:
            group['cases'] += 1
            group['refused'] += 1
            if answer.get('error') != str(error):
                if near_degenerate(closed):
                    group['thin'] += 1
                else:
                    what = f'the reference refuses with {error!s}; the kernel answers {json.dumps(answer)[:200]}'
                    report.fail('compose', what, request)
            continue
        if 'ok' not in answer:
            group['cases'] += 1
            if near_degenerate(closed):
                group['thin'] += 1
            else:
                report.fail('compose', f'the kernel refuses with {answer}; the reference accepts', request)
            continue
        if not answer['ok']['valid']:
            report.fail('compose', 'the kernel calls its own result invalid', request)
        scaled = affinity.scale(reference, UNITS, UNITS, origin=(0, 0))
        compare(report, 'compose', request, region_of(answer['ok']), scaled, BOOLEAN_TOLERANCE)


def offset_tolerance(distance):
    return abs(distance) * (OUTER + INNER) + OFFSET_GRID


def check_offset_distance(report, name, request, mine, source, distance):
    """What needs no oracle. Every point of an offset's boundary is at the offset distance from the source
    boundary: never nearer, and further only by the arc allowance. And a grown region contains its source,
    a shrunk one is contained in it."""
    group = report.group(name)
    if mine.is_empty or source.is_empty:
        return
    group['cases'] += 1
    d = abs(distance)
    edge = source.boundary
    nearest, furthest = math.inf, 0.0
    for polygon in mine.geoms:
        for ring in [polygon.exterior, *polygon.interiors]:
            coords = list(ring.coords)
            points = coords + [((ax + bx) / 2, (ay + by) / 2) for (ax, ay), (bx, by) in zip(coords, coords[1:])]
            distances = shapely.distance(shapely.points(points), edge)
            nearest, furthest = min(nearest, float(distances.min())), max(furthest, float(distances.max()))
    # Reported: the most by which a point is short of the distance, in units against the rounding allowed;
    # and the most by which one is beyond it, as a share of the arc allowance and the rounding.
    group['limit'] = OFFSET_GRID
    group['distance'] = max(group['distance'], d - nearest)
    group['area'] = max(group['area'], (furthest - d) / (OUTER * d + OFFSET_GRID))
    if nearest < d - OFFSET_GRID:
        what = f'a boundary point is {d - nearest:.4g} units nearer the source than the distance {d:.6g}'
        report.fail(name, what, request)
    elif furthest > d * (1 + OUTER) + OFFSET_GRID:
        what = f'a boundary point is {furthest - d:.4g} units beyond the distance {d:.6g}'
        report.fail(name, what + f' (allowed {OUTER * d + OFFSET_GRID:.4g})', request)
    outer, inner = (mine, source) if distance > 0 else (source, mine)
    if not thin(inner.difference(outer), OFFSET_GRID):
        report.fail(name, 'the offset does not contain, or is not contained in, its source', request)


def offset_error(boundary, source, distance):
    """How far a curve that claims to be the offset of `source` by `distance` is from being it: the most
    by which any of its points is nearer the source boundary, or further from it, than the distance."""
    if boundary.is_empty or source.is_empty:
        return 0.0
    coords = shapely.get_coordinates(boundary.segmentize(max(boundary.length / 2000, 1e-9)))
    distances = shapely.distance(shapely.points(coords), source.boundary)
    return max(0.0, abs(distance) - float(distances.min()), float(distances.max()) - abs(distance))


def resolved(kernel, report, name, cases):
    """The cases whose rings the kernel resolves into a region, with that region: an offset is compared
    with GEOS's offset of the same region, not of one that differs from it by a sliver under a unit wide,
    which an offset would blow up to its own width."""
    requests = [{'op': 'normalise', 'rings': to_mm(case[0]), 'check': True} for case in cases]
    kept = []
    for case, request, answer in zip(cases, requests, kernel.run(requests)):
        if 'ok' not in answer or not answer['ok']['valid']:
            report.fail(name, 'refused or invalid: ' + json.dumps(answer)[:300], request)
        elif answer['ok']['polygons']:
            kept.append((rings_of(answer['ok']), region_of(answer['ok']), *case[1:]))
    return kept


def swept_offset(source, distance):
    """The offset of a region made the slow, sure way: the region with the disc swept along each edge
    added or taken away. GEOS buffers single segments without the shortcuts it takes for polygons, so
    this is within the chords of its arcs (ORACLE * distance, on the inside of each disc) of the truth."""
    discs = []
    for polygon in getattr(source, 'geoms', [source]):
        for ring in [polygon.exterior, *polygon.interiors]:
            coords = list(ring.coords)
            discs += [LineString(edge).buffer(abs(distance), quad_segs=64) for edge in zip(coords, coords[1:])]
    swept = unary_union(discs)
    return source.union(swept) if distance > 0 else areal(source.difference(swept))


ORACLE = 1 - math.cos(math.pi / 256)


def compare_offset(report, name, request, mine, source, distance, reference=None, rounding=OFFSET_GRID):
    """An offset of `source` by `distance`, positive outwards.

    The test that decides is containment between two swept offsets. The kernel documents that its result
    is never short of the distance by more than the rounding, and beyond it only by the arc allowance
    OUTER * distance and the rounding: so it contains the swept offset by a little less and lies inside
    the one by that much more.

    GEOS's polygon buffer is compared as well, by area and Hausdorff distance, but cannot decide: it
    simplifies its input and is at times further from the true offset than its chords account for (its
    error is measured and reported as `GEOS buffer: distance`), and two curves each close to the true
    offset are still far apart at a sharp corner. Such cases are counted as thin."""
    d = abs(distance)
    group = report.group(name)
    group['cases'] += 1
    if not mine.is_valid:
        report.fail(name, 'GEOS calls the result invalid: ' + shapely.is_valid_reason(mine), request)
        return False
    sign = 1 if distance > 0 else -1
    # Growing: at least d - rounding, at most d (1 + OUTER) + rounding. Shrinking takes away at least
    # d - rounding and at most d (1 + OUTER) + rounding. The oracle's discs are a little small.
    least, most = d - rounding, (d * (1 + OUTER) + rounding) * (1 + 2 * ORACLE)
    smaller = swept_offset(source, least) if sign > 0 else swept_offset(source, -most)
    larger = swept_offset(source, most) if sign > 0 else swept_offset(source, -least)
    if not thin(smaller.difference(mine), rounding) or not thin(mine.difference(larger), rounding):
        what = f'not between the swept offsets for {least:.6g} and {most:.6g} units (distance {distance})'
        report.fail(name, what, request)
        return False
    if reference is None:
        reference = source.buffer(distance, quad_segs=SEGMENTS)
    reference = areal(reference)
    error = offset_error(reference.boundary, source, distance)
    geos = report.group('GEOS buffer: distance')
    geos['cases'] += 1
    geos['area'] = max(geos['area'], error / (INNER * d + 1))
    tolerance = d * OUTER + max(error, d * INNER) + rounding
    if mine.is_empty and reference.is_empty:
        return True
    difference = mine.symmetric_difference(reference)
    allowed = boundary_length(mine, reference) * tolerance
    apart = math.inf
    if not mine.is_empty and not reference.is_empty:
        apart = shapely.hausdorff_distance(mine.boundary, reference.boundary, densify=0.25)
    if difference.area <= allowed and apart <= tolerance:
        group['area'] = max(group['area'], difference.area / allowed)
        group['distance'] = max(group['distance'], apart / tolerance)  # as a share of the tolerance
        group['limit'] = 1.0
    else:
        group['thin'] += 1
    return True


def check_offset(kernel, report, rng, count):
    cases = []
    for _ in range(count):
        roll = rng.random()
        scale = 10 ** rng.uniform(3.5, 9)
        if roll < 0.6:
            rings = blobs(rng, scale)
        else:
            rings = boxes(rng) if roll < 0.75 else awkward(rng) if roll < 0.9 else lattice(rng)
        x0, y0, x1, y1 = extent(rings)
        size = max(x1 - x0, y1 - y0, 10)
        distance = min(2e10, max(20, round(size * 10 ** rng.uniform(-2.5, 0.3)))) * rng.choice([1, -1])
        cases.append((rings, distance))
    cases = resolved(kernel, report, 'offset', cases)
    requests = [
        {'op': 'offset', 'rings': rings, 'distance': distance / UNITS, 'check': True} for rings, _, distance in cases
    ]
    back = []
    for (_, source, distance), request, answer in zip(cases, requests, kernel.run(requests)):
        name = 'offset out' if distance > 0 else 'offset in'
        if 'ok' not in answer or not answer['ok']['valid']:
            report.fail(name, 'refused or invalid: ' + json.dumps(answer)[:300], request)
            continue
        mine = region_of(answer['ok'])
        compare_offset(report, name, request, mine, source, distance)
        check_offset_distance(report, name + ': distance', request, mine, source, distance)
        if answer['ok']['polygons']:
            again = {'op': 'offset', 'rings': rings_of(answer['ok']), 'distance': -distance / UNITS}
            back.append((source, distance, again))
    # Out and back in contains the source, and in and back out is contained in it, to the arc allowance:
    # the corners of the first offset lie outside the true arc, which the second cannot know.
    for (source, distance, request), answer in zip(back, kernel.run([b[2] for b in back])):
        group = report.group('offset and back')
        group['cases'] += 1
        if 'ok' not in answer:
            report.fail('offset and back', 'refused: ' + str(answer), request)
            continue
        returned = region_of(answer['ok'])
        allowance = abs(distance) * (OUTER + 1 - math.cos(STEP / 4)) + 2 * OFFSET_GRID
        if distance > 0:
            bracketed = thin(source.buffer(-allowance, quad_segs=2 * SEGMENTS).difference(returned), OFFSET_GRID)
        else:
            bracketed = thin(returned.difference(source.buffer(allowance, quad_segs=2 * SEGMENTS)), OFFSET_GRID)
        if not bracketed:
            report.fail('offset and back', f'offset by {distance} units and back does not bracket the source', request)


def check_pocket(kernel, report, rng, count):
    """Insets repeated until nothing is left, as the pocket operation makes them. Each pass is compared
    with the inset of the source by the whole depth so far, which shows that the passes do not drift."""
    cases = []
    for _ in range(count):
        scale = 10 ** rng.uniform(6, 8)
        rings = blobs(rng, scale, most=2) if rng.random() < 0.8 else boxes(rng, most=3)
        first = round(scale * rng.uniform(0.01, 0.1))
        cases.append((rings, first, round(first * rng.uniform(0.3, 1))))
    cases = resolved(kernel, report, 'pocket passes', cases)
    requests = [
        {'op': 'pocket', 'rings': rings, 'first': first / UNITS, 'step': step / UNITS, 'check': True}
        for rings, _, first, step in cases
    ]
    for (_, source, first, step), request, answer in zip(cases, requests, kernel.run(requests)):
        if 'ok' not in answer:
            report.fail('pocket passes', 'refused: ' + str(answer), request)
            continue
        passes = answer['ok']['passes']
        depth = first
        for number, item in enumerate(passes):
            if not item['valid']:
                report.fail('pocket passes', f'pass {number} is not valid', request)
                break
            # Each pass against the inset of the source by the whole depth: one rounding more a pass.
            if not compare_offset(
                report, 'pocket passes', request, region_of(item), source, -depth, rounding=OFFSET_GRID + number
            ):
                break
            depth += step
        else:
            # Nothing is left after the last pass: the inset one step further is empty too, or thin.
            if not thin(swept_offset(source, -depth), 2 * (depth * OUTER + OFFSET_GRID + len(passes))):
                report.fail('pocket passes', 'the insets stopped while the source still has room', request)


def pieces(geometry):
    if geometry.geom_type == 'LineString':
        return [geometry] if geometry.length > 0 else []
    return [line for part in getattr(geometry, 'geoms', []) for line in pieces(part)]


def check_parts(report, name, request, region, grown, shrunk, line, parts, tolerance):
    """The parts of a line inside a region. `grown` and `shrunk` are the region moved out and in by the
    tolerance: the line's length inside the region lies between its lengths inside those two, whatever a
    rounded end does at a grazing angle."""
    group = report.group(name)
    group['cases'] += 1
    group['limit'] = max(group['limit'], tolerance)
    whole = LineString(line)
    along = [whole.project(Point(p)) for part in parts for p in part]
    if any(b < a - tolerance for a, b in zip(along, along[1:])):
        report.fail(name, 'the parts are not in order along the line', request)
        return
    for a, b in parts:
        middle = Point((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
        off = max(whole.distance(Point(a)), whole.distance(Point(b)))
        outside = max(region.distance(middle) - tolerance, grown.distance(Point(a)), grown.distance(Point(b)))
        if off > 1 or outside > 0:
            report.fail(name, f'a part {a} to {b} is off the line or outside the region', request)
            return
    length = sum(math.dist(a, b) for a, b in parts)
    least, most = whole.intersection(shrunk).length, whole.intersection(grown).length
    reference = pieces(whole.intersection(region))
    expected = sum(part.length for part in reference)
    slack = tolerance * (len(parts) + len(reference) + 1)
    if not least - slack <= length <= most + slack:
        what = f'the parts total {length:.3f} units; the region allows {least:.3f} to {most:.3f}; GEOS finds {expected:.3f}'
        report.fail(name, what, request)
        return
    if len(parts) == len(reference) and abs(length - expected) <= slack:
        group['distance'] = max(group['distance'], abs(length - expected) / (len(parts) + 1))
    else:
        group['thin'] += 1  # a grazing line, or a part shorter than the tolerance on one side only


def check_clip(kernel, report, rng, count):
    """Lines cut to a region: through vertices, along edges and at random."""
    cases = []
    for _ in range(count):
        roll = rng.random()
        rings = blobs(rng) if roll < 0.6 else boxes(rng) if roll < 0.8 else awkward(rng)
        x0, y0, x1, y1 = extent(rings)
        pad = (x1 - x0 + y1 - y0) // 4 + 2
        lines = []
        for _ in range(rng.randint(1, 12)):
            if rng.random() < 0.4:
                ring = rng.choice(rings)
                a, b = rng.choice(ring), rng.choice(ring)  # through two vertices
                if rng.random() < 0.5:  # along an edge and beyond both ends
                    i = rng.randrange(len(ring))
                    a, b = ring[i], ring[(i + 1) % len(ring)]
                    a, b = [2 * a[0] - b[0], 2 * a[1] - b[1]], [2 * b[0] - a[0], 2 * b[1] - a[1]]
            else:
                a = [rng.randint(x0 - pad, x1 + pad), rng.randint(y0 - pad, y1 + pad)]
                b = [rng.randint(x0 - pad, x1 + pad), rng.randint(y0 - pad, y1 + pad)]
                if rng.random() < 0.3:
                    b[1] = a[1]
            if a != b:
                lines.append([a, b])
        cases.append((rings, lines))
    requests = [{'op': 'clip-lines', 'rings': to_mm(rings), 'lines': to_mm(lines)} for rings, lines in cases]
    tolerance = 2.0
    for (rings, lines), request, answer in zip(cases, requests, kernel.run(requests)):
        if 'ok' not in answer:
            report.fail('clip', 'refused: ' + str(answer), request)
            continue
        region = oracle_region(rings)
        grown, shrunk = region.buffer(tolerance), region.buffer(-tolerance)
        for line, parts in zip(lines, answer['ok']['segments']):
            cut = [on_grid(part) for part in parts]
            check_parts(report, 'clip', request, region, grown, shrunk, line, cut, tolerance)


def artwork_shapes(rng):
    shapes = []
    span = rng.choice([2.0, 10.0, 40.0])
    for _ in range(rng.randint(1, 25)):
        x, y = round(rng.uniform(0, span), 3), round(rng.uniform(0, span), 3)
        shape = {'polarity': 'clear' if rng.random() < 0.15 else 'dark'}
        kind = rng.randrange(5)
        if kind == 0:
            shape.update(type='disc', centre=[x, y], diameter=round(rng.uniform(0.05, 3), 3))
        elif kind in (1, 2):
            shape.update(type='rectangle' if kind == 1 else 'obround', centre=[x, y])
            shape.update(width=round(rng.uniform(0.05, 4), 3), height=round(rng.uniform(0.05, 4), 3))
        elif kind == 3:
            points = [[x, y]]
            for _ in range(rng.randint(0, 5)):
                points.append(
                    [round(points[-1][0] + rng.uniform(-5, 5), 3), round(points[-1][1] + rng.uniform(-5, 5), 3)]
                )
            if rng.random() < 0.2:
                points.append(points[0])
            shape.update(type='stroke', points=points, width=round(rng.uniform(0.05, 1.5), 3))
        else:
            ring = star(rng, x * UNITS, y * UNITS, rng.uniform(0.2, 4) * UNITS, rng.randint(3, 12))
            shape.update(type='polygon', rings=to_mm([ring]))
        shapes.append(shape)
    return shapes


def oracle_shape(shape):
    """A shape of the artwork through GEOS, in grid units, and the radius of its arcs."""
    x, y = (round(v * UNITS) for v in shape.get('centre', [0, 0]))
    w, h = round(shape.get('width', 0) * UNITS), round(shape.get('height', 0) * UNITS)
    if shape['type'] == 'disc':
        radius = shape['diameter'] * UNITS / 2
        return Point(x, y).buffer(radius, quad_segs=SEGMENTS), radius
    if shape['type'] == 'rectangle':
        return shapely.box(x - round(w / 2), y - round(h / 2), x + round(w / 2), y + round(h / 2)), 0
    if shape['type'] == 'obround':
        radius = min(w, h) / 2
        ends = [(x - (w / 2 - radius), y - (h / 2 - radius)), (x + (w / 2 - radius), y + (h / 2 - radius))]
        spine = LineString(ends) if ends[0] != ends[1] else Point(ends[0])
        return spine.buffer(radius, quad_segs=SEGMENTS), radius
    if shape['type'] == 'stroke':
        radius = shape['width'] * UNITS / 2
        points = on_grid(shape['points'])
        spine = LineString(points) if len(set(points)) > 1 else Point(points[0])
        return spine.buffer(radius, quad_segs=SEGMENTS), radius
    return oracle_region([list(map(list, on_grid(ring))) for ring in shape['rings']]), 0


def check_artwork(kernel, report, rng, count):
    """Dark and clear shapes in drawing order, then contours round the image."""
    cases = [(artwork_shapes(rng), [round(rng.uniform(0.05, 0.5), 3) * k for k in (1, 2)]) for _ in range(count)]
    requests = [{'op': 'artwork', 'shapes': shapes, 'offsets': offsets, 'check': True} for shapes, offsets in cases]
    for (shapes, offsets), request, answer in zip(cases, requests, kernel.run(requests)):
        if 'ok' not in answer or not answer['ok']['valid']:
            report.fail('artwork', 'refused or invalid: ' + json.dumps(answer)[:300], request)
            continue
        image = Polygon()
        widest = 0.0
        for shape in shapes:
            piece, radius = oracle_shape(shape)
            widest = max(widest, radius)
            image = image.union(piece) if shape['polarity'] == 'dark' else image.difference(piece)
        tolerance = widest * (OUTER + INNER) + OFFSET_GRID
        mine = region_of(answer['ok'])
        compare(report, 'artwork', request, mine, image, tolerance)
        # The contours are offsets of the kernel's own image.
        for distance, contour in zip(offsets, answer['ok']['contours']):
            if not contour['valid']:
                report.fail('artwork contours', 'a contour is not valid', request)
            elif not mine.is_empty:
                compare_offset(report, 'artwork contours', request, region_of(contour), mine, distance * UNITS)


# ---- The application's operations, in millimetres as it works ----------------------------------------


def cam_cases(rng, count):
    """Closed contours as a project gives them, and the settings of the four polygon operations."""
    cases = []
    for _ in range(count):
        scale = 10 ** rng.uniform(6.5, 8)  # contours of a few to a hundred millimetres
        rings = []
        if rng.random() < 0.2:
            rings = [[[x + 8000000, y + 8000000] for x, y in ring] for ring in boxes(rng, most=3)]
        else:
            for _ in range(rng.randint(1, 3)):
                cx, cy = rng.uniform(scale, 3 * scale), rng.uniform(scale, 3 * scale)
                radius = scale * rng.uniform(0.3, 1)
                rings.append(star(rng, cx, cy, radius, rng.randint(3, 30), rng.choice([0.1, 0.5])))
                if rng.random() < 0.4:
                    rings.append(star(rng, cx, cy, radius * rng.uniform(0.1, 0.4), rng.randint(3, 12), 0.2))
        paths = [ring + ring[:1] for ring in to_mm([r for r in rings if len(r) >= 3])]
        size = scale / UNITS
        operation = rng.choice(['outside', 'inside', 'pocket', 'fill'])
        project = {'operation': operation, 'mode': 'laser' if operation == 'fill' else 'router'}
        if operation in ('outside', 'inside'):
            if rng.random() < 0.5:
                project.update(mode='laser', kerf=round(rng.uniform(0.02, 1), 3))
            else:
                project['tool_diameter'] = round(rng.uniform(0.2, max(0.3, size / 2)), 3)
        elif operation == 'pocket':
            project['tool_diameter'] = round(rng.uniform(max(0.05, size / 15), max(0.1, size / 3)), 3)
            project['stepover'] = max(0.01, round(project['tool_diameter'] / 2 * rng.uniform(0.3, 1), 3))
        else:
            project['interval'] = round(max(0.02, size / rng.uniform(20, 200)), 3)
            project['hatch_angle'] = rng.choice([0, 0, 90, 45, round(rng.uniform(-360, 360), 2)])
            project['crosshatch'] = rng.random() < 0.3
        cases.append((project, paths))
    return cases


def hatch_rows(region, spacing, angle):
    """The rows studio.operations.hatch cuts across a region, turned back into the region's own frame."""
    rotated = affinity.rotate(region, -angle, origin=(0, 0))
    x0, y0, x1, y1 = rotated.bounds
    rows = []
    for i in range(math.ceil((y1 - y0) / spacing) + 1):
        y = min(y1, y0 + i * spacing)
        line = affinity.rotate(LineString([(x0 - 1, y), (x1 + 1, y)]), angle, origin=(0, 0))
        rows.append([list(line.coords[0]), list(line.coords[1])])
    return rows


def by_polygon(rings):
    """Tool paths as the application lists them, split into polygons: the holes of each, which run
    counter-clockwise, and then its outline, which runs clockwise."""
    groups, holes = [], []
    for ring in rings:
        if LinearRing(ring).is_ccw:
            holes.append(Polygon(ring).area)
        else:
            groups.append((Polygon(ring).area, sorted(holes), LinearRing(ring).length))
            holes = []
    return groups if not holes else None


def same_rings(report, name, request, mine, reference, tolerance):
    """Tool paths in the application's order and directions: polygons by area, smallest first, and for
    each its holes before its outline, holes counter-clockwise and outlines clockwise. The holes of one
    polygon are compared as a set: their order is the library's own. False when the two lists have
    different numbers of rings, which the caller judges."""
    if len(mine) != len(reference):
        return False
    own, wanted = by_polygon(mine), by_polygon(reference)
    if own is None or wanted is None or len(own) != len(wanted):
        report.fail(name, 'the rings do not run as holes and then an outline for each polygon', request)
        return True
    slack = 4 * tolerance * max(group[2] for group in wanted)
    ties = any(q[0] - sum(q[1]) - (p[0] - sum(p[1])) <= 2 * slack for p, q in zip(wanted, wanted[1:]))
    for (area, holes, _), (other_area, other_holes, _) in zip(own, wanted):
        if (
            len(holes) != len(other_holes)
            or abs(area - other_area) > slack
            or any(abs(a - b) > slack for a, b in zip(holes, other_holes))
        ):
            if not ties:  # polygons of nearly equal area may properly come in either order
                report.fail(name, 'the rings are the reference\'s but in another order', request)
            break
    return True


def scaled(geometry):
    """A geometry in millimetres, in grid units."""
    return affinity.scale(geometry, UNITS, UNITS, origin=(0, 0))


REFUSALS = (
    'Repair self-crossing or zero-area contours before CAM',
    'Contours cancel under even-odd fill',
    'This operation needs closed contours',
)


def cam_requests(operations, cases):
    """For each case, the kernel calls that make its operation: the region first, then the work."""
    requests, owners = [], []
    for index, (project, paths) in enumerate(cases):
        operation = project['operation']
        requests.append({'op': 'compose', 'paths': paths})
        owners.append(index)
        if operation in ('outside', 'inside'):
            radius = (project['tool_diameter'] if project['mode'] == 'router' else project['kerf']) / 2
            distance = radius if operation == 'outside' else -radius
            requests.append({'op': 'offset', 'rings': paths, 'distance': distance, 'outlines': True})
            owners.append(index)
        elif operation == 'pocket':
            first, step = project['tool_diameter'] / 2, project['stepover']
            requests.append({'op': 'pocket', 'rings': paths, 'first': first, 'step': step, 'outlines': True})
            owners.append(index)
        else:
            try:
                region = operations.region(paths)
            except ValueError:
                continue
            for angle in [project['hatch_angle']] + ([project['hatch_angle'] + 90] if project['crosshatch'] else []):
                rows = hatch_rows(region, project['interval'], angle)
                requests.append({'op': 'clip-lines', 'rings': paths, 'lines': rows})
                owners.append(index)
    return requests, owners


def check_cam(kernel, report, rng, count):
    """Inside, outside, pocket and fill as studio.operations computes them, against the same steps made
    from kernel calls the way the core will make them."""
    from studio import operations

    cases = cam_cases(rng, count)
    requests, owners = cam_requests(operations, cases)
    answers = {}
    for index, request, answer in zip(owners, requests, kernel.run(requests)):
        answers.setdefault(index, []).append((request, answer))
    for index, (project, paths) in enumerate(cases):
        operation = project['operation']
        name = 'cam ' + operation
        group = report.group(name)
        group['cases'] += 1
        request = {'project': project, 'paths': paths}
        composed = answers[index][0][1]
        radius = (project.get('tool_diameter', 0) if project['mode'] == 'router' else project.get('kerf', 0)) / 2
        try:
            reference = operations.prepare(project, paths)[0]
        except ValueError as error:
            group['refused'] += 1
            message = str(error)
            if message in REFUSALS:
                if composed.get('error') != message and not near_degenerate(paths, UNITS):
                    what = f'the reference refuses with {message}; the kernel answers {json.dumps(composed)[:200]}'
                    report.fail(name, what, request)
            elif message in ('Tool does not fit inside these contours', 'Cutter cannot enter this pocket'):
                result = answers[index][1][1].get('ok', {})
                left = result.get('polygons') if operation != 'pocket' else result.get('passes')
                if left:
                    remains = region_of(result if operation != 'pocket' else result['passes'][0], grid=False)
                    if thin(remains, 4 * offset_tolerance(radius * UNITS) / UNITS):
                        group['thin'] += 1
                    else:
                        report.fail(name, f'the reference refuses with {message}; the kernel finds room', request)
            elif message == 'Operation produced no toolpaths':
                # A row that only touches a corner is nothing to the reference. Moved by up to half a unit
                # to the grid, it can cut a few units off the corner instead.
                rows = [
                    part for _, answer in answers[index][1:] for parts in answer['ok']['segments'] for part in parts
                ]
                if any(math.dist(*part) > 4.5e-6 for part in rows):
                    report.fail(name, 'the reference finds no hatch rows; the kernel finds some', request)
            elif message == 'Pocket exceeds vertex limit':
                # The reference counts 100,000 vertices of GEOS's rings. The kernel's passes have far fewer
                # (it does not draw every arc again at every pass), so it is asked for the passes all the
                # same; where it stays under the limit the case is counted as thin.
                passes = answers[index][1][1].get('ok', {}).get('passes')
                if passes is None:
                    report.fail(name, 'the kernel refuses a pocket: ' + str(answers[index][1][1]), request)
                elif sum(len(ring) for item in passes for ring in item['outlines']) <= 100000:
                    group['thin'] += 1
            else:
                report.fail(name, 'the reference refuses with ' + message, request)
            continue
        if 'ok' not in composed:
            if near_degenerate(paths, UNITS):
                group['thin'] += 1
            else:
                report.fail(
                    name, f'the kernel refuses the contours with {composed}; the reference accepts them', request
                )
            continue
        work = answers[index][1:]
        if operation in ('outside', 'inside'):
            distance = radius if operation == 'outside' else -radius
            region = operations.region(paths)
            result = region.buffer(distance, quad_segs=SEGMENTS)
            answer = work[0][1]['ok']
            if compare_offset(
                report, name + ': region', request, region_of(answer), scaled(region), distance * UNITS, scaled(result)
            ):
                tolerance = offset_tolerance(radius * UNITS) / UNITS
                if not same_rings(report, name, request, answer['outlines'], reference, tolerance):
                    group['thin'] += 1  # a ring more or less; the regions agree, so it is thinner than the tolerance
        elif operation == 'pocket':
            # The reference's passes, made as it makes them, beside the kernel's.
            region = operations.region(paths)
            inset, depth = region.buffer(-radius, quad_segs=SEGMENTS), radius
            passes = work[0][1]['ok']['passes']
            rings, number = [], 0
            while not inset.is_empty and number < len(passes):
                rounding = OFFSET_GRID + number
                ok = compare_offset(
                    report,
                    name + ': passes',
                    request,
                    region_of(passes[number]),
                    scaled(region),
                    -depth * UNITS,
                    scaled(inset),
                    rounding,
                )
                if not ok:
                    break
                rings.extend(passes[number]['outlines'])
                inset, depth, number = (
                    inset.buffer(-project['stepover'], quad_segs=SEGMENTS),
                    depth + project['stepover'],
                    number + 1,
                )
            else:
                # One of the two may make a last pass that the other finds empty, if it is thin enough.
                last = (
                    scaled(inset)
                    if not inset.is_empty
                    else region_of(passes[number])
                    if number < len(passes)
                    else Polygon()
                )
                if not thin(last, 4 * offset_tolerance(depth * UNITS)) or len(passes) > number + 1:
                    report.fail(name, f'{len(passes)} passes; the reference makes a different number', request)
                elif len(rings) == len(reference) and not same_rings(
                    report, name, request, rings, reference, offset_tolerance(depth * UNITS) / UNITS
                ):
                    group['thin'] += 1
        else:
            check_fill(report, name, request, operations.region(paths), work, reference)


def check_fill(report, name, request, region, work, reference):
    """Hatch rows: the reference's, to the grid; or, where a row runs along an edge or grazes a corner
    and the two cannot be expected to cut it alike, rows that lie in the region and cover its width."""
    group = report.group(name)
    group['limit'] = max(group['limit'], 3.0)
    mine = []
    for _, answer in work:
        for i, parts in enumerate(answer['ok']['segments']):
            # Alternate rows run the other way, last part first, so that the beam sweeps to and fro.
            ordered = [[b, a] for a, b in reversed(parts)] if i % 2 else parts
            mine.extend(part for part in ordered if math.dist(*part) > 1e-6)
    if len(mine) == len(reference):
        ends = (max(math.dist(a[0], b[0]), math.dist(a[1], b[1])) for a, b in zip(mine, reference))
        worst = max(ends, default=0) * UNITS
        if worst <= 3:
            group['distance'] = max(group['distance'], worst)
            return
    grown, shrunk = region.buffer(3 / UNITS), region.buffer(-3 / UNITS)
    if any(grown.distance(Point(p)) > 0 for part in mine for p in part):
        report.fail(name, 'a hatch row leaves the region', request)
        return
    rows = [LineString(row) for row_request, _ in work for row in row_request['lines']]
    least = sum(row.intersection(shrunk).length for row in rows)
    most = sum(row.intersection(grown).length for row in rows)
    length = sum(math.dist(*part) for part in mine)
    slack = (len(mine) + len(reference) + 1) * 4 / UNITS
    if least - slack <= length <= most + slack:
        group['thin'] += 1
    else:
        report.fail(name, f'hatch rows total {length:.6f} mm; the region allows {least:.6f} to {most:.6f}', request)


LEGEND = """
cases     results compared (for pocket and cam passes, one for each pass)
          `GEOS overlay` counts cases where GEOS's answer differs by more than a sliver and exact
          arithmetic on the input shows the kernel right at a point of every part that differs
thin      accepted although not within the limit: what differs is nowhere wider than the tolerance (a
          sliver, a sharp corner of an offset, a hatch row along an edge)
refused   cases the reference refuses; the kernel must refuse them with the same message
area      the worst area of difference from GEOS as a share of what is allowed (boundary length times the
          tolerance); for `...: distance` rows, the worst overshoot of the offset distance as a share of
          the arc allowance; for `GEOS buffer: distance`, GEOS's own worst error as a multiple of what the
          chords of its arcs explain
distance  the worst Hausdorff distance from GEOS's result in grid units (nanometres), beside its limit;
          for offset, pocket and cam rows as a share of the tolerance (limit 1); for `...: distance` rows
          the worst shortfall from the offset distance in grid units
stray     the furthest the kernel's boundary is from GEOS's in any case, thin ones included: where two
          edges run nearly along each other, a sliver under a unit wide between them is kept by one
          library and lost by the other for as far as the edges run"""

GROUPS = {
    'boolean': (check_boolean, 1.0),
    'normalise': (check_normalise, 1.0),
    'compose': (check_compose, 0.5),
    'offset': (check_offset, 1.0),
    'pocket': (check_pocket, 0.1),
    'clip': (check_clip, 0.5),
    'artwork': (check_artwork, 0.05),
    'cam': (check_cam, 0.6),
}


def replay(cli, dump):
    """The requests of a --dump file through another build; the answers must be the same bytes."""
    lines = Path(dump).read_text().splitlines()
    requests, expected = [json.loads(line) for line in lines[0::2]], lines[1::2]
    kernel = Kernel(cli)
    different = 0
    for start in range(0, len(requests), 2000):
        rows = kernel.answers(requests[start : start + 2000])
        for request, row, wanted in zip(requests[start:], rows, expected[start:]):
            if row != wanted:
                different += 1
                if different <= 3:
                    print('DIFFERENT ' + json.dumps(request)[:600])
    print(f'{len(requests)} requests replayed in {kernel.seconds:.1f} s; {different} answers differ')
    return 1 if different else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cli', required=True, type=Path, help='the kerfkite-cam executable')
    parser.add_argument(
        '--cases', type=int, default=4000, help='cases in a full-size group; costlier groups take a share'
    )
    parser.add_argument('--seed', type=int, default=20261121)
    parser.add_argument('--only', help='comma-separated groups: ' + ', '.join(GROUPS))
    parser.add_argument('--dump', type=Path, help='write every request and answer to this file')
    parser.add_argument('--replay', type=Path, help='send the requests of a --dump file and compare the answers')
    args = parser.parse_args()
    if not args.cli.exists():
        raise SystemExit(f'kerfkite-cam not found: {args.cli}')
    if args.replay:
        return replay(args.cli, args.replay)
    kernel = Kernel(args.cli, args.dump)
    report = Report()
    print(f'Shapely {shapely.__version__}, GEOS {shapely.geos_version_string}; seed {args.seed}')
    for name in args.only.split(',') if args.only else GROUPS:
        check, share = GROUPS[name]
        total = max(1, round(args.cases * share))
        rng = random.Random(f'{args.seed}-{name}')
        started = time.perf_counter()
        for done in range(0, total, 250):
            check(kernel, report, rng, min(250, total - done))
        print(f'{name}: {total} cases in {time.perf_counter() - started:.1f} s', flush=True)
    print()
    print(f'{"check":<26}{"cases":>8}{"thin":>7}{"refused":>8}{"area":>9}{"distance":>11}{"limit":>8}{"stray":>12}')
    for name, group in report.groups.items():
        numbers = f'{group["area"]:>9.3f}{group["distance"]:>11.4g}{group["limit"]:>8.4g}{group["stray"]:>12.4g}'
        print(f'{name:<26}{group["cases"]:>8}{group["thin"]:>7}{group["refused"]:>8}{numbers}')
    print(LEGEND)
    print(
        f'{kernel.requests} requests, kernel time {kernel.seconds:.1f} s, at most {kernel.rounds} rounds of splitting'
    )
    print(f'{len(report.failures)} failures')
    return 1 if report.failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
