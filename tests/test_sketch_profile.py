import importlib.util
import json
import math
import unittest

from studio.sketch_profile import profile_geometry
from studio.solid_cad import Dimensions, rebuild
from studio.cad_exchange import inspect_step, write_step


def model(points, fillets, parameters=None, kind='extrude'):
    return {'version': 1, 'parameters': parameters or {}, 'features': [
        {'id': 'profile', 'kind': kind, 'points': points, 'fillets': fillets, 'height': 5, 'angle': 110}]}


class SketchGeometryTests(unittest.TestCase):
    def test_crossing_vertex_driven_by_source_geometry(self):
        args = '0,0,w,20,0,20,w,0'
        document = model([[f'cross_x({args})', f'cross_y({args})'], ['w', 20], [0, 20]], [0, 0, 0], {'w': 18})
        self.assertAlmostEqual(rebuild(document)['volume'], 90*5)
        reopened = json.loads(json.dumps(document))
        reopened['parameters']['w'] = 48
        self.assertAlmostEqual(rebuild(reopened)['volume'], 240*5)
        for expression in ('cross_x(1,2)', 'cross_y(0,0,1,0,0,1,1,1)', 'cross_x(0,0,1,0,2,-1,2,1)'):
            with self.assertRaises(ValueError):
                Dimensions({}).evaluate(expression)

    def test_tangent_arc_volume_and_endpoints_follow_dimensions(self):
        points = [[0, 0], [20, 0], [20, 10], [0, 10]]
        for winding in (points, list(reversed(points))):
            doc = model(winding, ['r'] * 4, {'r': 1.5})
            for radius in (1.5, 3):
                doc['parameters']['r'] = radius
                volume = rebuild(doc)['volume']
                expected = (20*10 - (4 - math.pi)*radius**2) * 5
                self.assertAlmostEqual(volume, expected, delta=.12)
                corners, outline = profile_geometry(doc['features'][0], Dimensions(doc['parameters']))
                self.assertGreater(len(outline), 4)
                for i, corner in enumerate(corners):
                    self.assertAlmostEqual(math.dist(corner.start, corner.center), radius)
                    self.assertAlmostEqual(math.dist(corner.end, corner.center), radius)
                    self.assertAlmostEqual(math.dist(corner.at(0), corner.start), 0)
                    self.assertAlmostEqual(math.dist(corner.at(1), corner.end), 0)
                    tangent = [corner.start[j] - corners[i-1].end[j] for j in range(2)]
                    radial = [corner.start[j] - corner.center[j] for j in range(2)]
                    self.assertAlmostEqual(sum(a*b for a,b in zip(tangent,radial)), 0)

    def test_concave_fillets_and_overlapping_radii(self):
        doc = model([[0,0],[20,0],[20,10],[10,10],[10,20],[0,20]], [0,0,0,2,0,0])
        self.assertAlmostEqual(rebuild(doc)['volume'], (300 + 4 - math.pi)*5, delta=.012)
        for radii in ([0,0,0,-1,0,0], [0,0,6,6,0,0], [1], [False]*6):
            doc['features'][0]['fillets'] = radii
            with self.assertRaises(ValueError):
                rebuild(doc)

    @unittest.skipUnless(importlib.util.find_spec('OCP'), 'requires optional STEP runtime')
    def test_step_preserves_exact_tangent_arcs(self):
        doc = model([[0,0],[20,0],[20,10],[0,10]], ['r'] * 4, {'r': 2})
        raw = write_step(model=doc)
        self.assertIn(b'CYLINDRICAL_SURFACE', raw)
        self.assertAlmostEqual(inspect_step(raw)['volume'], (200 - (4-math.pi)*4)*5, places=6)
        doc['parameters']['r'] = 5
        capsule = inspect_step(write_step(model=doc))
        self.assertAlmostEqual(capsule['volume'], (100 + 25*math.pi)*5, places=6)
        lathe = model([[3,0],[7,0],[7,10],[3,10]], [0,1,1,0], kind='revolve')
        exact = write_step(model=lathe)
        self.assertIn(b'TOROIDAL_SURFACE', exact)
        self.assertAlmostEqual(rebuild(lathe)['volume'], inspect_step(exact)['volume'], delta=.5)
