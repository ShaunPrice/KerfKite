"""Best-of-nine timings for the G-code and rotary generators on each backend, with output hashes.

Run from the repository root with the native extension built:

    python validation/benchmark_gcode.py

The hashes identify the generated program. Both backends must print the same hash for a job: a difference
means the machine output has changed, which is a defect, not an optimisation.
"""

import gc
import hashlib
import sys
import time

sys.path.insert(0, '.')
from validation.stress_cam import fixture
from studio.relief import prepare_relief
from studio.cam import generate
from studio.rotary import generate_rotary

N = 9
data = fixture()
relief = prepare_relief(data, backend='native')
project = dict(
    mode='router', protocol='grbl', width=680, height=240, feed=1200, plunge=300, power=1000, safe=5, depth=6, step=3,
    tool_diameter=3.175, paths=relief['paths'], path_depths=relief['path_depths'],
)  # fmt: skip
flat = dict(
    mode='router', protocol='grbl', width=680, height=240, feed=1200, plunge=300, power=1000, safe=5, depth=6, step=1,
    paths=relief['paths'][:67],
)  # fmt: skip
laser = dict(
    mode='laser', protocol='grbl', width=680, height=240, feed=1200, power=1000, passes=1, paths=relief['paths']
)
options = dict(diameter=100, wrap_axis='X', postprocessor='xpro-v5', output_axis='A')
jobs = {
    'gcode relief': lambda backend: generate(project, packed_segments=True, backend=backend),
    'gcode relief, list rows': lambda backend: generate(project, backend=backend),
    'gcode flat': lambda backend: generate(flat, packed_segments=True, backend=backend),
    'gcode laser': lambda backend: generate(laser, packed_segments=True, backend=backend),
    'rotary': lambda backend: generate_rotary(project, options, backend=backend),
}
backends = ['python']
try:
    generate(flat, backend='native')
    backends.append('native')
except ValueError:
    print('Native extension not installed: timing the Python loops only.')
best = {(job, backend): 9e9 for job in jobs for backend in backends}
digest = {}
for _ in range(N):
    for job, run in jobs.items():
        for backend in backends:
            gc.collect()
            started = time.perf_counter()
            result = run(backend)
            best[job, backend] = min(best[job, backend], time.perf_counter() - started)
            digest[job, backend] = hashlib.sha256('\n'.join(result['lines']).encode()).hexdigest()[:12]
for job in jobs:
    row = f'{job:24s}'
    for backend in backends:
        row += f'  {backend} {best[job, backend] * 1000:7.1f} ms {digest[job, backend]}'
    if len(backends) == 2:
        assert digest[job, 'python'] == digest[job, 'native'], f'{job}: the backends disagree'
        row += f'  x{best[job, "python"] / best[job, "native"]:.1f}'
    print(row)
