"""Compare the native core's sketch constraint solver with the NumPy reference in studio/sketch_constraints.py.

The core's solver is the reference's Gauss-Newton iteration statement for statement, with its own least squares
(native/core/src/least_squares.hpp) where the reference has NumPy's LAPACK and BLAS. The two therefore agree
to rounding, not to the bit. This script generates constrained sketches (polygons of 3 to 12 vertices with
fixed points, distances, parallel, perpendicular, angle and equal relations, some coordinates driven by
expressions, some sets contradictory, redundant or degenerate) and reports, for each:

* whether both solved it or both refused it, and with the same message;
* for a solved sketch, whether the diagnostics (degrees of freedom, redundant relations, iterations) are the
  same, the largest difference between the vertices, and whether every vertex lands on the same point of the
  kernels' 1 nm grid, which is what the solid and its outline are built from.

    PYTHONPATH=. python validation/sketch_constraint_check.py [--cases 2000] [--seed 1]

Where a system is ill-conditioned the two iterations can take different paths, as NumPy's own answer does from
one BLAS library to another: then the diagnostics may differ (an iteration more, a singular value either side of
the rank tolerance), one solver may converge within its 40 iterations where the other gives up, and two that give
up may name different relations as the one they could not satisfy. These are counted and reported. The check
fails on anything else: a vertex of a sketch both solved that differs by more than --tolerance (default 1e-6 mm,
the kernels' grid), a refusal other than "Cannot satisfy" that differs, more than --spread (default 1%) of the
sketches solved by one solver only, or more than that share of the solved sketches with other diagnostics.
"""

import argparse
import collections
import math
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from studio.sketch_constraints import solve_profile  # noqa: E402
from studio.solid_cad import Dimensions  # noqa: E402


def sketches(count, seed):
    rng = random.Random(seed)
    for _ in range(count):
        n = rng.randint(3, 12)
        turn = rng.uniform(0, math.tau)
        radius = rng.choice([5, 10, 40])
        points = [
            [
                round(radius * math.cos(turn + math.tau * k / n) + rng.uniform(-1, 1), 3),
                round(radius * math.sin(turn + math.tau * k / n) + rng.uniform(-1, 1), 3),
            ]
            for k in range(n)
        ]
        parameters = {'w': rng.choice([6, 12, 20]), 'a': rng.choice([30, 45, 60, 90])}
        if rng.random() < 0.2:
            points[0] = ['w', points[0][1]]
        constraints = []
        for _ in range(rng.randint(1, min(2 * n, 16))):
            kind = rng.choice(['fixed', 'distance', 'parallel', 'perpendicular', 'angle', 'equal', 'equal', 'distance'])
            if kind == 'fixed':
                i = rng.randrange(n)
                constraints.append(
                    {'kind': kind, 'point': i, 'value': list(points[i]) if rng.random() < 0.8 else [0, 0]}
                )
            elif kind == 'distance':
                a, b = rng.sample(range(n), 2)
                constraints.append({'kind': kind, 'points': [a, b], 'value': rng.choice(['w', 5, 8.5, 'w / 2', 30])})
            else:
                a, b = rng.sample(range(n), 2)
                relation = {'kind': kind, 'edges': [a, b]}
                if kind == 'angle':
                    relation['value'] = rng.choice(['a', 30, 120, -45, 0, 180])
                constraints.append(relation)
        damage = rng.random()
        if damage < 0.03:
            constraints.append({'kind': 'distance', 'points': [0, 0], 'value': 1})
        elif damage < 0.05:
            constraints.append({'kind': 'twist'})
        elif damage < 0.07:
            points[1] = list(points[0])
        yield {'points': points, 'constraints': constraints}, parameters


def outcome(feature, parameters, backend):
    try:
        points, report = solve_profile(feature, Dimensions(parameters), backend=backend)
    except ValueError as error:
        return 'error', str(error)
    return 'solved', (points, report)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cases', type=int, default=2000)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--tolerance', type=float, default=1e-6, help='largest allowed vertex difference, mm')
    parser.add_argument('--spread', type=float, default=0.01, help='largest share of path-dependent outcomes')
    args = parser.parse_args()
    tally = collections.Counter()
    largest = 0.0
    problems = []
    unsatisfied = 'Cannot satisfy sketch constraint'
    for feature, parameters in sketches(args.cases, args.seed):
        native, reference = outcome(feature, parameters, 'native'), outcome(feature, parameters, 'python')
        if native[0] != reference[0]:
            tally['solved by one only'] += 1
            refusal = native[1] if native[0] == 'error' else reference[1]
            if not refusal.startswith(unsatisfied):
                problems.append((feature, reference, native))
            continue
        if native[0] == 'error':
            if native[1] == reference[1]:
                tally['refused alike'] += 1
            elif native[1].startswith(unsatisfied) and reference[1].startswith(unsatisfied):
                tally['refused, naming another relation'] += 1
            else:
                tally['refused differently'] += 1
                problems.append((feature, reference, native))
            continue
        (got, got_report), (expect, expect_report) = native[1], reference[1]
        difference = max(abs(a - b) for p, q in zip(got, expect, strict=True) for a, b in zip(p, q, strict=True))
        largest = max(largest, difference)
        grid = all(
            round(a * 1e6) == round(b * 1e6)
            for p, q in zip(got, expect, strict=True)
            for a, b in zip(p, q, strict=True)
        )
        tally['solved'] += 1
        tally['same diagnostics' if got_report == expect_report else 'other diagnostics'] += 1
        tally['same to the bit' if got == expect else 'same grid points' if grid else 'other grid points'] += 1
        if difference > args.tolerance:
            problems.append((feature, reference, native))
    if tally['solved by one only'] > args.spread * args.cases:
        problems.append(('too many sketches solved by one solver only', tally['solved by one only'], args.cases))
    if tally['other diagnostics'] > args.spread * max(tally['solved'], 1):
        problems.append(
            ('too many solved sketches with other diagnostics', tally['other diagnostics'], tally['solved'])
        )
    print(', '.join(f'{key}: {value}' for key, value in sorted(tally.items())))
    print(f'largest vertex difference: {largest:.3g} mm')
    for feature, reference, native in problems[:5]:
        print('DIFFERENT', feature, '\n  numpy', reference, '\n  core ', native)
    if problems:
        raise SystemExit(f'{len(problems)} differences beyond what ill-conditioning explains')
    print('PASS the core solves or refuses the sketches as the NumPy reference does, to rounding')


if __name__ == '__main__':
    main()
