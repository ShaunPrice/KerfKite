"""Build millimetre-scale OpenSCAD-style regions and solids with Manifold."""

import math

import manifold3d as kernel
import numpy as np
from shapely.geometry import LinearRing

_MAX_COORD = 100_000
_MAX_VERTICES = 8_000
_MAX_TRIANGLES = 75_000
_MAX_WORK = 400_000
_MAX_SEGMENTS = 256
_DEFAULT_SEGMENTS = 48
_MISSING = object()


def _number(value):
    if type(value) not in (int, float) or not -_MAX_COORD <= value <= _MAX_COORD:
        raise ValueError('Expected a finite number within ±100000')
    return value


def _integer(value, low, high, label):
    if type(value) not in (int, float) or not low <= value <= high or int(value) != value:
        raise ValueError(f'{label} must be a whole number from {low} to {high}')
    return int(value)


def _flag(value):
    if type(value) is not bool:
        raise ValueError('Expected true or false')
    return value


def _vector(value, length, scalar=False):
    if scalar and type(value) in (int, float):
        value = [value] * length
    if not isinstance(value, list) or len(value) != length:
        raise ValueError(f'Expected a vector of {length} numbers')
    return [_number(v) for v in value]


def _segments(value):
    if type(value) in (int, float) and value == 0:
        return _DEFAULT_SEGMENTS
    return _integer(value, 3, _MAX_SEGMENTS, '$fn')


def _bind(node, positional=(), *, named=()):
    args, kwargs = node['args'], node['kwargs']
    if len(args) > len(positional):
        raise ValueError('Too many positional arguments')
    fields = dict(zip(positional, args))
    for key, value in kwargs.items():
        if key not in positional and key not in named:
            raise ValueError(f'Unknown argument {key!r}')
        if key in fields:
            raise ValueError(f'Duplicate argument {key!r}')
        fields[key] = value
    return fields


def _radius(fields, r='r', d='d', default=1):
    if d in fields and r in fields:
        raise ValueError(f'Choose one of {r} or {d}')
    radius = _number(fields[d]) / 2 if d in fields else _number(fields.get(r, default))
    if radius < 0:
        raise ValueError('Radius cannot be negative')
    return radius


def _convexity(fields):
    # OpenSCAD uses convexity only for its preview renderer.
    if 'convexity' in fields:
        _integer(fields['convexity'], 1, 1000, 'convexity')


def _polygon(fields):
    points = fields.get('points')
    if not isinstance(points, list) or not 3 <= len(points) <= 2048:
        raise ValueError('A polygon needs 3–2048 points')
    points = [_vector(p, 2) for p in points]
    paths = fields.get('paths', [list(range(len(points)))])
    if not isinstance(paths, list) or not 1 <= len(paths) <= 128:
        raise ValueError('A polygon needs 1–128 paths')
    if any(not isinstance(p, list) or len(p) < 3 for p in paths) or sum(map(len, paths)) > 2048:
        raise ValueError('Polygon paths need at least three indices each and at most 2048 in total')
    contours = []
    for path in paths:
        contour = [points[_integer(i, 0, len(points) - 1, 'Point index')] for i in path]
        ring = LinearRing(contour)
        if not ring.is_ring or ring.is_empty:
            raise ValueError('Polygon paths must enclose an area without crossing themselves')
        contours.append(contour)
    return kernel.CrossSection(contours, kernel.FillRule.EvenOdd)


