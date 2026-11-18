"""Smoke-test a compiled wheel with its installed interpreter in isolated mode."""
from studio import _relief_native
from studio.simulation import simulate

depths = _relief_native.contact([[-1.0]], 2., 2., 1., 1., 0., 1, 1, "flat")
assert depths == [[-1., -1.], [-1., -1.]], depths
assert callable(_relief_native.stock_heights)
stock = {"segments": [[0, 0, 8, 0, -1, 0, -1]], "tool_shape": "flat", "width": 8, "height": 8,
         "resolution": 16, "stock_thickness": 2, "tool_diameter": 1}
assert simulate(stock, backend="native")["heights"][0] == [-1.] * 16

# Exercise the installed CPython extension with sloping moves and multiple cutters.
from hashlib import sha256
import random
from studio.cam import generate
from studio.rotary import generate_rotary, wrap_job

rng = random.Random(86)
moves = [[rng.uniform(-2, 12), rng.uniform(-2, 12), rng.uniform(-2, 12), rng.uniform(-2, 12),
          -rng.random(), 0, -rng.random()] for _ in range(80)]
for cutter in ('flat', 'ball', 'v', 'drill'):
    case = {**stock, 'segments': moves, 'tool_shape': cutter, 'resolution': 32}
    reference = simulate(case, backend='python')['heights']
    native = simulate(case, backend='native')['heights']
    assert max(abs(v - w) for a, b in zip(reference, native, strict=True)
               for v, w in zip(a, b, strict=True)) < 1e-10, cutter

options = dict(diameter=10, wrap_axis='X', postprocessor='xpro-v5', output_axis='A',
               direction=1, angle_offset=0, feed_reference='surface')
for head in ('router', 'laser'):
    project = dict(protocol='grbl', mode=head, width=128, height=40, safe=5, feed=550, power=800,
                   plunge=125, depth=2, step=1, paths=[[[0, 5], [31.41593, 5.001], [62.83185, 7]]])
    if head == 'router':
        project['path_depths'] = [[-1.0411, -1.5, -0.75]]
    else:
        project['path_powers'] = [[0, 231.5, 0]]
    legacy = wrap_job(generate(project), options)['lines']
    packed = generate_rotary(project, options)['lines']
    assert sha256('\n'.join(legacy).encode()).digest() == sha256('\n'.join(packed).encode()).digest(), head
