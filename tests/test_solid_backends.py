"""KerfKite's solid and polygon kernels behind Studio's modelling, against manifold3d.

The kernel is the default wherever the compiled extension is installed; manifold3d stays as the fallback
for an installation without it and as the oracle here. This module holds the choice between them, the
interface studio/solid_kernel.py gives the kernel, and the differential test: every model Studio's own
tests and documents build is built on both and compared.

What may differ, and by how much:

* The kernels put every vertex on a grid of 1 nm. A vertex of a primitive is up to 0.71 nm from its exact
  place and one made by an operation up to 0.87 nm, so a surface lies within SHIFT_MM of the exact one for
  each of the two. A volume may therefore differ by 2 * SHIFT_MM times the surface area, a surface area by
  2 * SHIFT_MM times the length of the mesh edges, and the area of a drawing by 2 * SHIFT_MM times its
  perimeter. A horizontal section moves sideways by the shift divided by the slope of the face it cuts;
  no case here cuts a face flatter than 1 in 8, hence SECTION_SLOPE.
* A sphere is faceted differently (bands of latitude against a subdivided octahedron). Both have every
  vertex on the sphere and no facet wider than one step of 360 / $fn across, so both contain the sphere of
  radius r cos(step); `sphere_allowance` is what that leaves.
* A round offset is drawn with vertices on the true arc by both, in parts of at most 1.0625 steps, but the
  parts are not the same ones; `arc_allowance` bounds the slivers between the polygon and the arc.
* The side of a twisted extrusion is made of quadrilaterals that are not flat, and each backend may split
  them along the other diagonal. The outlines at the layers are compared instead, and the volume within
  the tetrahedra between the two diagonals.
"""

import contextlib
import copy
import importlib.util
import math
import os
import re
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

from studio import solid_kernel
from studio.assembly import example_assembly, solve
from studio.scad_geometry import build_script
from studio.solid_cad import _build, horizontal_section, rebuild

ROOT = Path(__file__).resolve().parents[1]
HAVE_KERNEL = solid_kernel.kernel_available()
HAVE_MANIFOLD = importlib.util.find_spec('manifold3d') is not None
BOTH = HAVE_KERNEL and HAVE_MANIFOLD

SHIFT_MM = 1e-6  # the grid's rounding, rounded up from 0.87 nm
BOX_MM = 8e-6  # bounds, after the few roundings a case goes through and the slope below
SECTION_SLOPE = 8
HEIGHTS = (0.2113, 0.5127, 0.8291)  # fractions of the height, clear of every face that lies in a plane


@contextlib.contextmanager
def backend(name):
    with mock.patch.dict(os.environ, KERFKITE_SOLID_BACKEND=name):
        yield solid_kernel.modelling()


def perimeter(section):
    return sum(float(np.linalg.norm(np.roll(ring, -1, axis=0) - ring, axis=1).sum()) for ring in section.to_polygons())


def drawing(section):
    return {
        'area': section.area(),
        'bounds': section.bounds(),
        'contours': section.num_contour(),
        'perimeter': perimeter(section),
    }


def edge_length(solid):
    mesh = solid.to_mesh64()
    corners = mesh.vert_properties[:, :3][np.asarray(mesh.tri_verts).astype(np.int64)]
    # Every edge belongs to two triangles.
    return float(np.linalg.norm(corners - np.roll(corners, -1, axis=1), axis=2).sum()) / 2


def measures(solid, kernel, heights=HEIGHTS):
    box = solid.bounding_box()
    silhouette = kernel.CrossSection(solid.project().to_polygons(), kernel.FillRule.Positive)
    return {
        'volume': solid.volume(),
        'area': solid.surface_area(),
        'edges': edge_length(solid),
        'bounds': box,
        'sections': [drawing(solid.slice(box[2] + f * (box[5] - box[2]))) for f in heights],
        'projection': drawing(silhouette),
    }


def sphere_allowance(radius, segments):
    """How far apart two polyhedra inscribed in a sphere may be: (volume, area of a section)."""
    step = math.tau / segments
    return (
        4 / 3 * math.pi * radius**3 * (1 - math.cos(step) ** 3),
        math.pi * radius**2 * (1 - math.cos(step) ** 2),
    )


def arc_allowance(radius, segments, turn):
    """The area between an arc turning by `turn` radians and a polygon inscribed in it in parts of at most
    1.0625 steps: for each part r^2 (t - sin t) / 2, which is at most r^2 t^3 / 12."""
    part = 1.0625 * math.tau / segments
    return radius**2 * turn * part**2 / 12