def _polyhedron(fields):
    """Construct a closed convex polyhedron from clockwise exterior face loops."""
    raw_points, faces = fields.get('points'), fields.get('faces')
    if not isinstance(raw_points, list) or not 4 <= len(raw_points) <= 256:
        raise ValueError('A polyhedron needs 4–256 vertices')
    if not isinstance(faces, list) or not 4 <= len(faces) <= 512:
        raise ValueError('A polyhedron needs 4–512 faces')
    if any(not isinstance(face, list) or not 3 <= len(face) <= 256 for face in faces):
        raise ValueError('Each face needs 3–256 indices')
    if sum(map(len, faces)) > 4096:
        raise ValueError('Polyhedron exceeds the face index budget')
    points = np.array([_vector(point, 3) for point in raw_points], dtype=float)
    tolerance = max(1e-9, np.ptp(points, axis=0).max() * 1e-9)
    triangles = []
    for face in faces:
        # Manifold uses CCW exterior winding; OpenSCAD's external faces are CW.
        indices = [_integer(i, 0, len(points) - 1, 'Vertex index') for i in reversed(face)]
        if len(set(indices)) != len(indices):
            raise ValueError('A face cannot repeat a vertex')
        relative = points[indices] - points[indices[0]]
        normal = np.cross(relative, np.roll(relative, -1, axis=0)).sum(axis=0)
        length = np.linalg.norm(normal)
        if length <= tolerance**2:
            raise ValueError('A polyhedron face needs a nonzero area')
        normal /= length
        distances = (points - points[indices[0]]) @ normal
        if abs(distances[indices]).max() > tolerance:
            raise ValueError('Polyhedron faces must be planar')
        if distances.max() > tolerance:
            raise ValueError('Use a convex polyhedron with faces wound clockwise when seen from outside')
        axis = int(np.argmax(abs(normal)))
        xy = relative[:, [(axis + 1) % 3, (axis + 2) % 3]].copy()
        xy[:, 1] *= np.sign(normal[axis])
        if not LinearRing(xy).is_ring:
            raise ValueError('Polyhedron face edges cannot cross')
        for triangle in kernel.triangulate([xy]):
            triangles.append([indices[i] for i in triangle])
    solid = kernel.Manifold(kernel.Mesh64(points, np.asarray(triangles, dtype=np.uint64)))
    if solid.status() != kernel.Error.NoError or solid.is_empty() or solid.volume() <= 0:
        raise ValueError('Polyhedron faces must enclose one consistently oriented solid')
    return solid


def _matrix(shape, value):
    if not isinstance(value, list) or len(value) not in (3, 4):
        raise ValueError('Use a 3×4 or 4×4 affine matrix')
    rows = [_vector(row, 4) for row in value]
    if len(rows) == 4 and rows[3] != [0, 0, 0, 1]:
        raise ValueError('The affine matrix must end in [0,0,0,1]')
    matrix = np.asarray(rows[:3])
    if _dimension(shape) == 2:
        if any(matrix[2, [0, 1, 3]]):
            raise ValueError('Keep the 2D drawing in the XY plane')
        matrix = matrix[:2, [0, 1, 3]]
    if abs(np.linalg.det(matrix[:, :-1])) < 1e-14:
        raise ValueError('The affine matrix must have a nonsingular linear part')
    return shape.transform(matrix)


def _primitive(node):
    op = node['op']
    if op in ('square', 'cube'):
        f = _bind(node, ('size', 'center'))
        dim = 2 if op == 'square' else 3
        size = _vector(f.get('size', 1), dim, scalar=True)
        if min(size) <= 0:
            raise ValueError('Size must be positive')
        factory = kernel.CrossSection.square if dim == 2 else kernel.Manifold.cube
        return factory(size, _flag(f.get('center', False)))
    if op in ('circle', 'sphere'):
        f = _bind(node, ('r',), named=('d', '$fn'))
        radius = _radius(f)
        if not radius:
            raise ValueError('Radius must be positive')
        factory = kernel.CrossSection.circle if op == 'circle' else kernel.Manifold.sphere
        return factory(radius, _segments(f.get('$fn', 0)))
    if op == 'cylinder':
        f = _bind(node, ('h', 'r1', 'r2', 'center'), named=('r', 'd', 'd1', 'd2', '$fn'))
        if {'r', 'd'} & f.keys() and {'r1', 'r2', 'd1', 'd2'} & f.keys():
            raise ValueError('Choose a common radius/diameter or individual end radii')
        radius = _radius(f)
        low, high = _radius(f, 'r1', 'd1', radius), _radius(f, 'r2', 'd2', radius)
        height = _number(f.get('h', 1))
        if height <= 0 or max(low, high) <= 0:
            raise ValueError('Cylinder height and at least one end radius must be positive')
        return kernel.Manifold.cylinder(height, low, high, _segments(f.get('$fn', 0)), _flag(f.get('center', False)))
    if op == 'polyhedron':
        f = _bind(node, ('points', 'faces', 'convexity'))
        _convexity(f)
        return _polyhedron(f)
    f = _bind(node, ('points', 'paths', 'convexity'))
    _convexity(f)
    return _polygon(f)


