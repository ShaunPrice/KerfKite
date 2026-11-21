"""Best-of-nine timings for the Python G-code and rotary generators, with output hashes.

Run from the repository root with the native relief extension built:

    python validation/benchmark_gcode.py

The hashes identify the generated program. A change that alters a hash has changed the machine output and
is not an optimisation; compare both the times and the hashes before and after.
"""

import hashlib, sys, time, gc, resource

sys.path.insert(0, '.')
from validation.stress_cam import fixture
from studio.relief import prepare_relief
from studio.cam import generate
from studio.rotary import generate_rotary

N = 9
data = fixture()
relief = prepare_relief(data, backend='native')
project = dict(
    mode='router',
    protocol='grbl',
    width=680,
    height=240,
    feed=1200,
    plunge=300,
    power=1000,
    safe=5,
    depth=6,
    step=3,
    tool_diameter=3.175,
    paths=relief['paths'],
    path_depths=relief['path_depths'],
)
flat = dict(
    mode='router',
    protocol='grbl',
    width=680,
    height=240,
    feed=1200,
    plunge=300,
    power=1000,
    safe=5,
    depth=6,
    step=1,
    paths=relief['paths'][:67],
)
laser = dict(
    mode='laser', protocol='grbl', width=680, height=240, feed=1200, power=1000, passes=1, paths=relief['paths']
)
options = dict(diameter=100, wrap_axis='X', postprocessor='xpro-v5', output_axis='A')
stages = {
    'gcode relief': lambda: generate(project, packed_segments=True),
    'gcode flat': lambda: generate(flat, packed_segments=True),
    'gcode laser': lambda: generate(laser, packed_segments=True),
    'rotary': lambda: generate_rotary(project, options),
}
best = {k: 9e9 for k in stages}
out = {}
for _ in range(N):
    for k, fn in stages.items():
        gc.collect()
        t = time.perf_counter()
        out[k] = fn()
        best[k] = min(best[k], time.perf_counter() - t)
for k, v in best.items():
    print(f'{k:14s} {v * 1000:8.1f} ms  sha {hashlib.sha256(chr(10).join(out[k]["lines"]).encode()).hexdigest()[:12]}')