# ---- the cases: every script and model of Studio's own tests and documents, and a few of each operation ---

DRAWINGS = [
    'difference() { square([30,20], center=true); circle(d=8, $fn=64); translate([10,0]) circle(r=2, $fn=64); }',
    'multmatrix([[1,1,0,5],[0,2,0,3],[0,0,1,0],[0,0,0,1]]) square(2);',
    'mirror([1,0]) scale([2,3]) translate([2,0]) square(4);',
    'polygon(points=[[0,0],[8,0],[8,8],[0,8],[2,2],[6,2],[6,6],[2,6]], paths=[[0,1,2,3],[4,5,6,7]]);',
    'offset(delta=2) hull(){square(2);translate([4,0]) square(2);}',
    'offset(delta=1.5,chamfer=true) polygon([[0,0],[10,0],[10,3],[4,3],[4,9],[0,9]]);',
    'offset(delta=-1) polygon([[0,0],[10,0],[10,3],[4,3],[4,9],[0,9]]);',
    'offset(delta=-1,chamfer=true) polygon([[0,0],[10,0],[10,3],[4,3],[4,9],[0,9]]);',
    # A corner sharper than the miter limit of 2 is cut square by both.
    'offset(delta=1) polygon([[0,0],[10,0],[0,1]]);',
    'offset(delta=0.75) difference(){ square(12); translate([6,6]) square(5,center=true); }',
    'projection() translate([0,0,-1]) cylinder(h=2,r1=4,r2=2,$fn=64);',
    'projection(cut=true) translate([0,0,-1]) cylinder(h=2,r1=4,r2=2,$fn=64);',
    'rotate([0,0,30]) translate([1,2]) scale([2,1]) circle(3,$fn=7);',
    'intersection(){ circle(5,$fn=40); translate([3,0]) circle(5,$fn=40); square(7); }',
    'union(){ square(4); translate([2,2]) square(4); translate([9,0]) circle(1,$fn=9); }',
    'hull(){ circle(2,$fn=12); translate([7,3]) circle(1,$fn=10); translate([2,8]) square(1); }',
]

# (script, radius, $fn, the turn of all its round corners in radians)
ROUND_OFFSETS = [
    ('offset(r=2,$fn=24) hull(){square(2);translate([4,0]) square(2);}', 2, 24, math.tau),
    ('offset(r=1.25,$fn=17) difference(){ square(12); translate([6,6]) circle(3,$fn=20); }', 1.25, 17, math.tau),
    # Shrinking rounds the two corners that point into the material: a quarter turn, and the hole's whole turn.
    ('offset(r=-1,$fn=32) polygon([[0,0],[10,0],[10,3],[4,3],[4,9],[0,9]]);', 1, 32, math.pi / 2),
    ('offset(r=-0.5,$fn=48) difference(){ square(12); translate([6,6]) square(4,center=true); }', 0.5, 48, math.tau),
    ('offset(r=3) square(10);', 3, 48, math.tau),
]

SOLID_SCRIPTS = [
    """size=24; module bore(x) { translate([x,5]) circle(1,$fn=12); }
       linear_extrude(height=3,center=true) difference() { square([size,10]); for (i=[4:4:12]) bore(i); }""",
    """intersection(){ difference(){ cube([20,12,6]); translate([2,2,-1]) cube([16,8,8]); }
       translate([10,-1,-1]) cube([20,14,8]); }""",
    'cylinder(h=6,d1=8,d2=0,center=true,$fn=64);',
    'rotate(a=90,v=[0,1,0]) cube([4,2,3]);',
    'rotate(a=33,v=[1,2,3]) cube([4,2,3]);',
    'rotate_extrude(angle=90,$fn=64) translate([2,0]) square([2,5]);',
    'rotate_extrude(angle=-200,$fn=40) translate([-6,0]) circle(2,$fn=16);',
    'rotate_extrude($fn=36) polygon([[2,0],[5,0],[4,3],[2,2]]);',
    'polyhedron([[0,0,0],[2,0,0],[0,2,0],[0,0,3]],[[0,1,2],[0,3,1],[0,2,3],[1,3,2]]);',
    # A face with a vertex in the middle of a side: the triangles must use it.
    """polyhedron([[0,0,0],[2,0,0],[4,0,0],[4,3,0],[0,3,0],[0,0,2],[2,0,2],[4,0,2],[4,3,2],[0,3,2]],
       [[0,1,2,3,4],[9,8,7,6,5],[0,5,6,1],[1,6,7,2],[2,7,8,3],[3,8,9,4],[4,9,5,0]]);""",
    'scale([1,2,0.5]) mirror([1,1,0]) difference(){ cube(6,center=true); cylinder(h=8,r=2,center=true,$fn=24); }',
    'linear_extrude(height=4,scale=[0.5,2]) translate([-3,-1]) square([6,2]);',
    'linear_extrude(height=4,scale=0) circle(3,$fn=12);',
    'hull(){ cube(4); translate([6,2,1]) cylinder(h=3,r=2,$fn=16); }',
    'difference(){ cube(10,center=true); for(a=[0:45:135]) rotate([0,0,a]) cube([14,2,2],center=true); }',
    'union(){ cube(4); translate([4,0,0]) cube(4); translate([2,2,4]) cylinder(h=2,r=1.5,$fn=20); }',
    'multmatrix([[1,0.3,0,2],[0,1,0.2,0],[0,0,1.5,1]]) cylinder(h=4,r=3,$fn=18);',
]

