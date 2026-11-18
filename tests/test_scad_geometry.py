import math
import unittest

from studio.scad_geometry import build_script


class ScriptGeometryTests(unittest.TestCase):
    def test_planar_boolean_preserves_holes(self):
        dim, region = build_script("""
            difference() {
                square([30,20], center=true);
                circle(d=8, $fn=64);
                translate([10,0]) circle(r=2, $fn=64);
            }
        """)
        self.assertEqual(dim, 2)
        self.assertEqual(region.bounds(), (-15,-10,15,10))
        self.assertEqual(region.num_contour(), 3)
        self.assertAlmostEqual(region.area(), 600 - 32*math.sin(math.tau/64)*20, places=6)

    def test_parametric_module_array_can_be_drilled_and_extruded(self):
        source = """
            size=20;
            module bore(x) { translate([x,5]) circle(1,$fn=12); }
            linear_extrude(height=3,center=true)
              difference() { square([size,10]); for (i=[4:4:12]) bore(i); }
        """
        dim, part = build_script(source, {'size':24})
        self.assertEqual(dim, 3)
        self.assertEqual(part.bounding_box(), (0,0,-1.5,24,10,1.5))
        self.assertEqual(part.slice(0).num_contour(), 4)
        self.assertAlmostEqual(part.volume(), (240-9)*3)

    def test_nested_booleans_are_not_flattened(self):
        _, part = build_script("""
            intersection(){
                difference(){ cube([20,12,6]); translate([2,2,-1]) cube([16,8,8]); }
                translate([10,-1,-1]) cube([20,14,8]);
            }
        """)
        self.assertEqual(part.bounding_box(), (10,0,0,20,12,6))
        self.assertAlmostEqual(part.volume(), (10*12-8*8)*6)

    def test_frustum_and_sphere(self):
        _, cone = build_script('cylinder(h=6,d1=8,d2=0,center=true,$fn=64);')
        self.assertEqual(cone.bounding_box(), (-4,-4,-3,4,4,3))
        self.assertAlmostEqual(cone.volume(), 32*4**2*math.sin(math.tau/64)*2)
        _, ball = build_script('translate([10,0,0]) sphere(d=6,$fn=32);')
        self.assertEqual(ball.bounding_box(), (7,-3,-3,13,3,3))
        self.assertAlmostEqual(ball.volume(), 4*math.pi*3**3/3, delta=4)

    def test_affine_and_axis_angle(self):
        _, part = build_script('rotate(a=90,v=[0,1,0]) cube([4,2,3]);')
        for actual, expected in zip(part.bounding_box(), [0,0,-4,3,2,0]):
            self.assertAlmostEqual(actual, expected)
        _, region = build_script('multmatrix([[1,1,0,5],[0,2,0,3],[0,0,1,0],[0,0,0,1]]) square(2);')
        self.assertEqual(region.bounds(), (5,3,9,7))
        self.assertAlmostEqual(region.area(), 8)
        _, mirror = build_script('mirror([1,0]) scale([2,3]) translate([2,0]) square(4);')
        self.assertEqual(mirror.bounds(), (-12,0,-4,12))

    def test_offset_hull_and_polygon_paths(self):
        _, region = build_script("""
            polygon(points=[[0,0],[8,0],[8,8],[0,8],[2,2],[6,2],[6,6],[2,6]],
                    paths=[[0,1,2,3],[4,5,6,7]]);
        """)
        self.assertAlmostEqual(region.area(), 48)
        self.assertEqual(region.num_contour(), 2)
        _, pad = build_script('offset(delta=2) hull(){square(2);translate([4,0]) square(2);}')
        self.assertEqual(pad.bounds(), (-2,-2,8,4))
        self.assertAlmostEqual(pad.area(), 60)

    def test_projection_and_slice_have_distinct_geometry(self):
        _, footprint = build_script('projection() translate([0,0,-1]) cylinder(h=2,r1=4,r2=2,$fn=64);')
        _, cut = build_script('projection(cut=true) translate([0,0,-1]) cylinder(h=2,r1=4,r2=2,$fn=64);')
        self.assertEqual(footprint.bounds(), (-4,-4,4,4))
        self.assertEqual(cut.bounds(), (-3,-3,3,3))
        self.assertGreater(footprint.area(), cut.area())

    def test_revolution_and_twisted_extrusion(self):
        _, part = build_script('rotate_extrude(angle=90,$fn=64) translate([2,0]) square([2,5]);')
        self.assertEqual(part.bounding_box(), (0,0,0,4,4,5))
        self.assertAlmostEqual(part.volume(), math.pi*(4**2-2**2)*5/4, delta=.05)
        _, twisted = build_script('linear_extrude(height=5,twist=90,slices=12,scale=.5) translate([4,0]) square(2);')
        mesh = twisted.to_mesh64()
        top = mesh.vert_properties[mesh.vert_properties[:,2] == 5]
        self.assertEqual(len(top), 4)
        self.assertAlmostEqual(top[:,0].min(), 0, places=9)
        self.assertAlmostEqual(top[:,1].min(), -3, places=9)

    def test_closed_polyhedron(self):
        source = 'polyhedron([[0,0,0],[2,0,0],[0,2,0],[0,0,3]],[[0,1,2],[0,3,1],[0,2,3],[1,3,2]]);'
        _, part = build_script(source)
        self.assertEqual(part.bounding_box(), (0,0,0,2,2,3))
        self.assertAlmostEqual(part.volume(), 2)
        with self.assertRaisesRegex(ValueError, 'clockwise'):
            build_script(source.replace('[1,3,2]', '[1,2,3]'))

    def test_errors_include_context_and_limits(self):
        for script, hint in [
            ('square(4); cube(5);', '2D and 3D'),
            ('linear_extrude(4) cube(5);', 'requires a 2D'),
            ('rotate_extrude() square(4,center=true);', 'one side'),
            ('cube([3,0,4]);', 'positive'),
            ('circle(d=2,r=1);', 'Choose one'),
            ('cylinder(h=4,d1=5,r=3);', 'common radius'),
            ('circle(r=4,$fn=99999);', '256'),
            ('linear_extrude(10,slices=256) circle(8,$fn=200);', 'triangle budget'),
            ('for(i=[1:30]) translate([i,0]) sphere(3,$fn=128);', 'budget'),
            ('scale(100000) cube(10);', '100000 mm'),
            ('polygon([[0,0],[1,1],[0,1],[1,0]]);', 'crossing'),
            ('polygon([[0,0],[1,0],[0,1]],[[0,1,1.5]]);', 'Point index'),
            ('difference(){cube(2);cube(2);}', 'empty'),
            ('multmatrix([[1,0,0,0],[2,0,0,0],[0,0,1,0]]) square(3);', 'nonsingular'),
        ]:
            with self.subTest(script=script), self.assertRaisesRegex(ValueError, hint):
                build_script(script)
        with self.assertRaisesRegex(ValueError, r'Line 2, column 3: sphere'):
            build_script('square(2);\n  sphere(-1);')


if __name__ == '__main__':
    unittest.main()
