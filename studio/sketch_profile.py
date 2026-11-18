"""Planar profiles with straight edges, cubic Béziers and tangent corner fillets."""
from dataclasses import dataclass
import math

from shapely.geometry import Polygon

from .sketch_constraints import solve_profile
from .sketch_curves import minimum_coordinate, resolve_curves, sample_cubic


def crossing_axis(values, axis):
    ax, ay, bx, by, cx, cy, dx, dy = values
    rx, ry, sx, sy = bx - ax, by - ay, dx - cx, dy - cy
    first, second = math.hypot(rx, ry), math.hypot(sx, sy)
    denominator = rx * sy - ry * sx
    if min(first, second) < 1e-8 or abs(denominator) <= 1e-12 * first * second:
        raise ValueError('The sketch trim reference lines are parallel or collapsed')
    t = ((cx - ax) * sy - (cy - ay) * sx) / denominator
    u = ((cx - ax) * ry - (cy - ay) * rx) / denominator
    if not -1e-8/first <= t <= 1+1e-8/first or not -1e-8/second <= u <= 1+1e-8/second:
        raise ValueError('The sketch trim reference segments no longer cross')
    t = min(1.0, max(0.0, t))
    return (ax + t * rx, ay + t * ry)[axis]


@dataclass
class Corner:
    start: tuple
    end: tuple
    center: tuple | None = None
    radius: float = 0
    start_angle: float = 0
    sweep: float = 0
    setback: float = 0
    outgoing_controls: list | None = None

    def at(self, fraction):
        angle = self.start_angle + self.sweep * fraction
        return (self.center[0] + self.radius * math.cos(angle),
                self.center[1] + self.radius * math.sin(angle))


def _corner(a, b, c, radius):
    if not radius:
        return Corner(b, b)
    u = (a[0] - b[0], a[1] - b[1])
    v = (c[0] - b[0], c[1] - b[1])
    u = tuple(value / math.hypot(*u) for value in u)
    v = tuple(value / math.hypot(*v) for value in v)
    cross = u[0] * v[1] - u[1] * v[0]
    if abs(cross) < 1e-10:
        raise ValueError('A fillet needs two edges meeting at a corner')
    angle = math.atan2(abs(cross), u[0] * v[0] + u[1] * v[1])
    setback = radius / math.tan(angle / 2)
    start = tuple(b[i] + setback * u[i] for i in range(2))
    end = tuple(b[i] + setback * v[i] for i in range(2))
    # Centre is one radius normal to the incoming line at its point of tangency.
    side = math.copysign(radius, cross)
    center = (start[0] - side * u[1], start[1] + side * u[0])
    start_angle = math.atan2(start[1] - center[1], start[0] - center[0])
    return Corner(start, end, center, radius, start_angle, -math.copysign(math.pi - angle, cross), setback)


