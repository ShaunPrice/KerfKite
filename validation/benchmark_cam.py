"""Small repeatable local timings; not a Raspberry Pi performance claim."""

import json, math, platform, statistics, time
from studio.cam import generate
from studio.relief import prepare_relief
from studio.simulation import simulate

base = dict(
    mode='router', width=200, height=120, feed=600, power=1000, depth=3, step=1, tool_diameter=3.175, stepover=1.2
)
square = [[[10, 10], [110, 10], [110, 80], [10, 80], [10, 10]]]
heightmap = [
    [0.4 + 0.3 * math.cos(i / 63 * math.pi) * math.cos(j / 63 * math.pi) for i in range(64)] for j in range(64)
]
relief = dict(
    heightmap=heightmap, width=100, height=70, depth=3, x=10, y=10, tool_diameter=3.175, tool_shape='ball', stepover=1
)
surface = prepare_relief(relief)
job = generate({**base, 'paths': surface['paths'], 'path_depths': surface['path_depths']})
workloads = {
    'pocket_100x70_mm': lambda: generate({**base, 'operation': 'pocket', 'paths': square}),
    'relief_contact_64x64': lambda: prepare_relief(relief),
    'relief_gcode': lambda: generate({**base, 'paths': surface['paths'], 'path_depths': surface['path_depths']}),
    'stock_preview_48x48': lambda: simulate(
        dict(
            segments=job['segments'],
            width=120,
            height=90,
            tool_diameter=3.175,
            tool_shape='ball',
            stock_thickness=10,
            resolution=48,
        )
    ),
    'laser_hatch_100x70': lambda: generate(
        {**base, 'mode': 'laser', 'operation': 'fill', 'interval': 0.2, 'paths': square}
    ),
}
results = {}
for name, fn in workloads.items():
    times = []
    for _ in range(3):
        start = time.perf_counter()
        r = fn()
        times.append(time.perf_counter() - start)
    results[name] = {'median_seconds': round(statistics.median(times), 4), 'runs': 3}
print(
    json.dumps(
        {
            'host': platform.platform(),
            'python': platform.python_version(),
            'results': results,
            'relief_vertices': sum(map(len, surface['paths'])),
            'relief_gcode_lines': len(job['lines']),
            'note': 'Local development host only; no Pi or native-language comparison performed.',
        },
        indent=2,
    )
)
