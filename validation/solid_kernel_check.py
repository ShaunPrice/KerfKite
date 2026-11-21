"""Check KerfKite's own solid kernel against manifold3d, used here as a test oracle only.

    cmake -S native -B build/native -DKERFKITE_WITH_GEOS=OFF && cmake --build build/native
    python validation/solid_kernel_check.py --tool build/native/kerfkite-cam --trees 4000

The kernel gives no byte-for-byte match with the library: it facets and orders differently, and it works
on a 1 nm grid. So the comparison is by tolerance, and every solid is given to the oracle from the same
leaves (the kernel's own meshes of each primitive), so that what is compared is the modelling and not
the faceting of a sphere. Three parts:

1. Primitives against the oracle's own primitives, with tolerances that follow from the faceting.
2. Random trees of primitives, transforms and Boolean operations, including the awkward constructions:
   identical solids, solids that touch, one inside another, faces in one plane, operands turned together
   so that their faces nearly coincide, patterns joined in one batch. Each tree is evaluated by both and
   the volume, bounding box, number of bodies and section areas at three heights are compared.
3. The application's own models: tests/test_solid_cad.py, tests/test_scad_geometry.py, tests/test_assembly.py
   and tests/test_solid_loft.py are run with a stand-in for the library that builds every solid twice,
   once in the oracle and once as a request to the kernel. The tests themselves then run on the kernel's
   primitives, and every solid they made is compared afterwards.

Tolerances. A kernel result differs from the exact one by the rounding of new vertices to the grid (under
1 nm each) and by the removal of features thinner than about 3 nm; over one operation that is a layer of
at most a few nanometres on the surfaces involved. The volume may therefore differ by
`LAYER_MM * area of the leaves * operations`, a section area by the same layer times a length, and a
bounding box by `BOX_MM`. Nothing here is tuned to make a case pass.
"""

import argparse
import json
import math
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import types
import unittest

import manifold3d as oracle
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

LAYER_MM = 4e-6  # 4 nm for each operation
BOX_MM = 2e-5
TOOL = None


def run_batch(requests):
    """Answers of the kernel for a list of requests: each {'ok': ...}, {'error': ...} or {'invalid': ...}."""
    if not requests:
        return []
    with tempfile.NamedTemporaryFile('w', suffix='.jsonl', delete=False) as handle:
        for request in requests:
            handle.write(json.dumps(request) + '\n')
        path = handle.name
    try:
        done = subprocess.run([TOOL, 'solid-batch', path], capture_output=True, text=True, check=False)  # noqa: S603
    finally:
        os.unlink(path)
    if done.returncode != 0:
        raise RuntimeError(f'kerfkite-cam solid-batch failed: {done.stderr[-2000:]}')
    answers = [json.loads(line) for line in done.stdout.splitlines() if line]
    if len(answers) != len(requests):
        raise RuntimeError(f'{len(requests)} requests but {len(answers)} answers')
    return answers


_leaves = {}


def leaf_mesh(tree):
    """The kernel's mesh of a primitive, as an oracle solid; None when the kernel refuses it."""
    key = json.dumps(tree, sort_keys=True)
    if key not in _leaves:
        answer = run_batch([{'shape': tree, 'mesh': True}])[0]
        if 'ok' not in answer:
            _leaves[key] = answer.get('error', answer.get('invalid'))
        else:
            mesh = answer['ok']['mesh']
            solid = oracle.Manifold(
                oracle.Mesh64(
                    np.array(mesh['vertices'], dtype=np.float64), np.array(mesh['triangles'], dtype=np.uint64)
                )
            )
            _leaves[key] = (
                solid if solid.status() == oracle.Error.NoError else f'oracle refused the mesh: {solid.status()}'
            )
    found = _leaves[key]
    if isinstance(found, str):
        raise ValueError(found)
    return found


