"""Time representative Studio solid models on KerfKite's solid kernel and on manifold3d.

    python validation/benchmark_solid.py [--repeat 5] [--only TEXT] [--json FILE]

Each model is rebuilt through Studio's own entry points (studio.solid_cad.rebuild for a parametric part,
studio.scad_geometry.build_script for a CAD script, with the mesh read back as the editor does) in a fresh
interpreter for each backend, REPEAT times, and the best time is kept: the machines this runs on are shared,
and the best of several runs is the figure least disturbed by other work. A hash of the kernel's vertices
and triangles is printed with each model, so that two builds of the kernel can be compared: a change that
only makes the kernel faster leaves every hash as it was.

native/tools/solid_bench.cpp times the same shapes on the kernel alone, without Studio around them.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def part(*features, parameters=None):
    return {'version': 1, 'parameters': parameters or {}, 'features': list(features)}


def bores(count_x, count_y, step, origin):
    """One cut feature for each row of a grid of bores: a pattern holds at most 64 instances."""
    return [
        {
            'id': f'bores{row}',
            'kind': 'cylinder',
            'op': 'cut',
            'radius': 2,
            'height': 8,
            'origin': [origin[0], origin[1] + row * step, -1],
            'pattern': {'kind': 'linear', 'count': count_x, 'step': [step, 0, 0]},
        }
        for row in range(count_y)
    ]


def bracket():
    """The part pinned in validation/check_installed_cam.py."""
    return part(
        {'id': 'base', 'kind': 'box', 'size': ['width', 30, 'thickness']},
        {
            'id': 'bores',
            'kind': 'cylinder',
            'op': 'cut',
            'radius': 2.4,
            'topRadius': 1.9,
            'height': 'thickness + 2',
            'origin': [9.3, 15, -1],
            'pattern': {'kind': 'polar', 'count': 5, 'angle': 50, 'center': ['half', 15, 0]},
        },
        {'id': 'boss', 'kind': 'sphere', 'radius': 7.1, 'origin': ['half', 15, 'thickness']},
        {
            'id': 'rib',
            'kind': 'extrude',
            'points': [[0, 0], [11.7, 0], [3.3, 8.9]],
            'height': 4.2,
            'rotation': [90, 0, 33],
            'origin': [4.4, 3.1, 'thickness - 0.5'],
        },
        {
            'id': 'groove',
            'kind': 'revolve',
            'op': 'cut',
            'points': [[9, 0], [11.5, 0], [11.5, 2.2], [9, 3.1]],
            'angle': 200,
            'rotation': [0, 0, 77],
            'origin': ['half', 15, 'thickness - 1.5'],
        },
        {'id': 'trim', 'kind': 'box', 'op': 'intersect', 'size': [60, 26.5, 30], 'origin': [-5, 1.75, -2]},
        {
            'id': 'post',
            'kind': 'script',
            'source': """
                $fn = 20;
                translate([31.3, 9.2, thickness - 1]) difference() {
                    hull() { cylinder(h=lift, r1=6, r2=4.2); translate([7.3, 2.1, 0]) cube([4, 5, 3]); }
                    translate([0, 0, -1]) linear_extrude(height=lift + 2, twist=40, slices=5, scale=0.8)
                        offset(r=0.6) square([3.1, 2.2], center=true);
                    rotate([0, 90, 17]) translate([-2.5, 0, 0]) cylinder(h=30, d=2.4, center=true);
                }
            """,
            'scriptParameters': {'lift': 5.5},
        },
        parameters={'width': '2 * half', 'half': 20.5, 'thickness': 6.25},
    )


def difference_chain():
    features = [{'id': 'block', 'kind': 'box', 'size': [60, 40, 12]}]
    for k in range(8):
        features.append({'id': f'slot{k}', 'kind': 'box', 'op': 'cut', 'size': [3, 30, 6], 'origin': [4 + 7 * k, 5, 8]})
    for k in range(8):
        features.append(
            {
                'id': f'bore{k}',
                'kind': 'cylinder',
                'op': 'cut',
                'radius': 1.6,
                'topRadius': 1.1,
                'height': 14,
                'origin': [5.5 + 7 * k, 20, -1],
            }
        )
    for k in range(4):
        features.append(
            {
                'id': f'cross{k}',
                'kind': 'cylinder',
                'op': 'cut',
                'radius': 1.5,
                'height': 70,
                'rotation': [0, 90, 0],
                'origin': [-5, 8 + 8 * k, 4],
            }
        )
    features.append({'id': 'dish', 'kind': 'sphere', 'op': 'cut', 'radius': 9, 'origin': [30, 20, 14]})
    return part(*features)


def holes_one_by_one(count):
    """A plate cut by one feature for each hole, as a user adds them."""
    features = [{'id': 'plate', 'kind': 'box', 'size': [80, 80, 6]}]
    for k in range(count):
        features.append(
            {
                'id': f'hole{k}',
                'kind': 'cylinder',
                'op': 'cut',
                'radius': 2,
                'height': 8,
                'origin': [5 + 10 * (k % 8), 5 + 10 * (k // 8), -1],
            }
        )
    return part(*features)


MODELS = [
    (
        'A plate with one bore',
        part(
            {'id': 'plate', 'kind': 'box', 'size': [40, 30, 6]},
            {'id': 'bore', 'kind': 'cylinder', 'op': 'cut', 'radius': 4, 'height': 8, 'origin': [20, 15, -1]},
        ),
    ),
    ('The pinned bracket: seven features', bracket()),
    (
        '64 overlapping bores in one pattern',
        part(
            {'id': 'plate', 'kind': 'box', 'size': [104, 30, 6]},
            {
                'id': 'bores',
                'kind': 'cylinder',
                'op': 'cut',
                'radius': 2,
                'height': 8,
                'origin': [4, 15, -1],
                'pattern': {'kind': 'linear', 'count': 64, 'step': [1.5, 0, 0]},
            },
        ),
    ),
    (
        '64 separate bores in eight patterns',
        part({'id': 'plate', 'kind': 'box', 'size': [80, 80, 6]}, *bores(8, 8, 10, [5, 5])),
    ),
    ('24 separate bores, one feature each', holes_one_by_one(24)),
    ('60 separate bores, one feature each', holes_one_by_one(60)),
    ('Two spheres, $fn = 48', '$fn = 48; union() { sphere(r=10); translate([7.3, 2.1, 1.7]) sphere(r=10); }'),
    ('Two spheres, $fn = 96', '$fn = 96; union() { sphere(r=10); translate([7.3, 2.1, 1.7]) sphere(r=10); }'),
    (
        'A union of 50 boxes',
        'for (k = [0:49]) translate([3.5 * (k % 10), 4 * floor(k / 10), 0.3 * (k % 3)])'
        ' rotate([7 * k, 0, 13 * k]) cube([6, 5, 4], center=true);',
    ),
    ('A difference chain: 21 cuts', difference_chain()),
    (
        'A revolved profile with two cuts',
        """
        $fn = 96;
        difference() {
            rotate_extrude() polygon([[4, 0], [12, 0], [12, 3], [8, 5], [8, 14], [4, 14]]);
            translate([-15, -2, 9]) cube([30, 4, 6]);
            translate([0, 20, 2.5]) rotate([90, 0, 0]) cylinder(h=40, r=1.5, $fn=48);
        }
        """,
    ),
    (
        'A twisted extrusion, bored and trimmed',
        """
        $fn = 48;
        intersection() {
            difference() {
                linear_extrude(height=20, twist=60, slices=12)
                    polygon([[10, 0], [3, 2], [0, 10], [-3, 2], [-10, 0], [-3, -2], [0, -10], [3, -2]]);
                translate([0, 0, -2]) cylinder(h=24, r=2);
            }
            translate([0, 0, 10]) sphere(r=11);
        }
        """,
    ),
]


def build(model):
    """What the editor asks for after an edit: the mesh, and for a part its outline too."""
    from studio.scad_geometry import build_script
    from studio.solid_cad import rebuild

    if isinstance(model, dict):
        result = rebuild(model)
        return result['vertices'], result['triangles']
    mesh = build_script(model)[1].to_mesh64()
    return mesh.vert_properties[:, :3].tolist(), mesh.tri_verts.tolist()


def worker(repeat, only):
    """Runs in a fresh interpreter with the backend chosen by the environment; prints one JSON line."""
    sys.path.insert(0, str(ROOT))
    from studio import solid_kernel

    rows = []
    for name, model in MODELS:
        if only and only.lower() not in name.lower():
            continue
        best = None
        for _ in range(repeat):
            start = time.perf_counter()
            vertices, triangles = build(model)
            took = time.perf_counter() - start
            best = took if best is None else min(best, took)
        shape = hashlib.sha256(json.dumps([vertices, triangles], separators=(',', ':')).encode()).hexdigest()[:16]
        rows.append({'model': name, 'seconds': best, 'triangles': len(triangles), 'hash': shape})
    print(json.dumps({'backend': solid_kernel.solid_backend(), 'rows': rows}))


def measure(backend, repeat, only):
    env = dict(os.environ, KERFKITE_SOLID_BACKEND=backend)
    command = [sys.executable, str(Path(__file__).resolve()), '--worker', '--repeat', str(repeat), '--only', only]
    done = subprocess.run(command, env=env, capture_output=True, text=True, check=False)  # noqa: S603
    if done.returncode:
        raise SystemExit(f'{backend}: {done.stderr.strip() or done.stdout.strip()}')
    answer = json.loads(done.stdout.strip().splitlines()[-1])
    if answer['backend'] != backend:
        raise SystemExit(f'Asked for the {backend} backend and got {answer["backend"]}')
    return answer['rows']


def milliseconds(seconds):
    return f'{seconds * 1e3:.0f} ms' if seconds < 0.9995 else f'{seconds:.2f} s'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--repeat', type=int, default=5, help='rebuilds of each model; the best is kept')
    parser.add_argument('--only', default='', help='only the models whose name contains this text')
    parser.add_argument('--backends', default='kernel,manifold', help='kernel, manifold or both, separated by a comma')
    parser.add_argument('--json', help='also write the measurements to this file')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    options = parser.parse_args()
    if options.repeat < 1:
        parser.error('--repeat must be at least 1')
    if options.worker:
        worker(options.repeat, options.only)
        return
    backends = [name.strip() for name in options.backends.split(',') if name.strip()]
    if not backends or any(name not in ('kernel', 'manifold') for name in backends):
        parser.error('--backends takes kernel, manifold or both')
    results = {name: measure(name, options.repeat, options.only) for name in backends}
    both = len(backends) == 2
    print(f'Best of {options.repeat} rebuilds of each model through Studio.')
    header = f'{"Model":<40}' + ''.join(f'{name:>12}' for name in backends) + (f'{"ratio":>8}' if both else '')
    print(header + f'{"triangles":>11}  kernel hash' if 'kernel' in results else header)
    for index, row in enumerate(results[backends[0]]):
        line = f'{row["model"]:<40}' + ''.join(
            f'{milliseconds(results[name][index]["seconds"]):>12}' for name in backends
        )
        if both:
            line += f'{results["kernel"][index]["seconds"] / results["manifold"][index]["seconds"]:>8.1f}'
        if 'kernel' in results:
            line += f'{results["kernel"][index]["triangles"]:>11}  {results["kernel"][index]["hash"]}'
        print(line)
    if options.json:
        Path(options.json).write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