# (script, sphere radius, $fn)
SPHERE_SCRIPTS = [
    ('translate([10,0,0]) sphere(d=6,$fn=32);', 3, 32),
    ('hull(){ cube(4); translate([6,2,1]) sphere(2,$fn=16); }', 2, 16),
    ('difference(){ cube(8,center=true); sphere(5,$fn=40); }', 5, 40),
    # Two fine spheres that overlap: within the intersection budget on both (see ScriptGeometryBudget.combine).
    ('difference(){ sphere(10,$fn=96); translate([6,3,2]) sphere(8,$fn=96); }', 10, 96),
]


def documented_scripts():
    """The complete programs of the scripting guide."""
    text = (ROOT / 'docs' / 'CAD-SCRIPTING.md').read_text(encoding='utf-8')
    return re.findall(r'```scad\n(.*?)```', text, re.S)


def plate():
    return {
        'version': 1,
        'parameters': {'width': '2 * half', 'half': 20, 'thickness': 6},
        'features': [
            {'id': 'base', 'kind': 'box', 'size': ['width', 30, 'thickness']},
            {
                'id': 'bore',
                'kind': 'cylinder',
                'op': 'cut',
                'radius': 4,
                'height': 'thickness + 2',
                'origin': ['half', 15, -1],
            },
        ],
    }


def patterned(pattern, origin, radius=1):
    model = plate()
    model['features'][1].update(origin=origin, radius=radius, pattern=pattern)
    return model


def loft(top='circle', base='rectangle'):
    feature = {'id': 'inlet', 'kind': 'loft', 'baseShape': base, 'baseWidth': 40, 'baseDepth': 24}
    feature.update(topShape=top, topWidth='outlet', topDepth=12, height=20, offset=[4, 0])
    return {'version': 1, 'parameters': {'outlet': 18}, 'features': [feature]}


def models():
    blind = plate()
    blind['features'][1]['origin'][2] = 2
    sketch = {'id': 'profile', 'kind': 'extrude', 'points': [[0, 0], [0, 3], [4, 0]], 'height': 5}
    sketch.update(rotation=[0, 0, 90], origin=[10, 2, 1])
    lathe = {'id': 'lathe', 'kind': 'revolve', 'points': [[0, 0], [4, 0], [4, 10], [0, 10]], 'angle': 90}
    bored = loft('rectangle')
    bored['features'].append(
        {'id': 'bore', 'kind': 'cylinder', 'op': 'cut', 'radius': 2, 'height': 22, 'origin': [0, 0, -1]}
    )
    scripted = plate()
    scripted['features'].append(
        {
            'id': 'boss',
            'kind': 'script',
            'source': 'translate([8,8,thickness]) cylinder(h=lift,r=3,$fn=24);',
            'scriptParameters': {'lift': 2.5},
        }
    )
    turned = plate()
    turned['features'][1].update(rotation=[20, 30, 40], origin=[20, 15, 3], height=30)
    return {
        'plate': plate(),
        'blind pocket': blind,
        'sketch': {'version': 1, 'features': [sketch]},
        'revolved sector': {'version': 1, 'features': [lathe]},
        'cone': {
            'version': 1,
            'features': [{'id': 'cone', 'kind': 'cylinder', 'radius': 3, 'topRadius': 0, 'height': 10}],
        },
        'linear pattern': patterned({'kind': 'linear', 'count': 'width / 10', 'step': [8, 0, 0]}, [6, 15, -1]),
        'polar pattern': patterned({'kind': 'polar', 'count': 4, 'angle': 90, 'center': ['half', 15, 0]}, [25, 15, -1]),
        'polar pattern at 50 degrees': patterned(
            {'kind': 'polar', 'count': 5, 'angle': 50, 'center': ['half', 15, 0]}, [28, 15, -1]
        ),
        'loft': loft(),
        'loft of rectangles': loft('rectangle'),
        'loft of ellipses': loft('ellipse', 'ellipse'),
        'bored loft': bored,
        'script feature': scripted,
        'turned bore': turned,
    }


