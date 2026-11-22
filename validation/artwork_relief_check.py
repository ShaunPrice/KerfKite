"""Compare the native core's artwork relief height field with the Shapely reference on generated artwork.

Every case is a set of closed contours (rectangles, some of them on the sampling grid's cell edges, turned
rectangles, polygons standing for circles, stars, outlines with holes, and contours that cross one another)
sampled raised or recessed on a grid of 16 to 128 points. For each case both height fields are made; a cell
where they differ is counted as a boundary case when the artwork's outline passes within two millionths of a
millimetre of the cell's edge, which is where the kernel's grid of a millionth of a millimetre may decide
otherwise than Shapely's doubles, and as a difference otherwise. Requests that are refused must be refused
with the reference's message. Synthetic artwork only.

    python validation/artwork_relief_check.py [--cases 300] [--seed 2026]
"""

import argparse
import math
import random
import sys
import time

from shapely.geometry import box

from studio.artwork_relief import _reference_raster, raster
from studio.operations import region


def regular(cx, cy, r, sides, turn=0.0):
    points = [
        [cx + r * math.cos(turn + 2 * math.pi * k / sides), cy + r * math.sin(turn + 2 * math.pi * k / sides)]
        for k in range(sides)
    ]
    return points + [points[0]]


def star(cx, cy, outer, inner, tips, turn=0.0):
    points = []
    for k in range(2 * tips):
        r = outer if k % 2 == 0 else inner
        a = turn + math.pi * k / tips
        points.append([cx + r * math.cos(a), cy + r * math.sin(a)])
    return points + [points[0]]


def rectangle(x0, y0, x1, y1):
    return [[x0, y0], [x1, y0], [x1, y1], [x0, y1], [x0, y0]]


def case(rng):
    n = rng.choice([16, 17, 24, 33, 48, 64, 101, 128])
    width, height = rng.choice([10.0, 50.0, 100.0, 127.0]) * rng.uniform(0.5, 1.5), rng.uniform(5, 150)
    if rng.random() < 0.4:
        width, height = float(rng.choice([16, 32, 50, 100])), float(rng.choice([15, 30, 64, 100]))
    x, y = rng.choice([0.0, 0.0, 5.0, rng.uniform(0, 40)]), rng.choice([0.0, 0.0, 2.5, rng.uniform(0, 40)])
    step_x, step_y = width / (n - 1), height / (n - 1)
    paths = []
    for _ in range(rng.randint(1, 5)):
        kind = rng.random()
        cx, cy = x + rng.uniform(0.1, 0.9) * width, y + rng.uniform(0.1, 0.9) * height
        size = rng.uniform(0.05, 0.4) * min(width, height)
        if kind < 0.25:
            # On the grid: edges at cell edges and sample points, where Shapely meets the cells exactly.
            i0, j0 = rng.randint(0, n - 4), rng.randint(0, n - 4)
            i1, j1 = rng.randint(i0 + 1, n - 1), rng.randint(j0 + 1, n - 1)
            paths.append(rectangle(x + i0 * step_x, y + j0 * step_y, x + i1 * step_x, y + j1 * step_y))
        elif kind < 0.45:
            paths.append(regular(cx, cy, size, rng.choice([3, 4, 5, 6, 48]), rng.uniform(0, math.pi)))
        elif kind < 0.6:
            paths.append(star(cx, cy, size, size * rng.uniform(0.3, 0.7), rng.randint(4, 9), rng.uniform(0, 1)))
        elif kind < 0.8:
            # An outline with a hole, as a letter O has.
            paths.append(regular(cx, cy, size, 40))
            paths.append(regular(cx, cy, size * rng.uniform(0.3, 0.8), 40, 0.1))
        else:
            paths.append(rectangle(cx - size, cy - size / 3, cx + size, cy + size / 3))
    return dict(
        paths=paths,
        width=width,
        height=height,
        x=x,
        y=y,
        resolution=n,
        kind=rng.choice(['raised', 'recessed']),
    )


REFUSALS = [
    dict(paths=[[[0, 0], [1, 0], [1, 1]]]),
    dict(paths=[[[0, 0], [2, 2], [2, 0], [0, 2], [0, 0]]]),
    dict(paths=[rectangle(1, 1, 5, 5), rectangle(1, 1, 5, 5)]),
    dict(paths=[]),
    dict(),
    dict(paths=5),
    dict(paths=[[[0, 0], [1, 'x']]]),
    dict(paths=[rectangle(1, 1, 5, 5)], width=0),
    dict(paths=[rectangle(1, 1, 5, 5)], width='wide'),
    dict(paths=[rectangle(1, 1, 5, 5)], height=True),
    dict(paths=[rectangle(1, 1, 5, 5)], x=-1),
    dict(paths=[rectangle(1, 1, 5, 5)], resolution=8),
    dict(paths=[rectangle(1, 1, 5, 5)], resolution=129),
    dict(paths=[rectangle(1, 1, 5, 5)], kind='embossed'),
    dict(paths=[rectangle(1, 1, 5, 5)], kind=1),
]


def answer(make, data):
    try:
        return make(data)
    except ValueError as error:
        return ('ValueError', str(error))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cases', type=int, default=300)
    parser.add_argument('--seed', type=int, default=2026)
    args = parser.parse_args()
    rng = random.Random(args.seed)  # noqa: S311 - reproducible test data
    cells = differing = boundary = refused = kept = 0
    core_time = reference_time = 0.0
    failures = []
    for index in range(args.cases):
        data = case(rng)
        began = time.perf_counter()
        native = answer(lambda d: raster(d, backend='native'), data)
        core_time += time.perf_counter() - began
        began = time.perf_counter()
        reference = answer(_reference_raster, data)
        reference_time += time.perf_counter() - began
        if isinstance(reference, tuple) or isinstance(native, tuple):
            refused += 1
            if native != reference:
                failures.append(f'case {index}: core {native!r:.120}, reference {reference!r:.120}')
            continue
        g = region(data['paths'])
        outline = g.boundary
        n, width, height = data['resolution'], data['width'], data['height']
        for row in range(n):
            for col in range(n):
                cells += 1
                if native[row][col] == reference[row][col]:
                    continue
                differing += 1
                kept += native[row][col]
                px, py = data['x'] + col * width / (n - 1), data['y'] + row * height / (n - 1)
                cell = box(px - width / (n - 1), py - height / (n - 1), px + width / (n - 1), py + height / (n - 1))
                if outline.distance(cell.boundary) <= 2e-6:
                    boundary += 1
                else:
                    failures.append(
                        f'case {index}: cell {row},{col} core {native[row][col]} reference {reference[row][col]}'
                    )
    for data in REFUSALS:
        native, reference = answer(lambda d: raster(d, backend='native'), data), answer(_reference_raster, data)
        if native != reference:
            failures.append(f'refusal {data!r:.80}: core {native!r}, reference {reference!r}')
    print(
        f'{args.cases} cases ({refused} refused), {cells} cells: {differing} differ, {boundary} of them at the '
        f'boundary, the core keeping material at {kept} of them; {len(REFUSALS)} refusals compared. '
        f'Core {core_time:.2f} s, reference {reference_time:.2f} s.'
    )
    for failure in failures[:20]:
        print('FAIL', failure)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
