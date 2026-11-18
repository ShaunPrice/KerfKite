import copy
import unittest

import numpy as np
from flask import Flask

from studio.assembly import example_assembly, register, solve


def block(name, origin=None):
    return {'id': name, 'origin': origin or [0, 0, 0], 'model': {'version': 1, 'features': [
        {'id': 'block', 'kind': 'box', 'size': [2, 3, 4]}]}}


def assembly(*parts, mates=()):
    return {'version': 1, 'parts': list(parts), 'mates': list(mates)}


class AssemblyTests(unittest.TestCase):
    def test_fixed_mate_honours_rotated_parent_and_local_frames(self):
        root, child, tip = block('Base', [3, 4, 5]), block('Arm'), block('Tool')
        root['rotation'] = [90, 0, 90]
        doc = assembly(tip, child, root, mates=[
            {'child': 'Arm', 'parent': 'Base', 'kind': 'fixed', 'parentFrame': {'origin': [4, 0, 0]}},
            {'child': 'Tool', 'parent': 'Arm', 'kind': 'fixed',
             'parentFrame': {'origin': [2, 0, 0], 'rotation': [0, 90, 0]},
             'childFrame': {'origin': [0, 0, 1], 'rotation': [0, 90, 0]}}])
        original = copy.deepcopy(doc)
        result = solve(doc, True)['parts']
        self.assertEqual(original, doc)
        arm = np.array(result['Arm']['matrix'])
        tool = np.array(result['Tool']['matrix'])
        np.testing.assert_allclose(arm[:3, 3], [3, 8, 5], atol=1e-12)
        np.testing.assert_allclose(tool @ [0, 0, 1, 1], arm @ [2, 0, 0, 1], atol=1e-12)
        np.testing.assert_allclose(arm[:3, :3], tool[:3, :3], atol=1e-12)
        self.assertTrue(result['Tool']['triangles'])
        root['origin'][2] += 8
        moved = solve(doc)['parts']
        for name in moved:
            self.assertAlmostEqual(moved[name]['bounds'][2] - result[name]['bounds'][2], 8)

    def test_degrees_of_freedom(self):
        for kind, offset, angles in [('planar', [2, 3, 0], [0, 0, 90]),
                                     ('concentric', [0, 0, 8], [0, 0, 90]),
                                     ('point', [0, 0, 0], [0, 90, 0])]:
            with self.subTest(kind=kind):
                doc = assembly(block('Root'), block('Joint'), mates=[{'child':'Joint', 'parent':'Root',
                               'kind':kind, 'offset':offset, 'rotation':angles}])
                mat = np.array(solve(doc)['parts']['Joint']['matrix'])
                np.testing.assert_allclose(mat[:3, 3], offset)
                np.testing.assert_allclose(mat[:3, 0], [0, 0, -1] if kind == 'point' else [0, 1, 0], atol=1e-12)

    def test_bore_clearance_and_offset_interference(self):
        doc = example_assembly()
        self.assertEqual(solve(doc)['contacts'], [])
        self.assertEqual(solve(doc)['interferences'], [])
        doc['mates'][0]['parentFrame']['origin'][0] += 4
        collisions = solve(doc)['interferences']
        self.assertEqual(collisions[0]['parts'], ['Bearing plate', 'Shaft'])
        self.assertGreater(collisions[0]['volume'], 100)

    def test_contact_separation_and_penetration(self):
        pair = assembly(block('A'), block('B', [2, 0, 0]))
        result = solve(pair)
        self.assertFalse(result['interferences'])
        self.assertEqual(result['contacts'], [{'parts':['A', 'B']}])
        pair['parts'][1]['origin'][0] += .01
        self.assertFalse(solve(pair)['contacts'])
        pair['parts'][1]['origin'][0] = 1.5
        self.assertAlmostEqual(solve(pair)['interferences'][0]['volume'], 6)

    def test_invalid_constraints(self):
        first = {'child':'B','parent':'A','kind':'fixed'}
        for mates in [[{**first,'parent':'none'}], [first, {**first,'parent':'C'}],
                      [first, {**first,'child':'A','parent':'B'}], [{**first,'kind':'hinge'}],
                      [{**first,'rotation':[1,0,0]}], [{**first,'kind':'planar','offset':[0,0,1]}],
                      [{**first,'kind':'concentric','offset':[1,0,0]}], [{**first,'kind':'point','offset':[0,1,0]}],
                      [{**first,'parentFrame':{'rotation':[0,float('nan'),0]}}]]:
            with self.subTest(mates=mates), self.assertRaises(ValueError):
                solve(assembly(block('A'), block('B'), block('C'), mates=mates))
        with self.assertRaises(ValueError):
            solve(assembly(block('same'), block('same')))

    def test_api_example_and_solve(self):
        app = Flask(__name__)
        register(app)
        client = app.test_client()
        doc = client.get('/api/assembly/example').get_json()
        result = client.post('/api/assembly/solve', json={'assembly':doc, 'mesh':True})
        self.assertEqual(result.status_code, 200)
        self.assertIn('vertices', result.get_json()['parts']['Shaft'])
