"""Random CAD scripts through Studio on both solid backends: KerfKite's kernels against manifold3d.

    python validation/solid_backend_check.py --drawings 600 --solids 400 --seed 1

tests/test_solid_backends.py compares the two on the models Studio's own tests and documents build. This
goes wider: generated drawings (polygons with holes, Boolean operations, offsets with each kind of corner)
and generated solids (every primitive, extrusions, revolutions, polyhedra, transforms, Boolean operations
and hulls, nested up to three deep), each built by studio.scad_geometry.build_script once on each backend.
It needs the compiled extension and manifold3d, which is the oracle here.

A case counts as agreeing when the areas or volumes are within the grid's tolerance (a layer of LAYER_MM on
the perimeter or the surface) and the bounds within BOX_MM. Round offsets are given the slivers between
their arcs and the true arc as well, and the depth of one part of an arc on their bounds, because the two
divide an arc differently; where a neck is about as wide as that depth one of them may part the region and
the other not, so their rings are not counted. An offset also magnifies the rounding of a very short edge:
moving the ends of an edge of length L by a nanometre turns it by up to 1.5 nm / L, and its offset moves by
that turn times the distance, which the bounds are allowed. Solids with a twisted extrusion are built on
both but not compared: the two may split the twisted sides along different diagonals, which changes the
volume.

Every case that does not agree is printed in full, and the run fails, unless it is one of the two ways the
oracle is known to differ (docs/SOLID-KERNEL.md, Testing), each recognised by a rule and not by the size of
the difference:

* A fin without thickness on the oracle's result: the volumes agree, the oracle's surface is the larger,
  and the kernel's bounds lie inside the oracle's.
* A hull over a flake: an operand of a hull() that the oracle holds as triangles enclosing no volume (two
  solids that only touch, intersected), where the kernel has the empty solid. The oracle's hull then
  reaches to the flake. Such a case is not comparable.
* A hull that is not one: a hull contains every point of its operands. Where the oracle's hull of two
  operands of the script leaves their vertices outside it and the kernel's holds them, the oracle is wrong.

Those are counted and printed as well, for the reader to judge.
"""

import argparse
import math
import os
import random
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from studio import solid_kernel  # noqa: E402
from studio.scad_geometry import build_script  # noqa: E402

LAYER_MM = 4e-6
BOX_MM = 2e-5


def number(rng, low, high):
    return round(rng.uniform(low, high), 2)


def star(rng, count, radius, cx=0.0, cy=0.0):
    points = []
    for i in range(count):
        angle = math.tau * (i + rng.uniform(0.1, 0.9)) / count
        reach = radius * rng.uniform(0.35, 1.0)
        points.append([round(cx + reach * math.cos(angle), 3), round(cy + reach * math.sin(angle), 3)])
    return points


def random_drawing(rng):
    """(script, the allowance in mm² for the faceting of its round corners, the allowance in mm for its bounds)."""
    shape = f'polygon({star(rng, rng.randint(3, 14), 20)});'
    if rng.random() < 0.5:
        shape = f'difference(){{ {shape} polygon({star(rng, rng.randint(3, 8), 4)}); }}'
    if rng.random() < 0.3:
        other = star(rng, rng.randint(3, 9), 12, rng.uniform(-15, 15), rng.uniform(-15, 15))
        shape = f'{rng.choice(["union", "intersection", "difference"])}(){{ {shape} polygon({other}); }}'
    distance = round(rng.choice([-1, 1]) * rng.uniform(0.05, 3), 3)
    kind = rng.choice(['miter', 'bevel', 'round', 'none'])
    if kind == 'none':
        return shape, 0.0, 0.0
    # What the rounding of the shortest edge of the shape, as the kernel has it, lets the offset move by.
    turned = abs(distance) * 1.5e-6 / shortest_edge(shape)
    if kind == 'miter':
        return f'offset(delta={distance}) {shape}', 0.0, turned
    if kind == 'bevel':
        return f'offset(delta={distance},chamfer=true) {shape}', 0.0, turned
    segments = rng.randint(5, 64)
    part = 1.0625 * math.tau / segments
    # r² t³ / 12 for each part of an arc; these shapes have at most six turns of round corners.
    slivers = distance * distance * part**2 / 12 * 6 * math.tau
    return f'offset(r={distance},$fn={segments}) {shape}', slivers, turned + abs(distance) * (1 - math.cos(part / 2))


def shortest_edge(script):
    os.environ['KERFKITE_SOLID_BACKEND'] = 'kernel'
    try:
        rings = build_script(script)[1].to_polygons()
    except ValueError:
        return 1.0
    return min(float(np.linalg.norm(np.roll(ring, -1, axis=0) - ring, axis=1).min()) for ring in rings)


