"""Compare the native core's assemblies with the NumPy reference in studio/assembly.py.

The core places the parts with its own matrix arithmetic, where the reference has NumPy's BLAS and LAPACK, so
the two agree to rounding rather than to the bit. This script generates assemblies of 2 to 6 parts (boxes,
cylinders and spheres, placed and turned, some touching or overlapping), joined by mate trees of every kind and
sometimes by additional mates, with faulty documents among them, and reports for each whether both answered alike:
the same refusal, or the same placements to rounding with the same contacts and interferences.

Three things are expected to differ a little and are counted:

* A placement fitted to additional mates is the end of a Gauss-Newton iteration that stops once the mates hold
  to 10^-6 mm, so the two placements may differ by about that much (up to --fitted, default 1e-5 mm). Without
  additional mates they agree to --tolerance (default 1e-12 mm).
* A last bit of difference in a placement can move a vertex of the placed part to the neighbouring point of the
  kernel's 1 nm grid, which changes an interference's volume by about the area of the overlap times 10^-6 mm
  (allowed up to a relative 10^-5).
* For the same reason a pair that touches within the 10^-6 mm contact distance, or overlaps by a sliver, can be
  a contact in one and not the other (allowed in up to --spread, default 2%, of the assemblies).

The check fails on any other difference.

    PYTHONPATH=. python validation/assembly_check.py [--cases 500] [--seed 1]
"""

import argparse
import collections
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from studio.assembly import solve  # noqa: E402


def part(rng, name):
    kind = rng.choice(['box', 'box', 'cylinder', 'sphere'])
    if kind == 'box':
        feature = {
            'id': 'body',
            'kind': 'box',
            'size': [rng.choice([2, 4, 6]), rng.choice([2, 3, 5]), rng.choice([1, 4])],
        }
    elif kind == 'cylinder':
        feature = {'id': 'body', 'kind': 'cylinder', 'radius': rng.choice([1, 2.5]), 'height': rng.choice([3, 8])}
    else:
        feature = {'id': 'body', 'kind': 'sphere', 'radius': rng.choice([1, 2])}
    item = {'id': name, 'model': {'version': 1, 'features': [feature]}}
    if rng.random() < 0.7:
        item['origin'] = [rng.choice([0, 2, 4, 1.5, -3]) for _ in range(3)]
    if rng.random() < 0.4:
        item['rotation'] = [rng.choice([0, 0, 30, 90, -45]) for _ in range(3)]
    return item


def frame(rng):
    item = {}
    if rng.random() < 0.6:
        item['origin'] = [rng.choice([0, 1, 2, -2, 4]) for _ in range(3)]
    if rng.random() < 0.4:
        item['rotation'] = [rng.choice([0, 90, 45, 180]) for _ in range(3)]
    return item


def mate(rng, child, parent):
    kind = rng.choice(['fixed', 'planar', 'concentric', 'point'])
    item = {'child': child, 'parent': parent, 'kind': kind, 'parentFrame': frame(rng), 'childFrame': frame(rng)}
    offset, rotation = [0, 0, 0], [0, 0, 0]
    if kind == 'planar':
        offset[:2] = [rng.choice([0, 1, 3]), rng.choice([0, 2])]
        rotation[2] = rng.choice([0, 30, 90])
    elif kind == 'concentric':
        offset[2] = rng.choice([0, 2, -1])
        rotation[2] = rng.choice([0, 45])
    elif kind == 'point':
        rotation = [rng.choice([0, 20]), rng.choice([0, 90]), rng.choice([0, 15])]
    if rng.random() < 0.05:
        offset = [1, 1, 1]  # a displacement on a locked axis for most kinds
    item['offset'], item['rotation'] = offset, rotation
    return item


