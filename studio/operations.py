"""Polygon CAM and drag-knife preparation. Geometry stays separate from G-code."""

import math
from numbers import Real
from shapely.geometry import Polygon, LineString, GeometryCollection, MultiPoint
from shapely.ops import voronoi_diagram
from shapely import affinity


def value(data, key, default, lo, hi):
    v = data.get(key, default)
    if isinstance(v, bool):
        raise ValueError(key + ' must be numeric')
    try:
        v = float(v)
    except (TypeError, ValueError):
        raise ValueError(key + ' must be numeric') from None
    if not math.isfinite(v) or not lo <= v <= hi:
        raise ValueError(f'{key} must be between {lo} and {hi}')
    return v


def router_tool_shape(settings):
    op = settings.get('operation', 'line')
    if op == 'vcarve':
        return 'v'
    return settings.get('tool_shape', 'drill' if op == 'drill' else 'flat')


def _point(point):
    return (
        isinstance(point, (list, tuple))
        and len(point) == 2
        and all(isinstance(v, Real) and not isinstance(v, bool) and math.isfinite(v) for v in point)
    )


def region(paths):
    # Paths arrive from request bodies and imported files: reject anything that is not a list of XY points.
    if not isinstance(paths, (list, tuple)) or not all(
        isinstance(path, (list, tuple)) and all(map(_point, path)) for path in paths
    ):
        raise ValueError('Contours must be lists of finite XY points')
    result = GeometryCollection()
    for path in paths:
        if len(path) < 4 or math.dist(path[0], path[-1]) > 1e-6:
            raise ValueError('This operation needs closed contours')
        p = Polygon(path)
        if not p.is_valid or p.area < 1e-9:
            raise ValueError('Repair self-crossing or zero-area contours before CAM')
        result = result.symmetric_difference(p)
    if result.is_empty:
        raise ValueError('Contours cancel under even-odd fill')
    return result


def polygons(g):
    if g.geom_type == 'Polygon':
        return [g]
    return [p for p in getattr(g, 'geoms', []) if p.geom_type == 'Polygon']


def outlines(g):
    result = []
    for p in sorted(polygons(g), key=lambda p: p.area):
        result.extend([[list(v) for v in ring.coords] for ring in p.interiors])
        result.append([list(v) for v in p.exterior.coords])
    return result


def lines(g):
    if g.geom_type == 'LineString':
        return [g]
    return [line for part in getattr(g, 'geoms', []) for line in lines(part)]


def hatch(g, spacing, angle=0):
    rotated = affinity.rotate(g, -angle, origin=(0, 0))
    x0, y0, x1, y1 = rotated.bounds
    n = math.ceil((y1 - y0) / spacing)
    if n > 20000:
        raise ValueError('Too many hatch rows; increase interval')
    result = []
    for i in range(n + 1):
        y = min(y1, y0 + i * spacing)
        segments = lines(rotated.intersection(LineString([(x0 - 1, y), (x1 + 1, y)])))
        for line in reversed(segments) if i % 2 else segments:
            coords = list(affinity.rotate(line, angle, origin=(0, 0)).coords)
            if i % 2:
                coords.reverse()
            if line.length > 1e-6:
                result.append([list(v) for v in coords])
    return result


def knife(paths, offset, lead):
    result = []
    for path in paths:
        p = [path[0]] + [b for a, b in zip(path, path[1:]) if math.dist(a, b) > 1e-7]
        if len(p) < 2:
            continue
        vectors = [((b[0] - a[0]) / math.dist(a, b), (b[1] - a[1]) / math.dist(a, b)) for a, b in zip(p, p[1:])]
        ux, uy = vectors[0]
        q = [
            [p[0][0] + (offset - lead) * ux, p[0][1] + (offset - lead) * uy],
            [p[0][0] + offset * ux, p[0][1] + offset * uy],
        ]
        for i, (vx, vy) in enumerate(vectors):
            x, y = p[i + 1]
            q.append([x + offset * vx, y + offset * vy])
            next_v = vectors[i + 1] if i + 1 < len(vectors) else vectors[0] if p[-1] == p[0] else None
            if next_v:
                a = math.atan2(vy, vx)
                b = math.atan2(next_v[1], next_v[0])
                delta = (b - a + math.pi) % (2 * math.pi) - math.pi
                n = max(1, math.ceil(abs(delta) / math.radians(5)))
                q.extend(
                    [
                        [x + offset * math.cos(a + delta * j / n), y + offset * math.sin(a + delta * j / n)]
                        for j in range(1, n + 1)
                    ]
                )
        result.append(q)
    return result


