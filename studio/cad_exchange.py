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
        weights = np.asarray(weights)
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


def surface_mesh(surface):
    poles, weights, kind, degrees = _surface_spec(surface)
    n = 24
    t = np.linspace(0, 1, n + 1)
    u, v = [_basis(t, size, d, kind) for size, d in zip(poles.shape[:2], degrees)]
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


def _ocp():
    try:
        import OCP
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


def _surface_face(surface):
    from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace
    from OCP.Geom import Geom_BezierSurface, Geom_BSplineSurface
    from OCP.TColStd import TColStd_Array1OfReal, TColStd_Array1OfInteger, TColStd_Array2OfReal
    from OCP.TColgp import TColgp_Array2OfPnt
    from OCP.gp import gp_Pnt
    poles, weights, kind, degrees = _surface_spec(surface)
    surface_mesh(surface)  # Same degeneracy checks as the editor preview.
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
    maker = BRepBuilderAPI_MakeFace(geometry, 1e-7)
    if not maker.IsDone():
        raise ValueError('Could not build the spline face')
    return maker.Face()


def _part_shape(model):
    """Translate the validated feature history into exact B-rep geometry in feature order."""
    from .solid_cad import _build
    from OCP.BRepAlgoAPI import BRepAlgoAPI_Common, BRepAlgoAPI_Cut, BRepAlgoAPI_Fuse
    from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace, BRepBuilderAPI_MakePolygon, BRepBuilderAPI_Transform
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
        elif kind in ('extrude', 'revolve'):
            wire = BRepBuilderAPI_MakePolygon()
            pts = [d.vector(pt, 2) for pt in f['points']]
            if pts[0] == pts[-1]:
                pts.pop()
            for a, b in pts:
                wire.Add(gp_Pnt(a, b, 0) if kind == 'extrude' else gp_Pnt(a, 0, b))
            wire.Close()
            face = BRepBuilderAPI_MakeFace(wire.Wire()).Face()
            if kind == 'extrude':
                shape = BRepPrimAPI_MakePrism(face, gp_Vec(0, 0, d.positive(f['height']))).Shape()
            else:
                angle = radians(d.evaluate(f.get('angle', 360)))
                shape = BRepPrimAPI_MakeRevol(face, gp_Ax1(gp_Pnt(), gp_Dir(0, 0, 1)), angle).Shape()
        elif kind == 'step':
            shape = _read_shape(_decode(f.get('content')))
        else:
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
            shape = (_surface_face(surface) if surface is not None else
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
        from .solid_cad import binary_stl, rebuild
        data = payload()
        if 'surface' in data:
            mesh = surface_mesh(data['surface'])
        elif 'content' in data:
            mesh = inspect_step(_decode(data['content']))
        else:
            mesh = rebuild(data.get('model'))
        return Response(binary_stl(mesh), mimetype='model/stl')
