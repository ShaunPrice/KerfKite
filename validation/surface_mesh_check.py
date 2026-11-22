"""Compare the native core's surface mesher with the Python mesher it replaces (studio/cad_exchange.py).

    python validation/surface_mesh_check.py [--cases 1500] [--seed 1]

The core tessellates Bézier and B-spline patches and joined sheets (native/core/src/surface.cpp) as the
reference does, and cuts a drawn outline with its exact planar arrangement where the reference clips cells
with Shapely and triangulates them by constrained Delaunay triangulation. For each generated surface, both are
asked; the surfaces cover Bézier and clamped B-spline patches of every size and degree, weights, named
parameters and expressions, sheets of two to six patches joined along U or V with position and tangent
continuity, trim bounds, outlines with holes, and requests the reference refuses (bad nets, degrees,
weights, trims, joins, loops and outlines). The criteria:

* the same refusal, or both mesh the surface; outlines whose loops touch are declined by the core;
* without an outline: the same triangles, every vertex within 1e-12 of the surface's size of the reference's,
  the same area to 1e-12 and the same resolved patches of a sheet, to the bit;
* with an outline: the bounds within 1e-8 of the surface's size, the same resolved patches, the same number of
  faces, and in each face the same number of vertices and triangles once points within 1e-7 of the size of one
  another are taken as one (Shapely's noding meets a grid line where it passes through a corner of the outline
  and may keep the point twice; the core's grid, rounded to the nanometre, passes beside such a corner and the
  core merges what it finds there); every vertex of either within 1e-6 of the size of a vertex of the other
  (both cut the same cells at the same points); and the same triangles but where a cell's four corners share a
  circle: there Shapely's triangulator takes either diagonal as rounding decides, and the core the one from the
  lowest corner, so an edge only one mesh has must be the diagonal of a quadrilateral the other mesh splits
  along its other diagonal.

The check fails on any other difference.
"""

import argparse
import collections
import json
import math
import random
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from studio import cad_exchange  # noqa: E402
from studio.cad_exchange import surface_mesh  # noqa: E402


def control_net(rng, rows, cols, kind, offset=(0.0, 0.0)):
    net = {
        'kind': kind,
        'poles': [
            [[offset[0] + i * 10 + rng.uniform(-3, 3), offset[1] + j * 10 + rng.uniform(-3, 3), rng.uniform(-8, 8)] for j in range(cols)]
            for i in range(rows)
        ],
    }  # fmt: skip
    if kind == 'bspline' and rng.random() < 0.7:
        net['degrees'] = [rng.randint(1, rows - 1), rng.randint(1, cols - 1)]
    if rng.random() < 0.5:
        net['weights'] = [[round(rng.uniform(0.2, 4), 3) for _ in range(cols)] for _ in range(rows)]
    if rng.random() < 0.15:
        net['parameters'] = {'h': rng.uniform(1, 5), 'w': 'h * 2 + 1', 'd': '-(w - h) / 2'}
        net['poles'][0][0] = ['w', 'h', 'd']
    return net


def outline(rng):
    def star(cx, cy, radius, corners):
        points = []
        for k in range(corners):
            angle = 2 * math.pi * (k + rng.uniform(-0.3, 0.3)) / corners
            r = radius * rng.uniform(0.55, 1.0)
            points.append([min(1.0, max(0.0, cx + r * math.cos(angle))), min(1.0, max(0.0, cy + r * math.sin(angle)))])
        return points

    cx, cy = rng.uniform(0.35, 0.65), rng.uniform(0.35, 0.65)
    loops = [star(cx, cy, rng.uniform(0.15, 0.6), rng.randint(3, 40))]
    for k in range(rng.choice([0, 0, 1, 2, 3])):
        loops.append(star(cx + rng.uniform(-0.05, 0.05), cy + (k - 1) * 0.09, 0.03, rng.randint(3, 7)))
    if rng.random() < 0.5:
        loops[0].reverse()
    return loops


def surface(rng):
    roll = rng.random()
    if roll < 0.45:
        given = control_net(rng, rng.randint(2, 8), rng.randint(2, 8), rng.choice(['bezier', 'bspline']))
    else:
        count, axis = rng.randint(2, 6), rng.choice(['u', 'v'])
        rows, cols, kind = rng.randint(2, 6), rng.randint(2, 6), rng.choice(['bezier', 'bspline'])
        patches = []
        for i in range(count):
            step = 10 * ((rows if axis == 'u' else cols) - 1) * i
            net = control_net(rng, rows, cols, kind, (step, 0) if axis == 'u' else (0, step))
            if kind == 'bspline':
                net['degrees'] = [min(2, rows - 1), min(3, cols - 1)]
            if i and rng.random() < 0.5:
                net['continuity'] = rng.choice(['tangent', 'tangent', 'position'])
            patches.append(net)
        given = {'kind': 'sheet', 'axis': axis, 'patches': patches}
    if rng.random() < 0.4:
        u0, v0 = rng.uniform(0, 0.45), rng.uniform(0, 0.45)
        given['trim'] = [u0, rng.uniform(u0 + 0.05, 1), v0, rng.uniform(v0 + 0.05, 1)]
    if rng.random() < 0.5:
        given['boundary'] = outline(rng)
    return refused(rng, given) if rng.random() < 0.12 else given


