"""Compare the Python-free native core with the Python reference.

    cmake -S native -B build/native && cmake --build build/native --config Release
    python validation/native_core_parity.py --cli build/native/kerfkite-cam

Eight comparisons, all against values computed by Python in this run:

1. Number text and arithmetic: fixed-point formatting, round() and repr() for several hundred thousand
   values, including decimal ties and values next to them; and math.dist and sum(), which Python computes
   with more care than the plain formulas. Also the core's answer to whether a line crosses itself, against
   shapely's LineString.is_simple, for random lines and lines built to sit on the edge of each rule.
2. Recorded jobs: every studio.cam.generate call the test suite makes, replayed through the native core.
3. Generated edge cases: projects with one to three settings replaced by unusual values, so that the
   reference's error messages are compared as well as its programs. A third as many again are closed
   contours for the polygon operations (offsets, pockets and hatch fills).
   Ramp, helix and holding-tab jobs have their own generator (entry_tab_cases), with contours that cross,
   touch, fold back or nearly close, and are counted separately. So are pockets entered by a helix
   (helix_pocket_cases): pockets of many shapes, with and without islands, for a range of cutters, depths
   and helix radii. And so are V-carves (vcarve_cases): words of block letters, shapes with holes, thin
   strokes, flat areas and round shapes, with settings from valid to invalid.
4. Rotary jobs: studio.rotary's generate_rotary for every post, wrap axis, feed reference, direction and
   drive on each kind of planar job, then for generated projects of all the kinds above with rotary setups
   that are sometimes refused (rotary_cases); and wrap_job for planar programs given as G-code, as generated
   or with lines added, removed or changed (rotary_wrap_cases). The suite's own calls are recorded as well.
5. Reliefs and stock previews: studio.relief.prepare_relief for height fields given as numbers and as image
   bytes (relief_cases), compared with the Python reference given the core's limits, and studio.simulation's
   simulate for the segments of generated jobs (stock_cases). Where the contact ran (CPU or GPU) is left out.
6. Plans for manual tool changes: studio.tool_changes.plan_tool_changes for router projects of operations with
   the same and with different tools, valid and not (tool_change_cases). Each stage is a whole job, compared
   with the job the Python CAM generates for it.
7. Fixture clearance: studio.fixture_collision.check_fixtures for generated programs, with lines added or changed,
   swept against generated boxes (fixture_cases). Lines that are not ASCII are declined.
8. STL height fields: studio.mesh_import.stl_heightmap for binary and ASCII meshes, at every resolution, damaged
   in the ways the reader must refuse (stl_cases).

For each job the native core must either reproduce the reference exactly (program lines, preview segments,
length, time, bounds and warnings, and for a rotary job its rotary setup; or the same error message) or
decline it as not ported yet. A different answer is a failure. The report gives the share of cases
reproduced, which is the measure of the port.

Offsets, pockets and fills have one implementation, the core's polygon kernel, which the Python reference
calls through the compiled extension (studio/operations.py). For those jobs the geometry of the operation
is therefore shared, not compared; what is compared is everything around it: the checks of the settings and
their order, the order of cutting, leads, tabs, ramp entries on the offset contours, laser power,
post-processing and the totals. Where a helix enters a pocket is decided by the same kernel, through the
same extension (native/core/src/pocket_entry.cpp), so the place is shared and the helix built on it is
compared. The paths and depths of a V-carve come from the same kernel in the same way
(native/core/src/vcarve.cpp), and the program built on them is compared.
validation/polygon_kernel_check.py compares the kernel itself with Shapely, tests/test_router_entry.py
checks every helix against the pocket's walls, and validation/vcarve_check.py and tests/test_vcarve.py check
every V-carve against the outline and against the Shapely V-carve.
Without the extension the reference falls back to Shapely, whose vertices differ, so this comparison stops
unless --without-polygon-operations is given, which leaves those jobs out and says so.
"""

import argparse
import base64
import collections
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).resolve().parent))
os.environ['KERFKITE_GCODE_BACKEND'] = 'python'  # the reference is the Python loop, not the extension


def number_text(cli, count, seed):
    rng = random.Random(seed)
    values = [0.0, -0.0, 0.5, 1.5, 2.5, -2.5, 0.125, 0.375, 2.675, 1e-7, -1e-7, 1e15, 123456789.125, 0.1 + 0.2]
    while len(values) < count:
        kind = rng.randrange(6)
        if kind == 0:  # any finite double of moderate size
            value = struct.unpack('<d', struct.pack('<Q', rng.getrandbits(64)))[0]
            if value != value or abs(value) > 1e12 or (value and abs(value) < 1e-12):
                continue
        elif kind == 1:  # job-sized coordinates
            value = rng.uniform(-2000, 2000)
        elif kind == 2:  # exact binary ties at each number of places
            value = rng.randrange(-4000000, 4000000) / 2 ** rng.randrange(1, 12)
        elif kind == 3:  # decimal ties, which are not exact in binary
            value = (rng.randrange(-2000000, 2000000) + 0.5) / 10 ** rng.randrange(0, 7)
        elif kind == 4:  # the doubles either side of a decimal tie
            value = (rng.randrange(-200000, 200000) + 0.5) / 10 ** rng.randrange(0, 5)
            value = math.nextafter(value, rng.choice((-math.inf, math.inf)))
        else:
            value = rng.choice((1, -1)) * 10 ** rng.uniform(-9, 9)
        values.append(value)
    cases = [(value, rng.randrange(10)) for value in values]
    request = ''.join(f'{value.hex()} {decimals}\n' for value, decimals in cases)
    answer = subprocess.run([str(cli), 'numbers'], input=request, capture_output=True, text=True, check=True).stdout
    rows = answer.splitlines()
    assert len(rows) == len(cases), 'The native core did not answer every number.'
    unwritten = 0
    for (value, decimals), row in zip(cases, rows, strict=True):
        fixed, rounded, shown = row.split('\t')
        if fixed == '-':
            # Only values too large for the exact 64-bit path may be refused, and no job number is that large.
            assert abs(value) * 10**decimals >= 2**63, f'{value!r} to {decimals} places was not written.'
            unwritten += 1
        else:
            assert fixed == f'{value:.{decimals}f}', f'{value!r}:.{decimals}f gave {fixed}'
        if decimals <= 3 and rounded != '-':
            assert float.fromhex(rounded) == round(value, decimals), f'round({value!r}, {decimals}) gave {rounded}'
            assert float.fromhex(rounded).hex() == round(value, decimals).hex()
        assert shown == repr(value), f'repr({value!r}) gave {shown}'
    return len(cases), unwritten


