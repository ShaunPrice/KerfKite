"""Smoke-test a compiled wheel with its installed interpreter in isolated mode."""

from studio import _relief_native
from studio.simulation import simulate

depths = _relief_native.contact([[-1.0]], 2.0, 2.0, 1.0, 1.0, 0.0, 1, 1, "flat")
assert depths == [[-1.0, -1.0], [-1.0, -1.0]], depths
assert callable(_relief_native.stock_heights)
stock = {
    "segments": [[0, 0, 8, 0, -1, 0, -1]],
    "tool_shape": "flat",
    "width": 8,
    "height": 8,
    "resolution": 16,
    "stock_thickness": 2,
    "tool_diameter": 1,
}
assert simulate(stock, backend="native")["heights"][0] == [-1.0] * 16

# Exercise the installed CPython extension with sloping moves and multiple cutters.
from hashlib import sha256
import random
from studio.cam import generate
from studio.rotary import generate_rotary, wrap_job

rng = random.Random(86)
moves = [
    [
        rng.uniform(-2, 12),
        rng.uniform(-2, 12),
        rng.uniform(-2, 12),
        rng.uniform(-2, 12),
        -rng.random(),
        0,
        -rng.random(),
    ]
    for _ in range(80)
]
for cutter in ('flat', 'ball', 'v', 'drill'):
    case = {**stock, 'segments': moves, 'tool_shape': cutter, 'resolution': 32}
    reference = simulate(case, backend='python')['heights']
    native = simulate(case, backend='native')['heights']
    assert max(abs(v - w) for a, b in zip(reference, native, strict=True) for v, w in zip(a, b, strict=True)) < 1e-10, (
        cutter
    )

options = dict(
    diameter=10,
    wrap_axis='X',
    postprocessor='xpro-v5',
    output_axis='A',
    direction=1,
    angle_offset=0,
    feed_reference='surface',
)
for head in ('router', 'laser'):
    project = dict(
        protocol='grbl',
        mode=head,
        width=128,
        height=40,
        safe=5,
        feed=550,
        power=800,
        plunge=125,
        depth=2,
        step=1,
        paths=[[[0, 5], [31.41593, 5.001], [62.83185, 7]]],
    )
    if head == 'router':
        project['path_depths'] = [[-1.0411, -1.5, -0.75]]
    else:
        project['path_powers'] = [[0, 231.5, 0]]
    legacy = wrap_job(generate(project), options)['lines']
    packed = generate_rotary(project, options)['lines']
    assert sha256('\n'.join(legacy).encode()).digest() == sha256('\n'.join(packed).encode()).digest(), head

# The C++ move loops must reproduce the Python reference byte for byte on the installed build.
for backend_project in (project, dict(project, path_depths=None, path_powers=None, mode='router', step=0.5)):
    backend_project = {key: value for key, value in backend_project.items() if value is not None}
    backend_project.pop('path_powers' if backend_project['mode'] == 'router' else 'path_depths', None)
    for form in ({}, {'packed_segments': True}):
        reference = generate(backend_project, backend='python', **form)
        accelerated = generate(backend_project, backend='native', **form)
        assert reference['lines'] == accelerated['lines'], backend_project['mode']
        assert list(reference['segments']) == list(accelerated['segments']), backend_project['mode']
    assert (
        generate_rotary(backend_project, options, backend='python')['lines']
        == generate_rotary(backend_project, options, backend='native')['lines']
    ), backend_project['mode']
for value in (0.0625, 2.5, -0.0004, 1234.56789, 0.1 + 0.2):
    for decimals in (0, 1, 3, 6, 9):
        assert _relief_native.fixed_text(value, decimals) == format(value, f'.{decimals}f'), (value, decimals)

# Probe levelling exists only in the native core, so its output is pinned here: the same plan, surface
# and levelled program on every platform a wheel is built for.
from studio import leveling

assert leveling.available()
level_job = generate(
    dict(
        protocol='grbl',
        mode='router',
        width=120,
        height=80,
        feed=500,
        power=12000,
        depth=0.12,
        step=0.12,
        paths=[[[11.3, 9.7], [97.25, 14.1], [83.6, 61.45], [20.05, 55.5], [11.3, 9.7]]],
    )
)
plan = leveling.probe_plan({'bounds': level_job['bounds'], 'width': 120, 'height': 80, 'spacing': 7.5, 'zero': True})
heights = [
    -31.4 + 0.0031 * x - 0.0017 * y + 0.00002 * (x - 50) * (x - 50) + 0.00003 * (y - 40) * (y - 40)
    for x, y in plan['points']
]
surface = leveling.height_map(
    plan, reports=[f'[PRB:{x:.3f},{y:.3f},{z:.3f}:1]' for (x, y), z in zip(plan['points'], heights)]
)
levelled = leveling.level(level_job['lines'], surface, max_segment=0.7)['lines']
digest = sha256('\n'.join(plan['lines'] + levelled).encode() + repr(surface['offsets']).encode()).hexdigest()[:16]
assert (len(plan['lines']), len(levelled), digest) == (356, 367, 'c8dc676458939e8a'), (
    len(plan['lines']),
    len(levelled),
    digest,
)