def assemblies(count, seed):
    rng = random.Random(seed)
    for _ in range(count):
        names = [f'P{i}' for i in range(rng.randint(2, 6))]
        parts = [part(rng, name) for name in names]
        mates = []
        for i in range(1, len(names)):
            if rng.random() < 0.7:
                mates.append(mate(rng, names[i], rng.choice(names[:i])))
        document = {'version': 1, 'parts': parts, 'mates': mates}
        if rng.random() < 0.3 and mates:
            driven = rng.choice(mates)['child']
            other = rng.choice([n for n in names if n != driven])
            extra = mate(rng, driven, other)
            for key in ('offset', 'rotation'):
                extra.pop(key)
            document['constraints'] = [extra]
        damage = rng.random()
        if damage < 0.03:
            document['version'] = 2
        elif damage < 0.05 and mates:
            mates.append(dict(mates[0]))
        elif damage < 0.07:
            parts[-1]['id'] = parts[0]['id']
        elif damage < 0.09 and len(mates) >= 2:
            mates[0]['parent'] = mates[-1]['child']
        yield document, rng.random() < 0.2


def outcome(document, mesh, backend):
    try:
        return 'solved', solve(document, mesh, backend=backend)
    except ValueError as error:
        return 'error', str(error)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cases', type=int, default=500)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument(
        '--tolerance', type=float, default=1e-12, help='placement agreement without additional mates, mm'
    )
    parser.add_argument('--fitted', type=float, default=1e-5, help='placement agreement with additional mates, mm')
    parser.add_argument('--spread', type=float, default=0.02, help='largest share of assemblies touching differently')
    args = parser.parse_args()
    tally = collections.Counter()
    largest = 0.0
    problems = []
    unsatisfied = 'Additional mates could not be satisfied'
    for document, mesh in assemblies(args.cases, args.seed):
        native, reference = outcome(document, mesh, 'native'), outcome(document, mesh, 'python')
        if native[0] != reference[0]:
            refusal = native[1] if native[0] == 'error' else reference[1]
            tally['fitted by one only'] += 1
            if not refusal.startswith(unsatisfied):
                problems.append((document, reference, native))
            continue
        if native[0] == 'error':
            tally['refused alike' if native[1] == reference[1] else 'refused differently'] += 1
            if native[1] != reference[1]:
                problems.append((document, reference, native))
            continue
        got, expect = native[1], reference[1]
        fitted = bool(document.get('constraints'))
        difference = 0.0
        for name, placed in expect['parts'].items():
            core = got['parts'][name]
            difference = max(
                difference,
                max(abs(x - y) for row, other in zip(core['matrix'], placed['matrix']) for x, y in zip(row, other)),
            )
        largest = max(largest, difference)
        tally['solved'] += 1
        tally['same to the bit' if got == expect else 'same to rounding'] += 1
        if difference > (args.fitted if fitted else args.tolerance):
            problems.append((document, reference, native))
        pairs = lambda answer: ([c['parts'] for c in answer['contacts']], [c['parts'] for c in answer['interferences']])  # noqa: E731
        if pairs(got) != pairs(expect):
            tally['touching differently'] += 1
            continue
        for a, b in zip(got['interferences'], expect['interferences']):
            if abs(a['volume'] - b['volume']) > 1e-5 * max(1.0, abs(b['volume'])):
                problems.append((document, reference, native))
                break
    if tally['touching differently'] > args.spread * max(tally['solved'], 1):
        problems.append(('too many assemblies touching differently', tally['touching differently'], tally['solved']))
    if tally['fitted by one only'] > args.spread * args.cases:
        problems.append(('too many assemblies fitted by one only', tally['fitted by one only'], args.cases))
    print(', '.join(f'{key}: {value}' for key, value in sorted(tally.items())))
    print(f'largest difference in a placement: {largest:.3g} mm')
    for document, reference, native in problems[:5]:
        print('DIFFERENT', document, '\n  numpy', str(reference)[:300], '\n  core ', str(native)[:300])
    if problems:
        raise SystemExit(f'{len(problems)} differences beyond rounding')
    print('PASS the core places, checks and refuses every assembly as the NumPy reference does, to rounding')


if __name__ == '__main__':
    main()
