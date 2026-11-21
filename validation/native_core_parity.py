"""Compare the Python-free native core with the Python reference.

    cmake -S native -B build/native && cmake --build build/native --config Release
    python validation/native_core_parity.py --cli build/native/kerfkite-cam

Three comparisons, all against values computed by Python in this run:

1. Number text: fixed-point formatting, round() and repr() for several hundred thousand values, including
   decimal ties and values next to them.
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
    parser.add_argument('--generated', type=int, default=6000, help='generated edge cases')
    parser.add_argument('--skip-suite', action='store_true', help='do not replay the calls made by the test suite')
    args = parser.parse_args()
    if not args.cli.is_file():
        raise SystemExit(f'kerfkite-cam not found: {args.cli}')

    compared, unwritten = number_text(args.cli, args.numbers, 86)
    print(f'PASS number text: {compared} values match Python ({unwritten} beyond the exact range, none job-sized)')

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
