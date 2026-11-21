"""Compare the Python-free native core with the Python reference.

    cmake -S native -B build/native && cmake --build build/native --config Release
    python validation/native_core_parity.py --cli build/native/kerfkite-cam

Three comparisons, all against values computed by Python in this run:

1. Number text and arithmetic: fixed-point formatting, round() and repr() for several hundred thousand
   values, including decimal ties and values next to them; and math.dist and sum(), which Python computes
   with more care than the plain formulas.
2. Recorded jobs: every studio.cam.generate call the test suite makes, replayed through the native core.
3. Generated edge cases: projects with one to three settings replaced by unusual values, so that the
   reference's error messages are compared as well as its programs.

For each job the native core must either reproduce the reference exactly (program lines, preview segments,
length, time, bounds and warnings, or the same error message) or decline it as not ported yet. A different
answer is a failure. The report gives the share of cases reproduced, which is the measure of the port.
"""

import argparse
import collections
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
    parser.add_argument('--skip-suite', action='store_true', help='do not replay the calls made by the test suite')
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

    from native_core_recorder import reference
    from studio.cam import generate

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
        for project in generated_cases(args.generated, 87):
            line, _ = reference(generate, project)
            if line is not None:
                lines.setdefault(line, 'generated')
        cases.write_text(''.join(line + '\n' for line in lines), encoding='utf-8')
        answers = subprocess.run(
            [str(args.cli), 'batch', str(cases)], capture_output=True, text=True, encoding='utf-8', check=True
        ).stdout.splitlines()
    assert len(answers) == len(lines), 'The native core did not answer every case.'

    tally = collections.Counter()
    declined = collections.Counter()
    failures = []
    for (line, origin), answer in zip(lines.items(), answers, strict=True):
        case, got = json.loads(line), json.loads(answer)
        expect = case['expect']
        if 'declined' in got:
            tally[origin, 'declined'] += 1
            declined[got['declined']] += 1
        elif 'raises' in expect:
            # The reference stopped with something other than a rejection; there is no behaviour to match.
            tally[origin, 'reference raised ' + expect['raises']] += 1
            if 'invalid' not in got and 'error' not in got:
                failures.append((case['project'], expect, got))
        elif strict_equal(expect, got):
            tally[origin, 'program' if 'ok' in got else 'rejection'] += 1
        else:
            failures.append((case['project'], expect, got))
    for origin in ('suite', 'generated'):
        total = sum(n for (o, _), n in tally.items() if o == origin)
        if not total:
            continue
        same = tally[origin, 'program'] + tally[origin, 'rejection']
        print(
            f'{origin:9s} {total:5d} cases: {tally[origin, "program"]} programs and {tally[origin, "rejection"]} rejections'
            f' reproduced ({same * 100 / total:.1f}%), {tally[origin, "declined"]} declined as not ported'
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
    print('PASS the native core reproduces or declines every case; none differs from the Python reference.')


if __name__ == '__main__':
    main()