def refused(rng, given):
    """A surface with one thing wrong, as an editor or a hand-written request might send it."""
    net = given['patches'][1] if given.get('kind') == 'sheet' else given
    change = rng.randrange(14)
    if change == 0:
        net['poles'] = net['poles'][:1]
    elif change == 1:
        net['poles'][0] = net['poles'][0][:1]
    elif change == 2:
        net['poles'][-1] = net['poles'][-1][:-1]
    elif change == 3:
        net['poles'][0][0] = [1, 2]
    elif change == 4:
        net['kind'] = 'nurbs'
    elif change == 5:
        net['kind'], net['degrees'] = 'bspline', [0, 1]
    elif change == 6:
        net['weights'] = [[0.0] * len(net['poles'][0]) for _ in net['poles']]
    elif change == 7:
        given['trim'] = [0.5, 0.4, 0, 1]
    elif change == 8:
        given['trim'] = [0, 1, 0]
    elif change == 9:
        given['boundary'] = [[[0.1, 0.1], [0.9, 0.9], [0.1, 0.9], [0.9, 0.1]]]
    elif change == 10:
        given['boundary'] = [[[0.1, 0.1], [0.4, 0.1], [0.4, 0.4]], [[0.6, 0.6], [0.8, 0.6], [0.8, 0.8]]]
    elif change == 11:
        given['boundary'] = [[[0.1, 0.1], [0.1, 0.1], [0.4, 0.4]]]
    elif change == 12:
        given['boundary'] = [[[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]], [[0.1, 0.1], [0.3, 0.2], [0.2, 0.3]]]
    else:
        net['poles'][0][0] = ['undefined_name', 0, 0]
    return given


def answer(given, backend):
    try:
        return surface_mesh(json.loads(json.dumps(given)), backend=backend)
    except ValueError as error:
        return 'refused: ' + str(error)
    except NotImplementedError:
        return None


def welded(vertices, triangles, near):
    """Vertices within near of one another as one, and the triangles that keep three corners."""
    order = np.argsort(vertices[:, 0], kind='stable')
    one = list(range(len(vertices)))
    for k, i in enumerate(order):
        for j in order[k + 1 :]:
            if vertices[j, 0] - vertices[i, 0] > near:
                break
            if np.abs(vertices[j] - vertices[i]).max() <= near:
                one[j] = one[i]
    keep = sorted(set(one))
    index = {i: k for k, i in enumerate(keep)}
    kept = [[index[one[i]] for i in t] for t in triangles]
    return vertices[keep], [t for t in kept if len(set(t)) == 3]


