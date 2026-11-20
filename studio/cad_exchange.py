"""Editable rational Bézier and B-spline patches with Open CASCADE STEP exchange."""
import base64
import binascii
from io import BytesIO
from math import comb, radians
from threading import RLock

import numpy as np
from flask import Response, jsonify, request

from .solid_cad import Dimensions

MAX_STEP_BYTES = 1_500_000
MAX_TRIANGLES = 60000
# OCCT's exchange controller has process-wide settings.
_exchange_lock = RLock()


def _poles(surface):
    if not isinstance(surface, dict):
        raise ValueError('Supply a Bézier control net')
    grid = surface.get('poles')
    if not isinstance(grid, list) or not 2 <= len(grid) <= 8:
        raise ValueError('A Bézier patch needs 2–8 rows of control points')
    if not isinstance(grid[0], list) or not 2 <= len(grid[0]) <= 8:
        raise ValueError('A Bézier patch needs 2–8 columns of control points')
    dimensions = Dimensions(surface.get('parameters', {}))
    coords = []
    for row in grid:
        if not isinstance(row, list) or len(row) != len(grid[0]):
            raise ValueError('Use a rectangular control net')
        coords.append([dimensions.vector(p, 3) for p in row])
    return np.array(coords)


def _surface_spec(surface):
    poles = _poles(surface)
    sizes = poles.shape[:2]
    kind = surface.get('kind', 'bezier')
    if kind not in ('bezier', 'bspline'):
        raise ValueError('Select a Bézier or clamped B-spline surface')
    degrees = [size-1 for size in sizes]
    if kind == 'bspline':
        degrees = surface.get('degrees', [min(3, size-1) for size in sizes])
        if not isinstance(degrees, list) or len(degrees) != 2 or any(
                type(d) is not int or not 1 <= d < size for d, size in zip(degrees, sizes)):
            raise ValueError('Each B-spline degree must be at least one and less than its pole count')
    weights = surface.get('weights')
    if weights is None:
        weights = np.ones(sizes)
    else:
        if not isinstance(weights, list) or len(weights) != sizes[0] or any(
                not isinstance(row, list) or len(row) != sizes[1] or any(
                    type(w) not in (int, float) or not .001 <= w <= 1000 for w in row) for row in weights):
            raise ValueError('Use a matching rectangular net of weights from 0.001 to 1000')
        weights = np.asarray(weights, dtype=float)
    return poles, weights, kind, degrees


def _knots(size, degree):
    # Open uniform knots, with full endpoint multiplicity and simple interior knots.
    distinct = np.linspace(0.0, 1.0, size-degree+1)
    counts = [degree+1, *([1]*(len(distinct)-2)), degree+1]
    return distinct, counts


def _basis(t, size, degree, kind):
    if kind == 'bezier':
        return np.stack([comb(degree, i)*(1-t)**(degree-i)*t**i for i in range(size)], axis=1)
    values, counts = _knots(size, degree)
    knots = np.repeat(values, counts)
    basis = np.array([((a <= t) & (t < b)).astype(float) for a, b in zip(knots[:-1], knots[1:])])
    for order in range(1, degree+1):
        next_basis = np.zeros((len(basis)-1, len(t)))
        for i in range(len(next_basis)):
            left = knots[i+order]-knots[i]
            right = knots[i+order+1]-knots[i+1]
            if left:
                next_basis[i] += (t-knots[i])/left*basis[i]
            if right:
                next_basis[i] += (knots[i+order+1]-t)/right*basis[i+1]
        basis = next_basis
    basis[-1, t==1] = 1
    return basis.T


FULL_UV = (0., 1., 0., 1.)
MAX_PATCHES = 16


def _trim(surface):
    bounds = surface.get('trim', FULL_UV)
    if not isinstance(bounds, (list, tuple)) or len(bounds) != 4 or any(
            type(value) not in (int, float) or not np.isfinite(value) for value in bounds):
        raise ValueError('Enter finite U and V trim bounds')
    if not all(0 <= low < high <= 1 for low, high in zip(bounds[::2], bounds[1::2])):
        raise ValueError('Trim bounds must increase between zero and one on each axis')
    return bounds