def _dimension(shape):
    return 2 if isinstance(shape, kernel.CrossSection) else 3


def _complexity(shape):
    return shape.num_vert() if _dimension(shape) == 2 else shape.num_tri()


def _transform_vector(value, dim, pad):
    if isinstance(value, list) and len(value) == 2 and dim == 3:
        value = [*value, pad]
    elif isinstance(value, list) and len(value) == 3 and dim == 2:
        value = _vector(value, 3)[:2]
    return _vector(value, dim)


def _rotate(shape, f):
    angle = f.get('a', 0)
    dim = _dimension(shape)
    if isinstance(angle, list):
        if 'v' in f:
            raise ValueError('Use Euler angles or one angle and a rotation axis')
        xyz = _vector(angle, 3)
        if dim == 2:
            if xyz[0] or xyz[1]:
                raise ValueError('Rotate a drawing about Z; extrude before rotating it out of plane')
            return shape.rotate(xyz[2])
        return shape.rotate(xyz)
    angle = _number(angle)
    x, y, z = _vector(f.get('v', [0, 0, 1]), 3)
    length = math.hypot(x, y, z)
    if not length:
        raise ValueError('Rotation axis must have a direction')
    if dim == 2:
        if x or y:
            raise ValueError('Rotate a drawing about Z; extrude before rotating it out of plane')
        return shape.rotate(math.copysign(1, z) * angle)
    if x == y == 0:
        return shape.rotate([0, 0, math.copysign(1, z) * angle])
    x, y, z = (axis / length for axis in (x, y, z))
    c, s = math.cos(math.radians(angle)), math.sin(math.radians(angle))
    t = 1 - c
    return shape.transform(
        [
            [t * x * x + c, t * x * y - s * z, t * x * z + s * y, 0],
            [t * x * y + s * z, t * y * y + c, t * y * z - s * x, 0],
            [t * x * z - s * y, t * y * z + s * x, t * z * z + c, 0],
        ]
    )


