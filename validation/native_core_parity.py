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
   A given program with text that is not ASCII is defined by the core: it refuses it, where the reference read
   digits and spaces of other scripts and copied them into the program; such cases are counted apart.
5. Reliefs and stock previews: studio.relief.prepare_relief for height fields given as numbers and as image
   bytes (relief_cases), compared with the Python reference given the core's limits, and studio.simulation's
   simulate for the segments of generated jobs (stock_cases). Where the contact ran (CPU or GPU) is left out.
6. Plans for manual tool changes: studio.tool_changes.plan_tool_changes for router projects of operations with
   the same and with different tools, valid and not (tool_change_cases). Each stage is a whole job, compared
   with the job the Python CAM generates for it.
7. Fixture clearance: studio.fixture_collision.check_fixtures for generated programs, with lines added or changed,
   swept against generated boxes (fixture_cases), lines that are not ASCII among them.
8. STL height fields: studio.mesh_import.stl_heightmap for binary and ASCII meshes, at every resolution, damaged
   in the ways the reader must refuse (stl_cases).
9. Solids and CAD scripts: parametric solid histories and their sections (solid_cad_cases), and CAD scripts for
   the script preview (cad_script_cases), both through the compiled extension's kernel.
10. Foreign drawings: studio.program_import's import_program for posted G-code, HP-GL plotter files and LinuxCNC
   INI files, sound, damaged and refused, with every filename suffix and unit, and text that is not ASCII
   (program_import_cases). DXF drawings, which the core reads with its own reader, are left out
   here and compared to the tolerance by validation/dxf_import_check.py. And studio.pdf_import's import_pdf for
   PDF pages written as producers write them (pdf_import_cases): paths, fills and their rules, clipping, forms,
   graphics states, optional content, text and images, page trees, boxes, units and rotations, every filter,
   cross-reference tables and streams, and damaged operators, documents and payloads. And
   studio.import_bundle's inspect_bundle for LightBurn bundles (bundle_import_cases): ZIP archives listed and
   read member by member, sound, unusual and damaged, ZIP64 records and fields and Info-ZIP Unicode paths
   included. The core reads archives as Python 3.13's zipfile does; with an earlier Python, an archive with a
   ZIP64 locator whose answer differs is counted as defined by the core, as is a member header before the start
   of the data, which the core reads as damage where zipfile raises "negative seek value".

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
import io
import itertools
import json
import math
import os
from pathlib import Path
import random
import re
import struct
import subprocess
import sys
import tempfile
import zipfile

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
# The core's refusal of a rotary source that is not ASCII (native/core/src/rotary.cpp).
NOT_ASCII_ROTARY = 'Rotary source G-code must be ASCII text'
BUNDLE_UNREADABLE = 'Cannot read the bundle or selected text member'


def bundle_defined(subject, expect, got):
    """Whether a bundle answer that differs from the reference is the core's own: a member header before the start
    of the data (zipfile's ValueError from the seek) read as damage, and, with a Python before 3.13, an archive
    with a ZIP64 locator, which those read with other rules."""
    if not isinstance(subject, dict) or not isinstance(subject.get('content'), str):
        return False
    if got.get('error') == BUNDLE_UNREADABLE and str(expect.get('error', '')).startswith('negative seek value'):
        return True
    if sys.version_info >= (3, 13):
        return False
    try:
        return b'PK\x06\x07' in base64.b64decode(subject['content'], validate=True)
    except ValueError:
        return False