def profile_geometry(feature, dimensions, solved=None):
    """Return exact corner descriptions and the sampled CAM/viewport loop."""
    raw = feature.get('points')
    if not isinstance(raw, list) or not 3 <= len(raw) <= 256:
        raise ValueError('A sketch needs 3–256 vertices')
    points = list(map(tuple, solve_profile(feature, dimensions)[0] if solved is None else solved))
    radii = feature.get('fillets', [0] * len(points))
    if not isinstance(radii, list) or len(radii) != len(points):
        raise ValueError('Supply one fillet radius per sketch vertex')
    radii = [dimensions.evaluate(r) for r in radii]
    if min(radii) < 0:
        raise ValueError('Fillet radii must be zero or positive')
    raw_curves = feature.get('curves', [None] * len(points))
    if not isinstance(raw_curves, list) or len(raw_curves) != len(points):
        raise ValueError('Supply one curve entry per outgoing sketch edge')
    if points[0] == points[-1]:
        if raw_curves[-1] is not None:
            raise ValueError('A duplicate closing vertex cannot start another cubic edge')
        raw_curves = raw_curves[:-1]
        if radii[-1]:
            raise ValueError('Set the fillet on the first vertex of a closed loop')
        points.pop()
        radii.pop()
    if len(points) < 3 or len(set(points)) != len(points):
        raise ValueError('Sketch vertices must be distinct')
    controls = resolve_curves(raw_curves, points, dimensions)
    for i, handles in enumerate(controls):
        if handles is not None and (radii[i] or radii[(i+1) % len(points)]):
            raise ValueError('Remove the endpoint fillet before making a curved edge. Use tangent handles to round a cubic join.')
    corners = [_corner(points[i-1], b, points[(i+1) % len(points)], radii[i]) for i, b in enumerate(points)]
    outline = []
    for i, corner in enumerate(corners):
        if corner.setback + corners[i-1].setback > math.dist(points[i-1], points[i]) + 1e-10:
            raise ValueError(f'Fillets exceed the available length of the edge leading to point {i+1}')
        if not outline or math.dist(outline[-1], corner.start) > 1e-10:
            outline.append(corner.start)
        if corner.radius:
            steps = max(2, math.ceil(abs(corner.sweep) / (math.pi / 48)))
            outline.extend(corner.at(j / steps) for j in range(1, steps))
            outline.append(corner.end)
        if controls[i] is not None:
            corner.outgoing_controls = controls[i]
            end = corners[(i+1) % len(points)].start
            if feature.get('kind') == 'revolve':
                radial = [p[0] for p in (corner.end, *controls[i], end)]
                if minimum_coordinate(*radial) < -1e-10:
                    raise ValueError('Revolve profile radii must be zero or greater along the entire curve')
            outline.extend(sample_cubic(corner.end, controls[i], end))
        if len(outline) > 16384:
            raise ValueError('Sketch exceeds 16384 sampled vertices')
    if len(outline) > 1 and math.dist(outline[0], outline[-1]) <= 1e-10:
        outline.pop()
    polygon = Polygon(outline)
    if not polygon.is_valid or polygon.area <= 1e-8:
        raise ValueError('Sketch must enclose an area without crossing itself')
    if feature.get('kind') == 'revolve' and any(x < 0 for x, _ in points + outline):
        raise ValueError('Revolve profile radii must be zero or greater')
    return corners, outline


def step_profile_face(feature, dimensions):
    from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeEdge, BRepBuilderAPI_MakeFace, BRepBuilderAPI_MakeWire
    from OCP.GC import GC_MakeArcOfCircle
    from OCP.Geom import Geom_BezierCurve
    from OCP.TColgp import TColgp_Array1OfPnt
    from OCP.gp import gp_Pnt

    def gp(point):
        a, b = point
        return gp_Pnt(a, b, 0) if feature['kind'] == 'extrude' else gp_Pnt(a, 0, b)

    corners, _ = profile_geometry(feature, dimensions)
    wire = BRepBuilderAPI_MakeWire()
    for i, corner in enumerate(corners):
        previous = corners[i-1]
        if previous.outgoing_controls:
            poles = TColgp_Array1OfPnt(1, 4)
            for index, point in enumerate((previous.end, *previous.outgoing_controls, corner.start), 1):
                poles.SetValue(index, gp(point))
            edge = BRepBuilderAPI_MakeEdge(Geom_BezierCurve(poles))
            if not edge.IsDone():
                raise ValueError('STEP could not construct the cubic sketch edge')
            wire.Add(edge.Edge())
        elif math.dist(previous.end, corner.start) > 1e-10:
            wire.Add(BRepBuilderAPI_MakeEdge(gp(corners[i-1].end), gp(corner.start)).Edge())
        if corner.radius:
            arc = GC_MakeArcOfCircle(gp(corner.start), gp(corner.at(.5)), gp(corner.end))
            if not arc.IsDone():
                raise ValueError('STEP could not form a circular sketch fillet')
            wire.Add(BRepBuilderAPI_MakeEdge(arc.Value()).Edge())
    if not wire.IsDone():
        raise ValueError('STEP could not close the sketch wire')
    face = BRepBuilderAPI_MakeFace(wire.Wire())
    if not face.IsDone():
        raise ValueError('STEP could not form a planar sketch face')
    return face.Face()