class ScriptGeometryBudget:
    def __init__(self):
        self.remaining = _MAX_WORK
        self.nodes = 0

    def spend(self, units):
        self.remaining -= units
        if self.remaining < 0:
            raise ValueError('Script exceeds the geometry work budget; reduce repetitions or $fn')

    def check(self, shape):
        dimension = _dimension(shape)
        if dimension == 3 and shape.status() != kernel.Error.NoError:
            raise ValueError('Operation produced an invalid solid')
        count = _complexity(shape)
        limit = _MAX_VERTICES if dimension == 2 else _MAX_TRIANGLES
        if count > limit:
            raise ValueError(f'Operation exceeds the {limit} geometry element limit')
        self.spend(count)
        if not shape.is_empty():
            box = shape.bounds() if dimension == 2 else shape.bounding_box()
            if any(not math.isfinite(n) or abs(n) > _MAX_COORD for n in box):
                raise ValueError('Geometry exceeds ±100000 mm')
        return shape

    def combine(self, op, shapes):
        if not shapes:
            raise ValueError('Supply child geometry')
        dim = _dimension(shapes[0])
        if any(_dimension(s) != dim for s in shapes):
            raise ValueError('Cannot combine 2D and 3D geometry; extrude or project first')
        if len(shapes) == 1 and op != 'hull':
            return shapes[0]
        shapes_type = kernel.CrossSection if dim == 2 else kernel.Manifold
        sizes = [_complexity(s) for s in shapes]
        total = sum(sizes)
        if total > (_MAX_VERTICES if dim == 2 else _MAX_TRIANGLES):
            raise ValueError('Too much input geometry in this operation; simplify the children')
        self.spend(total)
        if op == 'hull':
            return shapes_type.batch_hull(shapes)
        # Bound pairwise intersection work for overlapping operands; disjoint arrays
        # cost only their input size. The post-operation limit still checks the result.
        box = [s.bounds() if dim == 2 else s.bounding_box() for s in shapes]
        overlap = 0
        for i, a in enumerate(box):
            for j in range(i):
                b = box[j]
                if all(a[axis] <= b[dim + axis] and b[axis] <= a[dim + axis] for axis in range(dim)):
                    overlap += sizes[i] * sizes[j]
                    if overlap > 32_000_000:
                        raise ValueError('Overlapping geometry exceeds the intersection work budget; reduce $fn')
        operation = {
            'union': kernel.OpType.Add,
            'difference': kernel.OpType.Subtract,
            'intersection': kernel.OpType.Intersect,
        }[op]
        return shapes_type.batch_boolean(shapes, operation)

    def extrude(self, shape, op, f):
        if _dimension(shape) != 2:
            raise ValueError('Extrusion requires a 2D drawing')
        _convexity(f)
        if op == 'linear_extrude':
            height = _number(f.get('height', 1))
            if height <= 0:
                raise ValueError('Extrusion height must be positive')
            twist = _number(f.get('twist', 0))
            slices = _integer(f.get('slices', max(1, math.ceil(abs(twist) / 12))), 1, 256, 'slices')
            if abs(twist) / slices >= 180:
                raise ValueError('Use more extrusion slices for this twist')
            scale = _vector(f.get('scale', 1), 2, scalar=True)
            if min(scale) < 0:
                raise ValueError('Extrusion scale must be nonnegative')
            center = _flag(f.get('center', False))
            # Each wall edge becomes two triangles per slice, plus two triangulated caps.
            faces = 2 * shape.num_vert() * (slices + 1) + 4 * shape.num_contour()
            if faces > _MAX_TRIANGLES:
                raise ValueError('Extrusion exceeds the triangle budget; reduce slices or $fn')
            self.spend(faces)
            solid = shape.extrude(height, slices - 1, -twist, scale)
            return solid.translate([0, 0, -height / 2]) if center else solid
        angle = _number(f.get('angle', 360))
        if not 0 < abs(angle) <= 360:
            raise ValueError('Revolve angle must have a magnitude above 0 and at most 360 degrees')
        segments = _segments(f.get('$fn', 0))
        faces = 2 * shape.num_vert() * (segments + 1) + 4 * shape.num_contour()
        if faces > _MAX_TRIANGLES:
            raise ValueError('Revolution exceeds the triangle budget; reduce profile vertices or $fn')
        self.spend(faces)
        left, _, right, _ = shape.bounds()
        if left < 0 < right:
            raise ValueError('A revolved profile must lie on one side of the Y axis')
        negative_x = left < 0
        if negative_x:
            shape = shape.mirror([1, 0])
        out = shape.revolve(segments, abs(angle))
        if angle < 0:
            out = out.mirror([0, 1, 0])
        return out.rotate([0, 0, 180]) if negative_x else out

    def node(self, node, depth):
        if depth > 64 or self.nodes >= 1024:
            raise ValueError('Script exceeds the geometry nesting or instance limit')
        self.nodes += 1
        op = node['op']
        children = [self.node(n, depth + 1) for n in node['children']]
        try:
            shape = self.operation(node, op, children)
            return self.check(shape)
        except (ValueError, OverflowError, RuntimeError) as exc:
            location = f'Line {node.line}, column {node.column}: ' if hasattr(node, 'line') else ''
            raise ValueError(f'{location}{op}: {exc}') from exc

    def operation(self, node, op, children):
        if op in ('square', 'cube', 'circle', 'sphere', 'cylinder', 'polygon', 'polyhedron'):
            if children:
                raise ValueError('A primitive cannot contain children')
            return _primitive(node)
        if op in ('union', 'difference', 'intersection', 'hull'):
            _bind(node)
            return self.combine(op, children)
        shape = self.combine('union', children)
        dim = _dimension(shape)
        if op in ('translate', 'scale', 'mirror'):
            f = _bind(node, ('v',))
            value = f.get('v', _MISSING)
            if value is _MISSING:
                raise ValueError('Supply a vector')
            if op == 'scale' and type(value) in (int, float):
                value = [_number(value)] * dim
            if op == 'mirror' and dim == 2 and isinstance(value, list) and len(value) == 3 and value[2] != 0:
                raise ValueError('Use an in-plane mirror normal for a drawing')
            vector = _transform_vector(value, dim, 1 if op == 'scale' else 0)
            if op == 'scale' and 0 in vector:
                raise ValueError('Scale factors must be nonzero')
            if op == 'mirror' and not any(vector):
                raise ValueError('Mirror normal must have a direction')
            return getattr(shape, op)(vector)
        if op == 'multmatrix':
            return _matrix(shape, _bind(node, ('m',)).get('m'))
        if op == 'rotate':
            return _rotate(shape, _bind(node, ('a', 'v')))
        if op == 'linear_extrude':
            return self.extrude(shape, op, _bind(node, ('height', 'center', 'convexity', 'twist', 'slices', 'scale')))
        if op == 'rotate_extrude':
            return self.extrude(shape, op, _bind(node, ('angle', 'convexity'), named=('$fn',)))
        if op == 'offset':
            if dim != 2:
                raise ValueError('Offset needs a 2D drawing')
            f = _bind(node, ('r',), named=('delta', 'chamfer', '$fn'))
            if 'delta' in f and 'r' in f:
                raise ValueError('Choose delta or r')
            chamfer = _flag(f.get('chamfer', False))
            if chamfer and 'delta' not in f:
                raise ValueError('Use chamfer with delta')
            segments = _segments(f.get('$fn', 0))
            join = kernel.JoinType.Round
            if 'delta' in f:
                join = kernel.JoinType.Bevel if chamfer else kernel.JoinType.Miter
            estimated = shape.num_vert() * (segments if join == kernel.JoinType.Round else 2)
            self.spend(estimated)
            return shape.offset(_number(f.get('delta', f.get('r', 1))), join, circular_segments=segments)
        if op == 'projection':
            if dim != 3:
                raise ValueError('Projection needs a 3D solid')
            f = _bind(node, ('cut',), named=('convexity',))
            _convexity(f)
            if _flag(f.get('cut', False)):
                return shape.slice(0.0)
            return kernel.CrossSection(shape.project().to_polygons(), kernel.FillRule.Positive)
        raise ValueError(f'Unknown geometry operation {op!r}')


def build_script(source, parameters=None, *, budget=None):
    """Return (2, CrossSection) or (3, Manifold) for the evaluated source."""
    from .scad_language import evaluate

    geometry = budget if budget is not None else ScriptGeometryBudget()
    nodes = evaluate(source, parameters)
    if not nodes:
        raise ValueError('The script did not produce geometry')
    shapes = [geometry.node(n, 0) for n in nodes]
    result = geometry.check(geometry.combine('union', shapes))
    if result.is_empty() or (_dimension(result) == 3 and result.volume() <= 0):
        raise ValueError('The script produced an empty result')
    return _dimension(result), result
