"""Compare the native core's DXF reader with the Python reader it replaces (ezdxf's, in studio/program_import.py).

    python validation/dxf_import_check.py [--cases 1500] [--seed 1]

The core reads DXF itself (native/core/src/dxf.cpp) and flattens the true curves, where the reference
flattens ezdxf's Bézier approximations of them, so the two agree to the tolerance and not vertex for vertex.
For each generated drawing, written by ezdxf, both readers are asked; the drawings cover lines, polylines
with bulges in their own coordinate systems, 3D polylines, circles, arcs, ellipses and splines (polynomial
and rational), block references with base points, scales, rotations, mirroring, arrays and nesting, XCLIP
windows (some unsupported), layers with colour numbers, true colours, off and frozen, units, entities the
readers skip, and drawings beyond the limits. Splines are given by control points or by fit points, with end
tangents or without. The criteria:

* the same refusal, or both read the drawing;
* the same layers with the same colours and visibility, the same objects with the same names and layers,
  the same number of paths in each, and the same warnings;
* each path's ends within 1e-6 mm of the reference's, and every vertex of either path within the bound of
  the other path: 0.05 mm (both readers' chord tolerance) plus 3e-4 of the path's size (the radial error of
  ezdxf's cubic approximation of a quarter circle, which the reference flattens);
* in a drawing with XCLIP windows, the runs of each layer and kind of entity compared as sets of points,
  because the readers' chords cross a window's boundary at different points and graze it differently:
  every point of either reader's runs within the bound of the other's (with 3e-4 of the whole drawing's
  size, since a run does not tell its curve's radius), except within twice the bound of a window's boundary;
* a spline in model space compared with the curve itself, within 0.03 mm, because the reference draws
  rational splines approximately, splines of degree 1 and 2 with errors of millimetres, and closed (periodic)
  splines over the whole knot vector instead of the curve's domain.

Refusals the core does not share are counted by kind and listed; the check fails on any other difference.
"""

import argparse
import collections
import io
import math
import random
import sys
from pathlib import Path

import logging

import ezdxf
import numpy as np
from ezdxf import units
from ezdxf.xclip import XClip

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from studio.program_import import import_program  # noqa: E402

logging.getLogger('ezdxf').setLevel(logging.ERROR)
UNIT_CODES = [0, 0, 1, 4, 4, 4, 5, 6, 2, 14]


