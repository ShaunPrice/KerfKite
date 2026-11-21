"""Compare CAD/CAM kernels in fresh simulator processes, including peak RSS."""

import argparse
import json
import platform
import statistics
import subprocess
import sys
from pathlib import Path

from studio import relief, simulation
from .stress_cam import workload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, nargs='?', default=Path('native-results.json'))
    parser.add_argument('--worker', choices=['python', 'native'], help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        print(json.dumps(workload(backend=args.worker, capture_grid=True, measure_rotary=True)))
        return
    if relief._relief_native is None or simulation.stock_heights is None:
        parser.error('Build the relief and stock extension before benchmarking')
    result = {
        'platform': platform.platform(),
        'python_version': sys.version,
        'extension': Path(relief._relief_native.__file__).name,
        'python': [],
        'native': [],
    }
    grid_ref = None
    max_error = 0.0
    for _ in range(3):
        for backend in ('python', 'native'):
            trial = subprocess.run(
                [sys.executable, '-m', 'validation.benchmark_native', '--worker', backend],
                check=True,
                capture_output=True,
                text=True,
            )
            row = json.loads(trial.stdout)
            grid = row.pop('grid')
            if grid_ref is None:
                grid_ref = grid
            assert len(grid_ref) == len(grid) == 96
            for a, b in zip(grid_ref, grid):
                assert len(a) == len(b) == 96
                max_error = max(max_error, max(abs(x - y) for x, y in zip(a, b)))
            result[backend].append(row)
    assert max_error < 1e-10, max_error
    result['max_stock_error_mm'] = max_error
    assert len({r['sha256'] for backend in ('python', 'native') for r in result[backend]}) == 1
    assert len({r['rotary_sha256'] for backend in ('python', 'native') for r in result[backend]}) == 1
    result['hashes_match'] = True
    result['medians'] = {}
    for backend in ('python', 'native'):
        rows = result[backend]
        medians = {stage: statistics.median(r['stages'][stage] for r in rows) for stage in rows[0]['stages']}
        medians['rotary_generation'] = statistics.median(r['rotary_seconds'] for r in rows)
        medians['pipeline'] = statistics.median(sum(r['stages'].values()) for r in rows)
        peaks = [r['peak_rss_mib'] for r in rows if r['peak_rss_mib'] is not None]
        medians['peak_rss_mib'] = statistics.median(peaks) if peaks else None
        result['medians'][backend] = medians
    args.output.write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