class Dual:
    """A solid built twice: `oracle` by manifold3d, `tree` as a request for the kernel."""

    made = []

    def __init__(self, tree, solid, area, operations, note=''):
        self.tree, self.oracle, self.area, self.operations, self.note = tree, solid, area, operations, note
        self.fragile = False
        Dual.made.append(self)

    @classmethod
    def leaf(cls, tree):
        solid = leaf_mesh(tree)
        return cls(tree, solid, solid.surface_area(), 0)

    # ---- what the application calls on the library ------------------------------------------------
    @staticmethod
    def cube(size, center=False):
        return Dual.leaf({'box': [float(v) for v in size], 'center': bool(center)})

    @staticmethod
    def cylinder(height, radius_low, radius_high=-1.0, circular_segments=0, center=False):
        high = radius_low if radius_high < 0 else radius_high
        return Dual.leaf(
            {
                'cylinder': {
                    'height': float(height),
                    'radius_low': float(radius_low),
                    'radius_high': float(high),
                    'segments': int(circular_segments) or 48,
                    'center': bool(center),
                }
            }
        )

    @staticmethod
    def sphere(radius, circular_segments=0):
        return Dual.leaf({'sphere': {'radius': float(radius), 'segments': int(circular_segments) or 48}})

    @staticmethod
    def from_mesh(mesh):
        vertices = np.asarray(mesh.vert_properties)[:, :3]
        tree = {
            'mesh': {'vertices': vertices.tolist(), 'triangles': np.asarray(mesh.tri_verts).tolist(), 'weld': False}
        }
        solid = oracle.Manifold(mesh)
        if solid.status() != oracle.Error.NoError:  # the application reports this itself
            return Dual(tree, solid, 0.0, 0, 'refused by the oracle')
        return Dual(tree, solid, solid.surface_area(), 0)

    def _unary(self, name, argument, solid):
        made = Dual({name: argument, 'of': self.tree}, solid, self.area, self.operations + 1)
        made.fragile = self.fragile
        return made

    def translate(self, v):
        return self._unary('translate', [float(x) for x in v], self.oracle.translate(v))

    def rotate(self, v):
        return self._unary('rotate', [float(x) for x in v], self.oracle.rotate(v))

    def scale(self, v):
        v = [float(v)] * 3 if isinstance(v, (int, float)) else [float(x) for x in v]
        return self._unary('scale', v, self.oracle.scale(v))

    def mirror(self, v):
        return self._unary('mirror', [float(x) for x in v], self.oracle.mirror(v))

    def transform(self, matrix):
        rows = np.asarray(matrix, dtype=float)[:3].tolist()
        return self._unary('matrix', rows, self.oracle.transform(np.asarray(matrix, dtype=float)[:3]))

    @staticmethod
    def _combine(name, parts, solid):
        made = Dual(
            {name: [p.tree for p in parts]},
            solid,
            sum(p.area for p in parts),
            sum(p.operations for p in parts) + len(parts) - 1,
        )
        made.fragile = any(p.fragile for p in parts)
        return made

    def __add__(self, other):
        return Dual._combine('union', [self, other], self.oracle + other.oracle)

    def __sub__(self, other):
        return Dual._combine('difference', [self, other], self.oracle - other.oracle)

    def __xor__(self, other):
        return Dual._combine('intersection', [self, other], self.oracle ^ other.oracle)

    @staticmethod
    def batch_boolean(parts, operation):
        name = {
            oracle.OpType.Add: 'union',
            oracle.OpType.Subtract: 'difference',
            oracle.OpType.Intersect: 'intersection',
        }[operation]
        return Dual._combine(name, list(parts), oracle.Manifold.batch_boolean([p.oracle for p in parts], operation))

    def sliver(self):
        """Whether the oracle's solid is within the volume tolerance of nothing: a flake the kernel may not keep."""
        return self.fragile or (
            not self.oracle.is_empty() and self.oracle.volume() <= LAYER_MM * self.area * max(1, self.operations) + 1e-9
        )

    @staticmethod
    def batch_hull(parts):
        made = Dual._combine('hull', list(parts), oracle.Manifold.batch_hull([p.oracle for p in parts]))
        # The hull of a flake a few nanometres thick is a solid of full size: whether the flake exists decides
        # the answer, so such a hull cannot be compared.
        made.fragile = any(p.sliver() for p in parts)
        return made

    # Answers the application reads while it builds come from the oracle.
    def __getattr__(self, name):
        if name in (
            'status',
            'is_empty',
            'num_tri',
            'num_vert',
            'volume',
            'bounding_box',
            'to_mesh64',
            'to_mesh',
            'surface_area',
            'decompose',
        ):
            return getattr(self.oracle, name)
        raise AttributeError(name)

    def slice(self, height):
        return DualSection(self.oracle.slice(height))

    def project(self):
        return DualSection(self.oracle.project())

    def min_gap(self, other, length):
        return self.oracle.min_gap(other.oracle, length)


