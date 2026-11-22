"""Time the compiled relief contact on one thread, on all threads and on a GPU, and check they agree.

Each way runs in a process of its own, because the GPU is chosen once per process. For each job the script
prints the contact checks, the best of several runs in each way, the first GPU run (which includes setting the
GPU up) and a digest of the depths, which must be the same in every way. Synthetic maps only.

    python validation/benchmark_relief.py [--repeat 5] [--no-gpu] [--threads N]
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# name, map side, width, height, depth, ball diameter, stepover
JOBS = [
    ('256 map, 6 mm ball', 256, 100.0, 100.0, 4.0, 6.0, 0.5),
    ('1024 map, 6 mm ball', 1024, 100.0, 100.0, 4.0, 6.0, 1.1),
    ('1024 map, 12 mm ball', 1024, 100.0, 100.0, 4.0, 12.0, 1.1),
    ('1024 map, 14 mm ball', 1024, 100.0, 100.0, 4.0, 14.0, 1.2),
]

WORKER = r'''
import hashlib, json, math, random, sys, time
import numpy as np
sys.path.insert(0, sys.argv[1])
from studio import _relief_native as r
jobs, repeat = json.loads(sys.argv[2]), int(sys.argv[3])
out = []
for name, side, width, height, depth, diameter, step in jobs:
    # Ridges and noise from exact operations and a fixed generator, so the map, and the digest, are the same on
    # every computer.
    rng = np.random.default_rng(side)
    u = np.arange(side) / (side - 1)
    grid = -depth * (0.6 * ((3 * u[None, :] + 2 * u[:, None]) % 1.0) + 0.4 * rng.random((side, side)))
    maxima = np.maximum(np.maximum(grid[:-1, :-1], grid[:-1, 1:]), np.maximum(grid[1:, :-1], grid[1:, 1:]))
    packed = np.ascontiguousarray(maxima).tobytes()
    dx = width / (side - 1)
    nx, ny = math.ceil(width / min(dx, step)), math.ceil(height / step)
    times = []
    for _ in range(repeat):
        began = time.perf_counter()
        depths = r.contact_packed(packed, side - 1, side - 1, width, height, depth, diameter, 0.0, nx, ny, 'ball')
        times.append(time.perf_counter() - began)
    digest = hashlib.sha256(json.dumps(depths).encode()).hexdigest()[:16]
    out.append({'name': name, 'times': times, 'digest': digest, 'path': r.last_contact()})
print(json.dumps({'jobs': out, 'status': r.gpu_status()}))
'''


def checks(side, width, height, diameter, step):
    import math

    dx, dy = width / (side - 1), height / (side - 1)
    nx, ny, radius = math.ceil(width / min(dx, step)), math.ceil(height / step), diameter / 2
    total = 0
    for raster in range(ny + 1):
        py = height * raster / ny
        lo_j, hi_j = max(0, math.floor((py - radius) / dy)), min(side - 2, math.floor((py + radius) / dy))
        across = 0
        for segment in range(nx):
            start, end = width * segment / nx, width * (segment + 1) / nx
            lo_i, hi_i = max(0, math.floor((start - radius) / dx)), min(side - 2, math.floor((end + radius) / dx))
            across += max(0, hi_i - lo_i + 1)
        total += across * max(0, hi_j - lo_j + 1)
    return total


def way(threads, gpu, repeat):
    env = {**os.environ, 'KERFKITE_GPU': gpu}
    env.pop('KERFKITE_THREADS', None)
    if threads:
        env['KERFKITE_THREADS'] = str(threads)
    done = subprocess.run(  # noqa: S603 - this interpreter, with a fixed script
        [sys.executable, '-c', WORKER, str(ROOT), json.dumps(JOBS), str(repeat)],
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )
    if done.returncode:
        raise SystemExit(done.stderr)
    return json.loads(done.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repeat', type=int, default=5)
    parser.add_argument('--no-gpu', action='store_true', help='time the threads only')
    parser.add_argument('--threads', type=int, help='cap the threads at this many, as a smaller processor would have')
    args = parser.parse_args()
    ways = {'1 thread': way(1, 'off', max(1, args.repeat // 2)), 'threads': way(args.threads, 'off', args.repeat)}
    if not args.no_gpu:
        ways['GPU'] = way(None, 'on', args.repeat)
    print('GPU:', ways.get('GPU', ways['threads'])['status'])
    first_header = f" {'GPU first':>10}" if 'GPU' in ways else ''
    print(f"{'job':24} {'checks':>14} " + ' '.join(f'{name:>10}' for name in ways) + f'{first_header}  digest')
    agree = True
    for index, (name, side, width, height, _, diameter, step) in enumerate(JOBS):
        rows = [ways[name_]['jobs'][index] for name_ in ways]
        digests = {row['digest'] for row in rows}
        agree = agree and len(digests) == 1
        best = ' '.join(f'{min(row["times"]) * 1000:9.1f}ms' for row in rows)
        first = f' {ways["GPU"]["jobs"][index]["times"][0] * 1000:9.1f}ms' if 'GPU' in ways else ''
        print(
            f'{name:24} {checks(side, width, height, diameter, step):>14,} {best}{first}  {"/".join(sorted(digests))}'
        )
    for name_, result in ways.items():
        print(f'{name_}: last job on {result["jobs"][-1]["path"]}')
    if not agree:
        raise SystemExit('the ways disagree')


if __name__ == '__main__':
    main()