class BackendChoiceTests(unittest.TestCase):
    def test_choice_follows_the_extension_and_the_environment(self):
        with mock.patch.dict(os.environ, KERFKITE_SOLID_BACKEND='fast'):
            with self.assertRaisesRegex(ValueError, 'KERFKITE_SOLID_BACKEND must be auto, kernel or manifold'):
                solid_kernel.solid_backend()
        with mock.patch.dict(os.environ, KERFKITE_SOLID_BACKEND='manifold'):
            self.assertEqual(solid_kernel.solid_backend(), 'manifold')
        with mock.patch.object(solid_kernel, '_kernel_installed', False):
            with mock.patch.dict(os.environ, KERFKITE_SOLID_BACKEND='auto'):
                self.assertEqual(solid_kernel.solid_backend(), 'manifold')
            with mock.patch.dict(os.environ, KERFKITE_SOLID_BACKEND='kernel'):
                with self.assertRaisesRegex(ValueError, 'solid kernel is not installed'):
                    solid_kernel.solid_backend()
        with mock.patch.object(solid_kernel, '_kernel_installed', True):
            for wanted in ('auto', 'kernel'):
                with mock.patch.dict(os.environ, KERFKITE_SOLID_BACKEND=wanted):
                    self.assertEqual(solid_kernel.solid_backend(), 'kernel')
                    self.assertIs(solid_kernel.modelling(), solid_kernel)

    def test_an_extension_without_the_kernel_is_not_used(self):
        # No extension at all, and one built before the solid kernel was added.
        with (
            mock.patch.object(solid_kernel, '_kernel_installed', None),
            mock.patch.object(solid_kernel, '_solid_call', None),
        ):
            self.assertFalse(solid_kernel.kernel_available())
        old = mock.Mock(side_effect=ValueError('Unknown command'))
        with (
            mock.patch.object(solid_kernel, '_kernel_installed', None),
            mock.patch.object(solid_kernel, '_solid_call', old),
        ):
            self.assertFalse(solid_kernel.kernel_available())

    def test_neither_kernel_nor_library_is_a_clear_error(self):
        with (
            mock.patch.object(solid_kernel, '_kernel_installed', False),
            mock.patch.dict('sys.modules', {'manifold3d': None}),
        ):
            with self.assertRaisesRegex(ValueError, 'compiled KerfKite extension, or manifold3d'):
                rebuild(plate())
            with self.assertRaisesRegex(ValueError, 'compiled KerfKite extension, or manifold3d'):
                build_script('cube(2);')

    @unittest.skipUnless(HAVE_MANIFOLD, 'manifold3d is not installed')
    def test_without_the_extension_the_library_models(self):
        import manifold3d

        with mock.patch.object(solid_kernel, '_kernel_installed', False):
            self.assertIs(solid_kernel.modelling(), manifold3d)
            part = rebuild(plate())
            dimension, shape = build_script('linear_extrude(2) circle(3,$fn=12);')
        self.assertEqual(part['bounds'], [0, 0, 0, 40, 30, 6])
        self.assertIsInstance(shape, manifold3d.Manifold)
        self.assertEqual(dimension, 3)

    @unittest.skipUnless(HAVE_KERNEL, 'the compiled extension is not installed')
    def test_the_kernel_models_without_the_library(self):
        with mock.patch.dict('sys.modules', {'manifold3d': None}):
            part = rebuild(plate())
            section = horizontal_section(plate(), 3)
            _, drawn = build_script('offset(delta=1) difference(){ square(8); translate([4,4]) circle(2,$fn=16); }')
            _, solid = build_script('hull(){ cube(2); translate([3,0,0]) sphere(1,$fn=8); }')
            assembly = solve(example_assembly())
        self.assertEqual(part['bounds'], [0, 0, 0, 40, 30, 6])
        self.assertEqual(len(section['profiles']), 2)
        self.assertIsInstance(drawn, solid_kernel.CrossSection)
        self.assertIsInstance(solid, solid_kernel.Manifold)
        self.assertEqual(assembly['interferences'], [])