def primitive(rng):
    kind = rng.randint(0, 5)
    segments = rng.choice([6, 8, 12, 16, 20])
    flag = rng.choice(['true', 'false'])
    if kind == 0:
        return f'cube([{number(rng, 1, 8)},{number(rng, 1, 8)},{number(rng, 1, 8)}],center={flag});'
    if kind == 1:
        radii = f'r1={number(rng, 0.5, 4)},r2={number(rng, 0, 4)}'
        return f'cylinder(h={number(rng, 1, 8)},{radii},center={flag},$fn={segments});'
    if kind == 2:
        profile = [
            [number(rng, 0.2, 2), 0],
            [number(rng, 3, 5), number(rng, 0, 1)],
            [number(rng, 2, 5), number(rng, 2, 4)],
            [number(rng, 0.2, 1.5), number(rng, 2, 5)],
        ]
        angle = rng.choice([360, 90, -120, 200, 45.5])
        return f'rotate_extrude(angle={angle},$fn={segments}) polygon({profile});'
    if kind == 3:
        twist = rng.choice([0, 0, 30, -45])
        slices = rng.randint(1, 4) if twist else 1
        scale = rng.choice(['1', '0.5', '[1.5,0.7]', '1'])
        outline = (
            f'offset(delta={number(rng, 0.1, 0.8)}) square([{number(rng, 1, 5)},{number(rng, 1, 5)}],center=true);'
        )
        return f'linear_extrude(height={number(rng, 1, 6)},twist={twist},slices={slices},scale={scale},center={flag}) {outline}'
    if kind == 4:
        ring = f'difference(){{ circle({number(rng, 2, 4)},$fn={segments}); circle({number(rng, 0.5, 1.5)},$fn={segments}); }}'
        return f'linear_extrude(height={number(rng, 1, 6)}) {ring}'
    corners = f'[[0,0,0],[{number(rng, 1, 4)},0,0],[0,{number(rng, 1, 4)},0],[0,0,{number(rng, 1, 4)}]]'
    return f'polyhedron({corners},[[0,1,2],[0,3,1],[0,2,3],[1,3,2]]);'


def random_solid(rng, depth, hulled):
    """A script; the operands of each hull() in it are added to `hulled`, as scripts of their own."""
    if depth == 0 or rng.random() < 0.25:
        script = primitive(rng)
    else:
        operation = rng.choice(['union', 'difference', 'intersection', 'hull', 'union', 'difference'])
        operands = [random_solid(rng, depth - 1, hulled), random_solid(rng, depth - 1, hulled)]
        if operation == 'hull':
            hulled.extend(operands)
        script = f'{operation}(){{ {operands[0]} {operands[1]} }}'
    move = rng.randint(0, 6)
    if move == 0:
        return f'translate([{number(rng, -3, 3)},{number(rng, -3, 3)},{number(rng, -3, 3)}]) {script}'
    if move == 1:
        return f'rotate([{rng.choice([0, 90, 33.3])},{rng.choice([0, 180, -17])},{rng.choice([0, 45, 270, 12.5])}]) {script}'
    if move == 2:
        return f'scale([{rng.choice([1, 2, -1, 0.5])},{rng.choice([1, 1.5, -1])},{rng.choice([1, 0.7])}]) {script}'
    if move == 3:
        return f'mirror([{rng.choice([1, 0, 1])},{rng.choice([0, 1, 2])},{rng.choice([0, 0, 1])}]) {script}'
    if move == 4:
        shear = f'[[1,{number(rng, -0.5, 0.5)},0,{number(rng, -2, 2)}],[0,1,{number(rng, -0.5, 0.5)},0],[0,0,{number(rng, 0.5, 2)},0]]'
        return f'multmatrix({shear}) {script}'
    return script


def perimeter(section):
    return sum(float(np.linalg.norm(np.roll(ring, -1, axis=0) - ring, axis=1).sum()) for ring in section.to_polygons())


def build(script, backend):
    """What is compared, or the message of the refusal."""
    os.environ['KERFKITE_SOLID_BACKEND'] = backend
    try:
        dimension, shape = build_script(script)
    except ValueError as error:
        return str(error)
    if dimension == 2:
        return {'size': shape.area(), 'skin': perimeter(shape), 'box': shape.bounds(), 'parts': shape.num_contour()}
    return {'size': shape.volume(), 'skin': shape.surface_area(), 'box': shape.bounding_box(), 'parts': None}


def flake(script):
    """Whether the oracle holds this shape as triangles that enclose no volume."""
    from studio.scad_geometry import ScriptGeometryBudget
    from studio.scad_language import evaluate

    os.environ['KERFKITE_SOLID_BACKEND'] = 'manifold'
    budget = ScriptGeometryBudget()
    try:
        shape = budget.combine('union', [budget.node(node, 0) for node in evaluate(script, None)])
    except ValueError:
        return False
    return not shape.is_empty() and shape.volume() <= 1e-9