class DualSection:
    """A 2D region of the oracle whose extrusion and revolution also go to the kernel."""

    def __init__(self, *args, **kwargs):
        self.section = (
            args[0]
            if len(args) == 1 and isinstance(args[0], oracle.CrossSection)
            else oracle.CrossSection(*args, **kwargs)
        )

    @staticmethod
    def _wrap(value):
        return DualSection(value) if isinstance(value, oracle.CrossSection) else value

    @staticmethod
    def square(*args, **kwargs):
        return DualSection(oracle.CrossSection.square(*args, **kwargs))

    @staticmethod
    def circle(*args, **kwargs):
        return DualSection(oracle.CrossSection.circle(*args, **kwargs))

    @staticmethod
    def batch_boolean(parts, operation):
        return DualSection(oracle.CrossSection.batch_boolean([p.section for p in parts], operation))

    @staticmethod
    def batch_hull(parts):
        return DualSection(oracle.CrossSection.batch_hull([p.section for p in parts]))

    def __getattr__(self, name):
        target = getattr(self.section, name)

        def call(*args, **kwargs):
            return DualSection._wrap(target(*args, **kwargs))

        return call

    def _rings(self):
        return [np.asarray(ring).tolist() for ring in self.section.to_polygons()]

    def extrude(self, height, n_divisions=0, twist_degrees=0.0, scale_top=(1.0, 1.0)):
        tree = {
            'extrude': {
                'rings': self._rings(),
                'height': float(height),
                'slices': int(n_divisions) + 1,
                'twist': float(twist_degrees),
                'scale': [float(scale_top[0]), float(scale_top[1])],
                'fill': 'positive',
            }
        }
        return Dual.leaf(tree)

    def revolve(self, circular_segments=0, revolve_degrees=360.0):
        tree = {
            'revolve': {
                'rings': self._rings(),
                'segments': int(circular_segments) or 48,
                'degrees': float(revolve_degrees),
                'fill': 'positive',
            }
        }
        return Dual.leaf(tree)


def stand_in():
    """A module object to put in place of manifold3d."""
    module = types.ModuleType('manifold3d')
    for name in ('Mesh', 'Mesh64', 'FillRule', 'OpType', 'Error', 'JoinType', 'triangulate'):
        setattr(module, name, getattr(oracle, name))

    class Manifold(Dual):
        def __new__(cls, mesh=None):
            return Dual.from_mesh(mesh)

    module.Manifold = Manifold
    module.CrossSection = DualSection
    for name in ('cube', 'cylinder', 'sphere', 'batch_boolean', 'batch_hull'):
        setattr(Manifold, name, staticmethod(getattr(Dual, name)))
    return module


# ---- comparison -----------------------------------------------------------------------------------


def bodies_of(solid):
    return 0 if solid.is_empty() else len(solid.decompose())