class PackagingTests(unittest.TestCase):
    def test_every_build_carries_the_kernels(self):
        """The extension is built from every source of the solid kernel and of the commands Studio calls,
        each named once, and a source distribution ships them."""
        listed = re.findall(r"'((?:studio|native)/[\w/]+\.cpp)'", (ROOT / 'setup.py').read_text(encoding='utf-8'))
        self.assertEqual(len(listed), len(set(listed)), 'a source listed twice breaks the link')
        for source in listed:
            self.assertTrue((ROOT / source).is_file(), source)
        kernel = {path.relative_to(ROOT).as_posix() for path in (ROOT / 'native/core/src/solid').glob('*.cpp')}
        self.assertGreaterEqual(len(kernel), 12)
        needed = kernel | {'native/core/src/solid_command.cpp', 'native/core/src/polygon/command.cpp'}
        self.assertEqual(needed - set(listed), set())
        manifest = (ROOT / 'MANIFEST.in').read_text(encoding='utf-8')
        self.assertIn('recursive-include native/core *.hpp *.cpp', manifest)
        # The core's own build names the same solid sources.
        cmake = (ROOT / 'native/CMakeLists.txt').read_text(encoding='utf-8')
        for source in needed:
            self.assertIn(source.removeprefix('native/'), cmake)