def _sheet(surface):
    """Resolve the common edge in homogeneous coordinates before either mesh or STEP export."""
    if not isinstance(surface, dict):
        raise ValueError('Supply a surface')
    trim = _trim(surface)
    if surface.get('kind') != 'sheet':
        return [(_surface_spec(surface), trim)], []
    patches = surface.get('patches')
    if not isinstance(patches, list) or not 2 <= len(patches) <= MAX_PATCHES:
        raise ValueError(f'Use 2–{MAX_PATCHES} patches in a joined surface')
    if surface.get('axis') not in ('u', 'v'):
        raise ValueError('Select a U or V join')
    axis = 0 if surface['axis'] == 'u' else 1
    transverse = 1 - axis
    visible, resolved = [], []
    previous = None
    for i, patch in enumerate(patches):
        if not isinstance(patch, dict) or 'trim' in patch:
            raise ValueError('Apply trim bounds to the whole joined surface')
        spec = _surface_spec(patch)
        poles, weights, kind, degrees = spec
        if previous is not None:
            mode = patch.get('continuity', 'position')
            if mode not in ('position', 'tangent'):
                raise ValueError('Choose position or tangent continuity')
            prev_poles, prev_weights, _, prev_degrees = previous
            if (poles.shape[transverse] != prev_poles.shape[transverse]
                    or degrees[transverse] != prev_degrees[transverse]):
                raise ValueError('Joined edges need the same transverse pole count and degree')
            # Index the join direction first. The views update spec in-place.
            p, w = np.moveaxis(poles, axis, 0), np.moveaxis(weights, axis, 0)
            a, aw = np.moveaxis(prev_poles, axis, 0), np.moveaxis(prev_weights, axis, 0)
            p[0], w[0] = a[-1], aw[-1]
            if mode == 'tangent':
                # Endpoint derivative for an open uniform rational spline uses its
                # degree and first knot span. Match the full homogeneous derivative.
                prev_factor = prev_degrees[axis] * (len(a) - prev_degrees[axis])
                factor = degrees[axis] * (len(p) - degrees[axis])
                scale = prev_factor / factor
                w[1] = aw[-1] + scale * (aw[-1] - aw[-2])
                if np.any((w[1] < .001) | (w[1] > 1000)):
                    raise ValueError('Tangent join exceeds the weight range. Adjust parent weights or use Position.')
                h0, h1 = a[-1] * aw[-1, :, None], a[-2] * aw[-2, :, None]
                p[1] = (h0 + scale * (h0 - h1)) / w[1, :, None]
                if not np.all(np.abs(p[1]) <= 100000):
                    raise ValueError('Tangent join exceeds the modelling range')
        resolved.append({**patch, 'poles': poles.tolist(), 'weights': weights.tolist()})
        previous = spec
        window = list(trim)
        lo = max(0., trim[2 * axis] * len(patches) - i)
        hi = min(1., trim[2 * axis + 1] * len(patches) - i)
        if lo < hi:
            window[2 * axis:2 * axis + 2] = [lo, hi]
            visible.append((spec, window))
    return visible, resolved


def _patch_mesh(spec, trim):
    poles, weights, kind, degrees = spec
    n = 24
    u, v = [_basis(np.linspace(lo, hi, n + 1), size, d, kind)
            for lo, hi, size, d in zip(trim[::2], trim[1::2], poles.shape[:2], degrees)]
    numerator = np.einsum('ui,vj,ijk->uvk', u, v, poles * weights[:, :, None])
    denominator = np.einsum('ui,vj,ij->uv', u, v, weights)
    points = numerator/denominator[:, :, None]
    flat = points.reshape(-1, 3)
    triangles = []
    for i in range(n):
        for j in range(n):
            a = i*(n+1) + j
            triangles.extend(((a, a+n+1, a+1), (a+1, a+n+1, a+n+2)))
    triangles = np.array(triangles)
    v = flat[triangles]
    area = np.linalg.norm(np.cross(v[:, 1]-v[:, 0], v[:, 2]-v[:, 0]), axis=1).sum()/2
    if area < 1e-8:
        raise ValueError('The control net collapses to zero surface area')
    return dict(vertices=flat.tolist(), triangles=triangles.tolist(), area=float(area),
                bounds=[*flat.min(axis=0).tolist(), *flat.max(axis=0).tolist()], faces=1, solids=0)