def prepare(project, paths):
    op = project.get('operation', 'line')
    mode = project.get('mode')
    warnings = []
    depths = None
    if op == 'line' and mode != 'knife':
        return paths, None, []
    if project.get('path_depths') is not None:
        raise ValueError('Per-vertex depth contours require the line operation')
    if mode == 'knife':
        if op != 'line':
            raise ValueError('Knife uses contour lines, not pocket/fill operations')
        offset = value(project, 'knife_offset', 0.25, 0.01, 5)
        lead = value(project, 'knife_lead', 1, offset, 20)
        return (
            knife(paths, offset, lead),
            None,
            [
                'Knife alignment lead-in marks material before each contour. Test blade offset and corner swivels on scrap.'
            ],
        )
    g = region(paths)
    if op in ('outside', 'inside'):
        radius = (
            value(project, 'tool_diameter', 3.175, 0.01, 100) / 2
            if mode == 'router'
            else value(project, 'kerf', 0.1, 0, 5) / 2
        )
        offset = radius if op == 'outside' else -radius
        result = g.buffer(offset, quad_segs=32)
        if result.is_empty:
            raise ValueError('Tool does not fit inside these contours')
        paths = outlines(result)
        warnings.append(
            'Compensation follows even-odd regions; nested contours become holes. Features smaller than the cutter may be inaccessible.'
        )
    elif op == 'pocket':
        if mode != 'router':
            raise ValueError('Pocketing requires router mode')
        radius = value(project, 'tool_diameter', 3.175, 0.01, 100) / 2
        step = value(project, 'stepover', 1, 0.01, 100)
        if step > radius:
            raise ValueError('Pocket stepover cannot exceed cutter radius (50% diameter)')
        inset = g.buffer(-radius, quad_segs=32)
        if inset.is_empty:
            raise ValueError('Cutter cannot enter this pocket')
        paths = []
        while not inset.is_empty:
            paths.extend(outlines(inset))
            if sum(map(len, paths)) > 100000:
                raise ValueError('Pocket exceeds vertex limit')
            inset = inset.buffer(-step, quad_segs=32)
        warnings.append('Concentric pocket clearing preserves islands. Narrow residual features may remain.')
        if project.get('router_entry', 'plunge') == 'plunge':
            warnings.append('Plunge entry requires a centre-cutting tool.')
    elif op == 'fill':
        if mode != 'laser':
            raise ValueError('Hatch fill requires laser mode')
        interval = value(project, 'interval', 0.2, 0.02, 20)
        angle = value(project, 'hatch_angle', 0, -360, 360)
        paths = hatch(g, interval, angle)
        if project.get('crosshatch'):
            paths += hatch(g, interval, angle + 90)
    elif op == 'vcarve':
        if mode != 'router':
            raise ValueError('V-carving requires router mode')
        # Discrete medial-axis approximation from densely sampled boundaries.
        resolution = value(project, 'v_resolution', 0.5, 0.1, 5)
        angle = value(project, 'v_angle', 60, 10, 170)
        max_depth = value(project, 'depth', 1, 0.01, 50)
        radius = value(project, 'tool_diameter', 6, 0.1, 100) / 2
        samples = []
        for path in outlines(g):
            line = LineString(path)
            n = math.ceil(line.length / resolution)
            if len(samples) + n > 12000:
                raise ValueError('V-carve outline too complex; increase sampling interval')
            samples.extend([line.interpolate(line.length * i / n) for i in range(n)])
        edges = voronoi_diagram(MultiPoint(samples), edges=True).intersection(g)
        paths = []
        depths = []
        for edge in lines(edges):
            if edge.length < resolution * 0.1:
                continue
            n = max(1, math.ceil(edge.length / resolution))
            coords = []
            zs = []
            for i in range(n + 1):
                pt = edge.interpolate(edge.length * i / n)
                r = min(radius, max(0, pt.distance(g.boundary) - resolution))
                coords.append([pt.x, pt.y])
                zs.append(-min(max_depth, r / math.tan(math.radians(angle / 2))))
            if min(zs) < -1e-4:
                paths.append(coords)
                depths.append(zs)
        warnings.append(
            'Sampled medial-axis V-carve with conservative sampling allowance; fine detail and wide-area flat clearing require inspection.'
        )
    else:
        raise ValueError('Unsupported operation')
    if not paths:
        raise ValueError('Operation produced no toolpaths')
    return paths, depths, warnings