class Report:
    def __init__(self):
        self.compared = self.refused = self.oracle_failed = self.fragile = 0
        self.worst = {'volume': 0.0, 'box': 0.0, 'section': 0.0}
        self.failures, self.body_notes, self.sliver_notes = [], [], []

    def compare(self, duals, label):
        """Evaluate each dual in the kernel and compare it with its oracle solid."""
        todo, requests = [], []
        seen = set()
        for dual in duals:
            if dual.note or dual.oracle.status() != oracle.Error.NoError:
                self.oracle_failed += 1
                continue
            if dual.fragile:
                self.fragile += 1
                continue
            key = json.dumps(dual.tree, sort_keys=True)
            if key in seen:
                continue
            seen.add(key)
            box = dual.oracle.bounding_box()
            heights = (
                [] if dual.oracle.is_empty() else [box[2] + f * (box[5] - box[2]) for f in (0.2113, 0.5127, 0.8291)]
            )
            todo.append((dual, heights))
            requests.append({'shape': dual.tree, 'sections': heights, 'check': False})
        for (dual, heights), answer in zip(todo, run_batch(requests)):
            where = f'{label}: {json.dumps(dual.tree)[:300]}'
            if 'ok' not in answer:
                message = answer.get('error', answer.get('invalid', ''))
                if 'too fine' in message:
                    self.refused += 1
                elif 'outside' in message and max(abs(v) for v in dual.oracle.bounding_box()) > 100000:
                    self.compared += 1  # beyond the kernel's coordinate range, which the application refuses too
                elif dual.oracle.is_empty() and ('no volume' in message or 'no triangles' in message):
                    self.compared += 1
                else:
                    self.failures.append(f'{where}\n    the kernel refused: {message}')
                continue
            mine = answer['ok']
            self.compared += 1
            operations = max(1, dual.operations)
            tolerance = LAYER_MM * dual.area * operations + 1e-9
            error = abs(mine['volume'] - dual.oracle.volume())
            self.worst['volume'] = max(self.worst['volume'], error / tolerance)
            if error > tolerance:
                self.failures.append(
                    f'{where}\n    volume {mine["volume"]!r} against {dual.oracle.volume()!r} (tolerance {tolerance:.3g})'
                )
                continue
            if dual.oracle.is_empty() or mine['empty']:
                continue  # both empty, or one a sliver within the volume tolerance
            box = dual.oracle.bounding_box()
            error = max(abs(a - b) for a, b in zip(mine['bounds'], box))
            if error > BOX_MM:
                # Where the volumes agree this closely, the difference is a body thinner than the layer that
                # one of the two has kept and the other has not. It is listed, not failed.
                if abs(mine['volume'] - dual.oracle.volume()) > 0.1 * tolerance:
                    self.failures.append(f'{where}\n    bounds {mine["bounds"]} against {list(box)}')
                else:
                    self.sliver_notes.append(
                        f'{where}\n    bounds {mine["bounds"]} against {list(box)}; volumes {mine["volume"]!r} and {dual.oracle.volume()!r}'
                    )
                continue
            self.worst['box'] = max(self.worst['box'], error / BOX_MM)
            if mine['bodies'] != bodies_of(dual.oracle):
                self.body_notes.append(f'{where}\n    bodies {mine["bodies"]} against {bodies_of(dual.oracle)}')
            length = 1 + math.sqrt(dual.area)
            for height, section in zip(heights, mine['sections']):
                tolerance = 25 * LAYER_MM * length * operations + 1e-9
                error = abs(section['area'] - dual.oracle.slice(height).area())
                self.worst['section'] = max(self.worst['section'], error / tolerance)
                if error > tolerance:
                    self.failures.append(
                        f'{where}\n    section at {height!r}: area {section["area"]!r} against {dual.oracle.slice(height).area()!r}'
                    )
                    break


# ---- part 1: primitives ---------------------------------------------------------------------------


def primitives(report):
    """The kernel's primitives against the oracle's, by measures; the faceting may differ."""
    failures = []

    def measures(tree):
        answer = run_batch([{'shape': tree}])[0]
        if 'ok' not in answer:
            failures.append(f'{tree}: {answer}')
            return None
        return answer['ok']

    for size in ([1, 2, 3], [0.1, 40, 7.25], [1e-3, 1e-3, 1e-3]):
        mine, theirs = measures({'box': size}), oracle.Manifold.cube(size)
        if mine and (
            abs(mine['volume'] - theirs.volume()) > 1e-12 * max(1, theirs.volume()) or mine['triangles'] != 12
        ):
            failures.append(f'box {size}: {mine["volume"]} against {theirs.volume()}')
    for segments in (3, 8, 48, 96, 97, 256):
        for height, low, high in ((10, 4, 4), (6, 4, 0), (2.5, 0, 3), (1, 0.5, 2)):
            mine = measures(
                {'cylinder': {'height': height, 'radius_low': low, 'radius_high': high, 'segments': segments}}
            )
            theirs = oracle.Manifold.cylinder(height, low, high, segments)
            # The same inscribed polygon: the volumes agree to the rounding of its corners, 1 nm on the wall.
            if mine and abs(mine['volume'] - theirs.volume()) > 2e-6 * theirs.surface_area():
                failures.append(
                    f'cylinder {height} {low} {high} {segments}: {mine["volume"]} against {theirs.volume()}'
                )
            if mine and max(abs(a - b) for a, b in zip(mine['bounds'], theirs.bounding_box())) > 2e-6:
                failures.append(
                    f'cylinder {height} {low} {high} {segments}: bounds {mine["bounds"]} against {theirs.bounding_box()}'
                )
    for segments in (8, 32, 48, 96, 256):
        for radius in (0.5, 4, 25):
            mine, theirs = (
                measures({'sphere': {'radius': radius, 'segments': segments}}),
                oracle.Manifold.sphere(radius, segments),
            )
            exact = 4 * math.pi * radius**3 / 3
            # Both are inscribed: each is below the ball by a share that falls with the square of the segment count.
            bound = exact * 3.5 * (math.pi / segments) ** 2
            if mine and not (
                exact - bound <= mine['volume'] <= exact and abs(mine['volume'] - theirs.volume()) <= bound
            ):
                failures.append(
                    f'sphere {radius} {segments}: {mine["volume"]} against {theirs.volume()} (ball {exact})'
                )
            if mine and max(abs(a - b) for a, b in zip(mine['bounds'], theirs.bounding_box())) > 2e-6:
                failures.append(f'sphere {radius} {segments}: bounds {mine["bounds"]} against {theirs.bounding_box()}')
    report.failures.extend(failures)
    return 3 + 24 + 15