@unittest.skipUnless(HAVE_KERNEL, 'the compiled extension is not installed')
class KernelInterfaceTests(unittest.TestCase):
    """The parts of studio/solid_kernel.py that the models below do not reach."""

    def test_a_result_too_fine_for_the_grid_is_an_error_and_never_another_backend(self):
        refusal = ValueError('union: the result has features too fine for the 1 nm grid')
        for wanted in ('auto', 'kernel'):
            with mock.patch.dict(os.environ, KERFKITE_SOLID_BACKEND=wanted):
                with mock.patch.object(solid_kernel, '_solid_call', side_effect=refusal):
                    with mock.patch.object(solid_kernel, '_kernel_installed', True):
                        with self.assertRaisesRegex(ValueError, 'base: The solid kernel cannot represent this result'):
                            rebuild(plate())
                        with self.assertRaisesRegex(
                            ValueError, r'Line 1, column 1: cube: .*too fine for the 1 nm grid'
                        ):
                            build_script('cube(2);')

    def test_a_mesh_that_is_not_a_solid_has_a_status_like_the_library(self):
        vertices = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]], dtype=float)
        faces = np.array([[0, 2, 1], [0, 1, 3], [0, 3, 2], [1, 2, 3]])
        solid = solid_kernel.Manifold(solid_kernel.Mesh64(vertices, faces))
        self.assertEqual(solid.status(), solid_kernel.Error.NoError)
        self.assertAlmostEqual(solid.volume(), 1 / 6)
        self.assertEqual((solid.num_vert(), solid.num_tri()), (4, 4))
        open_mesh = solid_kernel.Manifold(solid_kernel.Mesh64(vertices, faces[:3]))
        self.assertEqual(open_mesh.status(), solid_kernel.Error.NotManifold)
        self.assertTrue(open_mesh.is_empty())
        self.assertTrue(open_mesh.reason)
        self.assertTrue(solid_kernel.Manifold().is_empty())
        # Two copies of every vertex: a solid once the mesh is merged, as a STEP tessellation is.
        doubled = solid_kernel.Mesh64(np.vstack((vertices, vertices)), np.vstack((faces[:2], faces[2:] + 4)))
        self.assertEqual(solid_kernel.Manifold(doubled).status(), solid_kernel.Error.NotManifold)
        self.assertTrue(doubled.merge())
        self.assertAlmostEqual(solid_kernel.Manifold(doubled).volume(), 1 / 6)

    def test_operators_gaps_and_defaults(self):
        a = solid_kernel.Manifold.cube([2, 2, 2])
        b = a.translate([1, 0, 0])
        self.assertEqual(((a + b).volume(), (a - b).volume(), (a ^ b).volume()), (12, 4, 4))
        self.assertEqual(a.min_gap(a.translate([5, 0, 0]), 10), 3)
        self.assertEqual(a.min_gap(a.translate([50, 0, 0]), 10), 10)
        self.assertEqual(a.scale([1, 2, 3]).mirror([0, 0, 1]).bounding_box(), (0, 0, -6, 2, 4, 0))
        self.assertEqual(solid_kernel.Manifold.cylinder(2, 1).num_tri(), 4 * 48 - 4)
        self.assertEqual(solid_kernel.Manifold.sphere(1).bounding_box(), (-1, -1, -1, 1, 1, 1))
        with self.assertRaisesRegex(ValueError, '3×4'):
            a.transform([[1, 0, 0], [0, 1, 0]])
        with self.assertRaisesRegex(ValueError, 'Expected 3 numbers'):
            a.translate([1, 2])

    def test_drawings(self):
        section = solid_kernel.CrossSection
        empty = section()
        self.assertTrue(empty.is_empty())
        self.assertEqual(
            (empty.bounds(), empty.area(), empty.num_vert(), empty.to_polygons()), ((0, 0, 0, 0), 0, 0, [])
        )
        self.assertTrue(section.batch_boolean([], solid_kernel.OpType.Add).is_empty())
        square = section.square([4, 2], center=True)
        self.assertEqual(square.bounds(), (-2, -1, 2, 1))
        self.assertTrue(square.mirror([0, 0]).is_empty())
        self.assertEqual(square.translate([2, 1]).mirror([1, 1]).bounds(), (-2, -4, 0, 0))
        self.assertEqual(section.circle(1).num_vert(), 48)
        self.assertEqual(square.offset(1).num_vert(), 4 + 4 * 12)
        self.assertEqual(square.transform([[0, -1, 0], [1, 0, 5]]).bounds(), (-1, 3, 1, 7))
        with self.assertRaisesRegex(ValueError, '2×3'):
            square.transform(np.eye(3))
        # The same vertices as the cylinder's, so a drawn circle and a cylinder coincide exactly.
        ring = section.circle(2.5, 20).to_polygons()[0]
        base = solid_kernel.Manifold.cylinder(1, 2.5, 2.5, 20).slice(0.5).to_polygons()[0]
        self.assertEqual(sorted(map(tuple, ring)), sorted(map(tuple, base)))
        self.assertEqual(section.circle(1, 20).translate([3, 0]).revolve().num_tri(), 2 * 20 * 48)
        self.assertEqual(section.circle(1, 20).extrude(2).num_tri(), 4 * 20 - 4)
        # A turned box is no longer exactly flat on the grid, yet its section has one point for each side.
        turned = solid_kernel.Manifold.cube([30, 20, 10]).rotate([20, 30, 40])
        self.assertEqual([len(turned.slice(z).to_polygons()[0]) for z in (-12, -3, 2.5)], [3, 5, 5])

    def test_triangles_of_a_face(self):
        # A notch, and a vertex in the middle of a straight side, in either direction.
        outline = [[0, 0], [2, 0], [4, 0], [4, 4], [2, 1], [0, 4]]
        for points in (outline, outline[::-1]):
            triangles = solid_kernel.triangulate([points])
            self.assertEqual(len(triangles), 4)
            p = np.array(points, dtype=float)
            a, b, c = (p[triangles[:, i]] for i in range(3))
            areas = ((b - a)[:, 0] * (c - a)[:, 1] - (b - a)[:, 1] * (c - a)[:, 0]) / 2
            self.assertTrue((abs(areas) > 0).all())
            self.assertAlmostEqual(abs(areas.sum()), 10)
        with self.assertRaisesRegex(ValueError, 'one polygon'):
            solid_kernel.triangulate([outline, outline])
        for flat in ([[0, 0], [1, 0], [2, 0]], [[0, 0], [1, 0], [2, 0], [3, 0]]):
            with self.assertRaisesRegex(ValueError, 'could not be divided'):
                solid_kernel.triangulate([flat])