def faces_of(mesh):
    """The mesh's faces, each its vertices and its triangles over them, ordered by their lowest corner."""
    vertices, triangles = np.array(mesh['vertices']), mesh['triangles']
    parent = list(range(len(vertices)))

    def root(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for a, b, c in triangles:
        parent[root(a)] = root(b)
        parent[root(b)] = root(c)
    groups = collections.defaultdict(list)
    for t in triangles:
        groups[root(t[0])].append(t)
    faces = []
    for group in groups.values():
        used = sorted({i for t in group for i in t})
        local = {i: k for k, i in enumerate(used)}
        faces.append((vertices[used], [[local[i] for i in t] for t in group]))
    return sorted(faces, key=lambda face: tuple(face[0].min(axis=0)))


def same_cells(ours, my_triangles, theirs, your_triangles, size):
    """None when two faces cut the same cells at the same points (every vertex of either within 1e-6 of the
    surface's size of one of the other's) and split them alike but for the diagonal of a cell whose corners share
    a circle: every edge only one face has is the diagonal of a quadrilateral the other splits the other way."""
    place = []
    for one, other in ((ours, theirs), (theirs, ours)):
        for start in range(0, len(one), 512):
            distance = np.sqrt(((one[start : start + 512, None, :] - other[None, :, :]) ** 2).sum(axis=2))
            if distance.min(axis=1).max() > 1e-6 * size:
                return f'a vertex is {distance.min(axis=1).max():g} from the other mesh'
            if one is ours:
                place.extend(distance.argmin(axis=1).tolist())

    def sides(triangles):
        found = collections.defaultdict(list)
        for t in triangles:
            for k in range(3):
                found[frozenset((t[k], t[(k + 1) % 3]))].append(frozenset(t))
        return found

    mine, yours = sides([[place[i] for i in t] for t in my_triangles]), sides(your_triangles)
    quads = []
    for one, other in ((mine, yours), (yours, mine)):
        corners = collections.Counter()
        for edge, triangles in one.items():
            if edge in other:
                continue
            if len(triangles) != 2:
                return 'a triangle differs from the reference by more than a diagonal'
            corners[triangles[0] | triangles[1]] += 1
        quads.append(corners)
    if quads[0] != quads[1]:
        return 'cells are split differently from the reference beyond their diagonals'
    return None


def compare(given, core, reference):
    """None when the two agree by the criteria, otherwise what differs."""
    if isinstance(core, str) or isinstance(reference, str):
        return None if core == reference else f'core {str(core)[:90]!r}, reference {str(reference)[:90]!r}'
    size = max(1.0, *(abs(b) for b in reference['bounds']))
    if core['faces'] != reference['faces'] or core['solids'] != reference['solids']:
        return 'faces or solids differ'
    if 'boundary' not in given:
        if core['triangles'] != reference['triangles']:
            return 'triangles differ'
        moved = float(np.abs(np.array(core['vertices']) - np.array(reference['vertices'])).max())
        if moved > 1e-12 * size:
            return f'a vertex moved {moved:g}'
        if abs(core['area'] - reference['area']) > 1e-12 * reference['area']:
            return 'area differs'
        if json.dumps(core.get('resolvedPatches'), sort_keys=True) != json.dumps(
            reference.get('resolvedPatches'), sort_keys=True
        ):
            return 'resolved patches differ'
        return None
    if max(abs(a - b) for a, b in zip(core['bounds'], reference['bounds'])) > 1e-8 * size:
        return 'bounds differ'
    if json.dumps(core.get('resolvedPatches'), sort_keys=True) != json.dumps(
        reference.get('resolvedPatches'), sort_keys=True
    ):
        return 'resolved patches differ'
    # Each face (piece) on its own, as points where the faces of a sheet meet are in both.
    # Points within 1e-7 of the size of one another are one: the reference keeps two where Shapely's noding finds
    # a grid line through a corner of the outline twice, and the core, whose grid passes beside the corner by a
    # step of a nanometre, one or two a little apart.
    near = 1e-7 * size
    ours = [welded(v, t, near) for v, t in faces_of(core)]
    theirs = [welded(v, t, near) for v, t in faces_of(reference)]
    if [len(v) for v, _ in ours] != [len(v) for v, _ in theirs] or [len(t) for _, t in ours] != [
        len(t) for _, t in theirs
    ]:
        return 'the faces have different numbers of vertices or triangles'
    for (mine, my_triangles), (yours, your_triangles) in zip(ours, theirs, strict=True):
        problem = same_cells(mine, my_triangles, yours, your_triangles, size)
        if problem:
            return problem
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cases', type=int, default=1500)
    parser.add_argument('--seed', type=int, default=1)
    args = parser.parse_args()
    if cad_exchange._core_cam is None:
        raise SystemExit('The compiled extension is not installed: there is no core to compare.')
    rng = random.Random(args.seed)  # noqa: S311 - generated cases, not secrets
    tally, failures, worst = collections.Counter(), [], collections.defaultdict(float)
    for _ in range(args.cases):
        given = surface(rng)
        core, reference = answer(given, 'native'), answer(given, 'python')
        kind = ('outline' if 'boundary' in given else 'net') + (' sheet' if given.get('kind') == 'sheet' else '')
        if core is None:
            tally[kind, 'declined'] += 1
            continue
        problem = compare(given, core, reference)
        if problem:
            failures.append((given, problem))
            continue
        if isinstance(reference, str):
            tally[kind, 'refused alike'] += 1
        else:
            tally[kind, 'meshed alike'] += 1
            if 'boundary' in given:
                worst[kind] = max(worst[kind], abs(core['area'] - reference['area']) / reference['area'])
    for kind in sorted({k for k, _ in tally}):
        print(
            f'{kind:14s} {tally[kind, "meshed alike"]:5d} meshed alike, {tally[kind, "refused alike"]:4d} refused alike, '
            f'{tally[kind, "declined"]:3d} declined'
            + (f'; areas within {worst[kind]:.2g} of each other' if kind in worst else '')
        )
    for given, problem in failures[:5]:
        print('DIFFERENT', problem, '\n  ', json.dumps(given)[:400])
    if failures:
        raise SystemExit(f'{len(failures)} surfaces differ beyond the criteria.')
    if not sum(n for (k, what), n in tally.items() if what == 'meshed alike' and k.startswith('outline')):
        raise SystemExit('No outline was meshed: the comparison did not exercise the trimmed mesher.')
    print('PASS the core meshes every surface as the reference does, within the criteria.')


if __name__ == '__main__':
    main()
