"""Studio's solids and drawings on KerfKite's own kernels, and the choice between them and the fallback.

Solid modelling is computed by the native core's solid kernel (native/core/src/solid) and 2D drawings by
its polygon kernel (native/core/src/polygon) whenever the compiled extension is installed. Both work on a
grid of 1 nm with exact arithmetic and give the same result on every platform. docs/SOLID-KERNEL.md
describes them.

Without the extension (the portable wheel, a source checkout without a compiler) Studio models with the
third-party library manifold3d, as it did before the kernel. KERFKITE_SOLID_BACKEND=manifold selects that
library where the extension is installed, and =kernel refuses to run without the extension. The library
also remains the oracle the kernel is tested against.

The classes below present the part of manifold3d's interface that Studio calls, so that solid_cad.py,
scad_geometry.py, solid_loft.py, cad_exchange.py and assembly.py are the same code on both: `modelling()`
returns this module or manifold3d. Lengths are millimetres and angles degrees.

Two differences are visible to a user. Circles and spheres are faceted by a segment count on both, but the
kernel's sphere is bands of latitude where the library subdivides an octahedron, so it has about twice the
triangles. And the kernel refuses a result it cannot represent on its grid ("features too fine for the 1 nm
grid"), which happens for some models whose faces almost coincide after a turn. Studio then reports the
error with the feature or script line; it does not hand the model to the other library, because the
two facet differently and a model must not change shape from one rebuild to the next.
"""

import enum
import json
import os
import sys

import numpy as np

try:
    from ._relief_native import core_call as _core_call, solid_call as _solid_call
except ImportError:  # the extension is absent, or was built before the solid kernel was added
    _core_call = _solid_call = None

_kernel_installed = None
_DEFAULT_SEGMENTS = 48
_TOO_FINE = 'too fine for the 1 nm grid'
_STRAIGHT_MM = 2e-6  # two grid units: what rounding a turned face's corners can bend a straight side by


def kernel_available():
    """Whether the compiled extension carries the native core's solid kernel and polygon command."""
    global _kernel_installed
    if _kernel_installed is None:
        try:
            _kernel_installed = _solid_call is not None and bool(
                _solid_call('{"shape": {"box": [1, 1, 1]}}')
                and _core_call('polygon', '{"op": "hull", "rings": [[[0, 0], [1, 0], [0, 1]]]}')
            )
        except (ValueError, RuntimeError):
            _kernel_installed = False
    return _kernel_installed


def solid_backend():
    """'kernel' when the native core models solids and drawings, 'manifold' when manifold3d does."""
    wanted = os.environ.get('KERFKITE_SOLID_BACKEND', 'auto')
    if wanted not in ('auto', 'kernel', 'manifold'):
        raise ValueError('KERFKITE_SOLID_BACKEND must be auto, kernel or manifold')
    if wanted == 'kernel' and not kernel_available():
        raise ValueError('The solid kernel is not installed: build the compiled KerfKite extension')
    return 'kernel' if wanted != 'manifold' and kernel_available() else 'manifold'


def modelling():
    """The module Studio models with: this one, or manifold3d as the fallback."""
    if solid_backend() == 'kernel':
        return sys.modules[__name__]
    try:
        import manifold3d
    except ImportError as exc:
        raise ValueError(
            'Solids need the compiled KerfKite extension, or manifold3d>=3.5,<4 in the Studio Python environment'
        ) from exc
    return manifold3d


class Error(enum.Enum):
    NoError = 0
    NotManifold = 1


class FillRule(enum.Enum):
    EvenOdd = 'evenodd'
    NonZero = 'nonzero'
    Positive = 'positive'


class OpType(enum.Enum):
    Add = 'union'
    Subtract = 'difference'
    Intersect = 'intersection'


class JoinType(enum.Enum):
    Round = 'round'
    Miter = 'miter'
    Bevel = 'bevel'


def _numbers(values, count):
    values = [float(v) for v in values]
    if len(values) != count:
        raise ValueError(f'Expected {count} numbers')
    return values


def _solid(node, kept=(), **ask):
    """One request to the solid kernel: its answer and the solid, kept for later requests."""
    request = json.dumps({'shape': node, **ask}, allow_nan=False)
    try:
        answer, handle = _solid_call(request, tuple(solid._handle for solid in kept))
    except ValueError as exc:
        if _TOO_FINE in str(exc):
            raise ValueError(
                f'The solid kernel cannot represent this result: {exc}. Two faces almost coincide here, '
                'usually after a rotation. Move, turn or resize one of the shapes by a small amount'
            ) from exc
        raise
    return json.loads(answer), handle


