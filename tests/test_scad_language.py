import json
import math
import unittest

from studio.scad_language import MAX_RANGE, MAX_SOURCE, ScadError, evaluate


def walk(node):
    yield node
    for child in node['children']:
        yield from walk(child)


class ScadLanguageTests(unittest.TestCase):
    def test_parameterized_bracket(self):
        script = '''
            // The drilling pattern is a reusable module.
            module hole(x, y, depth = thick + 2) {
                translate([x,y,-1]) cylinder(h=depth, d=4);
            }
            difference() {
                cube([width, 20, thick]);
                for (x=[4,width-4], y=[4,16]) hole(x, y);
            }
            /* Top-level defaults may follow their uses. */
            width = 30; thick = 3; $fn = 36;
        '''
        solid, = evaluate(script, {'width': 50})
        self.assertEqual(solid['op'], 'difference')
        self.assertEqual(solid['children'][0]['args'][0], [50, 20, 3])
        translations = [n for n in walk(solid) if n['op'] == 'translate']
        self.assertEqual([n['args'][0] for n in translations], [[4,4,-1],[4,16,-1],[46,4,-1],[46,16,-1]])
        self.assertEqual(translations[0]['children'][0]['kwargs'], {'h':5,'d':4,'$fn':36})
        self.assertEqual((solid.line, solid.column), (6,13))
        self.assertEqual(json.loads(json.dumps(solid)), solid)

    def test_functions_polygon_and_degree_angles(self):
        script = '''
            n = 6;
            function vertex(i, r=4) = r * [cos(i*360/n), sin(i*360/n)];
            polygon(points=[for(i=[0:n-1]) vertex(i)]);
        '''
        pts = evaluate(script)[0]['kwargs']['points']
        self.assertEqual(len(pts), 6)
        self.assertAlmostEqual(pts[1][0], 2)
        self.assertAlmostEqual(pts[1][1], 2*math.sqrt(3))

    def test_lexical_nested_modules_children_and_dynamic_fn(self):
        script = '''
            size = 4;
            module with_pair() { children(0); translate([10,0,0]) children(0); }
            module dot() { circle(size); }
            module row(size=9) {
                $fn = 20;
                with_pair() { dot(); }
            }
            row();
        '''
        nodes = list(walk(evaluate(script)[0]))
        self.assertEqual([n['args'] for n in nodes if n['op']=='circle'], [[4],[4]])
        self.assertEqual([n['kwargs'] for n in nodes if n['op']=='circle'], [{'$fn':20}]*2)
        self.assertEqual(evaluate('module a(){ if($children==1) children(); } a() cube();')[0]['op'], 'cube')

    def test_scope_defaults_and_override(self):
        self.assertEqual(evaluate('cube([x, twice, PI]); twice=2*x;x=4; x=7;', {'x':9})[0]['args'], [[9,18,math.pi]])
        script='module m(x=2,y=x+1) { cube([x,y,1]); } m(y=7); m(5);'
        first, second = evaluate(script)
        self.assertEqual(first['args'][0], [2,7,1])
        self.assertEqual(second['args'][0], [5,6,1])

    def test_arithmetic_logic_vectors_and_short_circuit(self):
        script = '''
            x = 2^3^2 == 512 && -2^2 == -4 && (2+3)*4 == 20 && !false;
            y = [2,3]*[4,5] - norm([0,3,4]);
            if (x || bad) square([x ? y : missing, [2,4][1]]);
            else bad();
        '''
        self.assertEqual(evaluate(script)[0]['args'], [[18,4]])
        self.assertEqual(evaluate('cube(([1,2,3]+[2,4,6])/3);')[0]['args'], [[1,2,3]])
        self.assertEqual(evaluate('cube((![1] && missing) ? 0 : max(ceil(.01),floor(2.8)));')[0]['args'],[2])
        self.assertEqual(evaluate('cube(atan2(1,1) + round(-.5));')[0]['args'],[44])

    def test_decimal_and_descending_ranges(self):
        node, = evaluate('polygon(points=[for(x=[0:.1:.3], y=[3:-2:0]) [x,y]]);')
        self.assertEqual(len(node['kwargs']['points']),8)
        self.assertAlmostEqual(node['kwargs']['points'][-1][0],.3)
        self.assertEqual(node['kwargs']['points'][-1][1],1)
        self.assertEqual(evaluate('// No shapes here\nfor(i=[0:-1:3]) cube(i);'),[])
        self.assertEqual(evaluate('/* header */  \n\n'),[])

    def test_multmatrix_and_polyhedron(self):
        script = '''multmatrix([[1,0,0,3],[0,1,0,0],[0,0,1,0],[0,0,0,1]])
            polyhedron(points=[[0,0,0],[1,0,0],[0,1,0],[0,0,1]],
                       faces=[[0,1,2],[0,3,1],[1,3,2],[2,3,0]]);'''
        self.assertEqual(evaluate(script)[0]['children'][0]['op'], 'polyhedron')

    def test_errors_have_source_locations(self):
        for src, message in (
            ('\n \tcube(1/0);','Invalid arithmetic'),
            ('cube(1,round(0,1));','Unknown function or invalid arguments'),
            ('square([1,z]);','Unknown variable'),
            ('bad=other; other=bad;circle(bad);','Circular'),
            ('foo(1);','unsupported geometry'),
            ('module m(x){cube(x);} m();','Missing argument'),
            ('module m(x=1){} m(z=2);','Unknown argument'),
            ('module m(x){} m(1,x=2);','Duplicate'),
            ('cube(x=1,2);','Positional arguments'),
            ('if (true) { cube();','Expected'),
            ('square(1); /* comment','Unterminated block comment'),
            ('for (i=[0:0:8])cube(i);','step must be nonzero'),
            ('square([1,2][3]);','Index outside'),
            ('module a(x,x){}','Duplicate binding'),
            ('include <part.scad>','External file command'),
            ('import("drawing.svg");','External file command'),
            ('children();','inside a module'),
            ('cube(1e99999);','finite'),
        ):
            with self.subTest(src=src), self.assertRaisesRegex(ScadError,message) as caught:
                evaluate(src)
            self.assertGreater(caught.exception.column,0)
            self.assertGreater(caught.exception.line,0)
        with self.assertRaisesRegex(ScadError,r'Line 3, column 6: Unknown variable'):
            evaluate('// one\n// two\ncube(oops);')

    def test_boundaries(self):
        for src in (
            ' '*(MAX_SOURCE+1), 'cube('*70+'1'+')'*70+';',
            'cube('+'+'.join(['1']*500)+');',
            'module loop(){loop();}loop();',
            'function forever(x)=forever(x);cube(forever(0));',
            f'for (i=[0:{MAX_RANGE}]) cube(i);',
            'for(i=[0:999], j=[0:999]) cube([i,j,1]);',
            'x=[0:999]; y=[x,x,x,x]; z=[y,y,y,y,y]; polygon(z);',
            'for(i=[0:500],j=[0:500]) {}',
            'square([for(i=[0:40],j=[0:40]) i+j]);',
        ):
            with self.subTest(src=src[:35]), self.assertRaises(ScadError):
                evaluate(src)
        for params in ([1], {'x':True}, {'x':float('inf')}, {'x':[1,2]}, {'false':0}):
            with self.subTest(params=params), self.assertRaises(ScadError):
                evaluate('cube(1);', params)


if __name__ == '__main__':
    unittest.main()