def drawing(rng, index):
    """One generated drawing as DXF text, and the unit chosen for it if it is unitless."""
    pick = rng.choice
    doc = ezdxf.new(pick(['R2000', 'R2010', 'R2018', 'R12']) if index % 7 else 'R2013', units=pick(UNIT_CODES))
    msp = doc.modelspace()
    layers = ['0']
    for n in range(rng.randint(0, 4)):
        name = pick(['Cut', 'Score', 'Engrave', 'Holes', 'Outline', 'Décor', 'cut']) + ('' if n == 0 else str(n))
        if name in doc.layers:
            continue
        attribs = {'color': pick([1, 2, 3, 5, 7, 8, 30, 141, 250, 255, -3, -7])}
        if rng.random() < 0.2:
            attribs['flags'] = 1  # frozen
        layer = doc.layers.new(name, dxfattribs=attribs)
        if rng.random() < 0.2 and doc.dxfversion > 'AC1009':
            layer.rgb = (rng.randint(0, 255), rng.randint(0, 255), rng.randint(0, 255))
        layers.append(name)
    size = pick([1, 10, 60, 150])
    if doc.units in (6, 2):
        size /= 100

    def p(scale=1.0):
        return (rng.uniform(-size, size) * scale, rng.uniform(-size, size) * scale)

    def extrusion():
        return pick([(0, 0, 1)] * 6 + [(0, 0, -1), (0, 0, 2), (0, 1, 1), (1e-9, 0, 1)])

    exact_splines = []

    def add(space, depth=0):
        roll = rng.random()
        attribs = {'layer': pick(layers)}
        if roll < 0.12:
            a, b = p(), p()
            z = pick([0, 0, 0, 1.5])
            space.add_line((*a, z), (*b, z if rng.random() < 0.9 else z + 3), dxfattribs=attribs)
        elif roll < 0.3:
            count = rng.randint(2, 9)
            points = [(*p(), pick([0, 0, 0, rng.uniform(-1.5, 1.5)])) for _ in range(count)]
            attribs.update(extrusion=extrusion(), elevation=pick([0, 0, 2.5]))
            space.add_lwpolyline(points, format='xyb', close=rng.random() < 0.5, dxfattribs=attribs)
        elif roll < 0.38:
            points = [p() for _ in range(rng.randint(2, 7))]
            if rng.random() < 0.5:
                points = [(x, y, 0, 0, pick([0, 0, 0.4, -1, 2])) for x, y in points]
                space.add_polyline2d(points, format='xyseb', close=rng.random() < 0.5, dxfattribs=attribs)
            else:
                z = pick([0, 3])
                space.add_polyline3d(
                    [(x, y, z + (rng.random() < 0.2)) for x, y in points], close=rng.random() < 0.5, dxfattribs=attribs
                )
        elif roll < 0.5:
            attribs['extrusion'] = extrusion()
            space.add_circle(p(), rng.uniform(0.05, size / 2 + 0.1), dxfattribs=attribs)
        elif roll < 0.62:
            attribs['extrusion'] = extrusion()
            start = rng.uniform(-400, 400)
            end = pick([start + rng.uniform(1, 359), rng.uniform(-400, 400), start + 360, start])
            space.add_arc(p(), rng.uniform(0.05, size / 2 + 0.1), start, end, dxfattribs=attribs)
        elif roll < 0.7:
            major = p(0.5)
            if major == (0, 0):
                major = (1, 0)
            start = rng.uniform(-7, 7)
            end = pick([0, start + rng.uniform(0.1, 6.2), math.tau, rng.uniform(-7, 7)])
            space.add_ellipse(p(), major_axis=(*major, 0), ratio=rng.uniform(0.05, 1), start_param=start,
                              end_param=end if end else math.tau, dxfattribs=attribs)  # fmt: skip
        elif roll < 0.78:
            # The reference draws the true curve only for clamped splines of degree 3, and only to the radial
            # error of its cubic pieces; in model space each spline is compared with the curve itself, on a
            # layer of its own, and in blocks only clamped cubic splines are drawn.
            degree = pick([1, 2, 3, 3]) if depth == 0 else 3
            control = [p() for _ in range(rng.randint(degree + 1, degree + 7))]
            if depth == 0:
                attribs['layer'] = f'Spline{len(exact_splines)}'
                exact_splines.append(attribs['layer'])
            roll = rng.random()
            if roll < 0.25 and depth == 0:
                weights = [rng.uniform(0.3, 2) for _ in control]
                space.add_rational_spline(control, weights, degree=degree, dxfattribs=attribs)
            elif roll < 0.5 and depth == 0:
                space.add_spline(dxfattribs=attribs).set_closed([(x, y, 0) for x, y in control], degree=degree)
            elif roll < 0.75:
                space.add_open_spline(control, degree=degree, dxfattribs=attribs)
            else:
                # Fit points only, which CAD programs interpolate, with end tangents or without; now and then a
                # point given twice, which the reference cannot interpolate and the core passes through once.
                fit = [(*p(), 0) for _ in range(rng.randint(2, 9))]
                if rng.random() < 0.1:
                    fit.insert(1, fit[0])
                spline = space.add_spline(fit_points=fit, dxfattribs=attribs)
                if rng.random() < 0.4:
                    spline.dxf.start_tangent = (*p(), 0)
                    spline.dxf.end_tangent = (*p(), 0)
        elif roll < 0.84:
            pick([lambda: space.add_text('Label', dxfattribs=attribs), lambda: space.add_point(p(), dxfattribs=attribs),
                  lambda: space.add_solid([p(), p(), p()], dxfattribs=attribs), lambda: space.add_mtext('Note', dxfattribs=attribs)])()  # fmt: skip
        elif depth < 2:
            name = f'B{index}_{depth}_{rng.randint(0, 999)}'
            if name in doc.blocks:
                return
            block = doc.blocks.new(name, base_point=pick([(0, 0), p(0.2)]))
            for _ in range(rng.randint(1, 4)):
                try:
                    add(block, depth + 1)
                except ezdxf.DXFVersionError:
                    pass
            arrayed = rng.random() < 0.2
            ref_attribs = {
                'layer': pick(layers),
                'rotation': pick([0, 0, 90, 30, -45, rng.uniform(0, 360)]),
                'xscale': pick([1, 1, 2, 0.5, -1]),
                'yscale': pick([1, 1, 2, 0.5, -1]),
            }
            if arrayed:
                ref_attribs.update(
                    row_count=rng.randint(1, 3),
                    column_count=rng.randint(1, 3),
                    row_spacing=size / 3,
                    column_spacing=size / 4,
                )
            if rng.random() < 0.1:
                ref_attribs['extrusion'] = (0, 0, -1)
            ref = space.add_blockref(name, p(0.3), dxfattribs=ref_attribs)
            if rng.random() < 0.35 and doc.dxfversion > 'AC1009':
                clip = XClip(ref)
                if rng.random() < 0.5:
                    clip.set_block_clipping_path([p(0.5), p(0.5)])
                else:
                    corners = sorted((math.atan2(y, x), (x, y)) for x, y in (p(0.6) for _ in range(rng.randint(3, 7))))
                    clip.set_block_clipping_path([c for _, c in corners])
                if rng.random() < 0.1:
                    clip.get_spatial_filter().dxf.has_front_clipping_plane = 1
                if rng.random() < 0.1:
                    clip.disable_clipping()

    for _ in range(rng.randint(0, 14)):
        try:
            add(msp)
        except ezdxf.DXFVersionError:
            pass  # an entity DXF R12 does not have
    if rng.random() < 0.02:
        for _ in range(130):
            msp.add_line(p(), p(), dxfattribs={'layer': 'Dense'})
    stream = io.StringIO()
    doc.write(stream)
    return stream.getvalue(), pick(['mm', 'mm', 'in', 'cm'])