ODD_LINES = [
    # Text that is not ASCII: the core refuses it in a rotary source, which the reference copied into the program.
    'G1 X\u0661',
    'M3 S\u0661\u0660\u0660\u0660',
    'G4 P\u0665',
    'G1\u00a0X1',
    'M5\u00a0',
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
            if rng.random() < 0.04:
                # Whole groups and then =, which some Python releases read and others refuse: the core declines it.
                data['heightmap_rgb'] += '=' * rng.randint(1, 4)
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
        # Read as the reference's str methods read them: other digits and spaces, letters str.upper() makes ASCII
        # or longer, and lines counted in code points.
        'G0 X\u0661\u0662', 'g0\u00a0x5', 'M3 \u017f1000', '\uff270 X1', 'G0 X1 \u00df', 'G0 X\u00e9',
        'G0 X1 ' + '\u00e9' * 200, 'G0 X1 ' + '\u00e9' * 260, 'G1\u3000X5 F\u0665\u0660\u0660', '\u0131',
        'G0 X\uff15', '\u2028', 'G0 Y\u0663\u2009',
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


def solid_cad_cases(count, seed):
    """Parametric solid histories of boxes, cylinders and spheres: named parameters with expressions (some
    circular, unknown, dividing by zero or too long), rotations and origins, linear and polar patterns, runs of
    additions, cuts and intersections, disabled features and the model errors; and horizontal sections."""
    rng = random.Random(seed)
    names = ['w', 'h', 'd', 'r', 'gap', 'n', 'angle', '_t2']

    def number():
        return rng.choice([rng.randint(1, 40), round(rng.uniform(0.5, 40), rng.choice([0, 1, 3])), 2.5, 10])

    def expression(known):
        if not known or rng.random() < 0.3:
            return number()
        pick = lambda: rng.choice(known) if rng.random() < 0.7 else str(number())  # noqa: E731
        forms = [
            lambda: f'{pick()}',
            lambda: f'{pick()} + {pick()}',
            lambda: f'{pick()} * 2',
            lambda: f'({pick()} + {pick()}) / 2',
            lambda: f'-{pick()} + 100',
            lambda: f' {pick()}*0.5 + 0.25 ',
            lambda: f'{pick()} / 2 + 1',
            lambda: f'1_0.5 + {pick()}',
            lambda: f'+{pick()} - -{pick()}',
        ]
        rare = [
            lambda: f'{pick()}/{pick()}',
            lambda: f'{pick()} - {pick()} - {pick()}',
            lambda: f'cross_x(0, 0, {pick()}, {pick()}, 0, {pick()}, {pick()}, 0)',
            lambda: 'cross_y(0, 0, 10, 10, 0, 10, 10, 0) + 1',
        ]
        if rng.random() < 0.15:
            forms = rare
        text = rng.choice(forms)()
        trouble = rng.random()
        if trouble < 0.008:
            text = text + ' / 0'
        elif trouble < 0.012:
            text = 'missing + ' + text
        elif trouble < 0.016:
            text = ' + '.join([text] * 20)
        elif trouble < 0.02:
            text = text + ' ** 2'
        elif trouble < 0.024:
            text = '1e9 * ' + text
        return text

    def vector(known, low=1):
        return [expression(known) if rng.random() < 0.5 else rng.choice([low, 5, 12.5]) for _ in range(3)]

    def sketch(item, known):
        # A polygon (regular, or on a coarse grid), sometimes closed by repeating its first vertex, with fillets
        # or one cubic edge with tangent options; positive radii for most revolutions.
        n = rng.randint(3, 7)
        if rng.random() < 0.5:
            turn = rng.uniform(0, 1)
            points = [
                [
                    round(10 + 8 * math.cos(2 * math.pi * k / n + turn), 3),
                    round(10 + 8 * math.sin(2 * math.pi * k / n + turn), 3),
                ]
                for k in range(n)
            ]
        else:
            points = [[rng.choice([0, 2, 5, 10, 15, 20]), rng.choice([0, 3, 7, 12, 20])] for _ in range(n)]
        if item['kind'] == 'revolve' and rng.random() < 0.8:
            points = [[abs(x) + 1, y] for x, y in points]
        if rng.random() < 0.3 and known:
            points[0] = [rng.choice(known), points[0][1]]
        item['points'] = points
        if rng.random() < 0.4:
            item['fillets'] = [rng.choice([0, 0, 0.5, 1, rng.choice(known) if known else 2]) for _ in points]
        elif rng.random() < 0.4:
            curves = [None] * n
            i = rng.randrange(n)
            a, b = points[i], points[(i + 1) % n]
            if isinstance(a[0], (int, float)) and isinstance(b[0], (int, float)):
                curves[i] = {
                    'controls': [
                        [round(a[0] + rng.uniform(-3, 3), 2), round(a[1] + rng.uniform(-3, 3), 2)],
                        [round(b[0] + rng.uniform(-3, 3), 2), round(b[1] + rng.uniform(-3, 3), 2)],
                    ]
                }
                for option in ('tangentStart', 'tangentEnd'):
                    if rng.random() < 0.3:
                        curves[i][option] = True
                item['curves'] = curves
        if rng.random() < 0.1:
            item['points'] = points + [list(points[0])]
            for key, closing in (('fillets', 0), ('curves', None)):
                if key in item:
                    item[key] = item[key] + [closing]
        if rng.random() < 0.25:
            # Relations the core's solver satisfies (the reference asks the same solver), some contradictory.
            relations = [{'kind': 'fixed', 'point': 0, 'value': [rng.choice([0, 2, 10]), rng.choice([0, 3])]}]
            for _ in range(rng.randint(1, 3)):
                kind = rng.choice(['distance', 'parallel', 'perpendicular', 'angle', 'equal'])
                a, b = rng.sample(range(n), 2)
                if kind == 'distance':
                    relations.append(
                        {'kind': kind, 'points': [a, b], 'value': rng.choice([5, 8, 12, 'w' if 'w' in known else 6])}
                    )
                else:
                    relations.append(
                        {'kind': kind, 'edges': [a, b], **({'value': rng.choice([30, 90])} if kind == 'angle' else {})}
                    )
            item['constraints'] = relations
        if item['kind'] == 'extrude':
            item['height'] = expression(known)
        elif rng.random() < 0.7:
            item['angle'] = rng.choice([360, 90, 180, 400, expression(known)])

    def feature(index, known, first):
        kind = rng.choice(['box', 'box', 'cylinder', 'cylinder', 'sphere', 'extrude', 'revolve', 'loft', 'script'])
        item = {'id': rng.choice([f'f{index}', f'Feature {index}', f'f{index}']), 'kind': kind}
        if kind == 'script':
            item['source'] = rng.choice([
                'cube([4, 3, 2]);', 'cylinder(h=5, r=2, $fn=12);', 'translate([2, 2, 0]) sphere(3, $fn=10);',
                'linear_extrude(height=3) offset(r=1, $fn=8) square([6, 4]);',
                'for (i = [0:2]) translate([i*4, 0, 0]) cube(2);', 'module m(s=2) cube(s); m(s=x);',
                f'cube([{rng.choice(known) if known else 3}, 2, 2]);', 'square(3);', 'cube(', 'cube(1); @',
            ])  # fmt: skip
            if rng.random() < 0.4:
                item['scriptParameters'] = rng.choice([{'x': 3}, {'x': 1.5, 'w': 2}, {'bad name': 1}, {'x': 'y'}, None])
        elif kind in ('extrude', 'revolve'):
            sketch(item, known)
        elif kind == 'loft':
            for level in ('base', 'top'):
                item[level + 'Shape'] = rng.choice(['rectangle', 'ellipse', 'circle', 'circle', 'rectangle', 'oval'])
                item[level + 'Width'] = expression(known)
                if rng.random() < 0.9:
                    item[level + 'Depth'] = expression(known)
            item['height'] = expression(known)
            if rng.random() < 0.4:
                item['offset'] = [rng.choice([0, 3, -5]), rng.choice([0, 2])]
        elif kind == 'box':
            item['size'] = vector(known)
        elif kind == 'cylinder':
            item['radius'] = expression(known)
            item['height'] = expression(known)
            if rng.random() < 0.3:
                item['topRadius'] = rng.choice([0, 1.5, 'r / 2' if 'r' in known else 2])
        else:
            item['radius'] = expression(known)
        if not first:
            item['op'] = rng.choice(['add', 'add', 'cut', 'cut', 'intersect'])
        if rng.random() < 0.4:
            item['origin'] = [rng.choice([0, -2, 5, 7.5]) for _ in range(3)]
        if rng.random() < 0.3:
            item['rotation'] = [rng.choice([0, 15, 30, 45, 90]) for _ in range(3)]
        if rng.random() < 0.2:
            if rng.random() < 0.5:
                item['pattern'] = {
                    'kind': 'linear',
                    'count': rng.choice([1, 2, 3]),
                    'step': [rng.choice([0, 6, 10, 10]), rng.choice([0, 3]), 0],
                }
            else:
                item['pattern'] = {
                    'kind': 'polar',
                    'count': rng.choice([2, 3, 4]),
                    'angle': rng.choice([45, 90, 120, 72, 360]),
                    'center': [rng.choice([0, 10]), 0, 0],
                }
        if not first and rng.random() < 0.1:
            item['enabled'] = False
        return item

    for case in range(count):
        parameters, known = {}, []
        for name in rng.sample(names, rng.randint(0, 5)):
            parameters[name] = expression(known + ([name] if rng.random() < 0.03 else []))
            known.append(name)
        features = [feature(i, known, i == 0) for i in range(rng.randint(1, 5))]
        model = {'version': 1, 'parameters': parameters, 'features': features}
        damage = rng.random()
        if damage < 0.02:
            model['version'] = 2
        elif damage < 0.04:
            features[0]['op'] = 'cut'
        elif damage < 0.06 and len(features) > 1:
            features[1]['id'] = features[0]['id']
        elif damage < 0.07:
            features[0]['kind'] = 'cone'
        elif damage < 0.08:
            model['features'] = []
        request = {'model': model, 'order': list(parameters)}
        if case % 4 == 3:
            yield 'solid_cad_section', {**request, 'z': rng.choice([0, 1, 2.5, 6, 'h / 2' if 'h' in known else 3, -1])}
        else:
            yield 'solid_cad', request


def cad_script_cases(count, seed):
    """CAD scripts for the /api/cad-script preview and for scripted features: every primitive and operation
    with sound and unsound arguments, modules with children(), functions, variables read before they are
    assigned, loops, ranges, comprehensions, conditionals and the built-in functions, $fn passed down; with the
    reader's faults (unknown and Unicode characters, Unicode digits, unterminated comments, external files),
    the evaluator's limits, parameters and overrides (some reserved), and scripts damaged at random."""
    rng = random.Random(seed)
    pick = rng.choice
    risk = [0.0]  # the chance of an unsound choice, set for each script

    def opt(good, bad=()):
        return pick(bad) if bad and rng.random() < risk[0] else pick(good)

    def number():
        return opt(['1', '2', '3', '4', '5', '6', '8', '10', '2.5', '.5', '1.5', '1e1', '7.', '0.25'],
                   ['0', '-1', '20', '1e-3', '360', '1e10', '٣', '1.٥'])  # fmt: skip

    def scalar(depth=0):
        roll = rng.random()
        if depth > 2 or roll < 0.4:
            return number()
        if roll < 0.55:
            return opt(['size', 'n', 'r', 'k', 'size / 2', 'PI'], ['true', 'undefined_name', 'x'])
        if roll < 0.72:
            op = opt(['+', '*', '+', '-'], ['/', '%', '^', '<', '>=', '==', '!=', '&&', '||', '-'])
            return f'({scalar(depth + 1)} {op} {scalar(depth + 1)})'
        if roll < 0.82:
            name = opt(['abs', 'floor', 'ceil', 'round', 'sqrt', 'twice', 'fact', 'max', 'min', 'pow', 'len', 'norm', 'cos', 'sin'],
                       ['tan', 'asin', 'acos', 'atan', 'sign', 'ln', 'log', 'exp', 'atan2', 'nosuch', 'deep'])  # fmt: skip
            if name in ('pow', 'atan2', 'min', 'max'):
                return f'{name}({scalar(depth + 1)}, {scalar(depth + 1)})'
            if name in ('len', 'norm'):
                return f'{name}({vector(depth + 1)})'
            if name == 'fact':
                return f'fact({pick(["3", "n", "1"])}) / 2'
            if name in ('sin', 'cos'):
                return f'(2 + {name}({scalar(depth + 1)}))'
            return f'{name}({scalar(depth + 1)})'
        if roll < 0.88:
            return f'({scalar(depth + 1)} > 3 ? {scalar(depth + 1)} : {scalar(depth + 1)})'
        if roll < 0.94:
            return f'{vector(depth + 1, 3)}[{opt(["0", "1", "2", "k % 2"], ["-1", "0.5", "3"])}]'
        return opt(['+'], ['-', '!']) + scalar(depth + 1)

    def vector(depth=0, size=None):
        roll = rng.random()
        count = size if size else opt([2, 3, 3, 4], [0, 1])
        if roll < 0.55 or depth > 2:
            return '[' + ', '.join(scalar(depth + 1) for _ in range(count)) + ']'
        if roll < 0.7:
            step = opt(['', ': 1', ': 2', ': 0.5'], [': -1', ': 0'])
            return f'[0{step} : {opt(["3", "n", "2.5"], ["1023", "2000", "-3"])}]'
        if roll < 0.85:
            return f'[for (i = [0:{max(count, 1) - 1}]) {opt(["i + 1", "i*2 + 1", "i/2 + 1"], ["[i, i]", "i - 1"])}]'
        return f'({vector(depth + 1, count)} {opt(["+", "-"], ["*", "/"])} {opt([vector(depth + 1, count)], [scalar(depth + 1), "[1]"])})'

    def size(dim):
        roll = rng.random()
        if roll < 0.6:
            return '[' + ', '.join(opt(['1', '2', '3', '5', '8', 'size', 'size/2', 'r', '4.5'], ['0', '-1', 'true'])
                                   for _ in range(dim)) + ']'  # fmt: skip
        if roll < 0.85:
            return opt(['2', 'size', '3.5', 'r', scalar()], ['[0, 1]', '[1, 2, 3, 4]', 'true'])
        return vector(0, dim)

    def facets():
        return opt(['', '', ', $fn=8', ', $fn=12', ', $fn=16', ', $fn=6', ', $fn=0'],
                   [', $fn=3', ', $fn=2', ', $fn=300', ', $fn=7.5', ', $fn=true'])  # fmt: skip

    def drawing(depth):
        roll = rng.random()
        if depth > 3 or roll < 0.3:
            primitive = opt(['square', 'square', 'circle', 'circle', 'polygon'], ['children', 'nonsense'])
            if primitive == 'square':
                return f'square({size(2)}{opt(["", ", center=true"], [", center=1", ", true"])});'
            if primitive == 'circle':
                return f'circle({opt(["r=", "d=", ""], ["r=-", "r=0*"])}{pick(["2", "3", "size/2", "r"])}{facets()});'
            if primitive == 'children':
                return pick(['children();', 'children(0);', 'children(5);', 'children(0.5);'])
            if primitive == 'nonsense':
                return pick(['cube(1);', 'sphere(2);', 'wheel(1);', 'circle(1) square(1);'])
            return polygon()
        operation = opt(['union', 'difference', 'intersection', 'hull', 'translate', 'rotate', 'scale', 'mirror',
                         'offset', 'multmatrix', 'projection', 'for', 'if', 'module', 'block'])  # fmt: skip
        inner = ' '.join(drawing(depth + 1) for _ in range(pick([1, 2, 2, 3])))
        if operation in ('union', 'difference', 'intersection', 'hull'):
            return f'{operation}() {{ {inner} }}'
        if operation == 'translate':
            return f'translate({opt(["[3, 0]", "[1, 2, 0]", "[size, 1]", "v=[2,2]", "[0, 0, 4]"], ["[1]", "3"])}) {{ {inner} }}'
        if operation == 'rotate':
            angle = opt(
                ['45', 'a=30', '[0, 0, 90]', 'a=60, v=[0,0,-1]', 'v=[0,0,1]'], ['[10, 0, 0]', 'a=10, v=[1,0,0]']
            )
            return f'rotate({angle}) {{ {inner} }}'
        if operation == 'scale':
            return f'scale({opt(["2", "[2, 0.5]", "[1, 1, 3]", "-1"], ["0", "[0, 1]"])}) {{ {inner} }}'
        if operation == 'mirror':
            return (
                f'mirror({opt(["[1, 0]", "[0, 1, 0]", "[1, 1]"], ["[0, 0]", "[1, 0, 1]", "[0, 0, 1]"])}) {{ {inner} }}'
            )
        if operation == 'offset':
            kind = opt(
                ['r=1', 'delta=1', 'delta=-0.25, chamfer=true', '0.5', 'delta=2'], ['r=0.5, delta=1', 'chamfer=true']
            )
            return f'offset({kind}{facets()}) {{ {inner} }}'
        if operation == 'multmatrix':
            matrix = opt(['[[1,0,0,2],[0,1,0,3],[0,0,1,0]]', '[[1,1,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]]',
                          '[[0,-1,0,1],[1,0,0,0],[0,0,1,0]]'],
                         ['[[0,0,0,0],[0,1,0,0],[0,0,1,0]]', '[[1,0,0,0],[0,1,0,0],[0,0,1,1]]',
                          '[[2,0,0,0],[0,2,0,0],[0,0,1,0],[0,0,1,1]]'])  # fmt: skip
            return f'multmatrix({matrix}) {{ {inner} }}'
        if operation == 'projection':
            return f'projection({opt(["", "cut=true", "cut=false"], ["true", "cut=1"])}) {{ {solid(depth + 1)} }}'
        if operation == 'for':
            loops = opt(['[0:2]', '[0:n-1]', '[1, 3]', '[0:0.5:1]'], ['5', '[0:1:2000]'])
            return f'for (i = {loops}) translate([i*4, 0]) {{ {inner} }}'
        if operation == 'if':
            return f'if ({scalar()} > 2) {{ {inner} }} else {{ {drawing(depth + 1)} }}'
        if operation == 'module':
            return f'plate({opt(["", "2", "w=3", "1, 2"], ["q=1", "1, 2, 3"])}) {{ {inner} }}'
        return f'{{ {inner} }}'

    def polygon():
        return opt([
            'polygon([[0,0],[6,0],[6,4],[0,4]]);',
            'polygon(points=[[0,0],[8,0],[8,8],[0,8],[2,2],[6,2],[6,6],[2,6]], paths=[[0,1,2,3],[4,5,6,7]]);',
            'polygon([for (a = [0:60:300]) [3*cos(a), 3*sin(a)]]);',
            'polygon([[0,0],[2,0],[2,2],[1,1],[0,2]]);',
            'polygon([[0,0],[5,0],[5,5],[0,5],[0,0]]);',
            'polygon([[0,0],[4,0],[4,0],[4,4],[0,4]]);',
            'polygon(points=[[0,0],[4,0],[2,3]], convexity=2);',
            'polygon([[0,0],[size,0],[size,r],[0,k]]);',
        ], [
            'polygon([[0,0],[4,4],[4,0],[0,4]]);',
            'polygon([[0,0],[1,0],[0,0]]);',
            'polygon([[0,0],[3,0]]);',
            'polygon(points=[[0,0],[4,0],[2,3]], paths=[[0,1,5]]);',
            'polygon(points=[[0,0],[4,0],[2,3]], paths=[[0,1]]);',
            'polygon([[0,0],[0,0],[0,0],[0,0]]);',
            'polygon([[0,0],[2,0],[2,2],[2,0],[0,2]]);',
            'polygon(points=[[0,0],[4,0],[2,3]], convexity=0);',
        ])  # fmt: skip

    def polyhedron():
        return opt([
            'polyhedron(points=[[0,0,0],[4,0,0],[0,4,0],[0,0,4]], faces=[[0,1,2],[0,3,1],[0,2,3],[1,3,2]]);',
            'polyhedron(points=[[0,0,0],[2,0,0],[2,2,0],[0,2,0],[0,0,2],[2,0,2],[2,2,2],[0,2,2]],'
            ' faces=[[0,1,2,3],[4,7,6,5],[0,4,5,1],[1,5,6,2],[2,6,7,3],[3,7,4,0]]);',
            'polyhedron(points=[[0,0,0],[4,0,0],[4,4,0],[0,4,0],[2,2,5]], faces=[[0,1,2,3],[0,4,1],[1,4,2],[2,4,3],[3,4,0]]);',
        ], [
            'polyhedron(points=[[0,0,0],[4,0,0],[0,4,0],[0,0,4]], faces=[[0,2,1],[0,1,3],[0,3,2],[1,2,3]]);',
            'polyhedron(points=[[0,0,0],[2,0,0],[2,2,0],[0,2,0],[0,0,2],[2,0,2],[2,2,2],[0,2,2]],'
            ' faces=[[0,1,2,3],[4,5,6,7],[0,4,5,1],[1,5,6,2],[2,6,7,3],[3,7,4,0]]);',
            'polyhedron(points=[[0,0,0],[4,0,0],[0,4,0],[0,0,4]], faces=[[0,1,1],[0,3,1],[0,2,3],[1,3,2]]);',
            'polyhedron(points=[[0,0,0],[4,0,0],[0,4,0],[0,0,4]], faces=[[0,1,9],[0,3,1],[0,2,3],[1,3,2]]);',
            'polyhedron(points=[[0,0,0],[4,0,0],[0,4,0]], faces=[[0,1,2],[0,1,2],[0,1,2],[0,1,2]]);',
            'polyhedron(points=[[0,0,0],[4,0,0],[0,4,0],[0,0,4]], faces=[[0,1,2],[0,3,1],[0,2,3]]);',
            'polyhedron(points=[[0,0,0],[4,0,0],[4,4,1],[0,4,0],[2,2,5]], faces=[[0,1,2,3],[0,4,1],[1,4,2],[2,4,3],[3,4,0]]);',
        ])  # fmt: skip

    def solid(depth):
        roll = rng.random()
        if depth > 3 or roll < 0.3:
            primitive = opt(['cube', 'cube', 'sphere', 'cylinder', 'cylinder', 'polyhedron', 'extrude'])
            if primitive == 'cube':
                return f'cube({size(3)}{opt(["", ", center=true"], [", center=1"])});'
            if primitive == 'sphere':
                return f'sphere({opt(["r=", "d=", ""], ["r=0*"])}{pick(["2", "3", "size/3"])}{facets()});'
            if primitive == 'cylinder':
                dims = opt(['h=4, r=2', 'h=size, d=3', 'h=3, r1=2, r2=0', 'h=3, d1=4, d2=2, center=true', '4, 2, 1'],
                           ['h=0, r=1', 'h=3, r=1, r1=2', 'h=2, r=-1', 'h=2, r1=0, r2=0', '3, 1, 1, 1, 1'])  # fmt: skip
                return f'cylinder({dims}{facets()});'
            if primitive == 'polyhedron':
                return polyhedron()
            return extrusion(depth)
        operation = opt(['union', 'difference', 'intersection', 'hull', 'translate', 'rotate', 'scale', 'mirror',
                         'multmatrix', 'extrude', 'for', 'if', 'module', 'block'])  # fmt: skip
        inner = ' '.join(solid(depth + 1) for _ in range(pick([1, 2, 2, 3])))
        if operation in ('union', 'difference', 'intersection', 'hull'):
            return f'{operation}() {{ {inner} }}'
        if operation == 'translate':
            return f'translate({opt(["[3, 0, 0]", "[1, 2]", "[0, 0, size]", "v=[1, 1, 1]"], ["[1, 2, 3, 4]"])}) {{ {inner} }}'
        if operation == 'rotate':
            angle = opt(['[0, 0, 45]', '[30, 20, 10]', 'a=90, v=[1, 0, 0]', 'a=45, v=[1, 1, 0]', 'a=30, v=[0, 0, -2]', '33'],
                        ['a=[1,2,3], v=[0,0,1]', 'a=10, v=[0,0,0]'])  # fmt: skip
            return f'rotate({angle}) {{ {inner} }}'
        if operation == 'scale':
            return f'scale({opt(["2", "[1, 2, 0.5]", "[2, 3]"], ["[1, 0, 1]"])}) {{ {inner} }}'
        if operation == 'mirror':
            return f'mirror({opt(["[1, 0, 0]", "[0, 1, 1]", "[1, 0]"], ["[0, 0, 0]"])}) {{ {inner} }}'
        if operation == 'multmatrix':
            matrix = opt(['[[1,0,0,2],[0,1,0,3],[0,0,1,4]]', '[[1,0.5,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]]',
                          '[[0,1,0,0],[1,0,0,0],[0,0,1,0]]'],
                         ['[[1,0,0,0],[0,1,0,0],[0,0,0,0]]', '[[1,0,0],[0,1,0]]'])  # fmt: skip
            return f'multmatrix(m={matrix}) {{ {inner} }}'
        if operation == 'extrude':
            return extrusion(depth)
        if operation == 'for':
            loops = opt(['i = [0:2]', 'i = [0:n-1], j = [0:1]', 'i = [1, 2]'], ['i = [0:n]', 'i = 3'])
            return f'for ({loops}) translate([i*5, 0, 0]) {{ {inner} }}'
        if operation == 'if':
            return f'if ({scalar()} < 4) {{ {inner} }}' + pick(['', f' else {solid(depth + 1)}'])
        if operation == 'module':
            return f'post({opt(["", "3", "h=2", "h=1, $fn=6"], ["2, 3", "x=1"])}) {{ {inner} }}'
        return f'{{ {inner} }}'

    def extrusion(depth):
        if rng.random() < 0.6:
            options = opt(['height=3', 'height=size', '3', 'height=2, twist=90', 'height=4, twist=45, slices=3, scale=0.5',
                           'height=2, center=true', 'height=2, scale=[1, 0.5]', 'height=1, convexity=3'],
                          ['height=0', 'height=2, twist=400, slices=1', 'height=2, slices=0', 'height=2, scale=-1'])  # fmt: skip
            return f'linear_extrude({options}) {{ {drawing(depth + 1)} }}'
        options = opt(['', 'angle=90', 'angle=-180', '$fn=12', 'angle=180, $fn=16'], ['angle=0', 'angle=400', '270, 4'])
        profile = opt(['translate([2, 0]) square([1, 2]);', 'translate([-4, 0]) square([1, 2]);', 'square([1, 2]);',
                       'translate([3, 1]) circle(r=1, $fn=8);'],
                      ['translate([-1, 0]) square([2, 2]);', drawing(depth + 1)])  # fmt: skip
        return f'rotate_extrude({options}) {{ {profile} }}'

    definitions = [
        'module plate(w=2, h=1) { linear_extrude(height=h) children(); }',
        'module plate(w=2, h=1) { offset(r=w/4) children(); }',
        'module post(h=3, r=1) { cylinder(h=h, r=r); translate([0, 0, h]) children(); }',
        'module post(h=3) { for (i = [0:$children-1]) translate([0, 0, i*h]) children(i); }',
        'function twice(x) = 2*x;',
        'function twice(x, y=1) = x*y + 1;',
        'function fact(n) = n <= 1 ? 1 : n * fact(n - 1);',
    ]
    extras = ['function deep(n) = deep(n + 1);', 'size = r * 2;', '$fn = 10;', '$fn = 0;', 'x = y; y = x;', 'n = 2;']
    faults = [
        'cube(1); include <x.scad>', 'cube(1) /* open', 'cube(1); @', 'cube(1); é', 'cube(٣);',
        'cube(1.٥e١);', 'cube(1);　sphere(1);', "cube(1); '", 'cube(1); \\', 'cube(1); \x07',
        'cube(1); ​', 'cube(1); \U0001F600', 'cube(1); ­', 'cube([1,2,3);', 'cube(1)', 'cube(1); }',
        'true = 1;', '$x = 1;', 'module for() {}', 'cube(1, 2, 3);', 'cube(size=1, size=2);', 'cube(center=true, 1);',
        'cube(1e10);', 'sphere(r=[for (i=[0:2000]) i][0]);', 'cube(x);', 'nosuch();', 'children();',
        'for (i = [0:1:9999]) cube(1);', 'for (i = [0:20], j = [0:20], k = [0:20]) cube(1);',
        'x = 1; cube(x == [1] ? 1 : 2);', 'a = [1, [2, [3]]]; cube(a[1][1][0]);',
        'function deep(n) = deep(n + 1); cube(deep(1));', 'function t(x) = x; cube(t(1, 2, 3));',
        'function t(x) = x; cube(t(y=2));', 'cube(len(5));', 'cube(norm([3, 4]));', 'cube(min([3, 1, 2]));',
        'cube(max());', 'cube([1, 2, 3] * [1, 1, 1]);', 'cube(1 / 0);', 'cube(1 % 0);', 'cube((-8) ^ (1/3));',
        'cube(sqrt(-1));', 'cube(ln(0));', 'cube(exp(800));', 'cube(tan(90));', 'cube(acos(2));',
        'cube(atan2(1, 1) / 10);', 'cube(round(-2.5) + 4);', 'cube(sign(-3) + 2);', 'cube([true, 2, 3]);',
        'cube(!0 + 1);', 'cube(1 < 2);', 'cube([1, 2] + [1]);', 'cube([1, 2] < [3, 4]);', '', '// only a comment',
        ';;;', 'square(0);', 'linear_extrude(1) cube(1);', 'union() { cube(1); square(1); }', 'hull() {}',
        'difference();', 'cube(1, $fn=8);', 'cube(1, $fa=12);', 'rotate_extrude() cube(1);', 'projection() square(1);',
        'offset(1) cube(1);', 'cube(1) cube(2);', 'square(1) { square(1); }', 'sphere(99999);',
        'translate([99999, 0, 0]) cube(10);', 'cube([1, 1, 1e-9]);',
        'for (i = [0:200]) translate([i * 0.1, 0, 0]) sphere(5, $fn=64);',
        'linear_extrude(height=1, slices=200) circle(10, $fn=200);', 'cube(1); /* a */ sphere(1); // end',
        'cube([floor(2.7), ceil(1.2), round(1.5)]);', 'cube([1, 2, 3][floor(1.9)]);',
        'v = [for (i = [0:3]) i]; cube(len(v));', 'cube(1 + [1, 2] * [3, 4]);',
        'cube([floor(1e5), 1, 1] * [floor(1e5), 1, 1] / 1e9);', 'cube(pow(2, 3));',
        'cube([0.1, 0.2, 0.3] * [10, 10, 10]);', 'x = [1:3]; cube(x[2]);', 'cube([0:-1:3] == [] ? 1 : 2);',
        'if (1) cube(1); else sphere(1);', 'if (0) cube(1);', 'cube(true ? 2 : 3);', 'cube(true + 1);',
    ]  # fmt: skip
    for _ in range(count):
        risk[0] = pick([0.0, 0.0, 0.02, 0.05, 0.2])
        if rng.random() < 0.15:
            source = pick(faults)
        else:
            parts = []
            if rng.random() < 0.95:
                parts += ['size = 6;', 'n = 3;', 'r = size / 3;', 'k = n + 1;']
            parts += [pick(definitions[0:2]), pick(definitions[2:4]), pick(definitions[4:6]), definitions[6]]
            if rng.random() < risk[0] * 3:
                parts.append(pick(extras))
            body = solid if rng.random() < 0.65 else drawing
            parts += [body(0) for _ in range(pick([1, 1, 2]))]
            rng.shuffle(parts)
            source = pick(['\n', ' ', '\r\n', '\n\t']).join(parts)
            damage = rng.random()
            if damage < 0.03 and source:
                at = rng.randrange(len(source))
                source = source[:at] + source[at + 1 :]
            elif damage < 0.05 and source:
                at = rng.randrange(len(source))
                source = source[:at] + pick(['#', '"', '.', '[', '(', '$', '&', '|', '1', 'e', ';', ' ']) + source[at:]
            elif damage < 0.06:
                source = source[: rng.randrange(len(source) + 1)]
        payload = {'source': source}
        if rng.random() < 0.4:
            payload['parameters'] = {'size': pick([6, 4.5, 'n * 2', 10]), 'n': pick([3, 2, 1, 'k']), 'k': 1}
            if rng.random() < 0.05:
                payload['parameters'][pick(['PI', 'for', 'children', 'r'])] = 2
            if rng.random() < 0.03:
                payload['parameters'] = pick([{'a': 'b', 'b': 'a'}, {'a': 'zz'}, {'a': 1e6}, [], None])
        if rng.random() < 0.15:
            payload['overrides'] = pick([{'size': 4}, {'n': 2.5, 'k': 1}, {'r': 1}, {'size': 8, 'z': 1}] * 3
                                        + [{'bad name': 1}, {'n': True}, None])  # fmt: skip
        if rng.random() < 0.02:
            payload['source'] = pick([None, 5, ['cube(1);'], 'x' * 70000])
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
        if len(raw) % 3 == 0 and rng.random() < 0.03:
            data['content_base64'] += '='  # refused, as Python 3.12.10 and later refuse it
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


def program_import_cases(count, seed):
    """Foreign drawings for /api/program-import: posted G-code with linear moves, arcs given by I/J and by R,
    absolute and incremental coordinates and centres, units, comments, tool changes, dwells and program ends;
    HP-GL with pens up and down, absolute and relative moves, arcs, circles, pen speeds and forces; LinuxCNC
    INI files with units, coordinates and axis limits; long runs of contours that are batched; every limit;
    each with words, values and statements made unsound at random, under every suffix and unit, and payloads
    the request itself refuses; and text that is not ASCII, which the core reads as Python's str methods do. DXF
    drawings are the core's to decline."""
    rng = random.Random(seed)
    pick = rng.choice
    risk = [0.0]

    def opt(good, bad=()):
        return pick(bad) if bad and rng.random() < risk[0] else pick(good)

    def outside_ascii(text):
        """The text with characters outside ASCII, which a reader takes as Python's str methods and re do:
        digits of other scripts, other spaces and line boundaries, letters str.upper() makes ASCII or longer,
        and comments."""
        chars = list(text)
        for _ in range(rng.randint(1, 4)):
            roll, at = rng.random(), rng.randrange(len(chars) + 1)
            digits = [k for k, c in enumerate(chars) if '0' <= c <= '9']
            spaces = [k for k, c in enumerate(chars) if c == ' ']
            if roll < 0.25 and digits:
                k = pick(digits)
                chars[k] = chr(pick([0x660, 0x6F0, 0x966, 0xFF10, 0x1D7CE]) + int(chars[k]))
            elif roll < 0.45 and spaces:
                chars[pick(spaces)] = pick(['\u00a0', '\u3000', '\u2009', '\u1680'])
            elif roll < 0.6:
                chars.insert(at, pick(['\u017f', '\u0131', '\u00df', '\ufb01', '\u00e9', '\u00c4', '\u01f0',
                                       '\u0149', '\u0390', '\u00b2', '\u216b', '\u0130']))  # fmt: skip
            elif roll < 0.75:
                chars.insert(at, pick(['\u2028', '\u2029', '\x85', '\r\u2028', '\x0b']))
            else:
                chars.insert(at, pick(['(Ø 6 mm °)', '; café', ' (Fräser)', '\ufeff', '# für']))
        return ''.join(chars)

    # Drawings that are not ASCII, each read once.
    yield from [
        {'name': 'part.nc', 'text': 'G21\nG0 X0 Y0 (Ø 6 mm)\nG1 X\u0663 Y\u0664 F100\n'},
        {'name': 'part.nc', 'text': 'G21\u2028G0 X0 Y0\r\u2028G1 X3\u00a0Y4 F100\nG1 X5 Y5 Q1'},
        {'name': 'part.nc', 'text': 'g21\ng0 x0 y0\ng1 x3 y4 \u017f1000 f100'},
        {'name': 'part.nc', 'text': 'G21\nG0 X0 Y0\nG1 X3 Y4 F100 \u00e9'},
        {'name': 'part.plt', 'text': 'IN;SP1;PU0,0;PD\u0664\u0660\u0660,0;\u00c4B1;'},
        {'name': 'part.plt', 'text': 'in;sp1;pu0,0;pd400\u3000400;\u00dfx;'},
        {'name': 'part.plt', 'text': 'IN;SP1;PD400,0,400,400;\ufb01;'},
        {'name': 'part.plt', 'text': 'IN;SP1;PD400,0,400,400;\u0390;'},
        {'name': 'machine.ini', 'text': '[TRAJ]\nLINEAR_UNITS = mm  # Millimeter für die Maschine\n[AXIS_X]\n'
         'MIN_LIMIT = -\u0661\nMAX_LIMIT = 300\n[AXIS_Y]\nMIN_LIMIT = 0\nMAX_LIMIT = 200\n'},
        {'name': 'machine.ini', 'text': '[TRAJ]\nLINEAR_UNITS=mm\n[ax\u0131s_x]\nMIN_LIMIT=0\nMAX_LIMIT=300\n'
         '[AXIS_Y]\nMIN_LIMIT=0\nMAX_LIMIT=200'},
        {'name': 'machine.ini', 'text': '#\u0130NCLUDE other.ini\n[TRAJ]\nLINEAR_UNITS=mm'},
        {'name': 'machine.ini', 'text': '# include\u00e9\n[TRAJ]\nLINEAR_UNITS=mm\n[AXIS_X]\nMIN_LIMIT=0\n'
         'MAX_LIMIT=300\n[AXIS_Y]\nMIN_LIMIT=0\nMAX_LIMIT=200'},
        {'name': 'machine.ini', 'text': '\u3000#\u00a0\u0131nclude x\n[TRAJ]\nLINEAR_UNITS=mm'},
        {'name': 'machine.ini', 'text': '# include\u00b7x\n[TRAJ]\nLINEAR_UNITS=mm'},
    ]  # fmt: skip

    def value(low, high, digits=3):
        roll = rng.random()
        if roll < 0.15:
            return str(rng.randint(int(low), int(high)))
        if roll < 0.2:
            return pick(['0', '-0', '.5', '5.', '+2', '-.25', '0.0', '1.000'])
        text = f'{rng.uniform(low, high):.{digits}f}'
        return text.rstrip('0').rstrip('.') if rng.random() < 0.3 else text

    def gcode():
        lines = []
        if rng.random() < 0.8:
            lines.append(
                opt(['G21', 'G20', 'G21 G90', 'G90 G21 G17', 'G20 G91.1', 'G21 G90.1', '%'], ['G21 G20', 'G18', 'G70'])
            )
        if rng.random() < 0.3:
            lines.append(opt(['T1 M6', 'T2 M06', 'T3 M6 (change)'], ['M6', 'T-1 M6', 'T1.5 M6']))
        x = y = None
        if rng.random() < 0.85:
            lines.append(pick(['G0 X0 Y0', 'G0 X0 Y0 Z5', 'G00 X1.5 Y-2 Z3']))
            x, y = (1.5, -2.0) if 'X1.5' in lines[-1] else (0.0, 0.0)
        centre_absolute = bool(lines) and 'G90.1' in lines[0]
        arcs = False  # whether G2 or G3 is the modal motion
        for _ in range(rng.randint(1, 40)):
            roll = rng.random()
            parts = []
            if roll < 0.12:
                x, y = rng.uniform(-40, 40), rng.uniform(-40, 40)
                parts = [opt(['G0', 'G00', 'G0'], ['G0 G1']), f'X{x:.3f}', f'Y{y:.3f}']
                arcs = False
                if rng.random() < 0.5:
                    parts.append(f'Z{value(-3, 10)}')
            elif roll < 0.22:
                parts = [opt(['G1', 'G01'], ['G1 G2']), f'Z{value(-6, 5)}', f'F{value(50, 900, 2)}']
                arcs = False
            elif roll < 0.55:
                parts = [opt(['G01', 'G1'] if arcs else ['G1', '', 'G01'], ['G80', 'G5'])]
                arcs = False
                for axis in sorted(rng.sample('XYZ', rng.randint(1, 3))):
                    if axis == 'Z':
                        parts.append(f'Z{value(-6, 5)}')
                    else:
                        number = rng.uniform(-40, 40)
                        x, y = (number, y) if axis == 'X' else (x, number)
                        parts.append(f'{axis}{number:.4f}')
                if rng.random() < 0.4:
                    parts.append(f'F{value(10, 12000, rng.choice([0, 1, 3, 6]))}')
            elif roll < 0.78 and x is not None and y is not None:
                motion = pick(['G2', 'G3', 'G02', 'G03'])
                form = rng.random()
                if form < 0.5:
                    i, j = rng.uniform(-15, 15), rng.uniform(-15, 15)
                    radius, turn = math.hypot(i, j), rng.uniform(-math.pi, math.pi)
                    ex, ey = x + i + radius * math.cos(turn), y + j + radius * math.sin(turn)
                    if abs(ex) > 50 or abs(ey) > 50:
                        continue
                    centre = [f'I{x + i:.4f}', f'J{y + j:.4f}'] if centre_absolute else [f'I{i:.4f}', f'J{j:.4f}']
                    parts = [motion, f'X{ex:.4f}', f'Y{ey:.4f}', *rng.sample(centre, opt([2], [1]))]
                    if rng.random() < 0.2:
                        parts = parts[:1] + parts[3:]  # a full circle back to the start
                        ex, ey = x, y
                elif form < 0.8:
                    ex, ey = rng.uniform(-30, 30), rng.uniform(-30, 30)
                    half = math.dist((x, y), (ex, ey)) / 2
                    radius = half * opt([1.0000001, 1.5, 3, 1.01], [1, 0.9]) * pick([1, -1])
                    parts = [motion, f'X{ex:.4f}', f'Y{ey:.4f}', f'R{radius:.5f}']
                else:
                    ex, ey = rng.uniform(-30, 30), rng.uniform(-30, 30)
                    parts = [
                        motion,
                        f'X{ex:.4f}',
                        f'Y{ey:.4f}',
                        opt([f'R{45 + rng.random():.3f}'], ['R1 I1', 'K2', '', 'I1']),
                    ]
                if rng.random() < 0.3:
                    parts.append(f'Z{value(-4, 1)}')
                x, y = ex, ey
                arcs = True
            elif roll < 0.85:
                parts = [opt(['M3 S12000', 'M5', 'M8', 'M9', 'S800', 'G4 P0.5', 'M4 S300', 'G94', 'G54 G40 G49'],
                             ['G4 X1', 'M7 P1', 'G33', 'M98'])]  # fmt: skip
            elif roll < 0.9:
                parts = [opt(['G90', 'G90', 'G91 G1 X5 Y-2\nG90', 'G90.1', 'G91.1'], ['G90 G91', 'G90.1 G91.1', 'G91'])]
                if 'X5' in parts[0]:
                    x, y = (x + 5, y - 2) if x is not None and y is not None else (None, None)
                    arcs = False
                centre_absolute = 'G90.1' in parts[0] or (centre_absolute and 'G91.1' not in parts[0])
            elif roll < 0.95:
                parts = [opt(['(comment)', '; note', 'G1 X1 (inline) Y2', 'N10 G1 X2 Y3', 'g1 x4 y5', 'G1X6Y7F100', '((nested) text)'],
                             ['#1=2', 'G1 X[1+2]', 'G1 X1 X2', 'O100', 'G1 X1e3', '(unclosed', 'G1 X 1 Y'])]  # fmt: skip
                found = re.findall(r'([XY])(\d+)', re.sub(r'\([^()]*\)', '', parts[0].upper()))
                for axis, number in found:
                    x, y = (float(number), y) if axis == 'X' else (x, float(number))
                if found and parts[0].upper().startswith(('G1', 'N10 G1')):
                    arcs = False
            else:
                parts = [opt(['M30', 'M2', 'M02'], ['M30\nG1 X1 Y1'])]
            if rng.random() < risk[0] * 0.3:
                parts.append(pick(['A1', 'X1e12', 'Y' + '9' * 400, 'F-5', 'Q1', '@', 'X99999999', 'F' + '1' * 30]))
            lines.append(' '.join(part for part in parts if part))
            if lines[-1].startswith(('M30', 'M2', 'M02')) and rng.random() < 0.8:
                break
        newline = pick(['\n', '\r\n', '\n', '\r', '\x0b', '\x1e'])
        return newline.join(lines) + pick(['', '\n', newline])

    def hpgl():
        out = [opt(['IN', 'IN;SP1', 'DF', 'IN;DF;SP2'], ['IN 1', 'SP 300', 'SP 1.5', 'XX'])]
        for _ in range(rng.randint(1, 30)):
            roll = rng.random()
            if roll < 0.4:
                count = rng.randint(1, 6) * 2
                command = pick(['PD', 'PA', 'PR', 'PU', 'PD'])
                reach = 1000 if command == 'PR' else 40000
                numbers = [
                    str(rng.randint(-reach, reach)) if rng.random() < 0.7 else value(-4000, 4000) for _ in range(count)
                ]
                separator = pick([',', ' ', ', ', ' ,', ',  '])
                out.append(command + separator.join(numbers) if numbers else command)
            elif roll < 0.5:
                out.append(pick(['PD', 'PU', 'PA', 'PR']))
            elif roll < 0.62:
                out.append(pick(['AA', 'AR']) + ','.join([str(rng.randint(-4000, 4000)), str(rng.randint(-4000, 4000)),
                                                         opt([value(-360, 360, 1)], ['0', '400'])] + ([value(1, 5)] if rng.random() < 0.3 else [])))  # fmt: skip
            elif roll < 0.7:
                out.append('CI' + value(1, 4000, 1) + (pick([',5', '']) if rng.random() < 0.5 else ''))
            elif roll < 0.82:
                out.append(pick(['VS', 'FS']) + value(0, 60, 1) + pick(['', ',1', ',2', ',' + str(rng.randint(0, 3))]))
            elif roll < 0.9:
                out.append(opt(['SP' + str(rng.randint(0, 8)), 'SP'], ['SP-1', 'SP256']))
            else:
                out.append(
                    opt(['', ' ', 'pd1,2,3,4', 'IN'], ['LB text', 'PD 1,2,3', 'PA 1e5,1', 'VS -1', 'CI 0', 'AA 1,2'])
                )
        separator = pick([';', ';\n', ';', '; '])
        return separator.join(out) + pick(['', ';', ';\n'])

    def ini():
        unit = opt(['mm', 'inch', 'metric', 'Imperial', 'MM'], ['cm', '', 'furlong'])
        lines = ['[EMC]', 'VERSION = 1.1', '[TRAJ]']
        if rng.random() > risk[0] * 0.5:
            lines.append(f'LINEAR_UNITS = {unit}')
        if rng.random() < 0.5:
            lines.append('COORDINATES = ' + opt(['X Y Z', 'XYZ', 'X Y Z A'], ['X Z', 'A B']))
        for axis in rng.sample('XYZ', 3):
            if axis == 'Z' and rng.random() < 0.3:
                continue
            lines.append(pick([f'[AXIS_{axis}]', f'[axis_{axis.lower()}]', f'[[AXIS_{axis}]]']))
            inch = unit.lower() in ('inch', 'imperial')
            low = rng.uniform(-15, 0) if inch else rng.uniform(-400, 0)
            span = opt([rng.uniform(0.05, 75) if inch else rng.uniform(1, 1900)], [0, -5, 3000, 0.5])
            for key, text in (('MIN_LIMIT', f'{low:.3f}'), ('MAX_LIMIT', f'{low + span:.3f}'),
                              ('MAX_VELOCITY', value(1, 200)), ('MAX_ACCELERATION', value(10, 3000))):  # fmt: skip
                if rng.random() < 0.9:
                    text = opt([text, text + 'e0', f'{float(text):.2E}'], ['', 'fast', '1e999', '-3', '0x10'])
                    lines.append(f'{key} = {text}' + pick(['', ' ; note', ' # note']))
            if rng.random() < risk[0] * 0.2:
                lines.append('MIN_LIMIT = 1')
        if rng.random() < risk[0] * 0.2:
            lines.insert(1, pick(['#INCLUDE other.inc', '  # include x', '#INCLUDED = 1']))
        return '\n'.join(lines) + '\n'

    def long_run():
        # More than 100 contours: batched by layer and depth semantics; some near the limits.
        size = pick([101, 150, 400, 2999, 3001])
        lines = ['G21 G90']
        for index in range(size):
            x = (index % 50) * 3.0
            y = (index // 50) * 3.0
            depth = pick(['-1', '-1', '2']) if rng.random() < 0.05 else '-1'
            lines += [f'G0 X{x:g} Y{y:g}', f'G1 Z{depth} F300', f'G1 X{x + 1:g}', 'G0 Z5']
            if rng.random() < 0.01:
                lines.append(f'T{rng.randint(1, 3)} M6')
        return '\n'.join(lines)

    suffixes = {
        'gcode': ['.nc', '.gcode', '.NGC', '.cnc', '.tap', '.Nc'],
        'hpgl': ['.plt', '.hpgl', '.HPG'],
        'ini': ['.ini', '.INI'],
    }
    for index in range(count):
        risk[0] = pick([0.0, 0.0, 0.0, 0.01, 0.03, 0.2])
        roll = rng.random()
        if roll < 0.45:
            kind, text = 'gcode', gcode()
        elif roll < 0.75:
            kind, text = 'hpgl', hpgl()
        elif roll < 0.88:
            kind, text = 'ini', ini()
        elif roll < 0.92:
            kind, text = 'gcode', long_run()
        else:
            kind, text = 'gcode', ''
        name = pick(['part', 'job', 'dir/sub/part', 'C:\\\\cut\\\\part', 'a.b', '.hidden', 'x..y']) + pick(
            suffixes[kind]
        )
        payload = {'name': name, 'text': text, 'unit': pick(['mm', 'mm', 'in', 'inch', 'cm', 'm', 'ft'])}
        if roll >= 0.92:
            payload = pick([
                {'name': 'part.nc', 'text': 'G21\nG1 X1 Y1\n', 'unit': 'yard'},
                {'name': 'part.nc', 'text': '', 'unit': 'mm'},
                {'name': 'part.nc', 'text': 'G1 X1\0', 'unit': 'mm'},
                {'name': 'part.svg', 'text': 'G1 X1', 'unit': 'mm'},
                {'name': 'part.', 'text': 'G1 X1', 'unit': 'mm'},
                {'name': 'part.nc', 'text': '\ufeff\ufeffG21\nG0 X0 Y0\nG1 X3 Y4 F100'},
                {'name': 'part.nc', 'text': 'G21 (Ä)\nG0 X0 Y0\nG1 X3 Y4 F100', 'unit': 'mm'},
                {'name': 'part.dxf', 'text': '0\nEOF\n', 'unit': 'mm'},
                {'name': 'x' * 1025 + '.nc', 'text': 'G1 X1', 'unit': 'mm'},
                {'name': 7, 'text': 'G1 X1', 'unit': 'mm'},
                {'name': 'part.nc', 'text': 5},
                {'name': 'part.nc', 'text': 'G1 X1', 'unit': None},
                {'text': 'G1 X1'},
                [],
                'G1 X1',
                {'name': 'part.plt', 'text': 'IN;SP1;PU0,0;PD400,0,400,400;CI40000000;', 'unit': 'mm'},
                {'name': 'part.plt', 'text': ';'.join(['IN', 'SP1'] + [f'VS{v}' for v in range(17)]) + ';PD1,1,9,9', 'unit': 'mm'},
                {'name': 'part.nc', 'text': 'G21\nG0 X0 Y0\nG2 X0 Y0 I1999 J0 F100', 'unit': 'mm'},
                {'name': 'part.nc', 'text': 'G21\nG0 X0 Y0 Z1\nG1 X10 Z-2 F100\nG1 Y10\nG1 Z-60\nG1 X0', 'unit': 'mm'},
                {'name': 'part.nc', 'text': 'G20\nG0 X0 Y0\nG3 X1 Y1 R-1 F12.3456789', 'unit': 'mm'},
            ])  # fmt: skip
        if rng.random() < 0.03 and isinstance(payload, dict) and isinstance(payload.get('text'), str):
            payload['text'] = pick(['\ufeff', '\ufeff\ufeff', '']) + payload['text'] + pick(['', ' é', '\u2028G1 X1'])
        elif rng.random() < 0.15 and isinstance(payload, dict) and isinstance(payload.get('text'), str):
            payload['text'] = outside_ascii(payload['text'])
        yield payload


def _lzw(data):
    """LZW codes for data, as PDF's LZWDecode reads them, written by pypdf's encoder (PDF 1.7, 7.4.4.2)."""
    from pypdf._codecs._codecs import LzwCodec

    return LzwCodec().encode(data)


def _run_length(data):
    out = bytearray()
    for i in range(0, len(data), 100):
        chunk = data[i : i + 100]
        out += bytes([len(chunk) - 1]) + chunk
    return bytes(out + b'\x80')


def pdf_document(pages, rng, objects_in_streams=False, table=None):
    """A PDF of the given pages, written as PDF producers write them: each page a dict of its entries, with
    'content' (bytes or a list of bytes) and optional 'forms', 'states' and 'images'; inherited attributes
    on a parent node. The cross-reference is a table or a stream, with objects in an object stream."""
    import zlib

    objects = {}

    def new(body):
        number = len(objects) + 1
        objects[number] = body
        return number

    def stream(data, extra=b''):
        kind = rng.choice(['plain', 'flate', 'flate', 'hex', 'a85', 'lzw', 'rl', 'chain'])
        if kind == 'flate':
            data, extra = zlib.compress(data), extra + b'/Filter /FlateDecode'
        elif kind == 'hex':
            data, extra = data.hex().encode() + b'>', extra + b'/Filter /ASCIIHexDecode'
        elif kind == 'a85':
            data, extra = base64.a85encode(data, adobe=True), extra + b'/Filter /ASCII85Decode'
        elif kind == 'lzw':
            data, extra = _lzw(data), extra + b'/Filter /LZWDecode'
        elif kind == 'rl':
            data, extra = _run_length(data), extra + b'/Filter /RunLengthDecode'
        elif kind == 'chain':
            data, extra = (
                base64.a85encode(zlib.compress(data), adobe=True),
                extra + b'/Filter [/ASCII85Decode /FlateDecode]',
            )
        return ('stream', b'<< /Length %d %s >>' % (len(data), extra), data)

    page_numbers = []
    parent = new(None)
    for page in pages:
        resources = []
        for name, (form_content, form_extra) in page.get('forms', {}).items():
            resources.append(
                (b'XObject', name, new(stream(form_content, b'/Type /XObject /Subtype /Form ' + form_extra)))
            )
        for name, image in page.get('images', {}).items():
            resources.append(
                (
                    b'XObject',
                    name,
                    new(
                        stream(
                            b'\x00' * 4,
                            b'/Type /XObject /Subtype /Image /Width 2 /Height 2 /BitsPerComponent 8 /ColorSpace /DeviceGray',
                        )
                    ),
                )
            )
        for name, state in page.get('states', {}).items():
            resources.append((b'ExtGState', name, new(b'<< /Type /ExtGState ' + state + b' >>')))
        grouped = {}
        for category, name, number in resources:
            grouped.setdefault(category, []).append(b'/%s %d 0 R' % (name, number))
        resource_text = b'<< ' + b' '.join(b'/%s << %s >>' % (k, b' '.join(v)) for k, v in grouped.items()) + b' >>'
        content = page['content']
        if isinstance(content, list):
            contents = b'[' + b' '.join(b'%d 0 R' % new(stream(c)) for c in content) + b']'
        else:
            contents = b'%d 0 R' % new(stream(content))
        entries = page.get('entries', b'/MediaBox [0 0 144 180]')
        if page.get('no_resources'):
            body = b'<< /Type /Page /Parent %d 0 R %s /Contents %s >>' % (parent, entries, contents)
        else:
            body = b'<< /Type /Page /Parent %d 0 R %s /Resources %s /Contents %s >>' % (
                parent,
                entries,
                resource_text,
                contents,
            )
        page_numbers.append(new(body))
    inherited = pages[0].get('inherited', b'') if pages else b''
    objects[parent] = b'<< /Type /Pages /Count %d /Kids [%s] %s >>' % (
        len(page_numbers),
        b' '.join(b'%d 0 R' % n for n in page_numbers),
        inherited,
    )
    catalog = new(b'<< /Type /Catalog /Pages %d 0 R >>' % parent)
    head = b'%PDF-1.' + (b'5' if table is False else b'4') + b'\n%\xe2\xe3\xcf\xd3\n'
    out = bytearray(head)
    offsets = {}
    use_stream = table is False or (table is None and rng.random() < 0.4)
    packed = {}
    if use_stream and objects_in_streams:
        # Plain dictionaries go into one object stream.
        members = [n for n, body in objects.items() if not isinstance(body, tuple)]
        if members:
            header, data = [], b''
            for n in members:
                header.append(b'%d %d' % (n, len(data)))
                data += objects[n] + b'\n'
            first = b' '.join(header) + b'\n'
            container = len(objects) + 1
            objects[container] = (
                'stream',
                b'<< /Type /ObjStm /N %d /First %d /Length %d >>' % (len(members), len(first), len(first + data)),
                first + data,
            )
            for index, n in enumerate(members):
                packed[n] = (container, index)
    for number in sorted(objects):
        if number in packed:
            continue
        body = objects[number]
        offsets[number] = len(out)
        if isinstance(body, tuple):
            _, dictionary, data = body
            out += b'%d 0 obj\n%s\nstream\n' % (number, dictionary) + data + b'\nendstream\nendobj\n'
        else:
            out += b'%d 0 obj\n%s\nendobj\n' % (number, body)
    size = max(objects) + 1
    if use_stream:
        xref_number = size
        size += 1
        rows = [b'\x00\x00\x00\xff\xff']
        for n in range(1, size):
            if n == xref_number:
                rows.append(b'\x01' + len(out).to_bytes(3, 'big') + b'\x00')
            elif n in packed:
                rows.append(b'\x02' + packed[n][0].to_bytes(3, 'big') + bytes([packed[n][1]]))
            else:
                rows.append(b'\x01' + offsets[n].to_bytes(3, 'big') + b'\x00')
        data = b''.join(rows)
        predictor = rng.random() < 0.5
        if predictor:
            previous, encoded = bytes(5), b''
            for row in rows:
                encoded += b'\x02' + bytes((a - b) & 0xFF for a, b in zip(row, previous))
                previous = row
            data = zlib.compress(encoded)
            parms = b'/DecodeParms << /Predictor 12 /Columns 5 >>'
        else:
            data = zlib.compress(data)
            parms = b''
        start = len(out)
        out += (
            b'%d 0 obj\n<< /Type /XRef /Size %d /W [1 3 1] /Root %d 0 R /Filter /FlateDecode %s /Length %d >>\nstream\n'
            % (xref_number, size, catalog, parms, len(data))
        )
        out += data + b'\nendstream\nendobj\n'
    else:
        start = len(out)
        out += b'xref\n0 %d\n0000000000 65535 f \n' % size
        for n in range(1, size):
            out += b'%010d 00000 n \n' % offsets[n]
        out += b'trailer\n<< /Size %d /Root %d 0 R >>\n' % (size, catalog)
    out += b'startxref\n%d\n%%%%EOF\n' % start
    return bytes(out)


def pdf_import_cases(count, seed):
    """PDF pages for /api/pdf-import: strokes and fills of lines, rectangles and curves (c, v, y) under
    transformation matrices; closed paths; fills by the even-odd and nonzero rules, nested, separate,
    touching and overlapping; clipping by rectangles, convex and other paths; forms with matrices and boxes,
    nested; graphics states with alpha, soft masks and dashes; marked and optional content; text, shading
    and images; page trees with inherited boxes and resources, crop boxes, user units and rotations; every
    filter, cross-reference tables and streams with predictors and object streams; and damaged operators,
    operands, documents and payloads."""
    rng = random.Random(seed)
    pick = rng.choice
    risk = [0.0]

    def opt(good, bad=()):
        return pick(bad) if bad and rng.random() < risk[0] else pick(good)

    def n(low=-20, high=160):
        value = rng.uniform(low, high)
        return pick(['%d' % round(value), '%.1f' % value, '%.3f' % value, '%.2f' % value])

    def path():
        roll = rng.random()
        if roll < 0.35:
            return f'{n()} {n()} {n(1, 60)} {n(1, 60)} re'
        if roll < 0.55:
            corners = [(n(), n()) for _ in range(rng.randint(2, 6))]
            text = f'{corners[0][0]} {corners[0][1]} m ' + ' '.join(f'{x} {y} l' for x, y in corners[1:])
            return text + pick(['', ' h', ' h'])
        if roll < 0.8:
            text = f'{n()} {n()} m '
            for _ in range(rng.randint(1, 3)):
                op = pick(['c', 'c', 'v', 'y'])
                count = 6 if op == 'c' else 4
                text += ' '.join(n() for _ in range(count)) + f' {op} '
            return text + pick(['', 'h'])
        if roll < 0.9:
            # Nested squares for fill rules.
            x, y, size = rng.uniform(0, 80), rng.uniform(0, 80), rng.uniform(20, 60)
            inner = size * rng.uniform(0.2, 0.7)
            outer = f'{x:.2f} {y:.2f} {size:.2f} {size:.2f} re'
            if rng.random() < 0.5:
                return outer + f' {x + (size - inner) / 2:.2f} {y + (size - inner) / 2:.2f} {inner:.2f} {inner:.2f} re'
            # The inner one drawn the other way round.
            a, b = x + (size - inner) / 2, y + (size - inner) / 2
            return (
                outer
                + f' {a:.2f} {b:.2f} m {a:.2f} {b + inner:.2f} l {a + inner:.2f} {b + inner:.2f} l {a + inner:.2f} {b:.2f} l h'
            )
        return opt(['0 0 m 50 50 l'], ['m', '1 2 3 l', '0 0 m 1 2 3 4 c', '5 5 l', '1 2 3 4 5 v', 'h 1 1 l'])

    def paint():
        return opt(['S', 'f', 'f*', 'B', 'b', 's', 'B*', 'b*', 'F', 'n', 'S', 'f'], ['W n', 'W* n'])

    def content(depth=0, forms=()):
        ops = []
        for _ in range(rng.randint(1, 14)):
            roll = rng.random()
            if roll < 0.45:
                ops.append(f'{path()} {paint()}')
            elif roll < 0.55:
                ops.append(
                    pick(['q', 'Q', 'q', 'Q']) if rng.random() < risk[0] else 'q ' + f'{path()} {paint()}' + ' Q'
                )
            elif roll < 0.63:
                ops.append(
                    pick(
                        [
                            f'1 0 0 1 {n()} {n()} cm',
                            f'2 0 0 2 {n()} {n()} cm',
                            '0 1 -1 0 100 0 cm',
                            f'{n(0.5, 2)} 0.2 -0.1 {n(0.5, 2)} 5 5 cm',
                        ]
                    )
                )
            elif roll < 0.7:
                ops.append(pick([f'{n()} {n()} {n(5, 80)} {n(5, 80)} re W n', '10 10 m 90 10 l 50 90 l h W n', '10 10 m 90 10 l 10 90 l 90 90 l h W n',
                                 '0 0 144 180 re W* n', '20 20 m 80 20 l 80 80 l 20 80 l W n']))  # fmt: skip
            elif roll < 0.75:
                ops.append(pick(['[3 2] 0 d', '[] 0 d', '/GS0 gs', '/GS1 gs', '/GS2 gs', '/GS3 gs']))
            elif roll < 0.8:
                ops.append(
                    pick(['/OC /L1 BDC 0 0 m 10 10 l S EMC', '/Span BMC 0 0 m 20 5 l S EMC', '/OC /L1 BDC /F0 Do EMC'])
                )
            elif roll < 0.86:
                ops.append(
                    pick(
                        [
                            'BT /F1 12 Tf 10 10 Td (Label) Tj ET',
                            'BT 7 Tr (Clip) Tj ET 0 0 m 5 5 l S',
                            'BT [(A) 2 (B)] TJ ET',
                            "BT (x) ' ET",
                        ]
                    )
                )
            elif roll < 0.9:
                ops.append(pick(['/Sh0 sh', '/Im0 Do']))
            elif roll < 0.96 and forms:
                ops.append(f'q 1 0 0 1 {n(0, 40)} {n(0, 40)} cm /{pick(forms)} Do Q')
            else:
                ops.append(
                    opt(
                        ['0 0 m 1 1 l S'],
                        [
                            'Q',
                            'EMC',
                            'q q',
                            '/Missing gs',
                            '/Nothing Do',
                            '1 0 0 cm',
                            '/Span BMC',
                            '1e400 0 m 1 1 l S',
                            '99999999999 0 m 1 1 l S',
                        ],
                    )
                )
        return ' '.join(ops).encode()

    for index in range(count):
        risk[0] = pick([0.0, 0.0, 0.0, 0.05, 0.2])
        forms = {}
        if rng.random() < 0.4:
            for k in range(rng.randint(1, 2)):
                extra = pick([b'/BBox [0 0 40 40]', b'/BBox [0 0 40 40] /Matrix [2 0 0 1 0 0]', b'/BBox [5 5 60 30] /Matrix [1 0 0 1 3 4]',
                              b'/BBox [0 0 40 40] /Group << /S /Transparency >>', b'/BBox [-10 -10 200 200]'])  # fmt: skip
                forms[f'F{k}'.encode()] = (content(1), extra)
        states = {
            b'GS0': b'/CA 0.5 /ca 0.5',
            b'GS1': pick([b'/ca 0', b'/CA 0']),
            b'GS2': pick([b'/SMask /None', b'/SMask << /S /Luminosity >>']),
            b'GS3': pick([b'/D [[2 1] 0]', b'/D [[] 0]']),
        }
        page = {
            'content': content(0, [f.decode() for f in forms]),
            'forms': forms,
            'states': states,
            'images': {b'Im0': True} if rng.random() < 0.5 else {},
        }
        roll = rng.random()
        entries = [b'/MediaBox [0 0 144 180]']
        if roll < 0.15:
            entries.append(pick([b'/CropBox [18 36 126 144]', b'/CropBox [0 0 300 300]', b'/CropBox [50 50 40 40]']))
        if rng.random() < 0.2:
            entries.append(
                pick([b'/Rotate 90', b'/Rotate 180', b'/Rotate 270', b'/Rotate -90', b'/Rotate 45', b'/Rotate 450'])
            )
        if rng.random() < 0.1:
            entries.append(pick([b'/UserUnit 2', b'/UserUnit 0.5']))
        if rng.random() < 0.1:
            entries.append(b'/Annots [<< /Type /Annot /Subtype /Text /Rect [0 0 10 10] >>]')
        page['entries'] = b' '.join(entries)
        if rng.random() < 0.1:
            page['content'] = [page['content'], content(0)]
        pages = [page]
        if rng.random() < 0.2:
            pages.append({'content': content(0), 'entries': b'', 'inherited': b'', 'states': states})
            pages[0]['inherited'] = b'/MediaBox [0 0 200 200]'
            pages[1]['entries'] = b''
        if rng.random() < 0.05:
            pages[0]['no_resources'] = True
        data = pdf_document(pages, rng, objects_in_streams=rng.random() < 0.5)
        number = rng.randint(1, len(pages)) if rng.random() < 0.9 else pick([0, len(pages) + 1, True, 1.0, '1'])
        payload = {'content': base64.b64encode(data).decode('ascii'), 'page': number}
        if rng.random() < 0.04:
            payload = pick([
                {'content': 'not base64!'},
                {'content': base64.b64encode(b'not a PDF').decode()},
                {'content': base64.b64encode(b'%PDF-1.5\nmissing xref').decode()},
                {'content': base64.b64encode(data[: len(data) // 2]).decode()},
                {'page': 1},
                [],
                {'content': 5},
            ])  # fmt: skip
        yield payload


def bundle_archive(
    members, rng, *, comment=b'', comment_field=None, prefix=b'', absolute=False, tail=b'', zip64=None
):  # fmt: skip
    """A ZIP archive written field by field, so that any field can be what a damaged or unusual archive holds.
    Each member is a dict: name (bytes), data, and optionally method, level, flags, local_flags, crc, size,
    csize, version, external, extra, local_name, local_extra, comment and offset; zip64, the directory fields
    (size, csize, offset) moved into a ZIP64 extra field, with zip64_cut bytes cut from its end; and
    unicode_path, the name an Info-ZIP Unicode path field gives, with unicode_version and unicode_crc. zip64, a
    dict, writes a ZIP64 end record and its locator: extensible (data after the record), classic (the end
    record keeps its own fields rather than the largest values), locator_offset (in place of the record's), and
    disks (the locator's count)."""
    import zlib

    blob = bytearray(prefix)
    base = 0 if absolute else len(prefix)
    directory = bytearray()
    for m in members:
        name, raw, method = m['name'], m['data'], m.get('method', 8)
        if method == 8:
            packer = zlib.compressobj(m.get('level', 6), zlib.DEFLATED, -15)
            packed = packer.compress(raw) + packer.flush()
        else:
            packed = raw
        flags = m.get('flags', 0)
        crc, size, csize = m.get('crc', zlib.crc32(raw)), m.get('size', len(raw)), m.get('csize', len(packed))
        local_name, local_extra = m.get('local_name', name), m.get('local_extra', b'')
        offset = len(blob) - base
        blob += struct.pack(
            '<4s2B4HL2L2H', b'PK\x03\x04', 20, 0, m.get('local_flags', flags), method, 0, 0x21, crc, csize, size,
            len(local_name), len(local_extra),
        )  # fmt: skip
        blob += local_name + local_extra + packed
        extra, note = m.get('extra', b''), m.get('comment', b'')
        offset = m.get('offset', offset)
        moved = m.get('zip64', ())
        if moved:
            values = [v for field, v in (('size', size), ('csize', csize), ('offset', offset)) if field in moved]
            field = struct.pack('<%dQ' % len(values), *values)
            field = field[: len(field) - m.get('zip64_cut', 0)]
            extra += struct.pack('<2H', 1, len(field)) + field
            size = 0xFFFFFFFF if 'size' in moved else size
            csize = 0xFFFFFFFF if 'csize' in moved else csize
            offset = 0xFFFFFFFF if 'offset' in moved else offset
        if 'unicode_path' in m:
            field = struct.pack('<BL', m.get('unicode_version', 1), m.get('unicode_crc', zlib.crc32(name)))
            field += m['unicode_path']
            extra += struct.pack('<2H', 0x7075, len(field)) + field
        directory += struct.pack(
            '<4s4B4HL2L5H2L', b'PK\x01\x02', 20, 3, m.get('version', 20), 0, flags, method, 0, 0x21, crc, csize,
            size, len(name), len(extra), len(note), 0, 0, m.get('external', 0o100644 << 16), offset,
        )  # fmt: skip
        directory += name + extra + note
    start = len(blob) - base
    blob += directory
    entries, size_cd, offset_cd = len(members), len(directory), start
    if zip64 is not None:
        record_at = len(blob) - base
        extensible = zip64.get('extensible', b'')
        blob += struct.pack(
            '<4sQ2H2L4Q', b'PK\x06\x06', 44 + len(extensible), 45, 45, 0, 0, entries, entries, size_cd, offset_cd
        )  # fmt: skip
        blob += extensible
        blob += struct.pack(
            '<4sLQL', b'PK\x06\x07', 0, zip64.get('locator_offset', record_at), zip64.get('disks', 1)
        )  # fmt: skip
        if not zip64.get('classic'):
            entries, size_cd, offset_cd = 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF
    blob += struct.pack(
        '<4s4H2LH', b'PK\x05\x06', 0, 0, entries, entries, size_cd, offset_cd,
        len(comment) if comment_field is None else comment_field,
    )  # fmt: skip
    return bytes(blob + comment + tail)


def bundle_import_cases(count, seed):
    """LightBurn bundles for /api/import-bundle: archives of device, material, preference and project members
    with LightBurn and other suffixes, stored and deflated, names in UTF-8 and code page 437, unsafe and
    unprintable names, folders and links, encrypted, patched and newer-version entries, extra fields, comments,
    data before and after the archive, member text in UTF-8, with a signature, and in UTF-16 either way round,
    and damage of every kind: changed sizes, checksums, offsets and names, cut and flipped bytes, members that
    inflate too far, and payloads that are not bundles at all. Each archive is listed and then read member by
    member."""
    import zlib

    rng = random.Random(seed)
    pick = rng.choice
    suffixes = ['.lbdev', '.lbprefs', '.clb', '.lbrn', '.lbrn2', '.lbset', '.LBDEV', '.Clb', '.txt', '.lbart', '']
    folders = ['', 'devices/', 'materials/', 'Prefs/', 'a/b/', './', 'x//', 'deep/./er/']

    def name():
        roll = rng.random()
        stem = pick(['one', 'Laser', 'ref', 'my prefs', 'pé', 'job-2', 'été', '中', 'x'])
        text = pick(folders) + stem + pick(suffixes)
        if roll < 0.06:
            text = pick(['../wrong.clb', '/abs.lbdev', 'c:drive.lbset', 'back\\slash.clb', 'a/../b.lbrn', '.lbdev',
                         'dot.lbdev.', 'dir.lbdev/', 'folder/', 'x.lbdev/.', 'tab\there.clb', 'bell\x07.lbset',
                         'line .clb', 'z' * 509 + '.clb', 'y' * 600 + '.clb', 'soft­.lbdev',
                         'k.Klb', ' space.lbrn'])  # fmt: skip
        if rng.random() < 0.6 and all(ord(c) < 128 for c in text):
            return text.encode(), 0
        if rng.random() < 0.7:
            return text.encode('utf-8'), 0x800
        try:
            return text.encode('cp437'), 0
        except UnicodeEncodeError:
            return text.encode('utf-8'), 0  # read as code page 437, as an unflagged archive is

    def body():
        roll = rng.random()
        words = pick(['{"name": "Laser", "speed": %d}' % rng.randint(1, 900), '<Library><Material name="Ply"/></Library>',
                      'réglage = été → \U0001f600', '', 'a' * rng.randint(1, 600),
                      json.dumps({'layers': [rng.random() for _ in range(rng.randint(0, 40))]})])  # fmt: skip
        if roll < 0.45:
            return words.encode()
        if roll < 0.55:
            return b'\xef\xbb\xbf' + words.encode()
        if roll < 0.63:
            return words.encode('utf-16')
        if roll < 0.68:
            return b'\xfe\xff' + words.encode('utf-16-be')
        if roll < 0.72:
            return (
                b'\xff\xfe'
                + words.encode('utf-16-le')
                + pick([b'\x00', b'\x00\xd8', b'\x00\xdc\x41\x00', b'\x3d\xd8A\x00'])
            )
        if roll < 0.77:
            return words.encode() + pick(
                [b'\xff', b'\xc3', b'\xed\xa0\x80', b'\xf4\x90\x80\x80', b'\xc0\xaf', b'\xe2\x82']
            )
        if roll < 0.81:
            return words.encode() + b'\x00bin'
        if roll < 0.84:
            return '\x00'.encode('utf-16')
        if roll < 0.87:
            return bytes(rng.randrange(256) for _ in range(rng.randint(0, 64)))
        return (words * rng.randint(1, 8)).encode()

    def member():
        raw_name, flags = name()
        m = {
            'name': raw_name,
            'flags': flags,
            'data': body(),
            'method': pick([0, 8, 8, 8]),
            'level': pick([0, 1, 6, 9]),
        }
        roll = rng.random()
        if roll < 0.04:
            m['external'] = pick([(0o120777 << 16), (0o040755 << 16) | 0x10, 0, 0o060644 << 16, 0x10])
        elif roll < 0.07:
            m['flags'] |= pick([1, 8, 0x20, 0x40, 2, 4])
        elif roll < 0.09:
            m['method'] = pick([12, 14, 99, 9])
        elif roll < 0.11:
            m['version'] = pick([45, 63, 64, 99])
        elif roll < 0.15:
            m['extra'] = pick([b'\xfe\xca\x04\x00abcd', b'\x0a\x00\x20\x00' + bytes(32), b'\x01\x00\x08\x00' + bytes(8),
                               b'\x75\x70\x05\x00\x01abcd', b'\x00\x00\x09\x00abc', b'\xfe', b'ab\x01\x00x'])  # fmt: skip
        elif roll < 0.18:
            m['local_extra'] = pick([b'\x55\x54\x05\x00\x01abcd', bytes(rng.randint(1, 30))])
        elif roll < 0.21:
            m['comment'] = pick([b'note', 'kommentar é'.encode(), b'PK\x05\x06'])
        elif roll < 0.25:
            m['crc'] = rng.randrange(1 << 32)
        elif roll < 0.30:
            m['size'] = max(0, len(m['data']) + rng.randint(-5, 5))
        elif roll < 0.33:
            m['csize'] = rng.randint(0, 64)
        elif roll < 0.35:
            m['local_name'] = pick([b'other.clb', raw_name.upper(), raw_name + b'x', b''])
        elif roll < 0.37:
            m['local_flags'] = m['flags'] ^ 0x800
        elif roll < 0.38:
            m['size'] = pick([4_000_001, 4_000_000, 2**32 - 1])
        elif roll < 0.385:
            m['data'], m['method'] = bytes(rng.randint(4_000_001, 4_200_000)), 8
            m['size'] = pick([100, 4_000_000, len(m['data'])])
        elif roll < 0.43:
            m['zip64'] = pick([('size', 'csize', 'offset'), ('size', 'csize'), ('offset',), ('csize',), ('size',)])
            if rng.random() < 0.15:
                m['zip64_cut'] = rng.randint(1, 8)
        elif roll < 0.48:
            path = pick(
                ['réglages/été.lbdev', 'Laser.clb', '中/x.lbset', 'nul\x00cut.clb', 'back\\slash.lbrn', '', 'a.txt']
            )
            m['unicode_path'] = path.encode() if rng.random() < 0.9 else b'\xff\xfe.clb'
            if rng.random() < 0.15:
                m['unicode_version'] = pick([0, 2])
            if rng.random() < 0.15:
                m['unicode_crc'] = rng.randrange(1 << 32)
            if rng.random() < 0.1:
                m['unicode_path'] = b''
        return m

    def damaged(blob):
        blob = bytearray(blob)
        roll = rng.random()
        if roll < 0.35 and blob:
            for _ in range(rng.randint(1, 4)):
                at = rng.randrange(len(blob))
                blob[at] ^= 1 << rng.randrange(8)
        elif roll < 0.55:
            del blob[rng.randint(0, len(blob)) :]
        elif roll < 0.7 and len(blob) > 22:
            # The end record's sizes and offsets.
            at = len(blob) - 22 + pick([8, 10, 12, 16])
            blob[at : at + 2] = struct.pack('<H', rng.randrange(1 << 16))
        elif roll < 0.85:
            at = rng.randint(0, len(blob))
            blob[at:at] = bytes(rng.randrange(256) for _ in range(rng.randint(1, 30)))
        else:
            blob = blob[: rng.randint(0, len(blob))] + b'PK\x05\x06' + bytes(rng.randint(0, 30))
        return bytes(blob)

    def payloads(blob, members):
        content = base64.b64encode(blob).decode()
        yield {'content': content}
        for index in range(min(members, 6)):
            yield {'content': content, 'index': index}
        if rng.random() < 0.2:
            yield {'content': content, 'index': pick([True, -1, members, 300, '0', None, 1.0, 10**30, 0.5])}

    made = 0
    while made < count:
        roll = rng.random()
        if roll < 0.05:
            cases = [
                [], 'text', {'index': 0}, {'content': 5}, {'content': 'not base64!'}, {'content': 'AQID='},
                {'content': ''}, {'content': 'éAAA'}, {'content': base64.b64encode(b'not a zip archive').decode()},
                {'content': base64.b64encode(b'PK\x05\x06' + bytes(18)).decode()},
                {'content': base64.b64encode(b'PK\x05\x06' + bytes(10)).decode()},
            ]  # fmt: skip
            if made < 2:
                cases.append({'content': 'A' * 5_333_340})
                cases.append({'content': base64.b64encode(bytes(4_000_001)).decode()})
            for payload in cases:
                made += 1
                yield payload
            continue
        if roll < 0.15:
            # An archive as zipfile writes it.
            buffer = io.BytesIO()
            with zipfile.ZipFile(buffer, 'w', compression=pick([zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED])) as archive:
                for _ in range(rng.randint(0, 6)):
                    raw_name, _ = name()
                    info = zipfile.ZipInfo(raw_name.decode('utf-8', 'replace'))
                    if rng.random() < 0.1:
                        info.external_attr = 0o120777 << 16
                    archive.writestr(info, body(), compress_type=pick([zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED]))
                if rng.random() < 0.2:
                    archive.comment = pick([b'bundle', b'PK\x05\x06', bytes(40)])
            blob, total = buffer.getvalue(), 6
        else:
            many = rng.random() < 0.03
            members = [member() for _ in range(rng.randint(257, 262) if many else rng.randint(0, 7))]
            if rng.random() < 0.04 and members:
                for m in members:
                    m['size'] = pick([3_999_999, 4_000_000])
            if rng.random() < 0.05 and len(members) > 1:
                members[1]['offset'] = 0  # two entries at one header
            if rng.random() < 0.03 and members:
                members[0]['offset'] = 2**32 - 10
            prefix = pick([b'', b'', b'', b'MZ stub ' * rng.randint(1, 8)])
            comment = pick([b'', b'', b'LightBurn bundle', b'PK\x05\x06xx', bytes(rng.randint(0, 80))])
            zip64 = None
            if rng.random() < 0.1:
                zip64 = {}
                roll = rng.random()
                if roll < 0.15:
                    zip64['extensible'] = bytes(rng.randint(1, 24))
                elif roll < 0.25:
                    zip64['classic'] = True
                elif roll < 0.32:
                    zip64['locator_offset'] = rng.randrange(1 << rng.choice([8, 16, 64]))
                elif roll < 0.36:
                    zip64['disks'] = pick([0, 2])
            blob = bundle_archive(
                members, rng, comment=comment, prefix=prefix, absolute=rng.random() < 0.5,
                comment_field=pick([None, None, None, 0, 200]), tail=pick([b'', b'', b'junk', b'\x00\x00']),
                zip64=zip64,
            )  # fmt: skip
            if rng.random() < 0.02:
                blob = blob[:-22] + b'PK\x06\x07' + bytes(16) + blob[-22:]
            total = len(members)
            if rng.random() < 0.3:
                blob = damaged(blob)
        for payload in payloads(blob, total):
            made += 1
            yield payload


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
    parser.add_argument('--solid-cad', type=int, default=300, help='generated parametric solid histories and sections')
    parser.add_argument('--cad-script', type=int, default=1000, help='generated CAD scripts for the script preview')
    parser.add_argument('--program-import', type=int, default=3000, help='generated G-code, HP-GL and INI drawings')
    parser.add_argument('--pdf-import', type=int, default=1500, help='generated PDF pages')
    parser.add_argument('--bundle-import', type=int, default=3000, help='generated LightBurn bundle requests')
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
        if shared:
            for kind, data in solid_cad_cases(args.solid_cad, 103):
                line = preview_reference(kind, data)
                if line is not None:
                    lines.setdefault(line, 'solid-cad')
            from studio.scad_geometry import _request as script_request

            for payload in cad_script_cases(args.cad_script, 104):
                line = preview_reference('cad_script', script_request(payload))
                if line is not None:
                    lines.setdefault(line, 'cad-script')
        for payload in program_import_cases(args.program_import, 105):
            line = preview_reference('program_import', payload)
            if line is not None:
                lines.setdefault(line, 'program-import')
        for payload in pdf_import_cases(args.pdf_import, 106):
            line = preview_reference('pdf_import', payload)
            if line is not None:
                lines.setdefault(line, 'pdf-import')
        for payload in bundle_import_cases(args.bundle_import, 107):
            line = preview_reference('bundle_import', payload)
            if line is not None:
                lines.setdefault(line, 'bundle-import')
        tally = collections.Counter()
        declined = collections.Counter()
        failures = []
        own_rules = collections.Counter()
        own_faults = []

        def answer_own(lines):
            """Put the cases the core declined with the reference's rules to it again with its own: each must be
            answered, as a program or a refusal, never declined or failed inside."""
            if not lines:
                return
            cases.write_text(''.join(line + '\n' for line in lines), encoding='utf-8')
            answers = subprocess.run(
                [str(args.cli), 'batch', '--own', str(cases)],
                capture_output=True,
                text=True,
                encoding='utf-8',
                check=True,
            ).stdout.split('\n')[:-1]
            assert len(answers) == len(lines), 'The native core did not answer every case by its own rules.'
            for line, answer in zip(lines, answers, strict=True):
                got = json.loads(answer)
                if 'ok' in got or 'error' in got:
                    for rule in got.get('own', ['(none named)']):
                        own_rules[rule] += 1
                else:
                    own_faults.append((json.loads(line), got))

        def compare(lines):
            """Put a set of cases to the native core and count how each was answered."""
            cases.write_text(''.join(line + '\n' for line in lines), encoding='utf-8')
            # One answer a line; split at newlines alone, as a member's text may hold other line separators.
            answers = subprocess.run(
                [str(args.cli), 'batch', str(cases)], capture_output=True, text=True, encoding='utf-8', check=True
            ).stdout.split('\n')[:-1]
            assert len(answers) == len(lines), 'The native core did not answer every case.'
            by_own_rules = []
            for (line, origin), answer in zip(lines.items(), answers, strict=True):
                case, got = json.loads(line), json.loads(answer)
                expect = case['expect']
                subject = case.get(
                    'project', case.get('job', case.get('tool_changes', case.get('fixtures', case.get('stl'))))
                )
                if subject is None:
                    subject = case.get('solid_cad', case.get('solid_cad_section', case.get('cad_script')))
                if subject is None:
                    subject = case.get('program_import', case.get('pdf_import', case.get('bundle_import')))
                if 'relief' in case and 'ok' in got:
                    got = {'ok': where_computed_left_out(got['ok'])}
                if not shared and 'project' in case and uses_polygon_operation(subject):
                    tally[origin, 'left out'] += 1
                elif 'declined' in got:
                    tally[origin, 'declined'] += 1
                    declined[got['declined']] += 1
                    by_own_rules.append(line)
                elif got.get('error') == NOT_ASCII_ROTARY and not json.dumps(subject, ensure_ascii=False).isascii():
                    # Defined by the core: a rotary source is ASCII. The reference read digits and spaces of
                    # other scripts there and copied them into the program a controller runs.
                    tally[origin, 'defined'] += 1
                elif 'raises' in expect:
                    # The reference stopped with something other than a rejection; there is no behaviour to match.
                    tally[origin, 'reference raised ' + expect['raises']] += 1
                    if 'invalid' not in got and 'error' not in got:
                        failures.append((subject, expect, got))
                elif strict_equal(expect, got):
                    tally[origin, 'program' if 'ok' in got else 'rejection'] += 1
                elif origin == 'bundle-import' and bundle_defined(subject, expect, got):
                    tally[origin, 'defined'] += 1
                else:
                    failures.append((subject, expect, got))
            answer_own(by_own_rules)

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
    for origin in (
        *origins,
        'tool-changes',
        'fixtures',
        'stl',
        'solid-cad',
        'cad-script',
        'program-import',
        'pdf-import',
        'bundle-import',
    ):
        total = sum(n for (o, _), n in tally.items() if o == origin)
        if not total:
            continue
        same = tally[origin, 'program'] + tally[origin, 'rejection']
        print(
            f'{origin:12s} {total:5d} cases: {tally[origin, "program"]} programs and {tally[origin, "rejection"]} rejections'
            f' reproduced ({same * 100 / total:.1f}%), {tally[origin, "declined"]} declined as not ported'
            + (f', {tally[origin, "defined"]} defined by the core' if tally[origin, 'defined'] else '')
            + (f', {tally[origin, "left out"]} with polygon operations NOT COMPARED' if not shared else '')
        )
    if declined:
        print(
            'Declined with the reference\'s rules: ' + ', '.join(f'{name} ({n})' for name, n in declined.most_common())
        )
    if own_rules:
        print('Answered by the core\'s own rules: ' + ', '.join(f'{name} ({n})' for name, n in own_rules.most_common()))
    for case, got in own_faults[:5]:
        print(
            'NOT ANSWERED by the core\'s own rules\n  case', json.dumps(case)[:300], '\n  native', json.dumps(got)[:300]
        )
    if own_faults:
        raise SystemExit(f'{len(own_faults)} declined cases were not answered by the core\'s own rules.')
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
    if shared and args.solid_cad and not tally['solid-cad', 'program']:
        raise SystemExit('No solid was rebuilt: the comparison did not exercise the parametric history.')
    if shared and args.cad_script and not tally['cad-script', 'program']:
        raise SystemExit('No CAD script was previewed: the comparison did not exercise the script engine.')
    if args.pdf_import and not tally['pdf-import', 'program']:
        raise SystemExit('No PDF page was read: the comparison did not exercise the PDF reader.')
    if args.bundle_import and not tally['bundle-import', 'program']:
        raise SystemExit('No bundle member was read: the comparison did not exercise the bundle reader.')
    if args.program_import and not tally['program-import', 'program']:
        raise SystemExit('No drawing was imported: the comparison did not exercise the program reader.')
    if not shared:
        print(
            'PASS for the cases compared; offsets, pockets, fills and V-carves were NOT COMPARED (no compiled extension).'
        )
        return
    print('PASS the native core reproduces or declines every case; none differs from the Python reference.')


if __name__ == '__main__':
    main()
