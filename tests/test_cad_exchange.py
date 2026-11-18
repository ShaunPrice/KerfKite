import base64
import importlib.util
from io import BytesIO
from math import pi
import unittest

from studio.cad_exchange import inspect_step, surface_mesh, write_step
from studio.solid_cad import rebuild

PATCH = {'poles': [[[0, 0, 0], [0, 12, 3]], [[20, 0, 4], [20, 12, 0]]]}
PLATE = {'version': 1, 'parameters': {'bore': 2}, 'features': [
    {'kind': 'box', 'id': 'plate', 'size': [12, 20, 5]},
    {'kind': 'cylinder', 'id': 'hole', 'radius': 'bore', 'height': 8,
     'origin': [6, 7, -1], 'op': 'cut'}]}


class SurfaceTest(unittest.TestCase):
    def test_boundary_interpolation_and_interior(self):
        mesh = surface_mesh(PATCH)
        self.assertEqual(mesh['vertices'][0], [0, 0, 0])
        self.assertEqual(mesh['vertices'][-1], [20, 12, 0])
        self.assertEqual(mesh['vertices'][12 * 25 + 12], [10, 6, 1.75])
        self.assertEqual(len(mesh['triangles']), 1152)
        self.assertGreater(mesh['area'], 240)

    def test_rational_bilinear_midpoint_and_clamped_span(self):
        model = {**PATCH, 'kind':'bspline', 'degrees':[1,1], 'weights':[[1,1],[1,3]]}
        mesh = surface_mesh(model)
        midpoint = mesh['vertices'][12*25+12]
        for actual, expected in zip(midpoint, [80/6, 8, 7/6]):
            self.assertAlmostEqual(actual, expected)
        self.assertEqual(mesh['vertices'][0], [0,0,0])
        self.assertEqual(mesh['vertices'][-1], [20,12,0])
        for change in ({'degrees':[2,1]}, {'weights':[[1,1],[1,0]]}):
            with self.assertRaises(ValueError):
                surface_mesh({**model, **change})

    def test_joined_trimmed_sheets_both_axes_and_reversible_domain(self):
        patch = {'poles': [[[0,0,0],[0,12,0]], [[20,0,0],[20,12,0]]]}
        for axis, delta in [('u', [20,0,0]), ('v', [0,12,0])]:
            child = {'poles': [[[v + d for v, d in zip(p,delta)] for p in row] for row in patch['poles']]}
            for mode in ('position', 'tangent'):
                sheet = {'kind':'sheet', 'patches':[patch,{**child,'continuity':mode}], 'axis':axis}
                display = surface_mesh(sheet)
                self.assertEqual((display['faces'], len(display['triangles'])), (2,2304))
                self.assertAlmostEqual(display['area'], 480)
                joined = surface_mesh({**sheet, 'trim':[.25,.75,.1,.8]})
                self.assertAlmostEqual(joined['area'], 480*.5*.7)
                visible = surface_mesh({**sheet,'trim':[0,.5,0,.5]})
                self.assertEqual(visible['faces'],1)
                self.assertEqual(len(visible['resolvedPatches']),2)
                self.assertAlmostEqual(visible['area'],120)
        for invalid in ([0,0,0,1], [0,1,.5,.3], [0,1,0,2], [0,1,float('nan'),1], [0,1]):
            with self.subTest(invalid=invalid),self.assertRaisesRegex(ValueError,'trim|Trim'):
                surface_mesh({**patch, 'trim':invalid})

    def test_rational_seam_derivatives_and_incompatible_edges(self):
        from studio.cad_exchange import _sheet, _basis
        import numpy as np
        parent = {'poles':[[[i*7,j*4,i*j+2] for j in range(3)] for i in range(4)],
                  'weights': [[1+.05*i*j for j in range(3)] for i in range(4)]}
        child = {'poles':[[[i*4+20,j*4,7] for j in range(3)] for i in range(5)],
                 'kind':'bspline','degrees':[2,2], 'continuity':'tangent'}
        patches, _ = _sheet({'kind':'sheet','axis':'u','patches':[parent,child]})
        def point(spec,u,v):
            p,w,kind,deg = spec
            bu,bv = [_basis(np.array([t]),n,d,kind)[0] for t,n,d in zip([u,v],p.shape[:2],deg)]
            blend = np.outer(bu,bv)*w
            return np.einsum('ij,ijk->k',blend,p)/blend.sum()
        left,right = patches[0][0],patches[1][0]
        eps = 1e-6
        for v in [0,.15,.43,.9,1]:
            seam = point(left,1.,v)
            np.testing.assert_allclose(seam, point(right,0.,v),atol=1e-10)
            np.testing.assert_allclose((seam-point(left,1-eps,v))/eps,
                                       (point(right,eps,v)-seam)/eps,atol=5e-4)
        with self.assertRaisesRegex(ValueError,'transverse'):
            surface_mesh({'kind':'sheet','axis':'u','patches':[parent,{**child,'degrees':[2,1]}]})

    def test_invalid_control_nets(self):
        for poles in ([[[0, 0, 0]]], [[], []], [[[1, 1, 1]]*2]*2,
                      [[[0, 0, 0], [1, 1, 1]], [[1, 1, 1]]],
                      [[[0, 0, 0], [1, 1, 1]], [[1, 1, 1], [0, 0, float('nan')]]]):
            with self.subTest(poles=poles), self.assertRaises(ValueError):
                surface_mesh({'poles': poles})