# Offsets, pockets and fills are computed by the native core's polygon kernel through this extension, with
# whole-number arithmetic and no call into the C maths library, so their programs are pinned as well: an
# inside offset, an outside offset, a pocket and a cross-hatch fill at an odd angle must come out the same
# on every platform a wheel is built for.
from studio import operations

assert operations.kernel_available() and operations.polygon_backend() == 'kernel'
outline = [[12.4, 9.1], [58.75, 6.3], [71.2, 31.85], [49.6, 52.4], [33.3, 38.7], [14.05, 47.9], [12.4, 9.1]]
island = [[30.5, 18.2], [44.1, 16.9], [46.3, 29.4], [35.2, 27.75], [30.5, 18.2]]
shape = dict(protocol='grbl', width=120, height=80, feed=500, power=12000, paths=[outline, island])
polygon_jobs = [
    generate(dict(shape, mode='router', operation='inside', tool_diameter=3.175, depth=1.2, step=0.6)),
    generate(dict(shape, mode='router', operation='outside', tool_diameter=6, depth=1, step=1)),
    generate(dict(shape, mode='router', operation='pocket', tool_diameter=4, stepover=1.6, depth=1, step=1)),
    generate(dict(shape, mode='laser', power=800, operation='fill', interval=0.45, hatch_angle=33.3, crosshatch=True)),
]
counts = tuple(len(job['lines']) for job in polygon_jobs)
digest = sha256('\n'.join(line for job in polygon_jobs for line in job['lines']).encode()).hexdigest()[:16]
assert (counts, digest) == ((344, 176, 684, 1737), '59a9b75edf5ac767'), (counts, digest)
# Isolation toolpaths exist only in the native core as well, on its own polygon kernel, whose arithmetic is in
# whole nanometres: the copper image, the contours and the unreachable places of this board are pinned, so
# that every platform a wheel is built for gives the same bytes. The board is written here because an
# installed wheel carries no fixtures. It has no arcs and no rotated macro, the two places where the Gerber
# importer itself calls the platform's mathematics library.
import json

from studio import pcb_import

assert pcb_import.isolation_available()
BOARD = '\n'.join(
    [
        '%FSLAX46Y46*%',
        '%MOMM*%',
        '%AMROUNDRECT*21,1,$1,$2-2x$3,0,0,0*21,1,$1-2x$3,$2,0,0,0*1,1,2x$3,$1/2-$3,$2/2-$3*'
        '1,1,2x$3,$3-$1/2,$2/2-$3*1,1,2x$3,$3-$1/2,$3-$2/2*1,1,2x$3,$1/2-$3,$3-$2/2*%',
        '%ADD10C,0.250000*%',
        '%ADD11C,1.600000X0.700000*%',
        '%ADD12R,1.500000X1.000000*%',
        '%ADD13O,2.000000X1.000000*%',
        '%ADD14P,1.500000X6*%',
        '%ADD15ROUNDRECT,1.600000X1.200000X0.300000*%',
        '%ADD16R,0.400000X0.300000*%',
        '%ADD17C,2.400000*%',
        'G01*',
        '%LPD*%',
        'G36*',
        'X1000000Y1000000D02*',
        'X19000000Y1000000D01*',
        'X19000000Y7000000D01*',
        'X1000000Y7000000D01*',
        'X1000000Y1000000D01*',
        'G37*',
        '%LPC*%',
        'D17*',
        'X4000000Y4000000D03*',
        'X8000000Y4000000D03*',
        'D10*',
        'X11000000Y2500000D02*',
        'X17000000Y5500000D01*',
        '%LPD*%',
        'D11*',
        'X4000000Y4000000D03*',
        'D15*',
        'X8000000Y4000000D03*',
        '%SRX3Y2I4.130000J2.770000*%',
        'D12*',
        'X2000000Y9000000D03*',
        'D10*',
        'X2000000Y9000000D02*',
        'X3310000Y10370000D01*',
        'D13*',
        'X3310000Y10370000D03*',
        '%SR*%',
        'D14*',
        'X16000000Y10000000D03*',
        'D16*',
        'X15000000Y12500000D02*',
        'X18000000Y13700000D01*',
        'D10*',
        'X15000000Y8900000D02*',
        'X18000000Y8900000D01*',
        'X15000000Y8520000D02*',
        'X18000000Y8520000D01*',
        'M02*',
    ]
)
isolated = pcb_import.isolation(
    BOARD,
    pcb_import.v_bit_width(0.1, 30, 0.2),
    passes=3,
    overlap=0.3,
    offset=[2.345, 1.005],
    mirror_x=12.5,
    clip=[[[2.5, 1.5], [24.0, 1.5], [24.0, 15.2], [2.5, 15.2], [2.5, 1.5]]],
)
digest = sha256(json.dumps(isolated, sort_keys=True, separators=(',', ':')).encode()).hexdigest()[:16]
summary = (isolated['islands'], isolated['path_count'], isolated['point_count'], isolated['unreachable_count'], digest)
assert summary == (13, 31, 2039, 1, '8f30b3062ac4865b'), summary
assert (isolated['tool_width'], isolated['smallest_gap'], isolated['clipped']) == (
    0.20718,
    0.13,
    {'cut': 1, 'dropped': 0},
)
