"""The native core's CAD script engine against the Python reference, to the bit."""

import json
import os
import unittest
from unittest.mock import patch

from studio import scad_geometry
from studio.scad_geometry import preview, preview_json
from studio.server import create_app
from studio.solid_kernel import kernel_available

try:
    from studio import _relief_native as native
except ImportError:
    native = None

AVAILABLE = native is not None and hasattr(native, 'core_cam') and kernel_available()


def outcome(payload, backend):
    """The answer as text that tells every bit, or the error; None when the core declines."""
    try:
        return json.dumps(preview(payload, backend=backend), sort_keys=True)
    except ValueError as error:
        return 'ValueError: ' + str(error)
    except NotImplementedError:
        return None


@unittest.skipUnless(AVAILABLE, 'the compiled extension with the solid kernel is not installed')
class NativeScriptTests(unittest.TestCase):
    def setUp(self):
        environment = patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop('KERFKITE_SOLID_BACKEND', None)

    def same(self, payload):
        native_answer, reference = outcome(payload, 'native'), outcome(payload, 'python')
        self.assertIsNotNone(native_answer, 'the core declined')
        self.assertEqual(native_answer, reference)
        return native_answer

    def test_drawings_and_solids_of_every_operation(self):
        sources = [
            'difference() { square([30,20], center=true); circle(d=8, $fn=64); translate([10,0]) circle(r=2, $fn=64); }',
            'polygon(points=[[0,0],[8,0],[8,8],[0,8],[2,2],[6,2],[6,6],[2,6]], paths=[[0,1,2,3],[4,5,6,7]]);',
            'offset(delta=2) hull() { square(2); translate([4,0]) square(2); }',
            'offset(r=-0.5, $fn=16) mirror([1, 1]) scale([2, 3]) rotate(30) square(4);',
            'multmatrix([[1,1,0,5],[0,2,0,3],[0,0,1,0],[0,0,0,1]]) square(2);',
            'intersection() { circle(5, $fn=24); translate([3, 0]) square(6, center=true); }',
            'projection() translate([0,0,-1]) cylinder(h=2,r1=4,r2=2,$fn=64);',
            'projection(cut=true) translate([0,0,-1]) cylinder(h=2,r1=4,r2=2,$fn=64);',
            'intersection(){ difference(){ cube([20,12,6]); translate([2,2,-1]) cube([16,8,8]); } sphere(r=11, $fn=24); }',
            'cylinder(h=6,d1=8,d2=0,center=true,$fn=64);',
            'rotate(a=90,v=[0,1,0]) cube([4,2,3]); rotate(a=35, v=[1, 2, 3]) translate([6, 0, 0]) cube(2);',
            'rotate_extrude(angle=90,$fn=64) translate([2,0]) square([2,5]);',
            'rotate_extrude(angle=-120, $fn=12) translate([-5, 0]) square([2, 3]);',
            'linear_extrude(height=5,twist=90,slices=12,scale=.5,center=true) translate([4,0]) square(2);',
            'hull() { cube(2); translate([5, 5, 5]) sphere(1, $fn=8); }',
            'polyhedron(points=[[0,0,0],[4,0,0],[0,4,0],[0,0,4]], faces=[[0,1,2],[0,3,1],[0,2,3],[1,3,2]]);',
            'polyhedron(points=[[0,0,0],[2,0,0],[2,2,0],[0,2,0],[0,0,2],[2,0,2],[2,2,2],[0,2,2]],'
            ' faces=[[0,1,2,3],[4,7,6,5],[0,4,5,1],[1,5,6,2],[2,6,7,3],[3,7,4,0]]);',
        ]
        for source in sources:
            with self.subTest(source=source):
                self.assertNotIn('ValueError', self.same({'source': source}))

    def test_the_language(self):
        sources = [
            'module ring(r=4, t=1) { difference() { circle(r=r, $fn=32); circle(r=r-t, $fn=32); } }'
            ' linear_extrude(height=3) ring(5);',
            'module stack(h=2) { for (i = [0:$children-1]) translate([0, 0, i*h]) children(i); }'
            ' stack() { cube(1); sphere(1, $fn=6); cylinder(h=1, r=1); }',
            'function fact(n) = n <= 1 ? 1 : n * fact(n - 1); cube(fact(4) / 6);',
            'v = [for (i = [0:3]) i * 2]; cube([len(v), max(v), norm([3, 4])]);',
            'cube(x); x = y + 1; y = 2;',
            '$fn = 6; module m() circle(2); linear_extrude(1) m();',
            'cube([floor(2.7), ceil(1.2), round(-2.5) + 4]); translate([4, 0, 0]) cube(pow(2, 1.5));',
            'cube([1e5, 1, 1] * [floor(1e4), 1e-5, 1] / 1e4 + 1);',
            'cube(٣);',
            'cube(1); é',
            'cube(1); \U0001f600',
            'cube(1); \x07',
            "cube(1); '",
            'cube(1) /* open',
            'include <a.scad>',
            'cube(1e10);',
            'cube(1 / 0);',
            'cube(sqrt(-1));',
            'cube([1, 2] + [1]);',
            'function d(n) = d(n + 1); cube(d(1));',
            'for (i = [0:20], j = [0:20], k = [0:20]) cube(1);',
            'x = y; y = x; cube(x);',
            'children();',
            'cube(1, 2, 3);',
            'union() { cube(1); square(1); }',
            'sphere(99999);',
            '',
            ';',
        ]
        for source in sources:
            with self.subTest(source=source):
                self.same({'source': source})

    def test_parameters_and_overrides(self):
        for payload in (
            {'source': 'cube([w, h, 1]);', 'parameters': {'w': 4, 'h': 'w / 2'}, 'overrides': {'h': 3}},
            {'source': 'w = 9; cube(w);', 'parameters': {'w': 2}},
            {'source': 'cube(a);', 'parameters': {'b': 'a', 'a': 'b'}},
            {'source': 'cube(a);', 'parameters': {'a': 'b', 'b': 'a'}},
            {'source': 'cube(PI);', 'parameters': {'PI': 2}},
            {'source': 'cube(1);', 'overrides': {'bad name': 1}},
            {'source': 'cube(1);', 'overrides': None},
            {'source': 'cube(1);', 'parameters': [], 'order': ['x']},
            {'source': None},
        ):
            with self.subTest(payload=payload):
                self.same(payload)

    def test_the_route_sends_the_core_text(self):
        client = create_app().test_client()
        headers = {'X-Studio-Token': client.get('/api/session').json['token']}
        payload = {'source': 'linear_extrude(2) circle(3, $fn=12);', 'parameters': {'w': 2}}
        with patch.object(scad_geometry, 'preview', wraps=scad_geometry.preview) as reference:
            response = client.post('/api/cad-script', json=payload, headers=headers)
        reference.assert_not_called()
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.get_data(), preview_json(payload))
        self.assertEqual(response.get_json()['dimension'], 3)
        failing = client.post('/api/cad-script', json={'source': 'cube(1'}, headers=headers)
        self.assertEqual(failing.status_code, 400)
        self.assertEqual(failing.get_json()['error'], "Line 1, column 7: Expected ','")

    def test_text_the_core_cannot_carry_is_answered_by_the_reference(self):
        payload = {'source': 'cube(1); \ud800'}
        self.assertIsNone(preview_json(payload))
        with self.assertRaises(NotImplementedError):
            preview(payload, backend='native')
        self.assertIn('Unsupported character', outcome(payload, 'auto'))


if __name__ == '__main__':
    unittest.main()