@unittest.skipUnless(importlib.util.find_spec('OCP'), 'Install the step extra for exchange interoperability tests')
class ExchangeTest(unittest.TestCase):
    def test_compound_part_analytic_geometry(self):
        data = write_step(model=PLATE)
        self.assertIn(b'CYLINDRICAL_SURFACE', data)
        part = inspect_step(data)
        self.assertEqual((part['solids'], part['faces']), (1, 7))
        self.assertTrue(part['solidOnly'])
        self.assertAlmostEqual(part['volume'], 12*20*5 - 4*pi*5, places=7)
        self.assertEqual(part['bounds'], [0, 0, 0, 12, 20, 5])
        source = base64.b64encode(data).decode()
        derived = {'version': 1, 'features': [
            {'kind': 'step', 'id': 'reference', 'content': source, 'origin': [30, 0, 0]},
            {'kind': 'box', 'id': 'split', 'op': 'cut', 'size': [5, 24, 8], 'origin': [38, 0, -1]}]}
        preview = rebuild(derived)
        exact = inspect_step(write_step(model=derived))
        self.assertEqual(exact['bounds'], [30, 0, 0, 38, 20, 5])
        self.assertAlmostEqual(preview['volume'], exact['volume'], delta=.2)

    def test_external_step_and_unit_conversion(self):
        # Exercise OCCT's independent primitive writer without Studio's feature translator.
        from OCP.BRepPrimAPI import BRepPrimAPI_MakeSphere
        from OCP.STEPControl import STEPControl_Writer, STEPControl_AsIs
        w = STEPControl_Writer()
        w.Transfer(BRepPrimAPI_MakeSphere(4).Shape(), STEPControl_AsIs)
        stream = BytesIO()
        w.WriteStream(stream)
        raw = stream.getvalue()
        self.assertIn(b'SI_UNIT(.MILLI.,.METRE.)', raw)
        # Same coordinates in centimetres have ten times the linear size.
        cm = raw.replace(b'SI_UNIT(.MILLI.,.METRE.)', b'SI_UNIT(.CENTI.,.METRE.)')
        sphere = inspect_step(cm)
        self.assertAlmostEqual(sphere['volume'], 4*pi*40**3/3, places=5)

    def test_surface_round_trip_and_solid_boundary(self):
        raw = write_step(surface=PATCH)
        for parsed in (inspect_step(raw), inspect_step(write_step(source=raw))):
            self.assertEqual((parsed['faces'], parsed['solids']), (1, 0))
            self.assertFalse(parsed['solidOnly'])
            self.assertAlmostEqual(parsed['area'], surface_mesh(PATCH)['area'], delta=.1)
        with self.assertRaisesRegex(ValueError, 'closed solids'):
            rebuild({'version': 1, 'features': [{'kind':'step','id':'open','content':base64.b64encode(raw).decode()}]})

    def test_transforms_and_patterns(self):
        def box_model(kind, **keys):
            return {'version':1, 'features':[dict(id='profile', kind=kind, **keys)]}
        cases = [
            box_model('extrude', points=[[0, 0], [8, 0], [6, 10]], height=4, rotation=[30, 45, 15], origin=[1,2,3]),
            box_model('revolve', points=[[3, 0], [6, 0], [6, 8], [3, 8]], angle=120),
            box_model('cylinder', radius=4, topRadius=0, height=12),
            box_model('box', size=[3,4,5], origin=[10,0,0], pattern={'kind':'polar','count':4,'angle':90}),
            box_model('box', size=[3,4,5], pattern={'kind':'linear','count':3,'step':[10,0,0]})]
        for model in cases:
            with self.subTest(model=model):
                expected = rebuild(model)
                actual = inspect_step(write_step(model=model))
                self.assertAlmostEqual(expected['volume'], actual['volume'], delta=actual['volume']*.003)
                for x, y in zip(expected['bounds'], actual['bounds']):
                    self.assertAlmostEqual(x, y, delta=.02)

    def test_multispan_nurbs_exact_step(self):
        poles = [[[i*6, j*4, (i-2)*(j-1)] for j in range(4)] for i in range(5)]
        weights = [[1]*4 for row in poles]
        weights[3][1] = 3.5
        patch = {'poles':poles,'weights':weights,'kind':'bspline','degrees':[2,2]}
        raw = write_step(surface=patch)
        self.assertIn(b'RATIONAL_B_SPLINE_SURFACE', raw)
        mesh = inspect_step(raw)
        self.assertEqual(mesh['solids'], 0)
        from OCP.BRepAdaptor import BRepAdaptor_Surface
        from OCP.TopAbs import TopAbs_FACE
        from OCP.TopoDS import TopoDS
        from studio.cad_exchange import _components, _read_shape
        face = TopoDS.Face_s(next(_components(_read_shape(raw), TopAbs_FACE)))
        evaluator = BRepAdaptor_Surface(face)
        sampled = surface_mesh(patch)
        for i, j in ((0,0), (8,6), (12,12), (19,10), (24,24)):
            actual = evaluator.Value(i/24, j/24).Coord()
            for value, expected in zip(actual, sampled['vertices'][i*25+j]):
                self.assertAlmostEqual(value, expected, places=8)
        self.assertAlmostEqual(sampled['area'], mesh['area'], delta=.003*mesh['area'])

    def test_joined_patch_step_is_sewn_trimmed_shell(self):
        from OCP.TopAbs import TopAbs_SHELL, TopAbs_EDGE
        from OCP.TopTools import TopTools_IndexedMapOfShape
        from OCP.TopExp import TopExp
        from studio.cad_exchange import _components, _read_shape
        patch = {'poles': [[[0,0,0],[0,9,2]], [[10,0,3],[10,9,0]]]}
        child = {'poles': [[[10,0,3],[10,9,0]], [[20,0,6],[20,9,-2]]], 'continuity':'tangent'}
        sheet = {'axis':'u','kind':'sheet','patches':[patch,child], 'trim':[.2,.85,.1,.9]}
        raw = write_step(surface=sheet)
        shell, = _components(_read_shape(raw), TopAbs_SHELL)
        edges=TopTools_IndexedMapOfShape()
        TopExp.MapShapes_s(shell, TopAbs_EDGE, edges)
        self.assertEqual(edges.Extent(),7)
        parsed = inspect_step(raw)
        preview = surface_mesh(sheet)
        self.assertEqual((parsed['faces'], parsed['solids']), (2,0))
        self.assertAlmostEqual(parsed['area'],preview['area'],delta=preview['area']*.001)
        for exact, display in zip(parsed['bounds'],preview['bounds']):
            self.assertAlmostEqual(exact,display,places=5)
        single = inspect_step(write_step(surface={**sheet, 'trim':[.62,.9,0,1]}))
        self.assertEqual(single['faces'],1)
        self.assertAlmostEqual(single['bounds'][0],12.4,places=6)

    def test_drawn_trim_with_hole_crossing_weighted_seam(self):
        from shapely.geometry import Polygon
        from OCP.TopAbs import TopAbs_SHELL
        from studio.cad_exchange import _components, _read_shape
        outline = [[.1,.1],[.9,.1],[.9,.9],[.7,.78],[.1,.9]]
        hole = [[.3,.3],[.65,.3],[.65,.55],[.3,.55]]
        # The plane is 20 × 12. Test the physical area independently of the tessellator.
        patch = {'poles':[[[0,0,0],[0,12,0]],[[10,0,0],[10,12,0]]]}
        child = {'poles':[[[10,0,0],[10,12,0]],[[20,0,0],[20,12,0]]]}
        sheet={'kind':'sheet','axis':'u','patches':[patch,child],'boundary':[outline,hole]}
        mesh=surface_mesh(sheet)
        expected=Polygon(outline,[hole]).area*240
        self.assertAlmostEqual(mesh['area'],expected,places=8)
        for tri in mesh['triangles']:
            points=[mesh['vertices'][index][:2] for index in tri]
            triangle=Polygon([(x/20,y/12) for x,y in points])
            self.assertLess(triangle.intersection(Polygon(hole)).area,1e-12)
        raw=write_step(surface=sheet)
        self.assertEqual(len(list(_components(_read_shape(raw),TopAbs_SHELL))),1)
        self.assertAlmostEqual(inspect_step(raw)['area'],expected,places=6)
        # Repeat with a rational curved face. STEP reconstructs its trim from
        # curves in parameter space, allowing non-straight edges in XYZ.
        child['weights']=[[1,1],[1,2]]
        child['poles'][1][1][2]=5
        self.assertAlmostEqual(inspect_step(write_step(surface=sheet))['area'],
                               surface_mesh(sheet)['area'],delta=.08)
        for invalid in ([[[0,0],[1,1],[0,1],[1,0]]], [outline,[[0,0],[.2,0],[0,.2]]]):
            with self.assertRaisesRegex(ValueError,'outline'):
                surface_mesh({**sheet,'boundary':invalid})

    def test_invalid_exchange(self):
        for data in (b'', b'ISO-10303-21;', b'ISO-10303-21;\nEND-ISO-10303-21;', b'X'*1500001):
            with self.assertRaises(ValueError):
                inspect_step(data)
        with self.assertRaisesRegex(ValueError, 'Choose one'):
            write_step(model=PLATE, surface=PATCH)