def true_spline(text, layer, scale):
    """The points of the spline on this layer, closely spaced over the curve's own domain, in millimetres."""
    doc = ezdxf.read(io.StringIO(text))
    spline = next(e for e in doc.modelspace() if e.dxftype() == 'SPLINE' and e.dxf.layer == layer)
    tool = spline.construction_tool()
    knots, degree, count = list(tool.knots()), tool.degree, tool.count
    low, high = knots[degree], knots[count]
    steps = sorted({low + (high - low) * i / 8000 for i in range(8001)} | {k for k in knots if low <= k <= high})
    points = [tool.point(t) for t in steps]
    return [[q.x * scale, q.y * scale] for q in points]


def outcome(payload, backend):
    try:
        return import_program(payload, backend=backend)
    except ValueError as error:
        return 'ValueError: ' + str(error)


def distances(points, paths, cutoff=np.inf):
    """The distance from each point to the nearest of the paths; inf where it is beyond the cutoff."""
    points = np.asarray(points, float)
    starts, ends = [], []
    for path in paths:
        path = np.asarray(path, float)
        if len(path) == 1:
            path = np.vstack([path, path])
        starts.append(path[:-1])
        ends.append(path[1:])
    start, end = np.vstack(starts), np.vstack(ends)
    low, high = np.minimum(start, end), np.maximum(start, end)
    out = []
    for chunk in np.array_split(points, max(1, len(points) // 128)):
        if not len(chunk):
            continue
        # Only segments whose boxes come within the cutoff of the chunk's box can be nearer than it.
        lo, hi = chunk.min(axis=0) - cutoff, chunk.max(axis=0) + cutoff
        keep = np.all(high >= lo, axis=1) & np.all(low <= hi, axis=1) if np.isfinite(cutoff) else slice(None)
        s0, s1 = start[keep], end[keep]
        if not len(s0):
            out.append(np.full(len(chunk), np.inf))
            continue
        along = s1 - s0
        square = np.maximum((along * along).sum(1), 1e-300)
        t = np.clip(((chunk[:, None, :] - s0[None]) * along[None]).sum(2) / square[None], 0, 1)
        nearest = s0[None] + t[..., None] * along[None]
        found = np.hypot(*(chunk[:, None, :] - nearest).transpose(2, 0, 1)).min(axis=1)
        found[found > cutoff] = np.inf
        out.append(found)
    return np.concatenate(out) if out else np.zeros(0)


def deviation(a, b, beyond_ends=True):
    """The largest distance from a vertex of the paths a to the paths b; with beyond_ends false, vertices
    whose nearest point of b is the end of one of b's paths are left out (a run that goes on past where the
    other's ends)."""
    a = np.asarray([q for path in a for q in path], float)
    starts, ends, first, last = [], [], [], []
    for path in b:
        path = np.asarray(path, float)
        if len(path) == 1:
            path = np.vstack([path, path])
        first += [True] + [False] * (len(path) - 2)
        last += [False] * (len(path) - 2) + [True]
        starts.append(path[:-1])
        ends.append(path[1:])
    start, end = np.vstack(starts), np.vstack(ends)
    first, last = np.asarray(first), np.asarray(last)
    along = end - start
    square = np.maximum((along * along).sum(1), 1e-300)
    worst = 0.0
    for chunk in np.array_split(a, max(1, len(a) // 256)):
        t = np.clip(((chunk[:, None, :] - start[None]) * along[None]).sum(2) / square[None], 0, 1)
        nearest = start[None] + t[..., None] * along[None]
        distances = np.hypot(*(chunk[:, None, :] - nearest).transpose(2, 0, 1))
        best = np.argmin(distances, axis=1)
        found = distances[np.arange(len(chunk)), best]
        if not beyond_ends:
            at = t[np.arange(len(chunk)), best]
            found = found[~((first[best] & (at == 0)) | (last[best] & (at == 1)))]
        if len(found):
            worst = max(worst, float(found.max()))
    return worst


grazing = [0]


def length(paths):
    return sum(math.dist(*pair) for path in paths for pair in zip(path, path[1:]))


def windows(text, scale):
    """The boundaries of the drawing's XCLIP windows in millimetres, as closed rings."""
    from ezdxf.math import BoundingBox2d, Matrix44

    rings = []

    def walk(space, transform, depth):
        for entity in space:
            if entity.dxftype() != 'INSERT' or depth > 12 or entity.block() is None:
                continue
            copies = entity.multi_insert() if entity.mcount > 1 else (entity,)
            clip = XClip(entity)
            for instance in copies:
                frame = instance.matrix44() @ transform
                if clip.is_clipping_enabled:
                    spatial = clip.get_spatial_filter()
                    vertices = spatial.boundary_vertices
                    if len(vertices) == 2:
                        vertices = BoundingBox2d(vertices).rect_vertices()
                    matrix = spatial.inverse_insert_matrix @ frame
                    ring = [(v.x, v.y) for v in matrix.transform_vertices(vertices)]
                    rings.append(ring + ring[:1])
                walk(entity.block(), frame, depth + 1)

    walk(ezdxf.read(io.StringIO(text)).modelspace(), Matrix44.scale(scale), 0)
    return rings


def sampled(paths, spacing=1.0):
    """The paths' vertices and points between them no more than spacing apart."""
    out = []
    for path in paths:
        for p, q in zip(path, path[1:]):
            count = max(1, math.ceil(math.dist(p, q) / spacing))
            out += [[p[0] + (q[0] - p[0]) * i / count, p[1] + (q[1] - p[1]) * i / count] for i in range(count)]
        out.append(path[-1])
    return [out] if out else []


def compare(core, reference, truth=lambda layer: None, boundaries=lambda: []):
    """The largest deviation as a share of the bound when the core's drawing matches the reference's within
    the criteria, else what differs. truth(layer) gives the true curve of a spline the reference does not
    draw exactly, which the core's path is compared with instead."""
    if isinstance(core, str) or isinstance(reference, str):
        return None if core == reference else 'outcome'
    if core['layers'] != reference['layers']:
        return 'layers'
    if core['warnings'] != reference['warnings']:
        return 'warnings'
    clipped = any('XCLIP' in line for line in reference['warnings'])
    if clipped:
        # Where a window cuts a curve, the cut is where each reader's chords cross its boundary, and where a
        # curve grazes the boundary one reader's chords can dip inside, or leave for a moment, where the
        # other's do not; a vertex on the boundary can end one run and start another. So the runs are compared
        # by layer and kind of entity, as sets of points: every point of either reader's runs (vertices and
        # points between them) within the bound of the other's, except within twice the bound of a window's
        # boundary. The bound has 3e-4 of the whole drawing's size, since a run does not tell its curve's
        # radius.
        everything = [q for o in reference['objects'] for path in o['paths'] for q in path]
        drawing_extent = np.ptp(np.asarray(everything, float), axis=0).max() if everything else 0
        bound = 0.05 + 3e-4 * drawing_extent
        rings = boundaries()
        grouped = []
        for objects in (core['objects'], reference['objects']):
            groups = collections.defaultdict(list)
            for o in objects:
                groups[o['name']].extend(o['paths'])
            grouped.append(groups)
        worst = 0.0
        for name in set(grouped[0]) | set(grouped[1]):
            exact = truth(name.split(' \u00b7 ')[0])
            if exact:
                # A model-space spline, which no window cuts: compared with the curve itself.
                for path in grouped[0].get(name, []):
                    if math.dist(path[0], exact[0]) > 1e-6 or math.dist(path[-1], exact[-1]) > 1e-6:
                        return 'ends'
                    worst = max(worst, max(deviation([path], [exact]), deviation([exact], [path])) / 0.03)
                if worst > 1 or name not in grouped[0] or name not in grouped[1]:
                    return 'outline'
                continue
            for mine, theirs in (
                (grouped[0].get(name), grouped[1].get(name)),
                (grouped[1].get(name), grouped[0].get(name)),
            ):
                if not mine:
                    continue
                points = np.asarray(sampled(mine)[0], float)
                found = distances(points, theirs, bound) if theirs else np.full(len(points), np.inf)
                near = found <= bound
                if near.any():
                    worst = max(worst, float(found[near].max()) / bound)
                if not near.all():
                    if not rings or not np.all(distances(points[~near], rings, 2 * bound) <= 2 * bound):
                        return 'outline'
                    grazing[0] += int((~near).sum())
        return worst
    if len(core['objects']) != len(reference['objects']):
        return 'object count'
    worst = 0.0
    for mine, theirs in zip(core['objects'], reference['objects']):
        if (mine['name'], mine['sourceLayer'], len(mine['paths'])) != (
            theirs['name'],
            theirs['sourceLayer'],
            len(theirs['paths']),
        ):
            return 'objects'
        exact = truth(mine['sourceLayer'])
        for a, b in zip(mine['paths'], [exact] * len(mine['paths']) if exact else theirs['paths']):
            extent = np.ptp(np.asarray(b, float), axis=0).max() if len(b) > 1 else 0
            bound = (0.03 if exact else 0.05) + (0 if exact else 3e-4 * extent)
            if math.dist(a[0], b[0]) > 1e-6 or math.dist(a[-1], b[-1]) > 1e-6:
                return 'ends'

            worst = max(worst, max(deviation([a], [b]), deviation([b], [a])) / bound)
            if worst > 1:
                return 'outline'
    return worst


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cases', type=int, default=1500)
    parser.add_argument('--seed', type=int, default=1)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    tally = collections.Counter()
    refused_by_one = collections.Counter()
    failures = []
    worst = 0.0
    vertices = collections.Counter()
    for index in range(args.cases):
        text, unit = drawing(rng, index)
        payload = {'name': f'case{index}.dxf', 'text': text, 'unit': unit}
        try:
            core = outcome(payload, 'native')
        except NotImplementedError as declined:
            tally['declined: ' + str(declined)] += 1
            continue
        reference = outcome(payload, 'python')
        if isinstance(core, str) != isinstance(reference, str):
            refused_by_one[
                (core if isinstance(core, str) else 'read')[:70],
                (reference if isinstance(reference, str) else 'read')[:70],
            ] += 1
            continue
        scale = 1.0
        if not isinstance(core, str):
            scale = {'mm': 1.0, 'in': 25.4, 'cm': 10.0}[unit]
            known = ezdxf.read(io.StringIO(text)).units
            if known:
                scale = units.conversion_factor(known, units.MM)
        verdict = compare(
            core,
            reference,
            lambda layer, t=text, s=scale: true_spline(t, layer, s) if layer.startswith('Spline') else None,
            lambda t=text, s=scale: windows(t, s),
        )
        if isinstance(verdict, str):
            failures.append((index, verdict, payload, core, reference))
            continue
        if isinstance(core, str):
            tally['refused alike'] += 1
        else:
            tally['read alike'] += 1
            worst = max(worst, verdict)
            for mine, theirs in zip(core['objects'], reference['objects']):
                vertices['core'] += sum(map(len, mine['paths']))
                vertices['reference'] += sum(map(len, theirs['paths']))
    for key, count in sorted(tally.items()):
        print(f'{key}: {count}')
    print(
        f'largest deviation: {worst:.3f} of the bound; vertices: core {vertices["core"]}, reference {vertices["reference"]}'
    )
    print(f'points that differ only within twice the bound of a window\'s boundary: {grazing[0]}')
    for (mine, theirs), count in refused_by_one.most_common():
        print(f'NOTE {count} refused by one reader only: core {mine!r}, reference {theirs!r}')
    for index, verdict, payload, core, reference in failures[:5]:
        print(f'DIFFERENT case {index}: {verdict}')
        brief = lambda value: (value if isinstance(value, str) else str({k: value[k] for k in ('layers', 'warnings')}))[
            :400
        ]
        print('  core     ', brief(core))
        print('  reference', brief(reference))
    if failures:
        raise SystemExit(f'{len(failures)} drawings differ beyond the criteria.')
    if not tally['read alike']:
        raise SystemExit('No drawing was read: the check did not exercise the reader.')
    print('PASS the core reads every drawing as the ezdxf reference does, within the tolerance')


if __name__ == '__main__':
    main()