def arithmetic(cli, count, seed):
    """math.dist and sum() as the core computes them, against Python's, to the last bit."""
    rng = random.Random(seed)

    def any_double():
        while True:
            value = struct.unpack('<d', struct.pack('<Q', rng.getrandbits(64)))[0]
            if value == value and abs(value) < 1e300 and (value == 0 or abs(value) > 1e-300):
                return value

    requests, expected = [], []
    for _ in range(count):
        kind = rng.randrange(8)
        if kind == 0:
            a, b = (any_double(), any_double()), (any_double(), any_double())
        elif kind == 1:
            a, b = (rng.uniform(0, 2000), rng.uniform(0, 2000)), (rng.uniform(0, 2000), rng.uniform(0, 2000))
        elif kind == 2:  # coordinates as designs hold them
            a = (round(rng.uniform(0, 700), 3), round(rng.uniform(0, 300), 3))
            b = (round(rng.uniform(0, 700), 3), round(rng.uniform(0, 300), 3))
        elif kind == 3:  # exact right triangles
            a = (float(rng.randrange(0, 2000)), float(rng.randrange(0, 2000)))
            b = (
                a[0] + rng.choice([3.0, 5.0, 8.0, 7.0, 20.0, 0.0]),
                a[1] + rng.choice([4.0, 12.0, 15.0, 24.0, 21.0, 0.0]),
            )
        elif kind == 4:  # nearly coincident points
            a = (rng.uniform(0, 1), rng.uniform(0, 1))
            b = (a[0] + rng.uniform(-1e-9, 1e-9), a[1] + rng.uniform(-1e-9, 1e-9))
        elif kind == 5:  # tiny magnitudes
            a, b = (rng.uniform(-1e-300, 1e-300), 0.0), (0.0, rng.uniform(-1e-305, 1e-305))
        elif kind == 6:  # one axis only
            a = (rng.uniform(0, 2000), rng.uniform(0, 2000))
            b = (a[0], rng.uniform(0, 2000))
        else:  # subnormal numbers
            a, b = (5e-324 * rng.randrange(1, 1000), 5e-324 * rng.randrange(1, 1000)), (0.0, 0.0)
        requests.append(f'dist {a[0].hex()} {a[1].hex()} {b[0].hex()} {b[1].hex()}\n')
        expected.append(math.dist(a, b))
    for _ in range(count // 5):
        n, kind = rng.randrange(0, 40), rng.randrange(4)
        if kind == 0:
            values = [rng.uniform(0, 50) for _ in range(n)]
        elif kind == 1:  # cancellation, where a running total and a compensated sum differ
            values = [rng.choice([1e16, 1.0, -1e16, 3.5, 1e-9, 0.1]) for _ in range(n)]
        elif kind == 2:
            values = [round(rng.uniform(0, 2000), 2) for _ in range(n)]
        else:
            values = [any_double() if rng.random() < 0.3 else rng.uniform(-1e6, 1e6) for _ in range(n)]
        requests.append(f'sum {n} {" ".join(value.hex() for value in values)}\n')
        expected.append(float(sum(values)))
    answer = subprocess.run(
        [str(cli), 'arithmetic'], input=''.join(requests), capture_output=True, text=True, check=True
    )
    answers = answer.stdout.split()
    assert len(answers) == len(expected), 'The native core did not answer every calculation.'
    for request, want, got in zip(requests, expected, answers, strict=True):
        got = float.fromhex(got)
        assert got.hex() == want.hex() or (got != got and want != want), (
            f'{request.strip()} gave {got!r}, Python {want!r}'
        )
    return len(expected)


def crossing_lines(count, seed):
    """Lines for the self-crossing test: random ones, and ones built to sit on the edge of each rule.

    GEOS calls a line simple when no two of its segments cross, overlap or touch, except neighbours at the
    vertex they share and the two ends of a closed line. The lines here have points on coarse grids, where
    shared vertices, vertices on other segments, overlaps and returns along a segment are common; random
    points; long regular lines (zigzags, combs of long parallel segments, fans, spirals, rings) left whole or
    spoilt by one crossing, one touch or one overlap somewhere; points a few units in the last place either
    side of another segment; and all of these moved and scaled, so that the coordinates round.
    """
    rng = random.Random(seed)

    def regular(n):
        kind = rng.randrange(7)
        # Strokes from far apart down to a few units in the last place apart, where the signs GEOS works out
        # need its double-double arithmetic and segments side by side cannot be told apart in groups.
        pitch = rng.choice([0.5, 0.5, 0.01, 0.001, 1e-9, 1e-12, 3e-14])
        if kind == 0:  # a zigzag of level strokes
            return [[100.0 if i % 2 else 0.0, i * pitch] for i in range(n)]
        if kind == 1:  # a comb of long parallel diagonals
            return [[(100.0 if i % 2 else 0.0) + i * pitch, 100.0 if i % 2 else 0.0] for i in range(n)]
        if kind == 2:  # a fan of rays, out along one and back along the next
            points = []
            for i in range(n // 2):
                a = 1.5 * i / (n // 2)
                ray = [[math.cos(a), math.sin(a)], [100 * math.cos(a), 100 * math.sin(a)]]
                points += ray if i % 2 == 0 else ray[::-1]
            return points
        if kind == 3:  # a square spiral inwards
            x = y = 0.0
            points = []
            for i in range(n):
                points.append([x, y])
                step = 100.0 - (i // 2) * (100.0 / n)
                dx, dy = ((1, 0), (0, 1), (-1, 0), (0, -1))[i % 4]
                x, y = x + dx * step, y + dy * step
            return points
        if kind == 4:  # a round spiral
            return [[0.01 * i * math.cos(i * 0.1), 0.01 * i * math.sin(i * 0.1)] for i in range(n)]
        if kind == 5:  # a ring, closed
            points = [[50 * math.cos(2 * math.pi * i / n), 50 * math.sin(2 * math.pi * i / n)] for i in range(n)]
            return points + [list(points[0])]
        # a walk of short steps that never turns back
        x = y = 0.0
        points = []
        for _ in range(n):
            points.append([x, y])
            x, y = x + rng.uniform(0.1, 1), y + rng.uniform(-1, 1)
        return points

    def spoil(points):
        """One fault somewhere in a regular line, or none."""
        points = [list(point) for point in points]
        fault = rng.randrange(8)
        i, j = rng.randrange(len(points) - 1), rng.randrange(len(points) - 1)
        a, b = points[j], points[j + 1]
        if fault == 0:  # a vertex moved onto the middle of another segment: a touch
            points[i] = [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2]
        elif fault == 1:  # a vertex moved far away: usually crossings
            points[i] = [rng.uniform(-10, 110), rng.uniform(-10, 110)]
        elif (
            fault == 2
        ):  # a vertex repeated later in the line: a shared vertex between segments that are not neighbours
            points.insert(i, list(a))
        elif fault == 3:  # a step back along the segment just drawn: an overlap
            points.insert(j + 1, [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2])
            points.insert(j + 1, list(b))
        elif fault == 4:  # a vertex within a few units in the last place of another segment's middle
            x, y = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
            for _ in range(rng.randrange(0, 4)):
                x = math.nextafter(x, rng.choice([-math.inf, math.inf]))
                y = math.nextafter(y, rng.choice([-math.inf, math.inf]))
            points[i] = [x, y]
        elif fault == 5:  # closed, exactly or nearly
            points.append([points[0][0] + rng.choice([0, 0, 1e-9]), points[0][1]])
        elif fault == 6:  # a spur out and back from a vertex
            points.insert(i + 1, list(points[i]))
            points.insert(i + 1, [points[i][0] + rng.uniform(-3, 3), points[i][1] + rng.uniform(-3, 3)])
        return points

    for _ in range(count):
        kind = rng.randrange(10)
        if kind < 3:  # a few points on a coarse grid
            scale = rng.choice([1, 1, 0.5, 0.1, 2.5, 1e-3])
            points = [[scale * rng.randrange(0, 5), scale * rng.randrange(0, 5)] for _ in range(rng.randrange(2, 12))]
        elif kind == 3:  # many points on a grid: certain to cross, in a line long enough for the tree
            points = [[rng.randrange(0, 40) * 0.25, rng.randrange(0, 40) * 0.25] for _ in range(rng.randrange(50, 200))]
        elif kind == 4:  # random points
            points = [[rng.uniform(0, 100), rng.uniform(0, 100)] for _ in range(rng.randrange(3, 60))]
        elif kind == 5:  # points in a line or nearly, in any order
            slope, base = rng.choice([0, 1, 0.5, 1 / 3, 0.1]), rng.uniform(0, 5)
            points = []
            for _ in range(rng.randrange(3, 70)):
                x = rng.choice([rng.randrange(0, 50) * 0.5, rng.uniform(0, 25)])
                y = base + slope * x
                if rng.random() < 0.1:
                    y = math.nextafter(y, rng.choice([-math.inf, math.inf]))
                points.append([x, y])
            if rng.random() < 0.5:
                points.sort()
        else:  # a regular line of 50 to 400 points, usually with one fault
            points = regular(rng.randrange(50, 400))
            if rng.random() < 0.8:
                points = spoil(points)
        if rng.random() < 0.3:  # moved and scaled, as a design in a work area is
            scale, dx, dy = rng.choice([1, 0.001, 1000, 1 / 3, 0.1]), rng.choice([0, 1000, 0.1]), rng.choice([0, 7.3])
            points = [[x * scale + dx, y * scale + dy] for x, y in points]
        if rng.random() < 0.05:
            points.reverse()
        yield points


def self_crossing(cli, count, seed):
    """line_is_simple of the core against shapely's LineString.is_simple, by each of the core's methods."""
    from shapely.geometry import LineString

    methods = ('usual', 'tree', 'pairs')

    def ask(requests):
        """The core's answer to each (method, points): [word, work, tree, reason]."""
        text = ''.join(
            f'{method} {" ".join(f"{float(x).hex()} {float(y).hex()}" for x, y in points)}\n'
            for method, points in requests
        )
        rows = subprocess.run(
            [str(cli), 'simple'], input=text, capture_output=True, text=True, check=True
        ).stdout.splitlines()
        assert len(rows) == len(requests), 'The native core did not answer every line.'
        return [row.split(' ', 3) for row in rows]

    tally = collections.Counter()
    lines = crossing_lines(count, seed)
    while True:
        some = list(itertools.islice(lines, 1500))  # a few thousand at a time: the request is text
        if not some:
            break
        answers = ask([(method, points) for points in some for method in methods])
        for index, points in enumerate(some):
            want = 'simple' if LineString(points).is_simple else 'crossing'
            tally[want] += 1
            for offset, method in enumerate(methods):
                got = answers[index * len(methods) + offset]
                if got[0] == 'declined':
                    # Only for an answer that depends on rounding inside GEOS: none of these lines is long
                    # enough to reach the limit on work.
                    assert 'rounding inside GEOS' in got[3] or 'below 1e-140' in got[3], (
                        f'{method} declined {points!r}: {got[3]}'
                    )
                    tally['declined', method] += 1
                else:
                    assert got[0] == want, f'is_simple is {want}, the core ({method}) says {got[0]}, for {points!r}'
                if method == 'usual':
                    tally['tree'] += got[2] == '1'

    # Long lines of the kinds that used to take time growing with the square of their length. Each is
    # simple, so nothing ends the search early, and the work is counted to show that it does not.
    long_lines = []
    for n in (2000, 8000):
        long_lines.append(
            [[(100.0 if i % 2 else 0.0) + i * 0.001, 100.0 if i % 2 else 0.0] for i in range(n)]
        )  # a comb
        fan = []
        for i in range(n // 2):
            a = 1.5 * i / (n // 2)
            ray = [[math.cos(a), math.sin(a)], [100 * math.cos(a), 100 * math.sin(a)]]
            fan += ray if i % 2 == 0 else ray[::-1]
        long_lines.append(fan)
        x = y = 0.0
        spiral = []
        for i in range(n):
            spiral.append([x, y])
            step = 100.0 - (i // 2) * (100.0 / n)
            dx, dy = ((1, 0), (0, 1), (-1, 0), (0, -1))[i % 4]
            x, y = x + dx * step, y + dy * step
        long_lines.append(spiral)
    for points, got in zip(long_lines, ask([('usual', points) for points in long_lines]), strict=True):
        assert got[0] == 'simple' and got[2] == '1', f'A long simple line of {len(points)} points was answered {got}'
        # Within a multiple of the length of the line times its logarithm. Testing every pair takes the
        # square of the length: forty times this bound for the longer lines here, and a job may hold a
        # line twelve times as long again.
        assert int(got[1]) < 100 * len(points) * math.log2(len(points)), (
            f'{len(points)} points took {got[1]} units of work'
        )
        tally['long'] += 1
    return sum(tally[want] for want in ('simple', 'crossing')), tally


WEIRD = [
    None, True, False, 0, -1, 1, 3, 5, 2.5, 0.5, 0.01, 1e9, 1e-9, -0.0, 10**30, 2000, 2000.0001, 101, 50.5,
    '5', ' 7.5 ', '1_0', '1__0', 'abc', '', 'nan', 'inf', '-Infinity', '1e2', '.5', '5.', '0x10', '+3', [], [1], {},
    'line', 'drill', 'inside', 'outside', 'pocket', 'vcarve', 'router', 'laser', 'knife', 'grbl', 'marlin', 'mach3',
    'mach4', 'linuxcnc', 'uccnc', 'fluidnc', 'seconds', 'milliseconds', 'plunge', 'ramp', 'helix', 'flat', 'ball',
]  # fmt: skip
KEYS = [
    'mode', 'width', 'height', 'feed', 'power', 'safe', 'depth', 'step', 'plunge', 'passes', 'paths', 'path_depths',
    'protocol', 'dwell_units', 'operation', 'tool_shape', 'router_entry', 'tabs', 'operations', 'tool_diameter',
]  # fmt: skip


def generated_cases(count, seed):
    """Projects near valid ones: mostly router line jobs, with a few settings replaced or removed."""
    rng = random.Random(seed)
    for _ in range(count):
        paths = [
            [
                [round(rng.uniform(0, 100), rng.randrange(0, 5)), round(rng.uniform(0, 50), rng.randrange(0, 5))]
                for _ in range(rng.randrange(2, 7))
            ]
            for _ in range(rng.randrange(1, 4))
        ]
        project = dict(
            mode='router', protocol=rng.choice(['grbl', 'grbl', 'marlin', 'linuxcnc', 'mach3', 'mach4', 'uccnc', 'fluidnc', 'grblhal', 'xpro-v5']),
            width=100, height=50, feed=rng.choice([600, 1200.5, 90]), power=rng.choice([12000, 1, 800.4]),
            depth=rng.choice([1, 0.3, 2.55]), step=rng.choice([0.5, 1, 0.07]), paths=paths,
        )  # fmt: skip
        if rng.random() < 0.5:
            project['dwell_units'] = rng.choice(['seconds', 'milliseconds'])
        if rng.random() < 0.35:
            project['path_depths'] = [
                [rng.choice([0, -0.0, -0.25, -1, -round(rng.uniform(0, 3), 3)]) for _ in path] for path in paths
            ]
        if rng.random() < 0.2:
            project['plunge'] = rng.choice([30, 250.25, 1000])
        if rng.random() < 0.2:
            project['safe'] = rng.choice([0.1, 3, 12.345])
        if rng.random() < 0.15:
            # An ordered job: shared settings on the project, geometry and overrides on each operation.
            operations = []
            for _ in range(rng.randrange(1, 4)):
                operation = dict(
                    paths=[
                        [list(point) for point in path] for path in rng.sample(paths, rng.randrange(1, len(paths) + 1))
                    ]
                )
                for key, choices in (
                    ('depth', [0.4, 1.5]), ('step', [0.2, 1]), ('feed', [300, 750.5]), ('tool_id', ['T1', 'T2', 1, True, None]),
                    ('tool_diameter', [3.175, 3, 6]), ('tool_shape', ['flat', 'ball', 'drill']), ('operation', ['line', 'vcarve', 'drill']),
                    ('protocol', [project['protocol'], 'grbl']), ('width', [100, 100.0, 120]), ('mode', ['router', 'laser']),
                    ('operations', [[]]), ('v_angle', [60, 60.0, 90]),
                ):  # fmt: skip
                    if rng.random() < (0.3 if key in ('depth', 'step', 'feed', 'tool_id') else 0.06):
                        operation[key] = rng.choice(choices)
                operations.append(operation)
            project['operations'] = rng.choice([operations] * 12 + [[], operations * 40, [None], {}])
            if rng.random() < 0.8:
                del project['paths']
            project.pop('path_depths', None)
        if 'operations' not in project and rng.random() < 0.35:
            # A laser job: leads, power ramps, perforation, holding bridges or raster powers.
            project['mode'] = 'laser'
            project.pop('path_depths', None)
            # Most are well inside the work area with modest settings, so that they produce a program;
            # the rest keep wider settings, which the reference mostly rejects.
            tidy = rng.random() < 0.65
            if tidy:
                project['protocol'] = rng.choice(['grbl', 'fluidnc', 'grblhal', 'xpro-v5'])
                project['paths'] = [
                    [[round(rng.uniform(20, 80), rng.randrange(0, 4)), round(rng.uniform(12, 38), rng.randrange(0, 4))] for _ in range(rng.randrange(3, 8))]
                    for _ in range(rng.randrange(1, 4))
                ]  # fmt: skip
            else:
                project['protocol'] = rng.choice(
                    ['grbl', 'grbl', 'fluidnc', 'grblhal', 'xpro-v5', 'marlin', 'linuxcnc']
                )
            if rng.random() < 0.5:
                project['passes'] = rng.choice([1, 2, 3, 2.5])
            if rng.random() < (0.7 if tidy else 0.5):
                project['paths'] = [path + [list(path[0])] for path in project['paths']]
            kind = rng.randrange(5)
            if kind in (0, 1):
                for key, choices, wide in (
                    ('lead_in', [0, 0.5, 3, 7], [0, 0.5, 3, 12]), ('lead_out', [0, 0.25, 2, 6.5], [0, 0.25, 2, 40]),
                    ('ramp_in', [0, 0.3, 2, 4.1], [0, 0.3, 2, 7.5, 200]), ('ramp_out', [0, 0.4, 1.7], [0, 0.4, 1.7, 9]),
                    ('start_power', [0, 20, 100, 33.3], [0, 20, 100, 33.3]), ('end_power', [0, 10, 100], [0, 10, 100]),
                    ('lead_shape', ['straight', 'arc-left', 'arc-right'], ['straight', 'arc-left', 'arc-right', 'curly']),
                ):  # fmt: skip
                    if rng.random() < 0.55:
                        project[key] = rng.choice(choices if tidy else wide)
            if kind in (1, 2):
                project['laser_pattern'] = rng.choice(
                    ['perforate', 'bridges'] if tidy else ['perforate', 'bridges', 'continuous', 'dotted']
                )
                for key, choices, wide in (
                    ('laser_gap', [0.05, 0.5, 1, 2.2], [0.01, 0.5, 1, 6, 300]), ('laser_dash', [0.3, 3, 11.5], [0.01, 0.2, 3, 25]),
                    ('laser_bridge_count', [1, 2, 3, 4], [1, 2, 4, 9, 2.5, 1000]),
                ):  # fmt: skip
                    if rng.random() < 0.6:
                        project[key] = rng.choice(choices if tidy else wide)
            if kind == 3:
                project['path_powers'] = [
                    [rng.choice([0, 1, 255.5, project['power'], round(rng.uniform(0, 1), 3)]) for _ in path]
                    for path in project['paths']
                ]
            if kind == 4 and rng.random() < 0.5:
                # Repeated and nearly repeated points, which the reference drops before measuring.
                for path in project['paths']:
                    path.insert(1, list(path[0]))
                    path.append([path[-1][0], min(50, path[-1][1] + 1e-10)])
        for _ in range(rng.choice([0, 0, 1, 1, 1, 2, 3])):
            what = rng.randrange(8)
            if what < 4:
                project[rng.choice(KEYS)] = rng.choice(WEIRD)
            elif what == 4:
                project.pop(rng.choice(KEYS), None)
            elif what == 5 and isinstance(project.get('paths'), list) and project['paths']:
                path = rng.choice(project['paths'])
                edit = rng.randrange(4)
                if edit == 0 and isinstance(path, list) and path:
                    point = rng.choice(path)
                    if isinstance(point, list) and point:
                        point[rng.randrange(len(point))] = rng.choice(WEIRD)
                elif edit == 1 and isinstance(path, list) and path:
                    path[rng.randrange(len(path))] = rng.choice([[1], [1, 2, 3], 'xy', None, [[1, 2]], {}])
                elif edit == 2 and isinstance(path, list):
                    del path[1:]
                else:
                    project['paths'][rng.randrange(len(project['paths']))] = rng.choice(WEIRD)
            elif what == 6 and isinstance(project.get('path_depths'), list) and project['path_depths']:
                row = rng.choice(project['path_depths'])
                edit = rng.randrange(3)
                if edit == 0 and isinstance(row, list) and row:
                    row[rng.randrange(len(row))] = rng.choice(WEIRD + [-50, -50.5, 0.25, -49.999])
                elif edit == 1 and isinstance(row, list):
                    row.append(-1)
                else:
                    project['path_depths'].append([0, -1])
            else:
                project['mode'] = rng.choice(['router', 'laser', 'knife', 'router'])
        yield project


def uses_polygon_operation(project):
    """Whether a project reads its contours as a region, at the top or in its ordered operations: an offset,
    a pocket, a fill or a V-carve, and also an unknown operation, which is refused only after the region has
    been read. Only lines and drilling do not."""
    if not isinstance(project, dict):
        return False
    parts = [project]
    if isinstance(project.get('operations'), list):
        parts += [part for part in project['operations'] if isinstance(part, dict)]
    return any('operation' in part and part['operation'] not in ('line', 'drill') for part in parts)


def polygon_cases(count, seed):
    """Closed contours for the polygon operations: inside and outside offsets, pockets, fills.

    Rectangles, regular and irregular polygons, alone, nested (holes under even-odd fill), overlapping
    and repeated; closed exactly, closed only to within the tolerance, or left open. Tool sizes run from
    ones that fit to ones that do not, and some contours sit close enough to the edge of the work area
    for the compensated path to leave it, so that the reference's rejections are compared as well.
    """
    rng = random.Random(seed)

    def contour(cx, cy, size, places):
        kind = rng.choice([0, 0, 1, 1, 2, 2, 2, 3, 4]) if rng.random() < 0.96 else 5
        if kind == 0:  # rectangle
            w, h = size * rng.uniform(0.4, 1), size * rng.uniform(0.4, 1)
            points = [[cx - w, cy - h], [cx + w, cy - h], [cx + w, cy + h], [cx - w, cy + h]]
        elif kind == 1:  # regular polygon, up to a fine circle
            n = rng.choice([3, 5, 6, 8, 24, 90])
            turn = rng.uniform(0, 1)
            points = [
                [cx + size * math.cos(turn + 2 * math.pi * i / n), cy + size * math.sin(turn + 2 * math.pi * i / n)]
                for i in range(n)
            ]
        elif kind == 2:  # star-shaped: simple, with sharp and reflex corners
            n = rng.randrange(4, 14)
            angles = sorted(rng.uniform(0, 2 * math.pi) for _ in range(n))
            points = [
                [cx + size * r * math.cos(a), cy + size * r * math.sin(a)]
                for a, r in ((a, rng.uniform(0.25, 1)) for a in angles)
            ]
        elif kind == 3:  # an L with a narrow leg, which an inside offset can split or remove
            leg = size * rng.choice([0.05, 0.15, 0.4])
            points = [[cx - size, cy - size], [cx + size, cy - size], [cx + size, cy - size + leg], [cx - size + leg, cy - size + leg], [cx - size + leg, cy + size], [cx - size, cy + size]]  # fmt: skip
        elif kind == 4:  # two lobes joined by a neck
            neck = size * rng.choice([0.02, 0.1, 0.3])
            points = [[cx - size, cy - size / 2], [cx - neck, cy - size / 2], [cx - neck, cy - neck], [cx + neck, cy - neck], [cx + neck, cy - size / 2], [cx + size, cy - size / 2], [cx + size, cy + size / 2], [cx + neck, cy + size / 2], [cx + neck, cy + neck], [cx - neck, cy + neck], [cx - neck, cy + size / 2], [cx - size, cy + size / 2]]  # fmt: skip
        else:  # arbitrary points: often self-crossing, which the reference refuses
            points = [
                [cx + rng.uniform(-size, size), cy + rng.uniform(-size, size)] for _ in range(rng.randrange(3, 7))
            ]
        if rng.random() < 0.3:
            points.reverse()
        if places is not None:
            points = [[round(x, places), round(y, places)] for x, y in points]
        close = rng.random()
        if close < 0.94:
            points.append(list(points[0]))
        elif close < 0.975:  # closed to within the tolerance but not exactly
            points.append([points[0][0] + rng.choice([1e-7, -4e-7, 0]), points[0][1] + rng.choice([5e-7, -1e-9])])
        elif close < 0.99:  # just outside the tolerance
            points.append([points[0][0] + 2e-6, points[0][1]])
        return points

    for _ in range(count):
        width, height = rng.choice([(100, 50), (100, 50), (300, 200), (40.5, 30)])
        places = rng.choice([None, None, 1, 2, 3])
        margin = rng.choice([0.5, 2, 8, 8, 8])
        operation = rng.choice(['inside', 'outside', 'inside', 'outside', 'pocket', 'pocket', 'pocket'])
        laser = rng.random() < 0.4
        if laser:  # the offset is half the kerf, and a region can be hatched
            operation = rng.choice(['inside', 'outside', 'inside', 'outside', 'fill', 'fill', 'fill'])
        # Pockets and fills write many moves for their area, so they stay small.
        size = rng.uniform(
            3 if operation == 'pocket' else 1.5,
            min(min(width, height) / 2 - margin, {'pocket': 6, 'fill': 10}.get(operation, 30)),
        )
        if size > 15 and rng.random() < 0.3:
            places = 0
        cx = rng.uniform(margin + size, width - margin - size)
        cy = rng.uniform(margin + size, height - margin - size)
        paths = [contour(cx, cy, size, places)]
        layout = rng.choice([0, 0, 1, 2, 2, 3, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5])
        if layout == 0:  # a hole
            paths.append(contour(cx, cy, size * rng.uniform(0.1, 0.6), places))
        elif layout == 1:  # a hole with an island in it
            paths.append(contour(cx, cy, size * 0.6, places))
            paths.append(contour(cx, cy, size * 0.2, places))
        elif layout == 2:  # overlapping contours
            paths.append(
                contour(
                    cx + size * rng.uniform(-0.6, 0.6),
                    cy + size * rng.uniform(-0.6, 0.6),
                    size * rng.uniform(0.3, 0.9),
                    places,
                )
            )
        elif layout == 3 and rng.random() < 0.2:  # the same contour twice, which cancels
            paths.append([list(point) for point in paths[0]])
        elif layout == 4:  # separate parts, some of equal area
            part = min(size / 2.5, 6)
            paths = [
                [[x, y], [x + part, y], [x + part, y + part * rng.choice([1, 1, 0.5])], [x, y + part], [x, y]]
                for x, y in (
                    (cx - size + i * part * 1.2, cy - size + j * part * 1.2) for i in range(2) for j in range(2)
                )
            ]
            rng.shuffle(paths)
        project = dict(
            mode='router', protocol=rng.choice(['grbl', 'grbl', 'linuxcnc', 'marlin', 'mach3']),
            width=width, height=height, feed=rng.choice([600, 1200.5]), power=rng.choice([12000, 800]),
            depth=rng.choice([1, 0.3, 1.2]), step=rng.choice([0.5, 1]), paths=paths,
            operation=operation,
        )  # fmt: skip
        if project['protocol'] in ('linuxcnc', 'mach3'):
            project['dwell_units'] = rng.choice(['seconds', 'milliseconds'])
        if rng.random() < 0.7:
            project['tool_diameter'] = rng.choice(
                [0.01, 0.4, 1, 1, 1, 2, 2, 2, 3, 3, 3.175, 3.175, 6, 6.35, 12.7, 45, 100]
            )
            if rng.random() < 0.08:
                project['tool_diameter'] = rng.choice([0.005, 101, '3', ' 1.5 ', True, None, 'wide', [3]])
        if project['operation'] == 'pocket':
            if rng.random() < 0.8:
                project['stepover'] = rng.choice([0.4, 0.5, 0.5, 0.75, 1, 1.5, 3])
                if rng.random() < 0.08:
                    project['stepover'] = rng.choice([0.01, 0.005, '0.6', None, False, 100.5])
            if isinstance(project.get('tool_diameter', 3.175), (int, float)) and rng.random() < 0.85:
                project['tool_diameter'] = rng.choice([1, 1, 1.5, 2, 2, 3])
                if 'stepover' in project and rng.random() < 0.9:
                    project['stepover'] = rng.choice([0.25, 0.4, 0.5, 0.5, 0.75])
                    if project['stepover'] > project['tool_diameter'] / 2 and rng.random() < 0.9:
                        project['stepover'] = project['tool_diameter'] / 2
        if operation == 'pocket' and rng.random() < 0.8:
            project['step'] = project['depth']
        if laser:
            project['mode'] = 'laser' if rng.random() < 0.95 else 'knife'
            project['protocol'] = rng.choice(['grbl', 'grbl', 'fluidnc', 'grblhal'])
            project.pop('dwell_units', None)
            if rng.random() < 0.75:
                project['kerf'] = rng.choice([0, 0.0, -0.0, 0.05, 0.1, 0.2, 0.25, 1, 5])
                if rng.random() < 0.08:
                    project['kerf'] = rng.choice([5.01, -0.1, '0.3', True, None, 'thin'])
            if rng.random() < 0.3:
                project['passes'] = rng.choice([1, 2, 3])
            for key, choices in (('lead_in', [0.5, 2]), ('lead_out', [0.5, 1.5]), ('ramp_in', [0.4, 2]), ('laser_pattern', ['perforate', 'bridges']), ):  # fmt: skip
                if rng.random() < 0.08:
                    project[key] = rng.choice(choices)
            if operation == 'fill' or rng.random() < 0.05:
                for key, choices in (('interval', [0.2, 0.25, 0.5, 0.5, 1, 2, 2, 20, 0.02, 0.01, '1']), ('hatch_angle', [0, 0.0, -0.0, 30, 45, 90, 90.0, 180, 270, 360, -90, -17.5, 33.3, 1e-9, 361, '45']), ('crosshatch', [True, True, False, 1, 0, 0.0, 'yes', '', [], [0], {}, None])):  # fmt: skip
                    if rng.random() < 0.6:
                        project[key] = rng.choice(choices)
            if rng.random() < 0.05:
                project['path_powers'] = [[100 for _ in path] for path in paths]
        for key, choices in (
            ('tabs', [{'width': 3, 'height': 0.5, 'count': 2}, False, None]), ('router_entry', ['plunge', 'ramp', 'helix']),
            ('tool_shape', ['flat', 'ball', 'drill']), ('path_depths', [None, [[-1 for _ in path] for path in paths]]),
            ('operation', ['line', 'drill', 'engrave', 'vcarve', 'vcarve', 'fill', 'pocket', None, 5]), ('stock_thickness', [1]),
        ):  # fmt: skip
            if rng.random() < 0.04:
                project[key] = rng.choice(choices)
        if rng.random() < 0.08:
            # An ordered job: a pocket or an offset and then another contour operation, with one tool.
            first = {key: project.pop(key) for key in ('paths', 'operation')}
            second = dict(
                paths=[[list(point) for point in path] for path in paths[:1]],
                operation=rng.choice(['outside', 'inside', 'line']),
            )
            project['operations'] = [first, second]
        yield project


def entry_tab_cases(count, seed):
    """Router line jobs with ramp or helix entries and holding tabs, on contours chosen to reach each check.

    About half are tidy: closed, simple contours and settings the reference accepts, including ties, numeric
    text and limits, so that programs are compared. The rest add crossings, touching vertices, spikes,
    repeated points, seams that almost close, cutters at the edge of the work area and settings of the wrong
    size or type, so that each rejection is compared in the reference's order.
    """
    rng = random.Random(seed)

    def contour(tidy):
        kind = rng.choice([0, 0, 1, 1, 5, 6]) if tidy else rng.randrange(12)
        if kind < 2:  # a rectangle, sometimes with vertices along its sides
            x, y = rng.choice([10, 12.5, 20.25, 3]), rng.choice([8, 10, 5.5, 3])
            w, h = rng.choice([40, 30.5, 12, 0.5] if not tidy else [40, 30.5, 12]), rng.choice([20, 15.25, 6])
            points = [[x, y], [x + w, y], [x + w, y + h], [x, y + h]]
            if rng.random() < 0.3:
                points.insert(1, [x + w / 2, y])
            if not tidy and rng.random() < 0.2:
                points.insert(rng.randrange(1, len(points)), [x + w / 4, y + h / 4])
        elif kind == 5:  # a regular polygon; many sides give short edges
            n = rng.choice([3, 4, 5, 6, 8, 12, 36, 90])
            cx, cy, r = rng.uniform(25, 75), rng.uniform(15, 35), rng.choice([10, 5.5, 2, 0.05])
            turn = rng.uniform(0, 6.3)
            points = [
                [cx + r * math.cos(turn + 2 * math.pi * i / n), cy + r * math.sin(turn + 2 * math.pi * i / n)]
                for i in range(n)
            ]
            if rng.random() < 0.5:
                points = [[round(px, 3), round(py, 3)] for px, py in points]
        elif kind == 6:  # a cutter that just fits, or just does not fit, in the work area
            edge = rng.choice(
                [1.5875, 1.5875 - 5e-9, 1.5875 - 2e-8, 1.5, 0, 3] if not tidy else [1.5875, 1.5875 - 5e-9, 3]
            )
            far = rng.choice(
                [100 - 1.5875, 100 - 1.5875 + 5e-9, 100 - 1.5875 + 2e-8, 60] if not tidy else [98.4125, 60]
            )
            points = [[edge, edge], [far, edge], [far, 30], [edge, 30]]
        elif kind == 7:  # a very small contour, around the lengths the reference refuses
            size = rng.choice([1e-5, 3e-6, 4e-6, 2.5e-6, 3e-7, 1e-7, 4e-10, 1e-3])
            x, y = rng.choice([20, 20.5, 10]), rng.choice([20, 7.25])
            points = [[x, y], [x + size, y], [x + size, y + size], [x, y + size]][: rng.choice([3, 4, 4])]
        elif kind == 8:  # a triangle with a point on, or just off, one side
            points = [[10, 10], [50, 10], [30, 40]]
            points.insert(1, rng.choice([[30, 10], [30, 10 + 1e-12], [20, 10], [30, 9.999999]]))
        else:  # points on a coarse grid: crossings, shared vertices, overlaps and spikes are common
            scale = rng.choice([1, 5, 2.5, 0.1])
            points = [
                [10 + scale * rng.randrange(0, 5), 10 + scale * rng.randrange(0, 5)] for _ in range(rng.randrange(2, 8))
            ]
        if rng.random() < 0.1:
            points.reverse()
        if rng.random() < 0.15:  # a repeated or nearly repeated vertex
            i = rng.randrange(len(points))
            points.insert(i, [points[i][0] + rng.choice([0, 0, 5e-10, 2e-9, 1e-7]), points[i][1]])
        seam = rng.random() * (0.85 if tidy else 1)
        if seam < 0.75:
            points.append(list(points[0]))
        elif seam < 0.8:  # closed twice
            points += [list(points[0]), list(points[0])]
        elif seam < 0.94:  # almost closed: inside and outside each tolerance the reference uses
            points.append(
                [points[0][0] + rng.choice([5e-10, 2e-9, 5e-7] if tidy else [5e-10, 5e-7, 2e-6, 1e-3]), points[0][1]]
            )
        return points

    for _ in range(count):
        tidy = rng.random() < 0.55

        def pick(good, bad):
            return rng.choice(good if tidy or rng.random() < 0.7 else bad)

        depth = rng.choice([1, 1, 2, 3, 0.3, 2.55, 6, 1.1])
        project = dict(
            mode='router', protocol=rng.choice(['grbl', 'grbl', 'grbl', 'marlin', 'linuxcnc', 'mach3', 'fluidnc']),
            width=100, height=50, feed=rng.choice([600, 1200.5, 90, 100]), power=rng.choice([12000, 800.4]),
            depth=depth, step=rng.choice([0.5, 1, 0.07, 3, 0.3, 0.1]),
            paths=[contour(tidy) for _ in range(rng.choice([1, 1, 1, 2, 3]))],
        )  # fmt: skip
        if project['protocol'] == 'mach3':
            project['dwell_units'] = pick(['seconds', 'milliseconds'], [''])
        if rng.random() < 0.3:
            project['plunge'] = rng.choice([30, 250.25, 1000, 600, 90])
        if rng.random() < 0.2:
            project['safe'] = rng.choice([0.1, 3, 12.345])
        if rng.random() < 0.25:
            project['tool_diameter'] = pick([3.175, 3, 1, 0.01, 2, '2'], [6, 12, 100, 0, None, True, 'wide'])
        if rng.random() < 0.12:
            project['tool_shape'] = pick(['flat'], ['ball', 'v', 'drill', None])
        if rng.random() < 0.1:
            project['operation'] = pick(['line'], ['inside', 'outside', 'pocket', 'drill', None, 'spiral'])
        want_entry, want_tabs = rng.random() < 0.65, rng.random() < 0.6
        if want_entry:
            project['router_entry'] = pick(['ramp'], ['helix', 'helix', 'helix', 'plunge', 'spiral', None, 1, True, []])
            if rng.random() < 0.5:
                project['entry_angle'] = pick(
                    [3, 0.25, 15, 1, 7.5, 10, '2.5', ' 5 ', 3.0], [0.2, 15.5, None, True, 'steep', []]
                )
            if rng.random() < 0.15:
                project['entry_radius'] = rng.choice([1, 0.05, 0.5, 50, 0.01, 51, '1', None, True])
        # The stock thickness that suits tabs (no more than the depth) also suits an entry only at the depth.
        fits = [depth, depth, str(depth)]
        if not want_tabs:
            fits.append(depth + 1e-9)
        elif not want_entry:
            fits += [depth - 0.1, depth - 0.05]
        thickness = pick(
            fits, [depth - 0.1, depth + 0.5, depth + 1e-7, 50, 50.5, 200, 201, None, True, 'thick', [], 0, 0.01]
        )
        if want_entry and not want_tabs and rng.random() < 0.4:
            project['stock_thickness'] = thickness
        if want_tabs:
            tabs = {}
            if tidy or rng.random() < 0.9:
                tabs['width'] = pick([4, 4, 2, 0.5, 0.01, 6.5, '3'], [10, 30, 100, 0, 101, None, True, 'w'])
            if tidy or rng.random() < 0.9:
                tabs['height'] = pick(
                    [0.1, 0.1, 0.15, 0.01, '0.12'], [0.5, 1, depth, depth - 0.01, 50, 0, None, False, 'h']
                )
            which = rng.random() if not tidy else rng.choice([0.2, 0.7])
            if which < 0.5:
                tabs['count'] = pick([1, 2, 2, 3, 4, 4, 6, 2.0, '3'], [9, 40, 100, 2.5, 0, 101, None, True, 'two'])
            if 0.45 < which < 0.95:
                tabs['positions'] = pick(
                    [
                        [0], [0.5], [0.25, 0.75], [0.75, 0.25], [0, 0.5], [0.999], [0.125, 0.375, 0.625, 0.875],
                        ['0.3', 0.8], [-0.0, 0.5], [round(rng.random(), 2) for _ in range(rng.randrange(1, 4))],
                        [rng.random() for _ in range(rng.randrange(1, 3))],
                    ],
                    [
                        [0.99, 0.01], [1], [0.5, 1.0], [0.5, 0.5], [0.1, 0.1000001], [0.0, -0.0], [0.2, None], [0.2, True],
                        [1.5], [-0.1], [], 'half', None, 0.5, [i / 101 for i in range(101)],
                        [rng.random() for _ in range(rng.randrange(3, 8))],
                    ],
                )  # fmt: skip
            if not tidy and rng.random() < 0.04:
                tabs[rng.choice(['depth', 'Width', 'spacing'])] = 1
            project['tabs'] = pick([tabs], [tabs, tabs, tabs, True, False, None, 0, 1, [], {}, 'tabs', [tabs]])
            if tidy or rng.random() < 0.9:
                project['stock_thickness'] = thickness
        if not tidy and rng.random() < 0.06:
            project['path_depths'] = [[rng.choice([0, -0.25, -1]) for _ in path] for path in project['paths']]
        if rng.random() < 0.06:
            # An ordered job: the entry or the tabs may be set on one operation only.
            operations = [dict(paths=project.pop('paths')), dict(paths=[contour(tidy)])]
            for key in ('router_entry', 'tabs', 'entry_angle', 'stock_thickness'):
                if key in project and rng.random() < 0.5:
                    operations[rng.randrange(2)][key] = project.pop(key)
            project.pop('path_depths', None)
            project['operations'] = operations
        if not tidy and rng.random() < 0.1:
            project[rng.choice(KEYS)] = rng.choice(WEIRD)
        yield project

    def ring(n, r):
        points = [[50 + r * math.cos(2 * math.pi * i / n), 25 + r * math.sin(2 * math.pi * i / n)] for i in range(n)]
        return points + [list(points[0])]

    if count:
        # Large jobs, for the limits on lines and vertices: a ramp of 2222 turns that the closing lap takes
        # past the line limit, with its neighbours either side; tabs that take a contour past the vertex
        # limit; and a long contour with both, which is cut.
        base = dict(mode='router', width=100, height=50, feed=600, power=12000)
        for r in (0.0492, 0.04927, 0.0494):
            yield dict(base, depth=6, step=3, router_entry='ramp', entry_angle=0.25, paths=[ring(90, r)])
        tabs = dict(width=4, height=0.5, count=4)
        yield dict(base, depth=2, step=2, stock_thickness=2, tabs=tabs, paths=[ring(99996, 20)])
        yield dict(base, depth=2, step=1, stock_thickness=2, tabs=tabs, router_entry='ramp', paths=[ring(6000, 20)])


def helix_pocket_cases(count, seed):
    """Pockets entered by a helix: the helix is placed inside the pocket, clear of its walls and islands.

    Pockets of many shapes (rectangles, polygons at any angle, stars, shapes with narrow legs and necks),
    alone, with islands, with islands inside islands and as several separate pockets, some of them too small
    for the helix; cutters, stepovers, depths, step-downs, helix radii and entry angles from ones that fit to
    ones that do not. About a fifth are untidy: contours that only nearly close or that cross, settings of
    the wrong size or type, tabs, depth paths, other cutters and operations, so that each rejection is
    compared in the reference's order. A few at the end reach the limits on lines.
    """
    rng = random.Random(seed)

    def contour(cx, cy, size, places, tidy):
        kind = rng.choice([0, 0, 1, 1, 2, 2, 3, 4]) if tidy or rng.random() < 0.9 else 5
        if kind == 0:  # rectangle
            w, h = size * rng.uniform(0.5, 1), size * rng.uniform(0.5, 1)
            points = [[cx - w, cy - h], [cx + w, cy - h], [cx + w, cy + h], [cx - w, cy + h]]
        elif kind == 1:  # regular polygon at any angle, up to a fine circle
            n = rng.choice([3, 5, 6, 8, 24, 90])
            turn = rng.uniform(0, 7)
            points = [
                [cx + size * math.cos(turn + 2 * math.pi * i / n), cy + size * math.sin(turn + 2 * math.pi * i / n)]
                for i in range(n)
            ]
        elif kind == 2:  # star-shaped: simple, with sharp and reflex corners
            n = rng.randrange(4, 12)
            angles = sorted(rng.uniform(0, 2 * math.pi) for _ in range(n))
            points = [
                [cx + size * r * math.cos(a), cy + size * r * math.sin(a)]
                for a, r in ((a, rng.uniform(0.35, 1)) for a in angles)
            ]
        elif kind == 3:  # an L with a leg that the helix may not fit, though the cutter does
            leg = size * rng.choice([0.25, 0.4, 0.7])
            points = [[cx - size, cy - size], [cx + size, cy - size], [cx + size, cy - size + leg], [cx - size + leg, cy - size + leg], [cx - size + leg, cy + size], [cx - size, cy + size]]  # fmt: skip
        elif kind == 4:  # two lobes joined by a neck
            neck = size * rng.choice([0.1, 0.25, 0.45])
            points = [[cx - size, cy - size / 2], [cx - neck, cy - size / 2], [cx - neck, cy - neck], [cx + neck, cy - neck], [cx + neck, cy - size / 2], [cx + size, cy - size / 2], [cx + size, cy + size / 2], [cx + neck, cy + size / 2], [cx + neck, cy + neck], [cx - neck, cy + neck], [cx - neck, cy + size / 2], [cx - size, cy + size / 2]]  # fmt: skip
        else:  # arbitrary points: often self-crossing, which the reference refuses
            points = [
                [cx + rng.uniform(-size, size), cy + rng.uniform(-size, size)] for _ in range(rng.randrange(3, 7))
            ]
        if rng.random() < 0.3:
            points.reverse()
        if places is not None:
            points = [[round(x, places), round(y, places)] for x, y in points]
        close = rng.random()
        if tidy or close < 0.8:
            points.append(list(points[0]))
        elif close < 0.9:  # closed to within the tolerance but not exactly
            points.append([points[0][0] + rng.choice([1e-7, -4e-7, 0]), points[0][1] + rng.choice([5e-7, -1e-9])])
        elif close < 0.95:  # just outside the tolerance
            points.append([points[0][0] + 2e-6, points[0][1]])
        return points

    for _ in range(count):
        tidy = rng.random() < 0.8
        width, height = rng.choice([(100, 50), (100, 50), (300, 200), (40.5, 30)])
        places = rng.choice([None, None, 1, 2, 3, 0])
        size = rng.uniform(2.5, 7)
        margin = rng.choice([4, 4, 4, 1, 0.3] if not tidy else [4])
        cx = rng.uniform(margin + size, width - margin - size)
        cy = rng.uniform(margin + size, height - margin - size)
        paths = [contour(cx, cy, size, places, tidy)]
        layout = rng.choice([0, 0, 0, 1, 1, 2, 3, 4, 5])
        if layout == 1:  # an island
            paths.append(contour(cx, cy, size * rng.uniform(0.08, 0.4), places, tidy))
        elif layout == 2:  # an island with a pocket in it
            paths.append(contour(cx, cy, size * 0.6, places, tidy))
            paths.append(contour(cx, cy, size * rng.choice([0.2, 0.35]), places, tidy))
        elif layout == 3:  # an island off centre, which may touch or cross the wall
            paths.append(
                contour(
                    cx + size * rng.uniform(-0.5, 0.5), cy + size * rng.uniform(-0.5, 0.5), size * 0.25, places, tidy
                )
            )
        elif layout == 4:  # separate pockets of several sizes: the helix may fit some and not others
            part = size / 2
            paths = [
                [[x, y], [x + w, y], [x + w, y + w * rng.choice([1, 1, 0.6])], [x, y + w], [x, y]]
                for x, y, w in (
                    (cx - size + i * part * 1.3, cy - size + j * part * 1.3, part * rng.choice([1, 1, 0.5, 0.35]))
                    for i in range(2)
                    for j in range(2)
                )
            ]
            rng.shuffle(paths)
        elif layout == 5:  # two pockets that overlap, which the even-odd rule turns into three
            paths.append(contour(cx + size * 0.7, cy + size * 0.3, size * 0.6, places, tidy))
        diameter = rng.choice([0.5, 1, 1, 1.5, 2, 2, 3, 3.175])
        project = dict(
            mode='router', protocol=rng.choice(['grbl', 'grbl', 'grbl', 'linuxcnc', 'marlin', 'mach3', 'fluidnc']),
            width=width, height=height, feed=rng.choice([600, 1200.5, 90]), power=rng.choice([12000, 800.4]),
            depth=rng.choice([1, 1, 0.3, 1.2, 2.55, 6]), step=rng.choice([0.5, 1, 1, 3, 10]), paths=paths,
            operation='pocket', router_entry='helix', tool_diameter=diameter,
            stepover=rng.choice([0.25, 0.4, 0.5, 0.5, 0.75, 1]),
        )  # fmt: skip
        if project['stepover'] > diameter / 2 and rng.random() < 0.95:
            project['stepover'] = diameter / 2
        if project['protocol'] in ('linuxcnc', 'mach3'):
            project['dwell_units'] = rng.choice(['seconds', 'milliseconds'])
        if rng.random() < 0.85:
            radii = [0.05, 0.1, 0.25, 0.5, 0.5, 0.75, 1, 1.5, diameter / 2, diameter / 4, '0.4', ' 0.25 ']
            project['entry_radius'] = rng.choice(radii if tidy else radii + [2, 3, 50, 0.04, 50.5, None, True, 'wide'])
            if tidy and float(project['entry_radius']) > diameter / 2 and rng.random() < 0.8:
                project['entry_radius'] = diameter / rng.choice([2, 2, 4])  # a helix as wide as the cutter allows
        elif diameter < 2 and tidy:
            project['entry_radius'] = diameter / 2  # the default radius of 1 is wider than this cutter
        if rng.random() < 0.6:
            angles = [3, 3, 5, 15, 1, 7.5, 10, '2.5', 3.0, 0.25]
            project['entry_angle'] = rng.choice(angles if tidy else angles + [0.2, 15.5, None, True, 'steep'])
            if project['entry_angle'] in (0.25, 1):
                project['depth'] = 0.3  # a shallow angle takes many turns: one short level of them is enough
        if rng.random() < 0.3:
            project['plunge'] = rng.choice([30, 250.25, 1000, 600, 90])
        if rng.random() < 0.2:
            project['safe'] = rng.choice([0.1, 3, 12.345])
        if rng.random() < 0.25:
            depth = project['depth']
            fits = [depth, depth + 1e-9, depth + 2, str(depth), 200]
            project['stock_thickness'] = rng.choice(
                fits if tidy else fits + [depth - 0.1, depth + 1e-7, 201, None, 'thick']
            )
        if not tidy:
            for key, choices in (
                ('tabs', [{'width': 3, 'height': 0.5, 'count': 2}, False, None]), ('tool_shape', ['flat', 'ball', 'drill', None]),
                ('path_depths', [None, [[-1 for _ in path] for path in paths]]), ('router_entry', ['ramp', 'plunge', 'spiral', None]),
                ('operation', ['line', 'inside', 'outside', 'drill', 'vcarve', 'fill', None]), ('mode', ['laser', 'knife']),
                ('tool_diameter', [0.01, 12.7, 100, '3', None, True, 'wide']), ('stepover', [0.01, 0.005, '0.3', None, 100.5]),
            ):  # fmt: skip
                if rng.random() < 0.06:
                    project[key] = rng.choice(choices)
            if rng.random() < 0.08:
                project[rng.choice(KEYS)] = rng.choice(WEIRD)
        if rng.random() < 0.06:
            # An ordered job: the pocket with its helix, and then its outline, with one tool.
            first = {key: project.pop(key) for key in ('paths', 'operation', 'router_entry')}
            second = dict(
                paths=[[list(point) for point in path] for path in paths[:1]],
                operation=rng.choice(['inside', 'line', 'pocket']),
                router_entry=rng.choice(['plunge', 'ramp', 'helix']),
            )
            project['operations'] = [first, second]
        yield project

    if count:
        # A pocket with a single pass and the longest helix allowed: 5553 turns of 36 chords are a program
        # of just under 200,000 lines; two turns more and the closing lap passes the line limit; three more
        # and the helix itself is refused.
        base = dict(
            mode='router', width=100, height=50, feed=600, power=12000, operation='pocket', router_entry='helix'
        )
        small = dict(base, tool_diameter=3, stepover=1.5, entry_radius=0.05, entry_angle=0.25, step=10)
        square = [[20, 20], [25, 20], [25, 25], [20, 25], [20, 20]]
        for depth in (7.602, 7.6047, 7.6061):
            yield dict(small, depth=depth, paths=[square])
        # A wide helix has more chords to a turn, and a second level starts at the depth of the first.
        yield dict(base, tool_diameter=100, stepover=1, entry_radius=50, depth=2, step=1, width=300, height=300,
                   paths=[[[20, 20], [280, 20], [280, 280], [20, 280], [20, 20]]], stock_thickness=2, entry_angle=15)  # fmt: skip


def vcarve_cases(count, seed):
    """V-carving: shapes of the kinds a carve is used for, with settings from valid to invalid.

    Words of block letters with straight and slanted strokes, from strokes thinner than the sampling allowance
    (nothing to carve) to thick ones; frames and letters with holes; thin strokes at any slope; flat areas
    far wider than the bit; round shapes of many short sides; stars; shapes nested, overlapping and apart.
    Sampling intervals, bit angles, depths, step-downs and tool diameters run over their whole ranges, so that
    the depth is limited by the walls, by the depth of cut and by the tool in turn. About a fifth are untidy:
    contours that only nearly close or that cross, settings of the wrong size or type, other heads, entries,
    tabs, depth paths and cutters, and ordered jobs, so that each rejection is compared in the reference's
    order. A few at the end reach the limits on samples and on points.
    """
    rng = random.Random(seed)

    def letter(kind, x, y, w, h, s):
        if kind == 'E':
            m = y + h / 2
            return [[[x, y], [x + w, y], [x + w, y + s], [x + s, y + s], [x + s, m - s / 2], [x + w * 0.7, m - s / 2], [x + w * 0.7, m + s / 2], [x + s, m + s / 2], [x + s, y + h - s], [x + w, y + h - s], [x + w, y + h], [x, y + h]]]  # fmt: skip
        if kind == 'L':
            return [[[x, y], [x + w, y], [x + w, y + s], [x + s, y + s], [x + s, y + h], [x, y + h]]]
        if kind == 'T':
            c = x + w / 2
            return [[[c - s / 2, y], [c + s / 2, y], [c + s / 2, y + h - s], [x + w, y + h - s], [x + w, y + h], [x, y + h], [x, y + h - s], [c - s / 2, y + h - s]]]  # fmt: skip
        if kind == 'H':
            m = y + h / 2
            return [[[x, y], [x + s, y], [x + s, m - s / 2], [x + w - s, m - s / 2], [x + w - s, y], [x + w, y], [x + w, y + h], [x + w - s, y + h], [x + w - s, m + s / 2], [x + s, m + s / 2], [x + s, y + h], [x, y + h]]]  # fmt: skip
        if kind == 'I':
            return [[[x, y], [x + s, y], [x + s, y + h], [x, y + h]]]
        if kind == 'O':  # an outline with a hole
            return [
                [[x, y], [x + w, y], [x + w, y + h], [x, y + h]],
                [[x + s, y + s], [x + w - s, y + s], [x + w - s, y + h - s], [x + s, y + h - s]],
            ]
        if kind == 'V':  # slanted strokes meeting at a point
            c = x + w / 2
            return [[[x, y + h], [c - s / 2, y], [c + s / 2, y], [x + w, y + h], [x + w - s, y + h], [c, y + s * 1.5], [x + s, y + h]]]  # fmt: skip
        c = x + w / 2  # 'A' without its bar: slanted strokes round a notch
        return [
            [[x, y], [x + s, y], [c, y + h - 2 * s], [x + w - s, y], [x + w, y], [c + s / 2, y + h], [c - s / 2, y + h]]
        ]

    def close(points, places, tidy):
        if places is not None:
            points = [[round(px, places), round(py, places)] for px, py in points]
        chance = rng.random()
        if tidy or chance < 0.85:
            points.append(list(points[0]))
        elif chance < 0.92:  # closed to within the tolerance but not exactly
            points.append([points[0][0] + rng.choice([1e-7, -4e-7, 0]), points[0][1] + rng.choice([5e-7, -1e-9])])
        elif chance < 0.96:  # just outside the tolerance
            points.append([points[0][0] + 2e-6, points[0][1]])
        return points

    for _ in range(count):
        tidy = rng.random() < 0.8
        width, height = rng.choice([(100, 50), (100, 50), (300, 200), (40.5, 30)])
        places = rng.choice([None, None, 1, 2, 3, 0])
        resolution = rng.choice([0.2, 0.3, 0.5, 0.5, 0.5, 1, 1, 2])
        x, y = rng.uniform(1, 12), rng.uniform(1, 8)
        kind = rng.choice(['word', 'word', 'word', 'frame', 'stroke', 'stroke', 'flat', 'round', 'star', 'nested', 'overlap', 'apart'])  # fmt: skip
        if kind == 'word':
            h = rng.uniform(4, 12)
            w = h * rng.uniform(0.5, 0.8)
            s = min(rng.choice([0.3, 0.6, 1, 1, 1.5, 2, 3]) * rng.uniform(0.8, 1.2), w / 3.2)
            contours = []
            for i in range(rng.randrange(1, 4)):
                contours += letter(rng.choice('ELTHIOVA'), x + i * (w + rng.choice([0.8, 1.5])), y, w, h, s)
        elif kind == 'frame':
            w, h = rng.uniform(5, 16), rng.uniform(5, 14)
            contours = letter('O', x, y, w, h, min(w, h) * rng.uniform(0.08, 0.45))
        elif kind == 'stroke':  # as thin as the allowance, or thinner
            length, thick, turn = rng.uniform(5, 22), rng.choice([0.25, 0.6, 1, 1.5, 2.5]), rng.uniform(0, math.pi / 2)
            ux, uy = math.cos(turn), math.sin(turn)
            contours = [[[x + 3, y], [x + 3 + length * ux, y + length * uy], [x + 3 + length * ux - thick * uy, y + length * uy + thick * ux], [x + 3 - thick * uy, y + thick * ux]]]  # fmt: skip
        elif kind == 'flat':  # far wider than the bit: the depth of cut or the tool limits the depth
            w, h = rng.uniform(8, 20), rng.uniform(8, 16)
            contours = [[[x, y], [x + w, y], [x + w, y + h], [x, y + h]]]
            resolution = rng.choice([0.5, 1, 1, 2, 5])
        elif kind == 'round':
            radius, sides = rng.uniform(2, 7), rng.choice([5, 8, 24, 90])
            turn = rng.uniform(0, 7)
            contours = [[[x + radius + radius * math.cos(turn + 2 * math.pi * i / sides), y + radius + radius * math.sin(turn + 2 * math.pi * i / sides)] for i in range(sides)]]  # fmt: skip
        elif kind == 'star':
            n = rng.randrange(4, 12)
            angles = sorted(rng.uniform(0, 2 * math.pi) for _ in range(n))
            contours = [[[x + 8 + 7 * r * math.cos(a), y + 8 + 7 * r * math.sin(a)] for a, r in ((a, rng.uniform(0.3, 1)) for a in angles)]]  # fmt: skip
        elif kind == 'nested':  # a hole with an island in it
            size = rng.uniform(6, 14)
            contours = [[[x + d, y + d], [x + size - d, y + d], [x + size - d, y + size - d], [x + d, y + size - d]] for d in (0, size * 0.18, size * 0.36)]  # fmt: skip
        elif kind == 'overlap':  # two shapes that overlap, which the even-odd rule turns into three
            size = rng.uniform(5, 10)
            contours = [[[x, y], [x + size, y], [x + size, y + size], [x, y + size]]]
            contours.append(
                [[px + size * rng.uniform(0.3, 0.7), py + size * rng.uniform(0.2, 0.6)] for px, py in contours[0]]
            )
        else:  # separate parts, some too small to carve
            part = rng.uniform(2, 5)
            contours = [
                [[px, py], [px + side, py], [px + side, py + side * rng.choice([1, 1, 0.5])], [px, py + side]]
                for px, py, side in (
                    (x + i * part * 1.3, y + j * part * 1.3, part * rng.choice([1, 1, 0.5, 0.25]))
                    for i in range(2)
                    for j in range(2)
                )  # fmt: skip
            ]
            rng.shuffle(contours)
        if rng.random() < 0.3:
            contours = [contour[::-1] for contour in contours]
        paths = [close(contour, places, tidy) for contour in contours]
        project = dict(
            mode='router', protocol=rng.choice(['grbl', 'grbl', 'grbl', 'linuxcnc', 'marlin', 'mach3', 'fluidnc']),
            width=width, height=height, feed=rng.choice([600, 1200.5, 90]), power=rng.choice([12000, 800.4]),
            paths=paths, operation='vcarve',
        )  # fmt: skip
        if project['protocol'] in ('linuxcnc', 'mach3'):
            project['dwell_units'] = rng.choice(['seconds', 'milliseconds'])
        if rng.random() < 0.8:
            project['v_resolution'] = resolution
            if not tidy and rng.random() < 0.25:
                project['v_resolution'] = rng.choice([0.1, 5, 0.09, 5.01, 0, '0.5', ' 1 ', None, True, 'fine', [0.5]])
        if rng.random() < 0.7:
            angles = [10, 20, 30, 45, 60, 60, 60.0, 90, 90, 120, 150, 170, '90']
            project['v_angle'] = rng.choice(angles if tidy else angles + [9.9, 170.5, 0, 180, None, False, 'sharp'])
        if rng.random() < 0.75:
            depths = [0.01, 0.3, 1, 1, 1.2, 2.55, 6, 50, '2']
            project['depth'] = rng.choice(depths if tidy else depths + [0.005, 50.5, 0, -1, None, True, 'deep'])
        if rng.random() < 0.7:
            diameters = [0.1, 0.5, 1, 3, 3.175, 6, 6, 6.35, 12.7, 100, '6']
            project['tool_diameter'] = rng.choice(
                diameters if tidy else diameters + [0.05, 100.5, 0, None, True, 'wide']
            )
        if rng.random() < 0.7:
            # Mostly a step-down that reaches the depth in a level or two: a carve is cut again at every level.
            project['step'] = rng.choice([1, 3, 10, 10, 10, 0.5] if tidy else [1, 3, 10, 0.5, 0.01, 10.5, None, 'half'])
        if rng.random() < 0.3:
            project['plunge'] = rng.choice([30, 250.25, 1000, 600, 90])
        if rng.random() < 0.2:
            project['safe'] = rng.choice([0.1, 3, 12.345])
        if not tidy:
            for key, choices in (
                ('mode', ['laser', 'knife', 'router', None]), ('router_entry', ['ramp', 'helix', 'plunge', 'spiral', None]),
                ('tabs', [{'width': 3, 'height': 0.5, 'count': 2}, False, None]), ('tool_shape', ['flat', 'ball', 'v', 'drill', None]),
                ('path_depths', [None, [[-1 for _ in path] for path in paths]]), ('stock_thickness', [1, 100]),
                ('operation', ['line', 'inside', 'outside', 'pocket', 'drill', 'fill', 'engrave', None]), ('passes', [1, 2, 1.5]),
            ):  # fmt: skip
                if rng.random() < 0.1:
                    project[key] = rng.choice(choices)
            if rng.random() < 0.1:
                project[rng.choice(KEYS + ['v_resolution', 'v_angle'])] = rng.choice(WEIRD)
            if rng.random() < 0.12:
                project['mode'] = rng.choice(['laser', 'laser', 'knife'])  # a head that cannot carve
            if rng.random() < 0.08 and paths:
                # A contour that crosses itself, or one moved off the edge of the work area.
                if rng.random() < 0.5 and len(paths[0]) > 4:
                    paths[0][1], paths[0][2] = paths[0][2], paths[0][1]
                else:
                    project['paths'] = [[[px - 20, py] for px, py in path] for path in paths]
        if rng.random() < 0.07:
            # An ordered job: the carve, and then a line traced with the same V-bit or, at times, another tool.
            first = {key: project.pop(key) for key in ('paths', 'operation')}
            second = dict(paths=[[list(point) for point in path] for path in paths[:1]], operation='line', depth=0.2)
            choice = rng.random()
            if choice < 0.6:
                second.update(tool_shape='v')
                for key in ('v_angle', 'tool_diameter'):
                    if key in project:
                        second[key] = project[key]
            elif choice < 0.8:
                second.update(tool_shape='v', v_angle=rng.choice([60, 90]))
            project['operations'] = [first, second] if rng.random() < 0.8 else [second, first]
        yield project

    if count:
        base = dict(mode='router', width=300, height=300, feed=600, power=12000, operation='vcarve', step=10)
        nested = [
            [[2 + d, 2 + d], [298 - d, 2 + d], [298 - d, 298 - d], [2 + d, 298 - d], [2 + d, 2 + d]]
            for d in (0, 0.5, 1)
        ]
        # 12,000 samples are the most an outline may have: the three squares, 3,540 mm of outline, take 11,998
        # at 0.2951 mm (and are then too thin to carve) and 12,001 at 0.295 mm.
        yield dict(base, paths=nested, v_resolution=0.2951)
        yield dict(base, paths=nested, v_resolution=0.295)
        yield dict(base, paths=nested, v_resolution=0.1)
        # A flat area whose carve has more points than a job may hold, and one just small enough to be cut.
        square = lambda side: [[[5, 5], [5 + side, 5], [5 + side, 5 + side], [5, 5 + side], [5, 5]]]
        yield dict(base, paths=square(200), v_resolution=0.5, depth=50, tool_diameter=100)
        yield dict(base, paths=square(110), v_resolution=0.5, depth=50, tool_diameter=100)
        yield dict(base, paths=square(110), v_resolution=0.5, depth=50, tool_diameter=100, step=10, v_angle=170)
        # Shapes on the edge of the work area, a shape too thin to carve, and the same shape given twice.
        yield dict(base, paths=[[[0, 0], [300, 0], [300, 6], [0, 6], [0, 0]]], v_resolution=1)
        yield dict(base, paths=[[[294, 0], [300, 0], [300, 300], [294, 300], [294, 0]]], v_resolution=1, v_angle=90)
        yield dict(base, paths=[[[10, 10], [40, 10], [40, 10.9], [10, 10.9], [10, 10]]])
        yield dict(base, paths=square(20) + square(20))


def drill_knife_cases(count, seed):
    """Drilling and drag-knife projects: most are valid, the rest have the awkward inputs each must handle."""
    rng = random.Random(seed)
    protocols = ['grbl', 'grbl', 'marlin', 'linuxcnc', 'mach3', 'mach4', 'uccnc', 'fluidnc', 'grblhal', 'xpro-v5']
    extra = ['drill_peck', 'drill_point_angle', 'knife_offset', 'knife_lead', 'path_powers']

    def ring(x, y, radius, sides, places, turns=1):
        """A closed outline of a hole; sides, rounding and turns decide whether the reference accepts it."""
        start, way = rng.choice([0, 0, rng.uniform(0, 7)]), rng.choice([1, -1])
        points = [
            [
                x + radius * math.cos(start + way * turns * 2 * math.pi * i / sides),
                y + radius * math.sin(start + way * turns * 2 * math.pi * i / sides),
            ]
            for i in range(sides)
        ]
        if places is not None:
            points = [[round(px, places), round(py, places)] for px, py in points]
        return points + [list(points[0])]

    def hole(diameter, tidy):
        radius = diameter / 2
        x, y = round(rng.uniform(8, 92), rng.randrange(0, 4)), round(rng.uniform(8, 42), rng.randrange(0, 4))
        kind = rng.choice([0, 0, 0, 1, 1, 1, 1, 2, 3]) if tidy else rng.randrange(12)
        if kind == 0:  # a point marker, sometimes with a repeat too close (or just too far) to be the same point
            near = 9e-9 if tidy else 1e-8
            return [[x, y]] * rng.choice([2, 2, 3]) + rng.choice([[], [], [[x + 5e-9, y]], [[x, y + near]]])
        if kind == 1:  # an outline the drill fits; eight sides turn by exactly the largest step allowed
            sides = rng.choice([8, 9, 12, 16, 24, 40])
            scale = rng.choice([1, 1, 1, 1.001, 0.999])
            return ring(x, y, radius * scale, sides, None if sides == 8 else rng.choice([None, None, 6, 4]))
        if kind == 2:  # repeated points inside an outline, which the reference drops before fitting
            points = ring(x, y, radius, 12, None)
            points.insert(3, list(points[2]))
            points.insert(7, [points[6][0] + 1e-9, points[6][1]])
            return points
        if kind == 3:  # touching the edge of the work area, or crossing it
            edge = radius + rng.choice([0, 0, -1e-9, 1e-9, 0.5] if tidy else [-2e-8, -0.5, -1e-9])
            x, y = rng.choice([(edge, y), (100 - edge, y), (x, edge), (x, 50 - edge)])
            return [[x, y], [x, y]]
        if kind == 4:  # an outline the drill does not quite fit, or one rounded too coarsely
            scale = rng.choice([1.002, 1.0021, 0.9979, 1.05])
            return ring(x, y, radius * scale, rng.choice([8, 12, 16]), rng.choice([None, 4, 3, 2]))
        if kind == 5:  # too few points, or a marker drawn as a short line
            return rng.choice(
                [ring(x, y, radius, rng.choice([3, 5, 7]), None), [[x, y], [x + rng.choice([1e-6, 1]), y]]]
            )
        if kind == 6:  # not closed, or closed only to within the tolerance
            points = ring(x, y, radius, 12, None)
            points[-1] = [points[0][0] + rng.choice([5e-7, 2e-6, 0.4]), points[0][1]]
            return points
        if kind == 7:  # twice round, or eight sides after rounding
            return rng.choice([ring(x, y, radius, 16, None, turns=2), ring(x, y, radius, 8, rng.choice([9, 4]))])
        if kind == 8:  # there and back along half a circle
            points = ring(x, y, radius, 16, None)[:9]
            return points + points[-2::-1]
        if kind == 9:  # points in a line, and the same line with a kink too small to fit a circle through
            points = [[x + 0.3 * i, y] for i in range(10)]
            if rng.random() < 0.5:
                points[4][1] += 1e-13
            return points + [list(points[0])]
        if kind == 10:  # uneven spacing: one step wider than an eighth of a turn
            angles = sorted(rng.uniform(0, 2 * math.pi) for _ in range(rng.choice([9, 14])))
            points = [[x + radius * math.cos(a), y + radius * math.sin(a)] for a in angles]
            return points + [list(points[0])]
        return ring(x, y, radius, rng.choice([8, 9, 12, 16, 24, 40]), rng.choice([None, 6, 4, 3]))

    def cut(low, high, tidy):
        """A knife path inside the work area: corners of every kind, repeats and steps too short to keep."""
        x, y = rng.uniform(low + 5, high - 5), rng.uniform(15, 35)
        size = rng.choice([1, 2.5, 4])
        kind = rng.randrange(10) if tidy or rng.random() < 0.6 else rng.randrange(10, 14)
        if kind < 3:
            points = [
                [round(rng.uniform(low + 5, high - 5), rng.randrange(0, 5)), round(rng.uniform(15, 35), rng.randrange(0, 5))]
                for _ in range(rng.randrange(2, 7))
            ]  # fmt: skip
        elif kind < 5:  # a rectangle: right angles, whose directions are exact
            x, y = round(x), round(y)
            points = [[x, y], [x + size, y], [x + size, y + size], [x, y + size]][:: rng.choice([1, -1])]
        elif kind == 5:  # straight on, then back the way it came
            points = [[x, y], [x + size, y], [x + 2 * size, y], [x + size, y], [x, y - size], [x, y - 2 * size]]
        elif kind == 6:  # a polygon with many small turns
            sides = rng.choice([3, 5, 6, 12, 72, 73])
            points = [
                [x + size * math.cos(2 * math.pi * i / sides), y + size * math.sin(2 * math.pi * i / sides)]
                for i in range(sides)
            ]
        elif kind == 7:  # repeats and near repeats, each compared with the last point kept (0.1 nm apart or less)
            step = rng.choice([1e-8, 9e-8, 1e-7, 1.1e-7, 0])
            points = [[x, y], [x, y], [x + step, y], [x + 2 * step, y], [x + 3, y], [x + 3, y + step], [x + 3, y + 2]]
        elif kind == 8:  # diagonals, and a turn of exactly half a circle
            points = [[x, y], [x + size, y + size], [x, y], [x - size, y + size], [x - size, y - size]]
        elif kind == 9:  # along the edge of the work area, including a negative zero
            points = rng.choice([
                [[-0.0, y], [-0.0, y + size]], [[0.0, y], [-0.0, y + size], [0.0, y + 2 * size]], [[x, 0], [x + size, 0]],
                [[x, 50], [x + size, 50], [x + size, 45]], [[99.5, y], [95, y], [95, y + 3]],
                [[x, 0.0], [x - size, -0.0], [x - size, 3]], [[x, 0.0], [x + size, -0.0], [x + size, 3]],
            ])  # fmt: skip
        elif kind == 10:  # no travel at all: a single point once repeats are dropped, which is refused
            return [[x, y]] * rng.choice([2, 3]) + [[x + 5e-8, y]]
        elif kind == 11:  # short steps out and one step back to the start (the reference once divided by zero)
            out = rng.choice([9e-8, 9e-8, 6e-8, 4e-8])  # two steps: beyond the tolerance together, or within it
            return [[x, y], [x + out, y], [x + 2 * out, y], [x, y]] + rng.choice([[], [[x + 2, y + 1]]])
        elif kind == 13:  # many steps each too short to keep, wandering or creeping away from the point kept
            points = [[x, y]]
            for _ in range(rng.randrange(2, 30)):
                step, turn = rng.choice([3e-8, 6e-8, 9.9e-8, 1.01e-7]), rng.uniform(0, 7)
                points.append([points[-1][0] + step * math.cos(turn), points[-1][1] + step * math.sin(turn)])
            return points + rng.choice([[], [[x, y]], [[x + 2, y + 1]], [[x + 2, y + 1], [x, y]]])
        else:  # a lead-in or a swivel that leaves the work area
            return rng.choice([[[0.5, y], [3, y]], [[99.9, y], [95, y], [95, y + 3]], [[x, 49.9], [x + size, 49.9]]])
        if kind != 9 and rng.random() < 0.45:
            points = points + [list(points[0])]
        return points

    for _ in range(count):
        tidy = rng.random() < 0.65  # valid settings throughout; the rest take wider values and stray edits
        project = dict(
            protocol=rng.choice(protocols), width=100, height=50, feed=rng.choice([600, 1200.5, 90]),
            power=rng.choice([12000, 1, 800.4]), depth=rng.choice([1, 0.3, 2.55, 6]), step=rng.choice([0.5, 1, 0.07]),
        )  # fmt: skip
        if project['protocol'] in ('mach3', 'uccnc') or rng.random() < 0.3:
            project['dwell_units'] = rng.choice(['seconds', 'milliseconds'])
        if rng.random() < 0.2:
            project['plunge'] = rng.choice([30, 250.25, 1000])
        if rng.random() < 0.2:
            project['safe'] = rng.choice([0.1, 3, 12.345])
        if rng.random() < 0.5:
            project['mode'] = 'router'
            project['operation'] = 'drill'
            diameter = rng.choice([3.175, 3.175, 3, 6, 0.5, 10])
            if diameter != 3.175 or rng.random() < 0.5:
                project['tool_diameter'] = rng.choice(
                    [diameter] * 8 + [str(diameter)] + ([] if tidy else [diameter + 0.5])
                )
            project['paths'] = [hole(diameter, tidy) for _ in range(rng.randrange(1, 5))]
            if rng.random() < 0.6:
                depth = project['depth']
                pecks = [0, -0.0, 0.5, 1, 0.3, 0.1, 0.07, 0.01, depth, depth / 3, depth * 2, 50, '0.5', ' 1 ']
                project['drill_peck'] = rng.choice(pecks if tidy else pecks + [0.005, 1e-9, 51, -1, None, True])
            if rng.random() < 0.25:
                angles = [118, 60, 170, 90.0, '118']
                project['drill_point_angle'] = rng.choice(angles if tidy else angles + [59.9, 170.5, None, True])
            if rng.random() < 0.3:
                project['tool_shape'] = rng.choice(
                    ['drill', 'flat'] * (9 if tidy else 2) + ([] if tidy else ['ball', 'v', None, 1])
                )
            if rng.random() < 0.08:
                project['router_entry'] = rng.choice(['plunge'] if tidy else ['plunge', 'ramp', 'helix', None])
            if not tidy and rng.random() < 0.1:
                project[rng.choice(['path_depths', 'path_powers'])] = rng.choice([None, [], [[0, -1]], False])
            if rng.random() < 0.004:
                # The line limit: mostly a little over it, because a job a little under it is a very long program.
                peck, holes = rng.choice([(0.01, 20), (0.5, 995)] * 3 + [(0.01, 19), (0.5, 994)])
                project.update(depth=50, drill_peck=peck, paths=[[[20, 20], [20, 20]]] * holes)
        else:
            project['mode'] = 'knife'
            narrow = not tidy and rng.random() < 0.3  # close to the edges
            project['paths'] = [cut(-4, 104, tidy) if narrow else cut(25, 75, tidy) for _ in range(rng.randrange(1, 4))]
            if narrow:
                project['paths'] = [[[min(100, max(0, px)), py] for px, py in path] for path in project['paths']]
            offsets = [0.25, 0.01, 0.5, 1, 2, 0.3, '0.3', ' 1.5 ']
            offset = rng.choice(offsets if tidy else offsets + [5, 0.009, 5.01, 0, True, None, 'abc', 'nan', []])
            if rng.random() < 0.6:
                project['knife_offset'] = offset
            if rng.random() < 0.6:
                leads = [1, 2, 3.5, 5.5, '2', '6']
                lead = rng.choice(leads if tidy else leads + [0.25, 20, 20.5, 0.01, None, False, 'inf', {}])
                if tidy and float(lead) < float(project.get('knife_offset', 0.25)):
                    lead = float(offset)  # a lead equal to the offset is the shortest allowed
                project['knife_lead'] = lead
            elif tidy and float(project.get('knife_offset', 0.25)) > 1:
                project['knife_lead'] = rng.choice([2, 3.5])  # the default lead of 1 is shorter than this offset
            if rng.random() < 0.15:
                project['operation'] = rng.choice(['line'] if tidy else ['line', 'pocket', 'inside', 'drill', None])
            if not tidy and rng.random() < 0.1:
                project[rng.choice(['path_depths', 'tabs', 'router_entry', 'tool_shape', 'passes'])] = rng.choice(
                    [None, [[0, -1]], 'ramp', 'drill', False, {}, 2]
                )
        if rng.random() < 0.12:
            # An ordered job: holes and contours with one tool, or several knife cuts.
            shared = {k: v for k, v in project.items() if k not in ('paths', 'operation')}
            operations = []
            if project['mode'] == 'router' and tidy:
                shared['tool_shape'] = 'flat'  # the one cutter that may both drill and cut a line
            for _ in range(rng.randrange(1, 4)):
                operation = dict(paths=[[list(point) for point in path] for path in project['paths']])
                if project['mode'] == 'router':
                    operation['operation'] = rng.choice(['drill', 'drill', 'line'])
                    for key, choices, valid in (
                        ('tool_shape', ['drill', 'flat'], False), ('drill_point_angle', [118, 118.0, 90], False),
                        ('depth', [0.4, 1.5], True), ('drill_peck', [0, 0.2], True),
                        ('tool_diameter', [shared.get('tool_diameter', 3.175), 3], False),
                    ):  # fmt: skip
                        if (valid or not tidy) and rng.random() < 0.25:
                            operation[key] = rng.choice(choices)
                elif rng.random() < 0.3:
                    operation['knife_offset'] = rng.choice([0.25, 0.4])
                operations.append(operation)
            project = dict(shared, operations=operations)
        for _ in range(0 if tidy else rng.choice([0, 0, 1, 1, 2])):
            if rng.random() < 0.8:
                project[rng.choice(KEYS + extra * 2)] = rng.choice(WEIRD)
            else:
                project.pop(rng.choice(KEYS + extra), None)
        yield project


def rotary_projects():
    """Planar jobs of each kind the rotary post takes: router levels, depths along the paths, and laser."""

    def wave(count, x0, y0, width, height, phase=0.0):
        return [
            [x0 + width * i / (count - 1), y0 + height * (0.5 + 0.5 * math.sin(phase + i * 0.37))] for i in range(count)
        ]

    router = dict(
        mode='router', protocol='grbl', width=300, height=200, feed=900, plunge=250, power=12000, safe=5, depth=2,
        step=1, paths=[wave(40, 10, 10, 250, 60), wave(25, 20, 90, 200, 80, 1.3), [[5, 5], [5, 5.0625], [6.5, 5]]],
    )  # fmt: skip
    relief = dict(
        router, depth=3, step=1.5, paths=[wave(60, 10, 20 + 10 * row, 260, 6, row) for row in range(8)],
        path_depths=[[-3 * (0.5 + 0.5 * math.cos(0.21 * i + row)) for i in range(60)] for row in range(8)],
    )  # fmt: skip
    laser = {key: value for key, value in router.items() if key not in ('depth', 'step', 'plunge', 'safe')}
    laser.update(mode='laser', power=800, passes=1)
    leads = dict(laser, laser_lead_in=2, laser_lead_out=1.5, laser_ramp_in=3, laser_ramp_out=2, passes=2)
    return {'router': router, 'relief': relief, 'laser': laser, 'laser leads': leads}


def rotary_options(rng):
    """A rotary setup, mostly valid, with zero to two settings replaced by values the post must refuse."""
    options = dict(
        diameter=rng.choice([20, 50.8, 80, 300]), wrap_axis=rng.choice('XY'),
        postprocessor=rng.choice(['grblhal', 'xpro-v5', 'linuxcnc']), feed_reference=rng.choice(['surface', 'cut']),
        direction=rng.choice([1, -1]), angle_offset=rng.choice([0, 90, -45.5, 12.5]),
    )  # fmt: skip
    if rng.random() < 0.4:
        options['fixture'] = rng.choice(
            [dict(axial_min=-1, axial_max=400, min_radius=1), dict(axial_min=0, axial_max=60, min_radius=2)]
        )
    if options['postprocessor'] != 'xpro-v5' and rng.random() < 0.3:
        options['output_axis'] = rng.choice('ABC')
    changes = [
        ('diameter', [1, 2000, 0.5, 2001, '80', True, None, 37.5, 1e-3]),
        ('wrap_axis', ['X', 'Y', 'Z', None, 1, 'x']),
        ('output_axis', ['A', 'B', 'C', 'D', None, 3]),
        ('postprocessor', ['linuxcnc', 'grblhal', 'xpro-v5', 'grbl', None]),
        ('angle_offset', [0, -360000, 360001, 7.25, '1', 359.9999]),
        ('direction', [1, -1, 0, 0.5, 1.0, -1.0, True]),
        ('feed_reference', ['surface', 'cut', 'radius', None]),
        ('fixture', [None, 5, dict(axial_min=0, axial_max=0, min_radius=1), dict(axial_min=-5, axial_max=50, min_radius=60),
                     dict(axial_min=-2000, axial_max=2000, min_radius=0), dict(axial_min=10, axial_max=60, min_radius=2),
                     dict(axial_min=0)]),
    ]  # fmt: skip
    for _ in range(rng.choice([0, 0, 0, 1, 1, 2])):
        key, choices = rng.choice(changes)
        if rng.random() < 0.85:
            options[key] = rng.choice(choices)
        else:
            options.pop(key, None)
    return options


def rotary_cases(count, seed):
    """Projects with a rotary setup: every post, wrap axis, feed reference, direction and drive for each kind
    of planar job, then count generated projects of every kind above, each with a rotary setup that is
    sometimes refused."""
    rng = random.Random(seed)
    for project in rotary_projects().values():
        for wrap, reference, post, direction, drive in itertools.product(
            'XY', ('surface', 'cut'), ('xpro-v5', 'grblhal', 'linuxcnc'), (1, -1), 'ABC'
        ):
            options = dict(
                diameter=80, wrap_axis=wrap, postprocessor=post, feed_reference=reference, direction=direction,
                angle_offset=12.5, output_axis=drive, fixture=dict(axial_min=-1, axial_max=400, min_radius=5),
            )  # fmt: skip
            yield (dict(project, protocol='linuxcnc') if post == 'linuxcnc' else project), options
    sources = itertools.chain(
        generated_cases(count // 2, seed + 1), drill_knife_cases(count // 6, seed + 2),
        entry_tab_cases(count // 6, seed + 3), polygon_cases(count // 10, seed + 4),
        helix_pocket_cases(count // 30, seed + 5), vcarve_cases(count // 30, seed + 6),
    )  # fmt: skip
    for project in sources:
        options = rotary_options(rng)
        if isinstance(project, dict) and rng.random() < 0.7:
            # Mostly a source the post takes, so that most of these reach the post's own checks.
            if project.get('mode') == 'laser' and options.get('postprocessor') == 'linuxcnc':
                options['postprocessor'] = 'grblhal'
            stream = options.get('postprocessor') in ('grblhal', 'xpro-v5')
            project = dict(
                project, protocol=rng.choice(['grbl', 'grblhal', 'xpro-v5', 'fluidnc']) if stream else 'linuxcnc'
            )
        yield project, options


# Lines put into a planar program to test the rotary post's reading of given G-code: comments, duplicate and
# lowercase words, blank and oversized lines, unusual numbers and separators, and commands it does not take.
ODD_LINES = [
    '(comment)',
    'G1 X1 X2',
    'g1 x1',
    '',
    '   ',
    'G1 X1;',
    'M3 S1000',
    'M4 S100',
    'G4 P1',
    'G4 P0.5',
    'G0 X1e3',
    'G1 X+1.5',
    'G1 X.5',
    'G1 X1.',
    'G1 X-0',
    'M30',
    'G93',
    'G20',
    'G91',
    'G2 X1 Y1 I1 J0',
    'T1 M6',
    'G1 A10',
    'G1 X1 F0',
    'G1 F-1',
    'M8',
    'S1000',
    'G0 Z5',
    'G1 Z-1 F100',
    '%',
    'N10 G1 X1',
    'G1 X1\tY2',
    'G1 X1\x0bY2',
    '\x1cG1 X1',
    'G1 X1\u00a0Y2',
    'X' * 300,
    'G1 X' + '1' * 250,
    'G0 X5 Y5',
    'G1 X5 Y5 F300',
    'G1 X10 Y10 Z-0.5 F300',
    'G1 X10 Y10 S500 F300',
    'M5',
    'G21',
    'G90',
    'G94',
    'G17',
    'G54',
    'G40',
    'G49',
    'G80',
    'G61',
    'G0 Z-3',
    'G1 X0 Y0 F100000',
    'G1 X-3000 Y0 F100',
    'G1 X1 Y1 F0.0001',
    'M3 S0',
    'M3 S200000',
    'M4 S0',
    'M3',
    'G0',
    'G1',
    'G4',
    'G4 P-1',
    'G4 P100000',
    'G1 X1 Y2 Z3 F4 S5',
    'G0 X1 Y2 Z3 F4',
    'G1 Y400 F100',
    'G1 X0.0005 F100',
    'G1 X999999999 F1',
    'G1 X1000000001 F1',
    'G00 X1',
    'G01 X1 F10',
    'M05',
    'M03 S100',
    'G1 X1 F10 S-1',
]


def rotary_wrap_cases(count, seed, generate):
    """Planar jobs given to the rotary post as G-code, as generated or with lines added, removed, swapped
    or changed, or with the job itself replaced, so that each check of the given program is compared."""
    rng = random.Random(seed)
    jobs = [
        {'mode': 'router', 'lines': ['G21', 'G90', 'G94', 'M3 S1000', 'G0 X0 Y0', 'G1 Z-1 F100', 'G1 X10 Y5 F500',
                                     'G1 X10 Y30 Z-2 F500', 'M5']},
        {'mode': 'laser', 'lines': ['G21', 'G90', 'G94', 'M5', 'G0 X0 Y0', 'M4 S100', 'G1 X10 Y5 F500 S300',
                                    'G1 X10 Y30 F500 S0', 'M5']},
    ]  # fmt: skip
    sources = itertools.chain(
        rotary_projects().values(), generated_cases(count // 3, seed + 1), drill_knife_cases(count // 10, seed + 2),
        entry_tab_cases(count // 10, seed + 3),
    )  # fmt: skip
    for project in sources:
        try:
            job = generate(project)
        except Exception:  # noqa: BLE001 - only programs are wanted here
            continue
        if isinstance(job['segments'], list):
            jobs.append(job)

    def changed(job):
        job = json.loads(json.dumps(job))
        lines = job['lines']
        kind = rng.randrange(11)
        if kind < 3 and lines:
            for _ in range(rng.randrange(1, 4)):
                lines.insert(rng.randrange(len(lines) + 1), rng.choice(ODD_LINES))
        elif kind == 3 and len(lines) > 1:
            del lines[rng.randrange(len(lines))]
        elif kind == 4 and len(lines) > 2:
            i, j = rng.sample(range(len(lines)), 2)
            lines[i], lines[j] = lines[j], lines[i]
        elif kind == 5 and lines:
            i = rng.randrange(len(lines))
            words = lines[i].split()
            if len(words) > 1:
                k = rng.randrange(1, len(words))
                words[k] = words[k][0] + rng.choice(['0', '-0', '1e2', '12.3456789', '-999', '.25', '1.', '+3'])
                lines[i] = ' '.join(words)
        elif kind == 6 and lines:
            lines.pop()
        elif kind == 7:
            job['lines'] = rng.choice([[], 'G1 X1', None, lines + [5], [None] + lines, ['M5'], ['M30'], ['M5', 'M30']])
        elif kind == 8:
            job['mode'] = rng.choice(['knife', None, 'laser', 'router', 1])
        elif kind == 9:
            job = rng.choice([[], 'job', None, {key: value for key, value in job.items() if key != 'mode'}])
        elif lines:
            i = rng.randrange(len(lines))
            lines[i] = '  ' + lines[i] + rng.choice([' ', '\t', '\r', '\x0c'])
        return job

    for _ in range(count):
        job, options = rng.choice(jobs), rotary_options(rng)
        if rng.random() < 0.7:
            # Mostly a post that takes the program: LinuxCNC for its own setup commands, and a stream post for laser.
            own = any(line in ('G40', 'G49', 'G80', 'G61') for line in job['lines'])
            if job['mode'] == 'laser' or not own:
                options['postprocessor'] = rng.choice(['grblhal', 'xpro-v5'])
                options.pop('output_axis', None)
            else:
                options['postprocessor'] = 'linuxcnc'
        yield (job if rng.random() < 0.3 else changed(job)), options


def relief_cases(count, seed):
    """Reliefs from height fields given as numbers and as image bytes, of every size up to 40 points a side,
    with settings from valid to invalid, and base64 text the reader must refuse."""
    rng = random.Random(seed)
    weird = [
        None, True, '5', -1, 0, 1e9, 2000, 0.0000001, 'ball', [], [[0, 1], [1]], [[0, 2], [1, 0]], 'AAAA', 'AA=A',
        '!!!!', 'AAAA====', [3, 2], [2.0, 2], [1025, 2], [True, 2], 'v', 10001,
    ]  # fmt: skip
    keys = [
        'width', 'height', 'depth', 'tool_diameter', 'stepover', 'allowance', 'tool_shape', 'x', 'y', 'heightmap',
        'heightmap_rgb', 'heightmap_size',
    ]  # fmt: skip
    for _ in range(count):
        rows, columns = rng.randint(2, 40), rng.randint(2, 40)
        data = dict(
            width=rng.choice([10, 25.5, 60, 100]), height=rng.choice([8, 20, 50.25]), depth=rng.choice([1, 2.5, 6]),
            tool_diameter=rng.choice([0.5, 1, 3.175, 6]), stepover=rng.choice([0.3, 0.5, 1, 2.5]),
            allowance=rng.choice([0, 0.1, 0.5]), tool_shape=rng.choice(['ball', 'flat', 'ball']),
            x=rng.choice([0, 5, 12.5]), y=rng.choice([0, 3]),
        )  # fmt: skip
        if rng.random() < 0.5:
            data['heightmap'] = [
                [rng.choice([0, 1, 0.5, rng.random(), round(rng.random(), 3)]) for _ in range(columns)]
                for _ in range(rows)
            ]
        else:
            raw = bytes(rng.randrange(256) for _ in range(rows * columns * 3))
            data['heightmap_rgb'] = base64.b64encode(raw).decode()
            data['heightmap_size'] = [columns, rows]
        for _ in range(rng.choice([0, 0, 0, 1, 2])):
            key = rng.choice(keys)
            if rng.random() < 0.8:
                data[key] = rng.choice(weird)
            else:
                data.pop(key, None)
        yield data


def stock_cases(count, seed, generate):
    """Stock previews of the segments of generated jobs, for every cutter, with settings and rows from valid to
    invalid."""
    rng = random.Random(seed)
    weird = [
        None, True, '20', 'x', -5, 1e9, 15.9, 96.9, [[1, 2, 3]], [[0, 0, 1, 1, '-0.5']], [[0, 0, 1, 1, None]],
        [[0, 0, 1, 1, -0.5, None, '-1.5']], [[0, 0, 1, 1, -50]], [[1e308, 0, 1e308, 0, 0]], 'drill', [],
    ]  # fmt: skip
    made = 0
    for project in itertools.chain(generated_cases(count * 3, seed + 1), drill_knife_cases(count, seed + 2)):
        if made >= count:
            break
        try:
            job = generate(project)
        except Exception:  # noqa: BLE001, S112 - only programs are wanted here
            continue
        made += 1
        data = dict(
            segments=job['segments'], width=rng.choice([100, 50, 300]), height=rng.choice([50, 80]),
            resolution=rng.choice([16, 24, 48, 96]), tool_diameter=rng.choice([1, 3.175, 6]),
            tool_shape=rng.choice(['flat', 'ball', 'v', 'drill']), v_angle=rng.choice([30, 60, 90.5]),
            stock_thickness=rng.choice([10, 1, 0.5]), x=rng.choice([0, 2]), y=0,
        )  # fmt: skip
        for _ in range(rng.choice([0, 0, 1])):
            data[rng.choice(['width', 'resolution', 'tool_shape', 'stock_thickness', 'segments', 'v_angle'])] = (
                rng.choice(weird)
            )
        yield data


def tool_change_cases(count, seed):
    """Router projects for manual tool-change plans: runs of operations with the same tool and changes between
    them, for lines, pockets, drilling and V-carving, with tool numbers, names, diameters, shapes and angles from
    valid to invalid, operations that disagree with the project, and projects the plan refuses."""
    rng = random.Random(seed)
    names = [
        'Tool 1', ' spaced ', '\u00a0no-break\u3000', 'x' * 80, 'x' * 81, '', '   ', 'tab\there', 'caf\u00e9',
        '\u2028', '\u0085end', 'end\u180e', None, 5, '3 mm flat', 'del\x7f',
    ]  # fmt: skip
    weird = [None, True, False, 0, 1, 1.0, 1.5, -3, 999, 1000, '3', [], {}, 3, 3.0, 2**40, 0.005, 100.0, 170, 9.99]

    def square(x, y, size):
        return [[x, y], [x + size, y], [x + size, y + size], [x, y + size], [x, y]]

    def operation():
        x, y = round(rng.uniform(10, 60), rng.randrange(0, 3)), round(rng.uniform(10, 60), rng.randrange(0, 3))
        kind = rng.choice(['line', 'line', 'pocket', 'inside', 'drill', 'vcarve', None])
        op = {}
        if kind == 'line' or kind is None:
            op['paths'] = [[[x, y], [x + rng.uniform(1, 20), y + rng.uniform(-5, 5)]]]
        elif kind == 'drill':
            op['paths'] = [[[x, y], [x, y]] for _ in range(rng.choice([1, 2]))]
            op['tool_shape'] = rng.choice(['drill', 'drill', 'flat'])
        else:
            op['paths'] = [square(x, y, rng.choice([8, 12, 20]))]
        if kind is not None:
            op['operation'] = kind
        if rng.random() < 0.93:
            op['tool_id'] = rng.choice([1, 1, 2, 2, 3, 1.0, 2.0, 12])
        if rng.random() < 0.06:
            op['tool_id'] = rng.choice(weird)
        if rng.random() < 0.6:
            op['tool_name'] = rng.choice(names) if rng.random() < 0.3 else f"Tool {op.get('tool_id')}"
        if rng.random() < 0.85:
            op['tool_diameter'] = rng.choice([3, 3.0, 1, 2.5, 6, 3.175])
        if rng.random() < 0.06:
            op['tool_diameter'] = rng.choice(weird)
        if rng.random() < 0.2:
            op['tool_shape'] = rng.choice(['flat', 'ball', 'v', 'drill', 'bull', None, 3])
        if rng.random() < 0.15:
            op['v_angle'] = rng.choice([60, 90, 45.5, 5, None, '60'])
        if rng.random() < 0.15:
            op['drill_point_angle'] = rng.choice([118, 135, 59, None])
        if rng.random() < 0.05:
            key = rng.choice(['mode', 'protocol', 'width', 'height', 'dwell_units'])
            op[key] = rng.choice(['router', 'laser', 'grbl', 100, 100.0, 120, True, None, 'ms'])
        if rng.random() < 0.02:
            op['operations'] = []
        return op

    for _ in range(count):
        project = dict(
            mode='router', protocol=rng.choice(['grbl', 'grbl', 'marlin', 'linuxcnc']), width=100, height=100,
            feed=600, power=1000, safe=5, depth=1, step=0.5, plunge=100, operation='line',
            operations=[operation() for _ in range(rng.choice([1, 1, 2, 3, 4, 6]))],
        )  # fmt: skip
        roll = rng.random()
        if roll < 0.04:
            project['mode'] = rng.choice(['laser', None, 'knife'])
        elif roll < 0.07:
            project['rotary'] = rng.choice([True, {}, [], 0, 0.0, 'x', '', {'a': 1}, None])
        elif roll < 0.11:
            project['operations'] = rng.choice([None, [], 'x', [1], [None]])
        elif roll < 0.15:
            del project['operations']
            project.update(operation())
        elif roll < 0.18:
            project['dwell_units'] = rng.choice(['ms', 's', None])
        yield project


def fixture_cases(count, seed, generate):
    """Fixture clearance checks of generated programs against generated boxes, with lines added or changed from the
    commands Studio posts to ones the check must refuse, and setups from valid to invalid."""
    rng = random.Random(seed)
    programs = []
    while len(programs) < 40:
        paths = [
            [[rng.uniform(5, 90), rng.uniform(5, 70)] for _ in range(rng.randint(2, 6))] for _ in range(rng.randint(1, 4))
        ]  # fmt: skip
        try:
            programs.append(
                generate(
                    dict(
                        mode='router',
                        protocol=rng.choice(['grbl', 'linuxcnc', 'marlin', 'mach3']),
                        width=100,
                        height=80,
                        power=8000,
                        depth=rng.choice([1, 2.5]),
                        feed=500,
                        step=1,
                        safe=rng.choice([4, 5, 10]),
                        dwell_units='seconds',
                        paths=paths,
                    )  # fmt: skip
                )['lines']
            )
        except ValueError:
            continue
    names = ['Clamp', ' spaced ', '\u00a0', 'x' * 80, 'x' * 81, '', 5, None, 'caf\u00e9', 'Vice jaw', '\u3000jaw']
    odd = [
        'G0 X1e3', 'G0 x10', 'g1 x5 y5 f300', 'G0 X1.', 'G0 X.5', 'G0 X-', 'G1 X10 X11', 'G4', 'G4 P1', 'M3 S1000',
        'M6 T2', 'G0', 'G0 F100', 'G2 X1', '  G0   Y20  ', 'G0\tX3', 'G0 X2000000', 'G0 X+5', 'G0 X-0', 'M30', '', 7,
        'G0 X5\x1fY3', 'T1', 'G0 X1 Y2 Z3 F4', 'G0 X' + '9' * 300, 'G0 Z\u00b2',
    ]  # fmt: skip
    for _ in range(count):
        lines = list(rng.choice(programs))
        for _ in range(rng.choice([0, 0, 0, 1, 2])):
            k = rng.randrange(len(lines) + 1)
            if rng.random() < 0.5:
                lines.insert(k, rng.choice(odd))
            elif lines:
                lines[min(k, len(lines) - 1)] = rng.choice(odd)
        if rng.random() < 0.08:
            lines = rng.choice([[], 'x', ['G21'], ['G21', 'G90'], ['G0 X1'], ['G21', 'G90', 'G0 X5']])
        fixtures = []
        for _ in range(rng.choice([1, 1, 2, 3, 5])):
            x, y, z = rng.uniform(0, 90), rng.uniform(0, 70), rng.choice([-3, 0, 2, 5, 7])
            fixture = {
                'name': rng.choice(names) if rng.random() < 0.15 else f'Clamp {len(fixtures)}',
                'min': [x, y, z],
                'max': [x + rng.choice([1, 2, 5, 20]), y + rng.choice([1, 3, 10]), z + rng.choice([1, 3, 6])],
            }
            if rng.random() < 0.05:
                fixture['min'] = rng.choice([[0, 0], [0, None, 0], [0, True, 0], 'x', [0, 0, 2e6]])
            if rng.random() < 0.03:
                fixture['max'] = list(fixture['min'])
            fixtures.append(fixture)
        tool = dict(
            diameter=rng.choice([2, 3.175, 6, 0.5]), stickout=rng.choice([4, 10, 20]),
            holder_diameter=rng.choice([12, 20, 35]), holder_length=rng.choice([15, 30]),
        )  # fmt: skip
        if rng.random() < 0.05:
            tool[rng.choice(list(tool))] = rng.choice([0, None, True, 3000, '3'])
        payload = {
            'start': rng.choice([[0, 0, 4], [0, 0, 10], [50, 40, 2], [0.0, -0.0, 5]]),
            'lines': lines,
            'tool': tool,
            'fixtures': fixtures,
        }
        if rng.random() < 0.04:
            payload[rng.choice(['start', 'tool', 'fixtures', 'lines'])] = rng.choice([None, [], {}, 'x', [1, 2]])
        yield payload


def stl_cases(count, seed):
    """STL meshes for relief height fields, binary and ASCII, of 1 to 60 triangles (some degenerate or flat), with
    the spellings, spacing and line breaks ASCII files have, at every resolution, damaged in the ways the reader
    must refuse."""
    rng = random.Random(seed)

    def triangle():
        base = [rng.uniform(-50, 50), rng.uniform(-50, 50), rng.uniform(-5, 5)]
        size = rng.choice([0.5, 3, 10, 40])
        points = [[base[i] + rng.uniform(-size, size) for i in range(3)] for _ in range(3)]
        if rng.random() < 0.05:
            points[1] = list(points[0])
        if rng.random() < 0.05:
            points = [[p[0], p[1], 0.0] for p in points]
        return points

    def written(value):
        return rng.choice([repr(value), f'{value:.6e}', f'{value:.4f}', f'{value:E}'])

    def ascii_stl(triangles):
        gap = lambda: rng.choice([' ', '  ', '\t', ' \t '])  # noqa: E731
        lines = [rng.choice(['solid', 'solid part', 'SOLID x', 'Solid'])]
        for points in triangles:
            lines.append(f"{rng.choice(['facet', 'FACET', 'Facet'])}{gap()}normal{gap()}0 0 1")
            lines.append(f'{gap()}outer{gap()}loop')
            lines += [f'{gap()}vertex{gap()}' + gap().join(written(v) for v in p) for p in points]
            lines += [f'{gap()}endloop', 'endfacet']
        lines.append(rng.choice(['endsolid', 'endsolid part', 'ENDSOLID']))
        text = rng.choice(['\n', '\r\n', '\r', '\n\n', '\x0c']).join(lines)
        damage = rng.random()
        for limit, before, after in (
            (0.03, 'endloop', 'endlop'), (0.05, 'vertex', 'vertex1e'), (0.07, 'solid', 'soli'),
            (0.09, 'endsolid', 'endsolid\nextra'), (0.11, '0 0 1', '0 0 1e400'), (0.13, 'endfacet', 'endfacetX'),
        ):  # fmt: skip
            if damage < limit:
                text = text.replace(before, after, 1)
                break
        else:
            if damage < 0.15:
                text = '  ' + text + '\n\n'
        return text.encode('ascii')

    def binary_stl(triangles):
        raw = bytearray(b'solid binary header'.ljust(80, b' ')) + struct.pack('<I', len(triangles))
        for points in triangles:
            raw += struct.pack('<3f', 0, 0, 1) + b''.join(struct.pack('<3f', *p) for p in points) + b'\0\0'
        damage = rng.random()
        if damage < 0.03:
            raw = raw[:-1]
        elif damage < 0.05:
            raw[96:100] = struct.pack('<f', float('inf'))
        elif damage < 0.06:
            raw = raw[:80] + struct.pack('<I', 0)
        return bytes(raw)

    for _ in range(count):
        triangles = [triangle() for _ in range(rng.choice([1, 2, 5, 20, 60]))]
        raw = ascii_stl(triangles) if rng.random() < 0.5 else binary_stl(triangles)
        data = {'content_base64': base64.b64encode(raw).decode(), 'resolution': rng.choice([2, 3, 8, 16, 33, 64, 128])}
        roll = rng.random()
        if roll < 0.03:
            data['resolution'] = rng.choice([1, 129, 64.0, True, None, '64'])
        elif roll < 0.05:
            del data['resolution']
        elif roll < 0.07:
            data['content_base64'] = rng.choice(['', '!!!!', 'AAAA=', None, 'QUFB'])
        yield data


def strict_equal(a, b):
    """Equality that tells 0 from 0.0 and True from 1, and compares floats by value and sign."""
    if type(a) is not type(b):
        return False
    if isinstance(a, list):
        return len(a) == len(b) and all(strict_equal(x, y) for x, y in zip(a, b, strict=True))
    if isinstance(a, dict):
        return a.keys() == b.keys() and all(strict_equal(a[key], b[key]) for key in a)
    if isinstance(a, float):
        return a.hex() == b.hex()
    return a == b


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cli', required=True, type=Path, help='the kerfkite-cam executable')
    parser.add_argument('--numbers', type=int, default=300000, help='number-text comparisons')
    parser.add_argument('--generated', type=int, default=12000, help='generated edge cases')
    parser.add_argument('--entries', type=int, default=6000, help='generated ramp, helix and holding-tab cases')
    parser.add_argument('--lines', type=int, default=20000, help='lines for the self-crossing test')
    parser.add_argument('--helix', type=int, default=5000, help='generated pockets entered by a helix')
    parser.add_argument('--vcarve', type=int, default=4000, help='generated V-carves')
    parser.add_argument(
        '--rotary', type=int, default=3000, help='generated rotary jobs, and half as many given programs'
    )
    parser.add_argument('--relief', type=int, default=1500, help='generated reliefs, and as many stock previews')
    parser.add_argument('--tool-changes', type=int, default=1500, help='generated plans for manual tool changes')
    parser.add_argument('--fixtures', type=int, default=1500, help='generated fixture clearance checks')
    parser.add_argument('--stl', type=int, default=1000, help='generated STL meshes for relief height fields')
    parser.add_argument('--skip-suite', action='store_true', help='do not replay the calls made by the test suite')
    parser.add_argument(
        '--without-polygon-operations',
        action='store_true',
        help='leave out offsets, pockets, fills and V-carves (for a reference without the compiled extension)',
    )
    args = parser.parse_args()
    if not args.cli.is_file():
        raise SystemExit(f'kerfkite-cam not found: {args.cli}')

    compared, unwritten = number_text(args.cli, args.numbers, 86)
    print(f'PASS number text: {compared} values match Python ({unwritten} beyond the exact range, none job-sized)')
    if sys.version_info >= (3, 12):
        print(
            f'PASS arithmetic: {arithmetic(args.cli, args.numbers, 88)} distances and sums match Python to the last bit'
        )
    else:
        # Python 3.10 and 3.11 computed math.dist and sum() differently in the last bit; the core follows 3.12.
        print('NOTE arithmetic not compared: it follows Python 3.12 and later, and this is an earlier Python')

    compared, found = self_crossing(args.cli, args.lines, 90)
    declined = ', '.join(f'{found["declined", method]} by {method}' for method in ('usual', 'tree', 'pairs'))
    print(
        f'PASS self-crossing: {compared} lines answered as shapely\'s is_simple answers them ({found["simple"]} simple,'
        f' {found["crossing"]} not; {found["tree"]} through the tree; declined: {declined}), and {found["long"]} long'
        ' lines within the bound on work'
    )

    from native_core_recorder import preview_reference, reference, rotary_reference, where_computed_left_out
    from studio.cam import generate
    from studio.rotary import generate_rotary, wrap_job
    from studio.operations import kernel_available, kernel_vcarve_available

    os.environ.pop('KERFKITE_POLYGON_BACKEND', None)  # the reference as Studio runs it
    os.environ.pop('KERFKITE_SOLID_BACKEND', None)
    shared = kernel_available()
    if shared and not kernel_vcarve_available():
        raise SystemExit(
            'The compiled extension (studio._relief_native) was built before the core had V-carving, so the'
            ' reference would carve with Shapely, whose paths differ from the kernel\'s. Build it again'
            ' (python setup.py build_ext --inplace).'
        )
    if shared:
        print(
            'NOTE the reference computes offsets, pockets, fills and V-carves with the core\'s polygon kernel,'
            ' through the compiled extension: for those jobs the operation is shared, and what surrounds it is'
            ' compared'
        )
    elif args.without_polygon_operations:
        print(
            'NOTE the compiled extension is not installed, so the reference would compute offsets, pockets, fills'
            ' and V-carves with Shapely: jobs with those operations are NOT COMPARED in this run'
        )
    else:
        raise SystemExit(
            'The compiled extension (studio._relief_native) is not installed, so offsets, pockets, fills and'
            ' V-carves cannot be compared: the reference would compute them with Shapely, whose vertices differ from the'
            ' kernel\'s. Build it (python setup.py build_ext --inplace), or pass --without-polygon-operations'
            ' to compare everything else.'
        )

    with tempfile.TemporaryDirectory() as scratch:
        cases = Path(scratch) / 'cases.jsonl'
        lines = {}
        if not args.skip_suite:
            recorded = Path(scratch) / 'suite.jsonl'
            environment = dict(os.environ, KERFKITE_CORE_CASES=str(recorded))
            environment['PYTHONPATH'] = os.pathsep.join(
                [str(Path(__file__).resolve().parent), environment.get('PYTHONPATH', '')]
            )
            run = subprocess.run(
                [sys.executable, '-m', 'pytest', '-q', '-p', 'no:cacheprovider', '-p', 'native_core_recorder', 'tests'],
                cwd=ROOT, env=environment, capture_output=True, text=True,
            )  # fmt: skip
            if run.returncode and not recorded.is_file():
                raise SystemExit(
                    'The test suite could not be run for recording:\n' + run.stdout[-2000:] + run.stderr[-2000:]
                )
            if run.returncode:
                # A failing test is another check's concern; the calls it made before failing are still valid cases.
                print('NOTE some tests failed while recording:', run.stdout.strip().splitlines()[-1])
            for line in recorded.read_text(encoding='utf-8').splitlines():
                lines.setdefault(line, 'suite')
        suite = len(lines)
        for project in itertools.chain(generated_cases(args.generated, 87), drill_knife_cases(args.generated // 3, 89)):
            line, _ = reference(generate, project)
            if line is not None:
                lines.setdefault(line, 'generated')
        for project in polygon_cases(args.generated // 3, 89):
            line, _ = reference(generate, project)
            if line is not None:
                lines.setdefault(line, 'generated')
        for project in entry_tab_cases(args.entries, 89):
            line, _ = reference(generate, project)
            if line is not None:
                lines.setdefault(line, 'entry/tabs')
        for project, options in rotary_cases(args.rotary, 95):
            line, _ = rotary_reference(generate_rotary, project, options, 'project')
            if line is not None:
                lines.setdefault(line, 'rotary')
        for job, options in rotary_wrap_cases(args.rotary // 2, 97, generate):
            line, _ = rotary_reference(wrap_job, job, options, 'job')
            if line is not None:
                lines.setdefault(line, 'rotary-wrap')
        for data in relief_cases(args.relief, 98):
            line = preview_reference('relief', data)
            if line is not None:
                lines.setdefault(line, 'relief')
        for data in stock_cases(args.relief, 99, generate):
            line = preview_reference('stock', data)
            if line is not None:
                lines.setdefault(line, 'stock')
        for project in tool_change_cases(args.tool_changes, 100):
            line = preview_reference('tool_changes', project)
            if line is not None:
                lines.setdefault(line, 'tool-changes')
        for payload in fixture_cases(args.fixtures, 101, generate):
            line = preview_reference('fixtures', payload)
            if line is not None:
                lines.setdefault(line, 'fixtures')
        for data in stl_cases(args.stl, 102):
            line = preview_reference('stl', data)
            if line is not None:
                lines.setdefault(line, 'stl')
        tally = collections.Counter()
        declined = collections.Counter()
        failures = []

        def compare(lines):
            """Put a set of cases to the native core and count how each was answered."""
            cases.write_text(''.join(line + '\n' for line in lines), encoding='utf-8')
            answers = subprocess.run(
                [str(args.cli), 'batch', str(cases)], capture_output=True, text=True, encoding='utf-8', check=True
            ).stdout.splitlines()
            assert len(answers) == len(lines), 'The native core did not answer every case.'
            for (line, origin), answer in zip(lines.items(), answers, strict=True):
                case, got = json.loads(line), json.loads(answer)
                expect = case['expect']
                subject = case.get(
                    'project', case.get('job', case.get('tool_changes', case.get('fixtures', case.get('stl'))))
                )
                if 'relief' in case and 'ok' in got:
                    got = {'ok': where_computed_left_out(got['ok'])}
                if not shared and 'project' in case and uses_polygon_operation(subject):
                    tally[origin, 'left out'] += 1
                elif 'declined' in got:
                    tally[origin, 'declined'] += 1
                    declined[got['declined']] += 1
                elif 'raises' in expect:
                    # The reference stopped with something other than a rejection; there is no behaviour to match.
                    tally[origin, 'reference raised ' + expect['raises']] += 1
                    if 'invalid' not in got and 'error' not in got:
                        failures.append((subject, expect, got))
                elif strict_equal(expect, got):
                    tally[origin, 'program' if 'ok' in got else 'rejection'] += 1
                else:
                    failures.append((subject, expect, got))

        compare(lines)
        # A helix into a pocket is a long program, and so is a V-carve, so these are compared a few hundred at
        # a time and not kept.
        seen = set()
        for origin, group in (
            ('helix', helix_pocket_cases(args.helix, 91)),
            ('v-carve', vcarve_cases(args.vcarve, 93)),
        ):
            while True:
                batch = list(itertools.islice(group, 250))
                if not batch:
                    break
                lines = {}
                for project in batch:
                    line, _ = reference(generate, project)
                    # A digest that is the same from run to run stands for the line, so the lines are not kept.
                    mark = None if line is None else hashlib.sha256(line.encode()).digest()
                    if mark is not None and mark not in seen:
                        seen.add(mark)
                        lines.setdefault(line, origin)
                if lines:
                    compare(lines)

    origins = ('suite', 'generated', 'entry/tabs', 'helix', 'v-carve', 'rotary', 'rotary-wrap', 'relief', 'stock')
    for origin in (*origins, 'tool-changes', 'fixtures', 'stl'):
        total = sum(n for (o, _), n in tally.items() if o == origin)
        if not total:
            continue
        same = tally[origin, 'program'] + tally[origin, 'rejection']
        print(
            f'{origin:12s} {total:5d} cases: {tally[origin, "program"]} programs and {tally[origin, "rejection"]} rejections'
            f' reproduced ({same * 100 / total:.1f}%), {tally[origin, "declined"]} declined as not ported'
            + (f', {tally[origin, "left out"]} with polygon operations NOT COMPARED' if not shared else '')
        )
    if declined:
        print('Not ported yet: ' + ', '.join(f'{name} ({n})' for name, n in declined.most_common()))
    if failures:
        for project, expect, got in failures[:5]:
            brief = lambda value: json.dumps(value)[:300]
            print('DIFFERENT\n  project', brief(project), '\n  python ', brief(expect), '\n  native ', brief(got))
        raise SystemExit(f'{len(failures)} cases differ from the reference.')
    if not tally['generated', 'program'] or (suite and not tally['suite', 'program']):
        raise SystemExit('No program was reproduced: the comparison did not exercise the native core.')
    if args.rotary and not (tally['rotary', 'program'] and tally['rotary-wrap', 'program']):
        raise SystemExit('No rotary program was reproduced: the comparison did not exercise the rotary post.')
    if args.relief and not (tally['relief', 'program'] and tally['stock', 'program']):
        raise SystemExit('No relief or stock preview was reproduced: the comparison did not exercise them.')
    if args.tool_changes and not tally['tool-changes', 'program']:
        raise SystemExit('No tool-change plan was reproduced: the comparison did not exercise the planner.')
    if args.fixtures and not tally['fixtures', 'program']:
        raise SystemExit('No fixture check was reproduced: the comparison did not exercise it.')
    if args.stl and not tally['stl', 'program']:
        raise SystemExit('No STL height field was reproduced: the comparison did not exercise the reader.')
    if not shared:
        print(
            'PASS for the cases compared; offsets, pockets, fills and V-carves were NOT COMPARED (no compiled extension).'
        )
        return
    print('PASS the native core reproduces or declines every case; none differs from the Python reference.')


if __name__ == '__main__':
    main()