def outside(points, solid):
    """The largest distance by which a point lies outside a face of the solid: none does, of a convex one that holds them."""
    mesh = solid.to_mesh64()
    vertices = np.asarray(mesh.vert_properties)[:, :3]
    a, b, c = (vertices[np.asarray(mesh.tri_verts).astype(np.int64)[:, i]] for i in range(3))
    normals = np.cross(b - a, c - a)
    lengths = np.linalg.norm(normals, axis=1)
    keep = lengths > 1e-12
    normals = normals[keep] / lengths[keep, None]
    return float(np.max(np.einsum('tk,tpk->tp', normals, points[None, :, :] - a[keep, None, :])))


def oracle_hull_wrong(first, second):
    """Whether the oracle's hull of two shapes leaves their vertices outside it while the kernel's holds them."""
    found = {}
    for backend in ('manifold', 'kernel'):
        os.environ['KERFKITE_SOLID_BACKEND'] = backend
        try:
            operands = [build_script(operand)[1] for operand in (first, second)]
            hull = build_script(f'hull(){{ {first} {second} }}')[1]
        except ValueError:
            return False
        points = np.vstack([np.asarray(operand.to_mesh64().vert_properties)[:, :3] for operand in operands])
        found[backend] = outside(points, hull)
    return found['manifold'] > 1e-4 and found['kernel'] <= 1e-5


class Tally:
    def __init__(self):
        self.agree = self.refused_by_both = self.too_fine = self.not_compared = 0
        self.oracle_flakes, self.differences = [], []
        self.worst = 0.0

    def take(self, script, allowance=0.0, depth=0.0, compare=True, hulled=()):
        theirs, ours = build(script, 'manifold'), build(script, 'kernel')
        if isinstance(ours, str) and solid_kernel._TOO_FINE in ours:
            self.too_fine += 1
        if isinstance(theirs, str) and isinstance(ours, str):
            self.refused_by_both += 1
            return
        if not compare and (isinstance(theirs, str) or isinstance(ours, str)):
            self.not_compared += 1  # what a twisted side leaves of a cut may be nothing on one of them
            return
        if not isinstance(theirs, str) and not isinstance(ours, str):
            if not compare:
                self.not_compared += 1
                return
            tolerance = LAYER_MM * max(theirs['skin'], ours['skin']) + allowance + 1e-7
            size = abs(theirs['size'] - ours['size'])
            box = max(abs(a - b) for a, b in zip(theirs['box'], ours['box']))
            if size <= tolerance and box <= BOX_MM + depth and (depth or theirs['parts'] == ours['parts']):
                self.agree += 1
                self.worst = max(self.worst, size / tolerance)
                return
            half = len(ours['box']) // 2
            inside = all(
                theirs['box'][i] - BOX_MM <= ours['box'][i]
                and ours['box'][i + half] <= theirs['box'][i + half] + BOX_MM
                for i in range(half)
            )
            if theirs['parts'] is None and size <= tolerance and theirs['skin'] > ours['skin'] and inside:
                self.oracle_flakes.append(('a fin without thickness', script, theirs, ours))
                return
        if any(flake(operand) for operand in hulled):
            self.oracle_flakes.append(('a hull over a flake', script, theirs, ours))
        elif any(oracle_hull_wrong(*hulled[i : i + 2]) for i in range(0, len(hulled), 2)):
            self.oracle_flakes.append(('a hull of the oracle that does not hold its operands', script, theirs, ours))
        else:
            self.differences.append(('DIFFERENCE', script, theirs, ours))

    def report(self, name):
        print(
            f'{name}: {self.agree} agree (largest difference {self.worst:.3g} of its tolerance), '
            f'{self.refused_by_both} refused by both, {self.not_compared} not compared (a twisted extrusion), '
            f'{self.too_fine} refused by the kernel as too fine, {len(self.oracle_flakes)} where the oracle '
            f'is wrong by a rule, {len(self.differences)} differences'
        )
        for label, script, theirs, ours in self.oracle_flakes + self.differences:
            print(f'  {label}: {script}\n    manifold3d: {theirs}\n    kernel:     {ours}')
        return not self.differences


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--drawings', type=int, default=600)
    parser.add_argument('--solids', type=int, default=400)
    parser.add_argument('--seed', type=int, default=1)
    arguments = parser.parse_args()
    if not solid_kernel.kernel_available():
        sys.exit('The compiled extension is not installed: python setup.py build_ext --inplace')
    rng = random.Random(arguments.seed)  # noqa: S311 (test cases, not secrets)
    drawings, solids = Tally(), Tally()
    for _ in range(arguments.drawings):
        script, allowance, depth = random_drawing(rng)
        drawings.take(script, allowance, depth)
    for _ in range(arguments.solids):
        hulled = []
        script = random_solid(rng, rng.randint(1, 3), hulled)
        solids.take(script, compare='twist=30' not in script and 'twist=-45' not in script, hulled=hulled)
    passed = drawings.report('drawings')
    passed = solids.report('solids') and passed
    sys.exit(0 if passed else 1)


if __name__ == '__main__':
    main()