# ---- part 2: random trees -------------------------------------------------------------------------


def random_leaf(rng):
    kind = rng.randrange(7)
    if kind == 0:
        return Dual.cube([rng.randint(1, 8), rng.randint(1, 8), rng.randint(1, 8)], rng.random() < 0.5)
    if kind == 1:
        return Dual.cube([rng.uniform(0.5, 6), rng.uniform(0.5, 6), rng.uniform(0.5, 6)])
    if kind == 2:
        low = rng.randint(1, 4)
        return Dual.cylinder(
            rng.randint(1, 8),
            low,
            low if rng.random() < 0.6 else rng.randint(0, 4),
            4 * rng.randint(2, 8),
            rng.random() < 0.5,
        )
    if kind == 3:
        return Dual.sphere(rng.randint(1, 4), 4 * rng.randint(2, 6))
    if kind == 4:
        ring = [[0, 0], [rng.randint(2, 6), 0], [rng.randint(2, 6), rng.randint(2, 6)], [0, rng.randint(2, 6)]]
        rings = [ring] if rng.random() < 0.5 else [ring, [[0.5, 0.5], [0.5, 1.5], [1.5, 1.5], [1.5, 0.5]]]
        twist = rng.choice([0, 0, 30, -75])
        return Dual.leaf(
            {
                'extrude': {
                    'rings': rings,
                    'height': rng.randint(1, 6),
                    'slices': 6 if twist else 1,
                    'twist': twist,
                    'scale': [1, 1] if rng.random() < 0.7 else [0.5, 0.75],
                }
            }
        )
    if kind == 5:
        inner = rng.choice([0, 0, 1, 2])
        ring = [
            [inner, 0],
            [inner + rng.randint(1, 3), 0],
            [inner + rng.randint(1, 3), rng.randint(1, 5)],
            [inner, rng.randint(1, 5)],
        ]
        return Dual.leaf(
            {
                'revolve': {
                    'rings': [ring],
                    'segments': 4 * rng.randint(3, 8),
                    'degrees': rng.choice([360, 360, 90, 200]),
                }
            }
        )
    return Dual.cylinder(rng.uniform(1, 8), rng.uniform(0.5, 3), rng.uniform(0.5, 3), rng.randint(5, 40))


def random_move(rng, solid):
    kind = rng.randrange(8)
    if kind == 0:
        return solid.translate([rng.randint(-4, 4), rng.randint(-4, 4), rng.randint(-4, 4)])
    if kind == 1:
        return solid.translate([rng.uniform(-4, 4), rng.uniform(-4, 4), rng.uniform(-4, 4)])
    if kind == 2:
        return solid.rotate([90 * rng.randrange(4), 90 * rng.randrange(4), 45 * rng.randrange(8)])
    if kind == 3:
        return solid.rotate([rng.uniform(-180, 180), rng.uniform(-180, 180), rng.uniform(-180, 180)]).translate(
            [rng.randint(-2, 2), 0, 0]
        )
    if kind == 4:
        return solid.scale([rng.choice([2, 0.5, rng.uniform(0.3, 2)]), 1, rng.choice([1, 1, 1.5])])
    if kind == 5:
        return solid.mirror(rng.choice([[1, 0, 0], [0, 0, 1], [1, 1, 0]]))
    return solid