def _regions(surface):
    from shapely import Polygon, box
    from shapely.affinity import affine_transform
    from shapely.geometry.polygon import orient

    patches, resolved = _sheet(surface)
    loops = surface.get('boundary')
    if loops is None:
        return [(spec, window, None) for spec, window in patches], resolved
    if not isinstance(loops, list) or not 1 <= len(loops) <= 9:
        raise ValueError('Draw an outline with up to eight interior holes')
    for ring in loops:
        if (not isinstance(ring, list) or not 3 <= len(ring) <= 64
                or any(not isinstance(pt, list) or len(pt) != 2 or
                       any(type(x) not in (int, float) or not 0 <= x <= 1 for x in pt) for pt in ring)
                or len(set(map(tuple, ring))) != len(ring)):
            raise ValueError('Each UV loop needs 3–64 distinct points inside the unit square')
    outline = Polygon(loops[0], loops[1:])
    if not outline.is_valid or outline.area < 1e-10:
        raise ValueError('Use a simple outline with non-overlapping holes inside it')
    u0,u1,v0,v1 = _trim(surface)
    kept = outline.intersection(box(u0,v0,u1,v1))
    if kept.geom_type != 'Polygon' or kept.area < 1e-10:
        raise ValueError('Trim bounds and outline must retain one connected region')
    count = len(resolved) or 1
    along_u = surface.get('axis', 'u') == 'u'
    first = int((u0 if along_u else v0) * count)
    pieces = []
    for i, (spec, window) in enumerate(patches, first):
        transform = [count,0,0,1,-i,0] if along_u else [1,0,0,count,0,-i]
        local = affine_transform(kept, transform).intersection(box(0,0,1,1))
        for item in getattr(local, 'geoms', [local]):
            if item.geom_type == 'Polygon' and item.area > 1e-12:
                pieces.append((spec, window, orient(item, sign=1.0)))
    if not pieces:
        raise ValueError('No surface remains inside this trim')
    return pieces, resolved


def _trimmed_mesh(spec, window, domain):
    if domain is None:
        return _patch_mesh(spec, window)
    # Clip a regular UV grid against the exact trim outline. Triangulating
    # the clipped cells preserves holes while sampling curvature across the face.
    import shapely

    u0,v0,u1,v1 = domain.bounds
    us, vs = np.linspace(u0,u1,25), np.linspace(v0,v1,25)
    cells = shapely.box(us[:-1,None],vs[None,:-1],us[1:,None],vs[None,1:]).ravel()
    clipped = shapely.intersection(cells, domain)
    polys = shapely.get_parts(shapely.get_parts(clipped))
    polys = polys[shapely.get_type_id(polys) == 3]
    facets = shapely.get_parts(shapely.constrained_delaunay_triangles(polys))
    if not len(facets):
        raise ValueError('The trimmed surface is too small to tessellate')
    uv = shapely.get_coordinates(facets).reshape(-1,4,2)[:,:3,:].copy()
    e1, e2 = uv[:,1,:]-uv[:,0,:], uv[:,2,:]-uv[:,0,:]
    clockwise = e1[:,0]*e2[:,1] < e1[:,1]*e2[:,0]
    uv[clockwise] = uv[clockwise][:,[0,2,1],:]
    coords, indices = np.unique(uv.reshape(-1,2), axis=0,return_inverse=True)
    triangles = indices.reshape(-1,3)
    poles, weights, kind, degrees = spec
    bu,bv = [_basis(coords[:,a],size,degree,kind) for a,(size,degree) in enumerate(zip(poles.shape[:2],degrees))]
    denominator=np.einsum('ui,uj,ij->u',bu,bv,weights)
    points=np.einsum('ui,uj,ijk->uk',bu,bv,poles*weights[:,:,None])/denominator[:,None]
    tri=points[triangles]
    area=float(np.linalg.norm(np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0]),axis=1).sum()/2)
    if area < 1e-8:
        raise ValueError('The retained surface collapses to zero area')
    return dict(vertices=points.tolist(), triangles=triangles.tolist(), area=area,
                bounds=[*points.min(axis=0).tolist(), *points.max(axis=0).tolist()], faces=1, solids=0)