@unittest.skipUnless(BOTH, 'needs the compiled extension and manifold3d')
class DifferentialTests(unittest.TestCase):
    """The kernel against manifold3d, on the same models through the same Studio code."""

    def both(self, build):
        results = []
        for name in ('manifold', 'kernel'):
            with backend(name) as kernel:
                results.append(build(kernel))
        return results

    def assert_boxes(self, theirs, ours):
        for a, b in zip(theirs, ours, strict=True):
            self.assertAlmostEqual(a, b, delta=BOX_MM)

    def assert_drawings(self, theirs, ours, allowance=0.0, slope=1):
        self.assertEqual(theirs['contours'], ours['contours'])
        self.assert_boxes(theirs['bounds'], ours['bounds'])
        self.assertAlmostEqual(theirs['area'], ours['area'], delta=2 * SHIFT_MM * slope * ours['perimeter'] + allowance)

    def assert_solids(self, theirs, ours):
        self.assertAlmostEqual(theirs['volume'], ours['volume'], delta=2 * SHIFT_MM * ours['area'])
        self.assertAlmostEqual(theirs['area'], ours['area'], delta=2 * SHIFT_MM * ours['edges'])
        self.assert_boxes(theirs['bounds'], ours['bounds'])
        for a, b in zip(theirs['sections'], ours['sections'], strict=True):
            self.assert_drawings(a, b, slope=SECTION_SLOPE)
        self.assert_drawings(theirs['projection'], ours['projection'])

    def test_drawings(self):
        for script in DRAWINGS:
            with self.subTest(script=script):
                theirs, ours = self.both(lambda kernel: drawing(build_script(script)[1]))
                self.assert_drawings(theirs, ours)

    def test_round_offsets_within_the_faceting_of_their_arcs(self):
        for script, radius, segments, turn in ROUND_OFFSETS:
            with self.subTest(script=script):
                theirs, ours = self.both(lambda kernel: drawing(build_script(script)[1]))
                self.assert_drawings(theirs, ours, allowance=arc_allowance(radius, segments, turn))

    def test_scripted_solids(self):
        for script in SOLID_SCRIPTS + documented_scripts():
            with self.subTest(script=script):
                (their_dimension, theirs), (our_dimension, ours) = self.both(
                    lambda kernel: (lambda dimension, shape: (dimension, shape))(*build_script(script))
                )
                self.assertEqual(their_dimension, our_dimension)
                if our_dimension == 2:
                    self.assert_drawings(drawing(theirs), drawing(ours))
                    continue
                with backend('manifold') as kernel:
                    theirs = measures(theirs, kernel)
                with backend('kernel') as kernel:
                    ours = measures(ours, kernel)
                self.assert_solids(theirs, ours)

    def test_the_guide_has_its_three_programs(self):
        self.assertEqual(len(documented_scripts()), 3)

    def test_models(self):
        for name, model in models().items():
            with self.subTest(model=name):
                theirs, ours = self.both(lambda kernel: measures(_build(copy.deepcopy(model))[0], kernel))
                self.assert_solids(theirs, ours)

    def test_spheres_within_their_faceting(self):
        ball = {'version': 1, 'features': [{'id': 'ball', 'kind': 'sphere', 'radius': 10}]}
        clamp = {'id': 'clamp', 'kind': 'box', 'size': [20, 20, 10], 'origin': [-10, -10, 0], 'op': 'intersect'}
        cases = [(lambda script=script: build_script(script)[1], r, n) for script, r, n in SPHERE_SCRIPTS]
        cases.append((lambda: _build(ball)[0], 10, 96))
        cases.append((lambda: _build(dict(ball, features=ball['features'] + [clamp]))[0], 10, 96))
        for build, radius, segments in cases:
            with self.subTest(radius=radius, segments=segments):
                theirs, ours = self.both(lambda kernel: measures(build(), kernel))
                volume, section = sphere_allowance(radius, segments)
                self.assertAlmostEqual(theirs['volume'], ours['volume'], delta=volume)
                # Where a sphere is cut by another shape, the cut runs across facets that differ: by their depth.
                depth = radius * (1 - math.cos(math.tau / segments))
                for a, b in zip(theirs['bounds'], ours['bounds'], strict=True):
                    self.assertAlmostEqual(a, b, delta=BOX_MM + depth)
                for a, b in zip(theirs['sections'], ours['sections'], strict=True):
                    self.assertEqual(a['contours'], b['contours'])
                    self.assertAlmostEqual(a['area'], b['area'], delta=section)
                self.assertAlmostEqual(theirs['projection']['area'], ours['projection']['area'], delta=section)

    def test_the_intersection_budget_refuses_the_same_scripts(self):
        heavy = 'difference(){ sphere(10,$fn=160); translate([6,3,2]) sphere(8,$fn=160); }'
        for name in ('manifold', 'kernel'):
            with self.subTest(backend=name), backend(name):
                with self.assertRaisesRegex(ValueError, 'intersection work budget'):
                    build_script(heavy)

    def test_twisted_extrusion_at_its_layers(self):
        script = 'linear_extrude(height=5,twist=90,slices=12,scale=.5) translate([4,0]) square(2);'
        layers = [k / 12 for k in range(12)]  # the top face itself is not a section: upper faces are exclusive
        theirs, ours = self.both(lambda kernel: measures(build_script(script)[1], kernel, layers))
        for a, b in zip(theirs['sections'], ours['sections'], strict=True):
            self.assert_drawings(a, b)
        self.assert_boxes(theirs['bounds'], ours['bounds'])
        # Between two layers each side is a quadrilateral that is not flat. Its two diagonals bound a
        # tetrahedron, and the choice of diagonal adds or leaves out exactly that.
        corners = np.array([[4, 0], [6, 0], [6, 2], [4, 2]], dtype=float)
        rings = []
        for k in range(13):
            angle = -math.radians(90) * k / 12
            turn = np.array([[math.cos(angle), -math.sin(angle)], [math.sin(angle), math.cos(angle)]])
            ring = corners @ turn.T * (1 - 0.5 * k / 12)
            rings.append(np.column_stack((ring, np.full(4, 5 * k / 12))))
        between = 0.0
        for low, high in zip(rings, rings[1:]):
            for i in range(4):
                a, b, c, d = low[i], low[(i + 1) % 4], high[(i + 1) % 4], high[i]
                between += abs(np.dot(b - a, np.cross(c - a, d - a))) / 6
        self.assertGreater(between, 0.5)
        self.assertAlmostEqual(theirs['volume'], ours['volume'], delta=between + 2 * SHIFT_MM * ours['area'])
        # The corners of the top are where the script puts them, on both.
        for result in (theirs, ours):
            self.assertAlmostEqual(result['bounds'][5], 5)
        with backend('kernel'):
            mesh = build_script(script)[1].to_mesh64()
        top = mesh.vert_properties[mesh.vert_properties[:, 2] == 5][:, :2]
        expected = rings[-1][:, :2]
        self.assertAlmostEqual(float(abs(np.sort(top, axis=0) - np.sort(expected, axis=0)).max()), 0, delta=SHIFT_MM)

    def test_assemblies(self):
        block = {'version': 1, 'features': [{'id': 'block', 'kind': 'box', 'size': [2, 3, 4]}]}
        pin = {'version': 1, 'features': [{'id': 'pin', 'kind': 'cylinder', 'radius': 1, 'height': 6}]}
        touching = {
            'version': 1,
            'parts': [
                {'id': 'A', 'model': block},
                {'id': 'B', 'model': block, 'origin': [2, 0, 0]},
                {'id': 'C', 'model': pin, 'origin': [1, 1.5, 1], 'rotation': [0, 25, 0]},
                {'id': 'D', 'model': block, 'origin': [9, 0, 0]},
            ],
        }
        for name, document in (('example', example_assembly()), ('touching and crossing', touching)):
            with self.subTest(assembly=name):
                theirs, ours = self.both(lambda kernel: solve(copy.deepcopy(document), True))
                self.assertEqual(theirs['contacts'], ours['contacts'])
                self.assertEqual(
                    [found['parts'] for found in theirs['interferences']],
                    [found['parts'] for found in ours['interferences']],
                )
                for a, b in zip(theirs['interferences'], ours['interferences'], strict=True):
                    self.assertAlmostEqual(a['volume'], b['volume'], delta=1e-4)
                for part in ours['parts']:
                    self.assert_boxes(theirs['parts'][part]['bounds'], ours['parts'][part]['bounds'])
                    self.assertEqual(theirs['parts'][part]['matrix'], ours['parts'][part]['matrix'])

    def test_previews_and_sections_through_the_api_functions(self):
        for name, model in models().items():
            with self.subTest(model=name):
                theirs, ours = self.both(lambda kernel: rebuild(copy.deepcopy(model)))
                self.assertEqual(len(theirs['profiles']), len(ours['profiles']))
                self.assert_boxes(theirs['bounds'], ours['bounds'])
                self.assertEqual(theirs['parameters'], ours['parameters'])
                height = (ours['bounds'][2] + ours['bounds'][5]) / 2 + 0.0137
                theirs, ours = self.both(lambda kernel: horizontal_section(copy.deepcopy(model), height))
                self.assertEqual(len(theirs['profiles']), len(ours['profiles']))
                self.assertTrue(all(ring[0] == ring[-1] for ring in ours['profiles']))


if __name__ == '__main__':
    unittest.main()