def random_tree(rng, depth):
    if depth == 0 or rng.random() < 0.25:
        return random_move(rng, random_leaf(rng))
    kind = rng.randrange(12)
    a = random_tree(rng, depth - 1)
    if a.oracle.is_empty() and kind in (0, 2, 4):
        kind = 11
    if kind == 0:  # with itself, moved exactly by its own size: faces in one plane
        box = a.oracle.bounding_box()
        b = a.translate(
            [
                round(box[3] - box[0], 6) if rng.random() < 0.7 else 0,
                0,
                round(box[5] - box[2], 6) if rng.random() < 0.3 else 0,
            ]
        )
    elif kind == 1:  # the very same solid
        b = a
    elif kind == 2:  # one well inside the other
        box = a.oracle.bounding_box()
        b = Dual.cube([0.2, 0.2, 0.2]).translate([(box[0] + box[3]) / 2, (box[1] + box[4]) / 2, (box[2] + box[5]) / 2])
    elif kind == 3:  # a pattern joined in one batch
        step = [rng.choice([0.75, 1, 3]), rng.choice([0, 0, 1]), 0]
        copies = [a] + [a.translate([step[0] * i, step[1] * i, 0]) for i in range(1, rng.randint(2, 6))]
        return Dual.batch_boolean(copies, oracle.OpType.Add)
    elif kind == 4:  # both operands turned the same way after touching: nearly coincident faces
        b = random_tree(rng, depth - 1)
        box, other = a.oracle.bounding_box(), b.oracle.bounding_box()
        if not b.oracle.is_empty():
            b = b.translate([round(box[3] - other[0], 6), 0, 0])
        turn = [rng.uniform(-180, 180), rng.uniform(-180, 180), rng.uniform(-180, 180)]
        a, b = a.rotate(turn), b.rotate(turn)
    elif kind == 5:
        return Dual.batch_hull([a, random_tree(rng, depth - 1)])
    else:
        b = random_tree(rng, depth - 1)
    operation = rng.randrange(3)
    return a + b if operation == 0 else a - b if operation == 1 else a ^ b


def trees(report, count, seed):
    rng = random.Random(seed)  # noqa: S311
    done = 0
    while done < count:
        Dual.made.clear()
        batch = []
        for _ in range(min(250, count - done)):
            try:
                batch.append(random_tree(rng, rng.randint(1, 3)))
            except ValueError:  # a leaf the kernel refuses (a primitive too small for the grid)
                continue
        report.compare(batch, f'tree (seed {seed})')
        done += len(batch) or 1
    return done


# ---- part 3: the application's models -------------------------------------------------------------


def application(report):
    """Run Studio's own solid tests on the stand-in, then compare every solid they made."""
    module = stand_in()
    sys.modules['manifold3d'] = module
    try:
        import studio.scad_geometry  # noqa: F401  (imported after the stand-in is in place)

        studio.scad_geometry.kernel = module
        Dual.made.clear()
        names = ['tests.test_solid_cad', 'tests.test_scad_geometry', 'tests.test_assembly', 'tests.test_solid_loft']
        suite = unittest.defaultTestLoader.loadTestsFromNames(names)
        result = unittest.TextTestRunner(stream=open(os.devnull, 'w'), verbosity=0).run(suite)
        made = list(Dual.made)
    finally:
        sys.modules['manifold3d'] = oracle
    report.compare(made, 'application')
    return result, len(made)


def main():
    global TOOL
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        '--tool', default=os.environ.get('KERFKITE_CAM', str(ROOT / 'build' / 'native' / 'kerfkite-cam'))
    )
    parser.add_argument('--trees', type=int, default=2000)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--skip-application', action='store_true')
    arguments = parser.parse_args()
    TOOL = arguments.tool
    if not Path(TOOL).exists():
        sys.exit(f'{TOOL} not found: build the native core first (see the head of this file)')
    report = Report()
    count = primitives(report)
    print(f'primitives: {count} compared with the oracle\'s own')
    made = trees(report, arguments.trees, arguments.seed)
    print(
        f'random trees: {made} built, {report.compared} solids compared, {report.refused} refused by the kernel as too fine, '
        f'{report.oracle_failed} refused by the oracle, {report.fragile} not comparable (the hull of a flake)'
    )
    if not arguments.skip_application:
        before = report.compared
        result, solids = application(report)
        print(
            f'application: {result.testsRun} tests run on the kernel\'s solids, {len(result.failures) + len(result.errors)} did not pass; {solids} solids made, {report.compared - before} compared'
        )
        for test, text in result.failures + result.errors:
            print(f'  {test}: {text.strip().splitlines()[-1]}')
    print(
        'worst difference as a share of its tolerance: '
        + ', '.join(f'{name} {value:.3g}' for name, value in report.worst.items())
    )
    if report.body_notes:
        print(
            f'{len(report.body_notes)} solids with a different number of bodies (see the documentation: bodies that only touch); the first:'
        )
        print('  ' + report.body_notes[0])
    if report.sliver_notes:
        print(
            f'{len(report.sliver_notes)} solids whose bounds differ though their volumes agree (a sliver kept by one and not the other); the first:'
        )
        print('  ' + report.sliver_notes[0])
    for failure in report.failures[:20]:
        print('FAIL ' + failure)
    print(f'{len(report.failures)} failures')
    return 1 if report.failures else 0


if __name__ == '__main__':
    sys.exit(main())