def _polygon(op, **request):
    """One request to the polygon kernel: the rings of the region it answers with, and its area."""
    answer = json.loads(_core_call('polygon', json.dumps(dict(request, op=op), allow_nan=False)))
    rings = []
    for polygon in answer['polygons']:
        rings.append(polygon['shell'])
        rings.extend(polygon['holes'])
    return rings, answer['area']


def _between(a, b, c):
    """Whether b lies within _STRAIGHT_MM of the straight line from a to c, between the two."""
    ux, uy, vx, vy = c[0] - a[0], c[1] - a[1], b[0] - a[0], b[1] - a[1]
    square, across, along = ux * ux + uy * uy, ux * vy - uy * vx, ux * vx + uy * vy
    # Plain arithmetic only, so that the answer is the same wherever Python runs.
    return square > 0 and across * across <= _STRAIGHT_MM * _STRAIGHT_MM * square and 0 <= along <= square


def _straightened(rings):
    """Section rings without the points that only divide a straight side.

    The kernel itself leaves out a point inside a flat face, exactly. A face that has been turned is not
    exactly flat on the grid any more: its triangles meet in a crease of under a nanometre, and the section
    has a point where it crosses the crease. Such a point lies within the grid's rounding of the line
    between its neighbours and is dropped here, so that an outline sent to CAM has one move for each side.
    """
    straight = []
    for ring in rings:
        kept = []
        for point in ring:
            while len(kept) >= 2 and _between(kept[-2], kept[-1], point):
                kept.pop()
            kept.append(point)
        # Where the ring closes: its last point, then its first.
        while len(kept) > 3 and _between(kept[-2], kept[-1], kept[0]):
            kept.pop()
        while len(kept) > 3 and _between(kept[-1], kept[0], kept[1]):
            kept.pop(0)
        straight.append(kept)
    return straight


class Mesh64:
    """Vertices (n × 3) and triangles (m × 3, counter-clockwise seen from outside)."""

    def __init__(self, vert_properties, tri_verts):
        self.vert_properties = np.asarray(vert_properties, dtype=np.float64)
        self.tri_verts = np.asarray(tri_verts)
        self.welded = False

    def merge(self):
        """Ask for vertices at the same place to be joined when the mesh becomes a solid."""
        self.welded = True
        return True