def surface_mesh(surface):
    patches, resolved = _regions(surface)
    pieces = [_trimmed_mesh(*patch) for patch in patches]
    if not resolved:
        return pieces[0]
    vertices, triangles = [], []
    for piece in pieces:
        offset = len(vertices)
        triangles.extend([[offset + index for index in tri] for tri in piece['triangles']])
        vertices.extend(piece['vertices'])
    bounds = np.array([piece['bounds'] for piece in pieces])
    return dict(vertices=vertices, triangles=triangles, area=sum(p['area'] for p in pieces),
                bounds=[*bounds[:, :3].min(axis=0), *bounds[:, 3:].max(axis=0)],
                faces=len(pieces), solids=0, resolvedPatches=resolved)


def _ocp():
    try:
        import OCP  # noqa: F401  (availability probe)
    except (ImportError, OSError) as exc:
        raise ValueError('Install the Studio step extra to enable Open CASCADE STEP exchange: python -m pip install ".[step]"') from exc


def _decode(source):
    if not isinstance(source, str) or len(source) > 4*((MAX_STEP_BYTES+2)//3):
        raise ValueError('Select a STEP file up to 1.5 MB')
    try:
        return base64.b64decode(source, validate=True)
    except (ValueError, binascii.Error) as exc:
        raise ValueError('Invalid STEP file encoding') from exc


def _read_shape(raw):
    from OCP.IFSelect import IFSelect_RetDone
    from OCP.STEPControl import STEPControl_Reader
    if not isinstance(raw, bytes) or len(raw) > MAX_STEP_BYTES or not raw.lstrip().upper().startswith(b'ISO-10303-21;'):
        raise ValueError('Supply an ISO 10303-21 STEP file up to 1.5 MB')
    if b'END-ISO-10303-21;' not in raw[-100:].upper() or raw.count(b'=') > 35000:
        raise ValueError('Incomplete STEP document or STEP entity budget exceeded')
    reader = STEPControl_Reader()
    if reader.ReadStream('studio.step', BytesIO(raw)) != IFSelect_RetDone:
        raise ValueError('Open CASCADE could not read this STEP document')
    reader.SetSystemLengthUnit(1.0)  # Normalize translated geometry to millimetres.
    if not 1 <= reader.NbRootsForTransfer() <= 256 or not reader.TransferRoots():
        raise ValueError('STEP contains no transferable geometry or too many roots')
    shape = reader.OneShape()
    if shape.IsNull():
        raise ValueError('STEP contains no transferable shape')
    return shape


def _components(shape, kind):
    from OCP.TopExp import TopExp_Explorer
    it = TopExp_Explorer(shape, kind)
    while it.More():
        yield it.Current()
        it.Next()


def _mesh_shape(shape):
    from OCP.Bnd import Bnd_Box
    from OCP.BRep import BRep_Tool
    from OCP.BRepBndLib import BRepBndLib
    from OCP.BRepGProp import BRepGProp
    from OCP.BRepMesh import BRepMesh_IncrementalMesh
    from OCP.GProp import GProp_GProps
    from OCP.TopAbs import TopAbs_FACE, TopAbs_SOLID, TopAbs_REVERSED
    from OCP.TopLoc import TopLoc_Location
    from OCP.TopoDS import TopoDS
    faces = list(_components(shape, TopAbs_FACE))
    if not 1 <= len(faces) <= 2000:
        raise ValueError('Import a shape with 1–2000 faces')
    bounds = Bnd_Box()
    BRepBndLib.AddOptimal_s(shape, bounds, False, False)
    bbox = list(bounds.Get())
    if not all(np.isfinite(bbox)) or max(map(abs, bbox)) > 100000:
        raise ValueError('STEP geometry exceeds the 100000 mm modelling range')
    diagonal = np.linalg.norm(np.array(bbox[3:])-bbox[:3])
    mesher = BRepMesh_IncrementalMesh(shape, max(.02, diagonal/1500), False, .18, False)
    if not mesher.IsDone():
        raise ValueError('Could not tessellate this STEP shape')
    vertices, triangles = [], []
    for sub in faces:
        face = TopoDS.Face_s(sub)
        location = TopLoc_Location()
        mesh = BRep_Tool.Triangulation_s(face, location)
        if mesh is None or not mesh.NbTriangles():
            raise ValueError('STEP contains an unmeshable face')
        if mesh.NbTriangles() + len(triangles) > MAX_TRIANGLES:
            raise ValueError('STEP preview exceeds 60000 triangles')
        offset = len(vertices)-1  # OCCT's nodes start at one.
        trsf = location.Transformation()
        for i in range(1, mesh.NbNodes()+1):
            vertices.append(list(mesh.Node(i).Transformed(trsf).Coord()))
        for i in range(1, mesh.NbTriangles()+1):
            a, b, c = mesh.Triangle(i).Get()
            if face.Orientation() == TopAbs_REVERSED:
                b, c = c, b
            triangles.append([offset+a, offset+b, offset+c])
    solids = list(_components(shape, TopAbs_SOLID))
    solid_faces = sum(sum(1 for _ in _components(s, TopAbs_FACE)) for s in solids)
    props = GProp_GProps()
    BRepGProp.SurfaceProperties_s(shape, props)
    area = props.Mass()
    vol = GProp_GProps()
    BRepGProp.VolumeProperties_s(shape, vol, True)
    return dict(vertices=vertices, triangles=triangles, bounds=bbox, area=area,
                faces=len(faces), solids=len(solids), solidOnly=bool(solids) and solid_faces == len(faces),
                volume=vol.Mass())


def inspect_step(raw):
    _ocp()
    with _exchange_lock:
        try:
            return _mesh_shape(_read_shape(raw))
        except (RuntimeError, OverflowError) as exc:
            raise ValueError('Open CASCADE could not translate this STEP shape') from exc


def step_manifold(feature, kernel):
    mesh = inspect_step(_decode(feature.get('content')))
    if not mesh['solidOnly']:
        raise ValueError('Select STEP geometry containing only closed solids')
    mesh64 = kernel.Mesh64(np.array(mesh['vertices']), np.array(mesh['triangles'], dtype=np.uint64))
    mesh64.merge()
    part = kernel.Manifold(mesh64)
    if part.status() != kernel.Error.NoError or part.is_empty():
        raise ValueError('Tessellated STEP geometry could not form a manifold solid')
    return part


def _surface_face(spec, window, domain):
    from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace
    from OCP.Geom import Geom_BezierSurface, Geom_BSplineSurface
    from OCP.TColStd import TColStd_Array1OfReal, TColStd_Array1OfInteger, TColStd_Array2OfReal
    from OCP.TColgp import TColgp_Array2OfPnt
    from OCP.gp import gp_Pnt
    poles, weights, kind, degrees = spec
    _patch_mesh(spec, window)  # Apply the same degeneracy check as the editor.
    grid = TColgp_Array2OfPnt(1, len(poles), 1, len(poles[0]))
    for i, row in enumerate(poles):
        for j, xyz in enumerate(row):
            grid.SetValue(i+1, j+1, gp_Pnt(*xyz))
    weight_grid = TColStd_Array2OfReal(1, len(poles), 1, len(poles[0]))
    for i, row in enumerate(weights):
        for j, weight in enumerate(row):
            weight_grid.SetValue(i+1, j+1, float(weight))
    if kind == 'bezier':
        geometry = Geom_BezierSurface(grid, weight_grid)
    else:
        knots, multiplicities = [], []
        for size, degree in zip(poles.shape[:2], degrees):
            distinct, counts = _knots(size, degree)
            values = TColStd_Array1OfReal(1, len(distinct))
            mults = TColStd_Array1OfInteger(1, len(distinct))
            for i, (value, count) in enumerate(zip(distinct, counts), 1):
                values.SetValue(i, value)
                mults.SetValue(i, count)
            knots.append(values)
            multiplicities.append(mults)
        geometry = Geom_BSplineSurface(grid, weight_grid, *knots, *multiplicities, *degrees)
    if domain is not None:
        from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeEdge, BRepBuilderAPI_MakeWire
        from OCP.BRepLib import BRepLib
        from OCP.BRepCheck import BRepCheck_Analyzer
        from OCP.GCE2d import GCE2d_MakeSegment
        from OCP.gp import gp_Pnt2d

        def uv_wire(ring):
            wire = BRepBuilderAPI_MakeWire()
            coords = list(ring.coords)
            for a,b in zip(coords, coords[1:]):
                segment = GCE2d_MakeSegment(gp_Pnt2d(*a),gp_Pnt2d(*b)).Value()
                wire.Add(BRepBuilderAPI_MakeEdge(segment,geometry).Edge())
                if not wire.IsDone():
                    raise ValueError('Could not connect a UV trim wire')
            return wire.Wire()

        maker = BRepBuilderAPI_MakeFace(geometry, uv_wire(domain.exterior), True)
        for hole in domain.interiors:
            maker.Add(uv_wire(hole))
        if not maker.IsDone():
            raise ValueError('Could not trim this STEP face')
        face=maker.Face()
        if not BRepLib.BuildCurves3d_s(face) or not BRepCheck_Analyzer(face).IsValid():
            raise ValueError('The UV outline could not form a valid STEP face')
        return face
    maker = BRepBuilderAPI_MakeFace(geometry, *window, 1e-7)
    if not maker.IsDone():
        raise ValueError('Could not build the spline face')
    return maker.Face()


def _surface_shape(surface):
    from OCP.BRepBuilderAPI import BRepBuilderAPI_Sewing
    from OCP.BRepCheck import BRepCheck_Analyzer
    from OCP.TopAbs import TopAbs_SHELL
    patches, _ = _regions(surface)
    faces = [_surface_face(*patch) for patch in patches]
    if len(faces) == 1:
        return faces[0]
    sewing = BRepBuilderAPI_Sewing(1e-7)
    for face in faces:
        sewing.Add(face)
    sewing.Perform()
    shape = sewing.SewedShape()
    if (shape.IsNull() or shape.ShapeType() != TopAbs_SHELL or sewing.NbDeletedFaces()
            or sewing.NbMultipleEdges() or not BRepCheck_Analyzer(shape).IsValid()):
        raise ValueError('The trimmed patches could not form a valid connected STEP shell')
    return shape


def _part_shape(model):
    """Translate the validated feature history into exact B-rep geometry in feature order."""
    from .solid_cad import _build
    from OCP.BRepAlgoAPI import BRepAlgoAPI_Common, BRepAlgoAPI_Cut, BRepAlgoAPI_Fuse
    from OCP.BRepBuilderAPI import BRepBuilderAPI_Transform
    from OCP.BRepPrimAPI import (BRepPrimAPI_MakeBox, BRepPrimAPI_MakeCone, BRepPrimAPI_MakeCylinder,
                                BRepPrimAPI_MakePrism, BRepPrimAPI_MakeRevol, BRepPrimAPI_MakeSphere)
    from OCP.gp import gp_Ax1, gp_Dir, gp_Pnt, gp_Trsf, gp_Vec
    _, d, _ = _build(model)
    result = None

    def move(shape, distances):
        tr = gp_Trsf()
        tr.SetTranslation(gp_Vec(*distances))
        return BRepBuilderAPI_Transform(shape, tr).Shape()

    def rotate(shape, axis, degrees, center=(0, 0, 0)):
        tr = gp_Trsf()
        tr.SetRotation(gp_Ax1(gp_Pnt(*center), gp_Dir(*axis)), radians(degrees))
        return BRepBuilderAPI_Transform(shape, tr).Shape()

    def combine(left, right, op):
        builder = {'add': BRepAlgoAPI_Fuse, 'cut': BRepAlgoAPI_Cut, 'intersect': BRepAlgoAPI_Common}[op](left, right)
        if not builder.IsDone() or builder.Shape().IsNull():
            raise ValueError('Could not apply a STEP solid Boolean')
        return builder.Shape()

    for f in model['features']:
        if not f.get('enabled', True):
            continue
        kind = f['kind']
        if kind == 'box':
            shape = BRepPrimAPI_MakeBox(*d.vector(f['size'], 3)).Shape()
        elif kind == 'sphere':
            shape = BRepPrimAPI_MakeSphere(d.positive(f['radius'])).Shape()
        elif kind == 'cylinder':
            r, h = d.positive(f['radius']), d.positive(f['height'])
            top = d.evaluate(f.get('topRadius', r))
            shape = (BRepPrimAPI_MakeCylinder(r, h) if abs(top-r) < 1e-10 else BRepPrimAPI_MakeCone(r, top, h)).Shape()
        elif kind == 'loft':
            from .solid_loft import loft_shape
            shape = loft_shape(f,d)
        elif kind in ('extrude', 'revolve'):
            from .sketch_profile import step_profile_face
            face = step_profile_face(f, d)
            if kind == 'extrude':
                shape = BRepPrimAPI_MakePrism(face, gp_Vec(0, 0, d.positive(f['height']))).Shape()
            else:
                angle = radians(d.evaluate(f.get('angle', 360)))
                shape = BRepPrimAPI_MakeRevol(face, gp_Ax1(gp_Pnt(), gp_Dir(0, 0, 1)), angle).Shape()
        elif kind == 'step':
            shape = _read_shape(_decode(f.get('content')))
        else:
            if kind == 'script':
                raise ValueError('Script surfaces are tessellated. Export this part as STL, or suppress scripted features for exact STEP exchange.')
            raise ValueError('This feature has no STEP translation')
        for axis, degrees in zip(np.eye(3), d.vector(f.get('rotation', [0, 0, 0]), 3)):
            if degrees:
                shape = rotate(shape, axis, degrees)
        shape = move(shape, d.vector(f.get('origin', [0, 0, 0]), 3))
        pat = f.get('pattern')
        if pat:
            base = shape
            for i in range(1, int(d.evaluate(pat['count']))):
                if pat['kind'] == 'linear':
                    instance = move(base, [v*i for v in d.vector(pat['step'], 3)])
                else:
                    instance = rotate(base, (0, 0, 1), d.evaluate(pat['angle'])*i, d.vector(pat.get('center', [0, 0, 0]), 3))
                shape = combine(shape, instance, 'add')
        result = shape if result is None else combine(result, shape, f.get('op', 'add'))
    return result


def write_step(*, surface=None, model=None, source=None):
    _ocp()
    from OCP.IFSelect import IFSelect_RetDone
    from OCP.STEPControl import STEPControl_AsIs, STEPControl_Writer
    if sum(v is not None for v in (surface, model, source)) != 1:
        raise ValueError('Choose one part, surface or imported STEP document')
    with _exchange_lock:
        try:
            shape = (_surface_shape(surface) if surface is not None else
                     _part_shape(model) if model is not None else _read_shape(source))
            writer = STEPControl_Writer()
            stream = BytesIO()
            if writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone or writer.WriteStream(stream) != IFSelect_RetDone:
                raise ValueError('Open CASCADE could not write the STEP document')
            return stream.getvalue()
        except (RuntimeError, OverflowError) as exc:
            raise ValueError('Could not translate the geometry to STEP') from exc


def register(app):
    def payload():
        data = request.get_json()
        if not isinstance(data, dict):
            raise ValueError('Supply a CAD exchange request')
        return data

    @app.post('/api/solid-cad/surface')
    def exchange_surface():
        return jsonify(surface_mesh(payload().get('surface')))

    @app.post('/api/solid-cad/step/import')
    def exchange_import():
        return jsonify(inspect_step(_decode(payload().get('content'))))

    @app.post('/api/solid-cad/step/export')
    def exchange_export():
        data = payload()
        content = _decode(data['content']) if 'content' in data else None
        raw = write_step(surface=data.get('surface'), model=data.get('model'), source=content)
        return Response(raw, mimetype='model/step', headers={'Content-Disposition': 'attachment; filename="model.step"'})

    @app.post('/api/solid-cad/exchange-stl')
    def exchange_stl():
        from .solid_cad import binary_stl, model_stl
        data = payload()
        if 'surface' in data:
            mesh = surface_mesh(data['surface'])
        elif 'content' in data:
            mesh = inspect_step(_decode(data['content']))
        else:
            return Response(model_stl(data.get('model')), mimetype='model/stl')
        return Response(binary_stl(mesh), mimetype='model/stl')
