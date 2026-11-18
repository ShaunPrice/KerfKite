"""Cubic Bézier sketch edges in the construction plane, in millimetres."""

import math


def resolve_curves(raw, points, dimensions):
    """Resolve the two handles of each outgoing edge and any endpoint tangencies."""
    count = len(points)
    if not isinstance(raw, list) or len(raw) != count:
        raise ValueError('Supply one curve entry per outgoing sketch edge')
    controls = []
    for curve in raw:
        if curve is None:
            controls.append(None)
            continue
        if not isinstance(curve, dict) or set(curve) - {'controls', 'tangentStart', 'tangentEnd'}:
            raise ValueError('Invalid cubic sketch edge')
        poles = curve.get('controls')
        if not isinstance(poles, list) or len(poles) != 2:
            raise ValueError('A cubic sketch edge needs two planar control points')
        if any(type(curve.get(key, False)) is not bool for key in ('tangentStart', 'tangentEnd')):
            raise ValueError('A cubic tangent option must be true or false')
        controls.append([tuple(dimensions.vector(p, 2)) for p in poles])

    for i, curve in enumerate(raw):
        if not curve:
            continue
        for side, option in enumerate(('tangentStart', 'tangentEnd')):
            if not curve.get(option):
                continue
            joint = points[(i + side) % count]
            neighbor = (i + (1 if side else -1)) % count
            neighbor_curve = raw[neighbor]
            if neighbor_curve and neighbor_curve.get('tangentStart' if side else 'tangentEnd'):
                raise ValueError('At a shared cubic vertex, choose only one handle to follow its neighbour')
            if neighbor_curve:
                guide = controls[neighbor][1 - side]
            else:
                guide = points[(i + (2 if side else -1)) % count]
            length = math.dist(joint, controls[i][side])
            baseline = math.dist(joint, guide)
            if min(length, baseline) <= 1e-9:
                raise ValueError('Tangent alignment needs a nonzero handle and neighbouring direction')
            controls[i][side] = tuple(v + (v - ref) * length / baseline for v, ref in zip(joint, guide))
            if any(abs(v) > 100_000 for v in controls[i][side]):
                raise ValueError('Aligned curve handle exceeds the sketch coordinate range')
    return controls


def sample_cubic(start, controls, end):
    """Subdivide by de Casteljau until the control hull lies within the chord tolerance."""
    span = max(math.dist(start, p) for p in (*controls, end))
    tolerance = min(0.01, span * 0.00025)
    result = []

    def midpoint(a, b):
        return tuple((x + y) / 2 for x, y in zip(a, b))

    def subdivide(a, b, c, d, depth):
        dx, dy = d[0] - a[0], d[1] - a[1]
        chord = math.hypot(dx, dy)
        if chord:
            # Hull must also project onto the segment in traversal order. This
            # preserves turns and catches curves which double back on the chord.
            projections = [((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / chord for p in (b, c)]
            deviations = [abs((p[0] - a[0]) * dy - (p[1] - a[1]) * dx) / chord for p in (b, c)]
            if (
                max(deviations) <= tolerance
                and -1e-12 <= projections[0] <= projections[1] + 1e-12
                and projections[1] <= chord + 1e-12
            ):
                result.append(d)
                return
        if depth >= 20 or len(result) >= 4096:
            raise ValueError('Curve reverses direction at a cusp or exceeds the sketch sampling budget')
        ab, bc, cd = midpoint(a, b), midpoint(b, c), midpoint(c, d)
        abc, bcd = midpoint(ab, bc), midpoint(bc, cd)
        mid = midpoint(abc, bcd)
        subdivide(a, ab, abc, mid, depth + 1)
        subdivide(mid, bcd, cd, d, depth + 1)

    subdivide(start, *controls, end, 0)
    return result


def minimum_coordinate(p0, p1, p2, p3):
    """Minimum of a scalar cubic on the closed interval, including interior extrema."""
    a, b, c = -p0 + 3 * p1 - 3 * p2 + p3, 2 * (p0 - 2 * p1 + p2), p1 - p0
    disc = b * b - 4 * a * c
    roots = []
    if a == 0:
        if b:
            roots = [-c / b]
    elif disc >= 0:
        q = -0.5 * (b + math.copysign(math.sqrt(disc), b))
        roots = [q / a, c / q] if q else [0.0]
    values = [p0, p3]
    for t in roots:
        if 0 < t < 1:
            s = 1 - t
            values.append(s**3 * p0 + 3 * s * s * t * p1 + 3 * s * t * t * p2 + t**3 * p3)
    return min(values)