class Manifold:
    """A solid held by the kernel. It never changes; every operation returns another."""

    def __init__(self, mesh=None):
        self._status = Error.NoError
        self._mesh = None
        if mesh is None:
            self._info, self._handle = _solid({'union': []})
            return
        node = {
            'mesh': {
                'vertices': mesh.vert_properties[:, :3].tolist(),
                'triangles': mesh.tri_verts.astype(np.int64).tolist(),
                'weld': mesh.welded,
            }
        }
        try:
            self._info, self._handle = _solid(node)
        except ValueError as exc:
            # manifold3d reports a mesh that is not a closed solid by its status; the callers test that.
            self._info, self._handle = _solid({'union': []})
            self._status = Error.NotManifold
            self.reason = str(exc)

    @classmethod
    def _of(cls, node, kept=()):
        solid = cls.__new__(cls)
        solid._status = Error.NoError
        solid._mesh = None
        solid._info, solid._handle = _solid(node, kept)
        return solid

    # ---- primitives ---------------------------------------------------------------------------------

    @staticmethod
    def cube(size, center=False):
        return Manifold._of({'box': _numbers(size, 3), 'center': bool(center)})

    @staticmethod
    def cylinder(height, radius_low, radius_high=-1.0, circular_segments=0, center=False):
        return Manifold._of(
            {
                'cylinder': {
                    'height': float(height),
                    'radius_low': float(radius_low),
                    'radius_high': float(radius_low if radius_high < 0 else radius_high),
                    'segments': int(circular_segments) or _DEFAULT_SEGMENTS,
                    'center': bool(center),
                }
            }
        )

    @staticmethod
    def sphere(radius, circular_segments=0):
        return Manifold._of(
            {'sphere': {'radius': float(radius), 'segments': int(circular_segments) or _DEFAULT_SEGMENTS}}
        )

    # ---- maps ---------------------------------------------------------------------------------------

    def _map(self, how):
        return Manifold._of({**how, 'of': {'kept': 0}}, [self])

    def translate(self, offset):
        return self._map({'translate': _numbers(offset, 3)})

    def rotate(self, degrees):
        """About X, then Y, then Z."""
        return self._map({'rotate': _numbers(degrees, 3)})

    def scale(self, factors):
        return self._map({'scale': _numbers(factors, 3)})

    def mirror(self, normal):
        return self._map({'mirror': _numbers(normal, 3)})

    def transform(self, matrix):
        """p' = M p + t, from the three rows of [M t]."""
        rows = np.asarray(matrix, dtype=np.float64)
        if rows.shape != (3, 4):
            raise ValueError('Use a 3×4 affine matrix')
        return self._map({'matrix': rows.tolist()})

    # ---- Boolean operations and hulls ---------------------------------------------------------------

    @staticmethod
    def batch_boolean(solids, op):
        solids = list(solids)
        return Manifold._of({op.value: [{'kept': i} for i in range(len(solids))]}, solids)

    @staticmethod
    def batch_hull(solids):
        solids = list(solids)
        return Manifold._of({'hull': [{'kept': i} for i in range(len(solids))]}, solids)

    def __add__(self, other):
        return Manifold.batch_boolean([self, other], OpType.Add)

    def __sub__(self, other):
        return Manifold.batch_boolean([self, other], OpType.Subtract)

    def __xor__(self, other):
        return Manifold.batch_boolean([self, other], OpType.Intersect)

    # ---- measures -----------------------------------------------------------------------------------

    def status(self):
        return self._status

    def is_empty(self):
        return self._info['empty']

    def volume(self):
        return self._info['volume']

    def surface_area(self):
        return self._info['area']

    def num_tri(self):
        return self._info['triangles']

    def num_vert(self):
        return self._info['vertices']

    def bounding_box(self):
        return tuple(self._info['bounds'])

    def to_mesh64(self):
        if self._mesh is None:
            mesh = _solid({'kept': 0}, [self], mesh=True)[0]['mesh']
            self._mesh = Mesh64(
                np.array(mesh['vertices'], dtype=np.float64).reshape(-1, 3),
                np.array(mesh['triangles'], dtype=np.uint32).reshape(-1, 3),
            )
        return self._mesh

    def slice(self, height=0.0):
        """The material at Z = height. Lower faces are inclusive, upper faces exclusive."""
        section = _solid({'kept': 0}, [self], sections=[float(height)])[0]['sections'][0]
        return CrossSection(_straightened(section['rings']), FillRule.Positive)

    def project(self):
        """The outline seen along Z."""
        return CrossSection(_solid({'kept': 0}, [self], projection=True)[0]['projection']['rings'], FillRule.Positive)

    def min_gap(self, other, search_length):
        """The distance between the two surfaces when it is below search_length, otherwise search_length."""
        ask = {'to': {'kept': 1}, 'search_length': float(search_length)}
        return _solid({'kept': 0}, [self, other], min_gap=ask)[0]['min_gap']


class CrossSection:
    """A 2D region held as the rings of the polygon kernel: outer rings counter-clockwise, holes clockwise."""

    def __init__(self, contours=None, fillrule=FillRule.Positive):
        rings = [[[float(x), float(y)] for x, y in contour] for contour in ([] if contours is None else contours)]
        self._rings, self._area = _polygon('normalise', rings=rings, fill=fillrule.value) if rings else ([], 0.0)

    @classmethod
    def _of(cls, op, **request):
        region = cls.__new__(cls)
        region._rings, region._area = _polygon(op, **request)
        return region

    # ---- primitives ---------------------------------------------------------------------------------

    @staticmethod
    def square(size, center=False):
        width, height = _numbers(size, 2)
        x, y = (-width / 2, -height / 2) if center else (0.0, 0.0)
        return CrossSection([[[x, y], [x + width, y], [x + width, y + height], [x, y + height]]])

    @staticmethod
    def circle(radius, circular_segments=0):
        """A regular polygon inscribed in the circle with a vertex on +X: the section of the kernel's cylinder."""
        return CrossSection._of('circle', radius=float(radius), segments=int(circular_segments) or _DEFAULT_SEGMENTS)

    # ---- measures -----------------------------------------------------------------------------------

    def is_empty(self):
        return not self._rings

    def num_vert(self):
        return sum(map(len, self._rings))

    def num_contour(self):
        return len(self._rings)

    def area(self):
        return self._area

    def bounds(self):
        if not self._rings:
            return (0.0, 0.0, 0.0, 0.0)
        xs = [x for ring in self._rings for x, _ in ring]
        ys = [y for ring in self._rings for _, y in ring]
        return (min(xs), min(ys), max(xs), max(ys))

    def to_polygons(self):
        return [np.array(ring, dtype=np.float64) for ring in self._rings]

    # ---- maps ---------------------------------------------------------------------------------------

    def _map(self, rows):
        return CrossSection._of('transform', rings=self._rings, fill='positive', matrix=rows)

    def translate(self, offset):
        x, y = _numbers(offset, 2)
        return self._map([[1.0, 0.0, x], [0.0, 1.0, y]])

    def scale(self, factors):
        x, y = _numbers(factors, 2)
        return self._map([[x, 0.0, 0.0], [0.0, y, 0.0]])

    def mirror(self, normal):
        """In the line through the origin with this normal."""
        x, y = _numbers(normal, 2)
        length = x * x + y * y
        if not length:
            return CrossSection()
        return self._map(
            [[1 - 2 * x * x / length, -2 * x * y / length, 0.0], [-2 * x * y / length, 1 - 2 * y * y / length, 0.0]]
        )

    def rotate(self, degrees):
        return CrossSection._of('rotate', rings=self._rings, fill='positive', degrees=float(degrees))

    def transform(self, matrix):
        """p' = M p + t, from the two rows of [M t]."""
        rows = np.asarray(matrix, dtype=np.float64)
        if rows.shape != (2, 3):
            raise ValueError('Use a 2×3 affine matrix')
        return self._map(rows.tolist())

    # ---- Boolean operations, hulls and offsets ------------------------------------------------------

    @staticmethod
    def batch_boolean(regions, op):
        regions = list(regions)
        if not regions:
            return CrossSection()
        first = regions[0]._rings
        rest = [ring for region in regions[1:] for ring in region._rings]
        if op is OpType.Add:
            return CrossSection(first + rest, FillRule.Positive)
        if op is OpType.Subtract:
            return CrossSection._of('difference', a=first, b=rest, fill='positive')
        result = regions[0]
        for region in regions[1:]:
            result = CrossSection._of('intersection', a=result._rings, b=region._rings, fill='positive')
        return result

    @staticmethod
    def batch_hull(regions):
        return CrossSection._of('hull', rings=[ring for region in regions for ring in region._rings])

    def offset(self, delta, join_type=JoinType.Round, miter_limit=2.0, circular_segments=0):
        """Grown (delta > 0) or shrunk; a miter longer than miter_limit × delta is cut square."""
        return CrossSection._of(
            'offset',
            rings=self._rings,
            fill='positive',
            distance=float(delta),
            join=join_type.value,
            turn_segments=int(circular_segments) or _DEFAULT_SEGMENTS,
            miter_limit=float(miter_limit),
        )

    # ---- into solids --------------------------------------------------------------------------------

    def extrude(self, height, n_divisions=0, twist_degrees=0.0, scale_top=(1.0, 1.0)):
        """From Z = 0 to height in n_divisions + 1 layers; the top is turned, then scaled."""
        return Manifold._of(
            {
                'extrude': {
                    'rings': self._rings,
                    'height': float(height),
                    'slices': int(n_divisions) + 1,
                    'twist': float(twist_degrees),
                    'scale': _numbers(scale_top, 2),
                    'fill': 'positive',
                }
            }
        )

    def revolve(self, circular_segments=0, revolve_degrees=360.0):
        """About the Y axis of the drawing, which becomes Z; X is the radius."""
        return Manifold._of(
            {
                'revolve': {
                    'rings': self._rings,
                    'segments': int(circular_segments) or _DEFAULT_SEGMENTS,
                    'degrees': float(revolve_degrees),
                    'fill': 'positive',
                }
            }
        )


def triangulate(polygons):
    """Triangles (as vertex numbers) of one simple polygon, by cutting off one corner at a time.

    Every vertex is used, including one lying on a straight side, so the triangles of two faces that share
    such a vertex still meet edge to edge.
    """
    if len(polygons) != 1:
        raise ValueError('Triangulate one polygon at a time')
    points = [(float(x), float(y)) for x, y in polygons[0]]

    def cross(a, b, c):
        return (points[b][0] - points[a][0]) * (points[c][1] - points[a][1]) - (points[b][1] - points[a][1]) * (
            points[c][0] - points[a][0]
        )

    order = list(range(len(points)))
    if sum(cross(order[0], order[i], order[i + 1]) for i in range(1, len(order) - 1)) < 0:
        order.reverse()
    triangles = []
    while len(order) > 3:
        for at in range(len(order)):
            a, b, c = order[at - 1], order[at], order[(at + 1) % len(order)]
            # A corner can go when it turns left and no other vertex lies in or on its triangle.
            if cross(a, b, c) > 0 and not any(
                cross(a, b, p) >= 0 and cross(b, c, p) >= 0 and cross(c, a, p) >= 0 for p in order if p not in (a, b, c)
            ):
                triangles.append([a, b, c])
                del order[at]
                break
        else:
            raise ValueError('The polygon could not be divided into triangles')
    if cross(*order) <= 0:
        raise ValueError('The polygon could not be divided into triangles')
    triangles.append(order)
    return np.array(triangles, dtype=np.int64)
