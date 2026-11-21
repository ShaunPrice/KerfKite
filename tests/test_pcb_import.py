"""Gerber and Excellon import through the native core, checked against geometry worked out here.

The fixtures in tests/pcb are written by hand for these tests. Every expectation below is stated or computed
in Python, independently of the core: coordinates from the file's own format, arcs and macro shapes from
their definitions, areas from their formulae.
"""

import math
import random
import unittest
from pathlib import Path
from unittest import mock

from shapely.geometry import Point, Polygon

from studio import pcb_import
from studio.cam import generate

HAS_CORE = pcb_import.available()
FIXTURES = Path(__file__).parent / 'pcb'
HEAD = '%FSLAX46Y46*%\n%MOMM*%\n'  # millimetres, six decimal places
SETTINGS = dict(mode='router', protocol='grbl', width=60, height=40, feed=300, power=10000, depth=1.8)


def fixture(name):
    return pcb_import.read(FIXTURES / name)


def mm(*values):
    """Coordinates of HEAD's format."""
    return ''.join(f'{letter}{round(value * 1e6)}' for letter, value in zip('XYIJ', values))


def inch(digits, decimals=4):
    """A whole number of an inch format with leading zeros omitted, in millimetres."""
    return round(digits / 10**decimals * 25.4, 6)


def rotate(point, degrees):
    c, s = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
    return [point[0] * c - point[1] * s, point[0] * s + point[1] * c]


def area(points):
    return sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(points, points[1:])) / 2


def image(aperture):
    """A macro aperture's shape: its parts in order, each adding to or cutting from what came before."""
    shape = Polygon()
    for part in aperture['parts']:
        if part['shape'] == 'circle':
            piece = Point(part['x'], part['y']).buffer(part['diameter'] / 2, quad_segs=256)
        else:
            piece = Polygon(part['points'])
        shape = shape.union(piece) if part['exposure'] else shape.difference(piece)
    return shape


def summary(obj):
    """An object without its flattened points, for comparing with a literal."""
    return {key: value for key, value in obj.items() if key != 'points'}


class Base(unittest.TestCase):
    def assertPoints(self, actual, expected, places=6):
        self.assertEqual(len(actual), len(expected))
        for a, e in zip(actual, expected):
            self.assertAlmostEqual(a[0], e[0], places)
            self.assertAlmostEqual(a[1], e[1], places)

    def assertSamePolygon(self, points, expected):
        self.assertEqual(points[0], points[-1])  # closed
        self.assertLess(
            Polygon(points).symmetric_difference(Polygon(expected)).area, 1e-5
        )  # points are to the nanometre

    def assertArc(self, obj, centre, radius, start_degrees, sweep_degrees, tolerance):
        """The flattened points lie on the arc, run the right way, and no chord strays beyond the tolerance."""
        points = obj['points']
        self.assertEqual(points[0], obj['start'])
        self.assertEqual(points[-1], obj['end'])
        turned = 0
        for a, b in zip(points, points[1:]):
            self.assertAlmostEqual(math.dist(a, centre), radius, 5)
            self.assertAlmostEqual(math.dist(b, centre), radius, 5)
            middle = [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2]
            self.assertLessEqual(radius - math.dist(middle, centre), tolerance + 1e-6)
            step = math.remainder(
                math.atan2(b[1] - centre[1], b[0] - centre[0]) - math.atan2(a[1] - centre[1], a[0] - centre[0]),
                2 * math.pi,
            )
            self.assertGreater(step * sweep_degrees, 0)  # every chord turns the same way as the arc
            turned += step
        self.assertAlmostEqual(math.degrees(turned), sweep_degrees, 4)
        first = math.degrees(math.atan2(points[0][1] - centre[1], points[0][0] - centre[0]))
        self.assertAlmostEqual(math.remainder(first - start_degrees, 360), 0, 4)

    def refused(self, call, *fragments):
        with self.assertRaises(ValueError) as caught:
            call()
        for fragment in fragments:
            self.assertIn(fragment, str(caught.exception))


@unittest.skipUnless(HAS_CORE, 'The compiled extension with the native core is not installed')
class GerberTests(Base):
    def test_a_current_layer_is_read_as_the_objects_it_draws(self):
        layer = pcb_import.gerber(fixture('top_copper.gbr'))
        self.assertEqual(layer['format'], 'gerber')
        self.assertEqual(layer['units'], 'mm')
        self.assertEqual(
            layer['coordinate_format'], {'integer': 4, 'decimal': 6, 'omitted_zeros': 'leading', 'notation': 'absolute'}
        )
        self.assertEqual(layer['file_function'], 'Copper,L1,Top')
        self.assertEqual(layer['attributes']['.GenerationSoftware'], 'KerfKite,fixture,1')
        self.assertEqual(layer['warnings'], [])
        apertures = {a['code']: a for a in layer['apertures']}
        self.assertEqual(apertures[10], {'code': 10, 'shape': 'circle', 'diameter': 0.25, 'hole': 0})
        self.assertEqual(apertures[11], {'code': 11, 'shape': 'circle', 'diameter': 1.6, 'hole': 0.8})
        self.assertEqual(apertures[12], {'code': 12, 'shape': 'rectangle', 'width': 1.5, 'height': 1, 'hole': 0})
        self.assertEqual(apertures[13], {'code': 13, 'shape': 'obround', 'width': 2, 'height': 1, 'hole': 0})
        self.assertEqual(
            apertures[14], {'code': 14, 'shape': 'polygon', 'diameter': 2, 'vertices': 6, 'rotation': 30, 'hole': 0.5}
        )
        self.assertEqual(
            [summary(o) for o in layer['objects']],
            [
                {'type': 'line', 'polarity': 'dark', 'aperture': 10, 'start': [5, 5], 'end': [15, 5]},
                {'type': 'line', 'polarity': 'dark', 'aperture': 10, 'start': [15, 5], 'end': [15, 10]},
                {
                    'type': 'arc',
                    'polarity': 'dark',
                    'aperture': 10,
                    'start': [15, 10],
                    'end': [10, 15],
                    'centre': [10, 10],
                    'clockwise': False,
                },
                {'type': 'flash', 'polarity': 'dark', 'aperture': 11, 'at': [5, 5]},
                {'type': 'flash', 'polarity': 'dark', 'aperture': 12, 'at': [15, 5]},
                {'type': 'flash', 'polarity': 'dark', 'aperture': 13, 'at': [10, 15]},
                {'type': 'flash', 'polarity': 'dark', 'aperture': 14, 'at': [20, 15]},
                {'type': 'flash', 'polarity': 'dark', 'aperture': 15, 'at': [20, 5]},
                {'type': 'region', 'polarity': 'dark'},
                {'type': 'line', 'polarity': 'clear', 'aperture': 10, 'start': [3, 20], 'end': [7, 20]},
            ],
        )
        self.assertEqual(layer['counts'], {'flash': 5, 'line': 3, 'arc': 1, 'region': 1})
        self.assertArc(layer['objects'][2], [10, 10], 5, 0, 90, layer['arc_tolerance'])
        # The region: a 6 by 4 rectangle with a half disc of radius 3 on top, drawn anticlockwise.
        contour = layer['objects'][8]['points']
        self.assertEqual(contour[0], contour[-1])
        self.assertEqual(contour[:3], [[2, 18], [8, 18], [8, 22]])
        exact = 24 + math.pi * 9 / 2
        self.assertLessEqual(area(contour), exact)
        self.assertGreater(area(contour), exact - math.pi * 3 * layer['arc_tolerance'])
        # Dark objects with their apertures: the region's left edge, the lowest and rightmost pads, the dome.
        self.assertEqual(layer['bounds'], [2, 5 - 0.8, 20 + 1, 22 + 3])

    def test_a_macro_with_variables_builds_the_shape_it_describes(self):
        layer = pcb_import.gerber(fixture('top_copper.gbr'))
        rounded = {a['code']: a for a in layer['apertures']}[15]
        self.assertEqual(rounded['macro'], 'RoundRect')
        # 1.6 by 1.2 with corners of radius 0.3: two crossing rectangles and four discs.
        self.assertEqual([part['shape'] for part in rounded['parts']], ['polygon'] * 2 + ['circle'] * 4)
        self.assertSamePolygon(rounded['parts'][0]['points'], [[-0.8, -0.3], [0.8, -0.3], [0.8, 0.3], [-0.8, 0.3]])
        self.assertSamePolygon(rounded['parts'][1]['points'], [[-0.5, -0.6], [0.5, -0.6], [0.5, 0.6], [-0.5, 0.6]])
        self.assertEqual(
            [(part['x'], part['y'], part['diameter']) for part in rounded['parts'][2:]],
            [(0.5, 0.3, 0.6), (-0.5, 0.3, 0.6), (-0.5, -0.3, 0.6), (0.5, -0.3, 0.6)],
        )
        self.assertAlmostEqual(image(rounded).area, 1.6 * 1.2 - (4 - math.pi) * 0.09, 4)

    def test_each_macro_primitive_matches_its_definition(self):
        layer = pcb_import.gerber(fixture('macros.gbr'))
        self.assertEqual(layer['warnings'], [])
        parts = {a['code']: a['parts'] for a in layer['apertures']}
        # 1: a circle of diameter 0.8 at (1, 0.5), turned a quarter turn about the macro origin.
        self.assertEqual(parts[10], [{'shape': 'circle', 'exposure': True, 'diameter': 0.8, 'x': -0.5, 'y': 1}])
        # 20: a line 0.5 wide from (0, 0) to (2, 0), turned 45 degrees.
        self.assertSamePolygon(
            parts[11][0]['points'], [rotate(p, 45) for p in ([0, -0.25], [2, -0.25], [2, 0.25], [0, 0.25])]
        )
        # 21: a 2 by 1 rectangle centred on (0.5, 0.25), turned 30 degrees about the origin, not its centre.
        self.assertSamePolygon(
            parts[12][0]['points'], [rotate(p, 30) for p in ([-0.5, -0.25], [1.5, -0.25], [1.5, 0.75], [-0.5, 0.75])]
        )
        # 22 (deprecated): a 2 by 1 rectangle by its lower left corner.
        self.assertSamePolygon(parts[13][0]['points'], [[0, 0], [2, 0], [2, 1], [0, 1]])
        # 4: a triangle turned half a turn.
        self.assertSamePolygon(parts[14][0]['points'], [[0, 0], [-1, 0], [-0.5, -1]])
        # 5: a pentagon of diameter 2 about (0.5, 0), first vertex on the X axis, then turned 18 degrees.
        pentagon = [[0.5 + math.cos(math.radians(72 * k)), math.sin(math.radians(72 * k))] for k in range(5)]
        self.assertSamePolygon(parts[15][0]['points'], [rotate(p, 18) for p in pentagon])
        # 7: a ring of diameters 2 and 1.2 with a cross of width 0.3 cut out of it, turned 45 degrees.
        thermal = image({'parts': parts[16]})
        self.assertEqual([part['exposure'] for part in parts[16]], [True, False, False, False])

        def strip(radius, gap=0.3):  # the part of a disc with x > 0 and |y| < gap / 2
            return gap / 2 * math.sqrt(radius**2 - gap**2 / 4) + radius**2 * math.asin(gap / (2 * radius))

        self.assertAlmostEqual(thermal.area, math.pi * (1 - 0.36) - 4 * (strip(1) - strip(0.6)), 4)
        self.assertEqual(len(thermal.geoms), 4)  # four separate arcs of copper
        self.assertFalse(thermal.contains(Point(rotate([0.8, 0], 45))))  # the gaps lie on the turned axes
        self.assertTrue(thermal.contains(Point(0.8, 0)))
        # Exposure off cuts from what the macro has built so far: a ring with a square notch.
        self.assertAlmostEqual(image({'parts': parts[17]}).area, math.pi * (1 - 0.25) - 0.25**2, 4)
        # $4 = 1 + 2 x 3, $5 = -(1 - 2) / 2, $6 = $7 + 1 with $7 never set.
        self.assertEqual(parts[18], [{'shape': 'circle', 'exposure': True, 'diameter': 7, 'x': 0.5, 'y': 1}])
        self.assertEqual([o['at'] for o in layer['objects']][-1], [40, 10])
        self.assertEqual(layer['bounds'][2:], [40 + 0.5 + 3.5, 10 + 1 + 3.5])

    def test_macro_dimensions_follow_the_file_units(self):
        text = '%FSLAX24Y24*%\n%MOIN*%\n%AMBAR*21,1,$1,0.05,0.1,0,0*%\n%ADD10BAR,0.2*%\nD10*\nX10000Y0D03*\nM02*\n'
        layer = pcb_import.gerber(text)
        self.assertSamePolygon(
            layer['apertures'][0]['parts'][0]['points'], [[0, -0.635], [5.08, -0.635], [5.08, 0.635], [0, 0.635]]
        )
        self.assertEqual(layer['objects'][0]['at'], [25.4, 0])

    def test_an_older_inch_file_with_deprecated_codes(self):
        layer = pcb_import.gerber(fixture('legacy_inch.gbr'))
        self.assertEqual(layer['units'], 'inch')
        self.assertEqual(layer['coordinate_format']['decimal'], 4)
        self.assertIsNone(layer['file_function'])
        self.assertEqual(
            layer['warnings'], ['Coordinates without an operation code were read as D01, a deprecated usage']
        )
        self.assertEqual(layer['apertures'][0], {'code': 10, 'shape': 'circle', 'diameter': 0.254, 'hole': 0})
        self.assertEqual(
            layer['apertures'][1], {'code': 11, 'shape': 'rectangle', 'width': 1.524, 'height': 1.016, 'hole': 0}
        )
        objects = layer['objects']
        corners = [
            [inch(1000), inch(1000)],
            [inch(5000), inch(1000)],
            [inch(5000), inch(3000)],
            [inch(1000), inch(3000)],
        ]
        self.assertEqual(
            [[o['start'], o['end']] for o in objects[:3]], [list(pair) for pair in zip(corners, corners[1:])]
        )
        # Single-quadrant arcs give their centre offsets without a sign; the reader has to find the centre.
        self.assertEqual(objects[3]['centre'], [0, inch(3000)])
        self.assertFalse(objects[3]['clockwise'])
        self.assertArc(objects[3], [0, 7.62], 2.54, 0, 90, 0.005)
        self.assertEqual(objects[4]['centre'], [inch(1000), inch(4000)])
        self.assertTrue(objects[4]['clockwise'])
        self.assertArc(objects[4], [2.54, 10.16], 2.54, 180, -90, 0.005)
        # In single-quadrant mode an arc that ends where it starts has no length.
        self.assertEqual(
            summary(objects[5]), {**summary(objects[5]), 'type': 'line', 'start': [2.54, 12.7], 'end': [2.54, 12.7]}
        )
        self.assertEqual([o['at'] for o in objects[6:]], [[7.62, 5.08], [10.16, 5.08], [5.08, 5.08]])
        self.assertEqual({o['aperture'] for o in objects[6:]}, {11})
        self.assertEqual(layer['counts'], {'flash': 3, 'line': 4, 'arc': 2, 'region': 0})

    def test_trailing_zero_omission_and_incremental_coordinates(self):
        text = (
            '%FSTAX23Y23*%\n%MOMM*%\n%ADD10C,0.2*%\nD10*\nG01*\n'
            'X15Y-0125D02*\n'  # padded on the right to five digits: 15.000 and -1.250
            'X00002Y1D01*\n'  # 0.002 and 10.000
            'G91*\nX01Y005D01*\nX-002D01*\nM02*\n'  # steps of (1, 0.5) and (-0.2, 0)
        )
        layer = pcb_import.gerber(text)
        self.assertEqual(
            layer['coordinate_format'],
            {'integer': 2, 'decimal': 3, 'omitted_zeros': 'trailing', 'notation': 'absolute'},
        )
        self.assertEqual(
            [[o['start'], o['end']] for o in layer['objects']],
            [[[15, -1.25], [0.002, 10]], [[0.002, 10], [1.002, 10.5]], [[1.002, 10.5], [0.802, 10.5]]],
        )
        self.assertEqual(len(layer['warnings']), 2)  # both are deprecated, and said to be

    def test_a_full_circle_and_arc_tolerance(self):
        text = HEAD + f'%ADD10C,0.1*%\nD10*\nG75*\n{mm(30, 10)}D02*\nG02{mm(30, 10, -20, 0)}D01*\nM02*\n'
        counts = {}
        for tolerance in (0.0005, 0.005, 0.1):
            layer = pcb_import.gerber(text, arc_tolerance=tolerance)
            (circle,) = layer['objects']
            self.assertEqual(layer['arc_tolerance'], tolerance)
            self.assertArc(circle, [10, 10], 20, 0, -360, tolerance)
            counts[tolerance] = len(circle['points'])
            # No finer than it has to be: one chord fewer would break the tolerance.
            fewer = 2 * math.pi / (counts[tolerance] - 2)
            self.assertGreater(20 * (1 - math.cos(fewer / 2)), tolerance)
            self.assertEqual(layer['bounds'], [-10.05, -10.05, 30.05, 30.05])
        self.assertGreater(counts[0.0005], counts[0.005])
        self.assertGreater(counts[0.005], counts[0.1])
        tiny = HEAD + f'%ADD10C,0.1*%\nD10*\nG75*\n{mm(0.002, 0)}D02*\nG03{mm(0.002, 0, -0.002, 0)}D01*\nM02*\n'
        self.assertEqual(
            len(pcb_import.gerber(tiny)['objects'][0]['points']), 9
        )  # never fewer than eight chords a turn
        for bad in (0, 0.0001, 0.5, 'fine'):
            self.refused(lambda bad=bad: pcb_import.gerber(text, arc_tolerance=bad), 'Arc tolerance')

    def test_an_arc_whose_centre_is_slightly_off_is_blended_and_reported(self):
        text = HEAD + f'%ADD10C,0.1*%\nD10*\nG75*\n{mm(5, 0)}D02*\nG03{mm(0, 5.05, -5, 0)}D01*\nM02*\n'
        layer = pcb_import.gerber(text)
        points = layer['objects'][0]['points']
        radii = [math.hypot(*point) for point in points]
        self.assertEqual((points[0], points[-1]), ([5, 0], [0, 5.05]))
        self.assertEqual(radii, sorted(radii))  # the radius runs evenly from 5 to 5.05: no step at either end
        for k, radius in enumerate(radii):
            self.assertAlmostEqual(radius, 5 + 0.05 * k / (len(radii) - 1), 5)
        self.assertEqual(
            layer['warnings'],
            [
                '1 arc has a centre that does not match both ends, by up to 0.050 mm; the radius was run evenly from one end to the other'
            ],
        )

    def test_older_habits_that_have_one_reading_are_read_with_a_warning(self):
        text = HEAD + f'%ADD10R,1X0.5*%\nD10*\n{mm(4, 0)}D01*\nM02*\n'  # no G01, no starting position, a rectangle
        layer = pcb_import.gerber(text)
        self.assertEqual((layer['objects'][0]['start'], layer['objects'][0]['end']), ([0, 0], [4, 0]))
        self.assertEqual(layer['bounds'], [-0.5, -0.25, 4.5, 0.25])
        self.assertEqual(
            layer['warnings'],
            [
                'A line is drawn before any position is set; it starts from the origin',
                'Lines are drawn before G01 selects straight moves; straight moves were assumed',
                'Lines are drawn with a rectangular aperture, a deprecated usage',
            ],
        )
        zero = pcb_import.gerber(HEAD + '%ADD10R,0X1*%\n%ADD11C,0*%\nG70*\nM00*\n')  # M00 ends a file as M02 does
        self.assertEqual(zero['warnings'], ['Aperture D10 has no area', 'The file changes units part way through'])
        self.assertEqual(zero['units'], 'inch')

    def test_step_and_repeat_copies_the_block_along_y_then_x(self):
        layer = pcb_import.gerber(fixture('panel_sr.gbr'))
        expected = []
        for ix in range(3):
            for iy in range(2):
                dx, dy = 5 * ix, 4 * iy
                expected += [
                    {'type': 'flash', 'polarity': 'dark', 'aperture': 10, 'at': [1 + dx, 1 + dy]},
                    {'type': 'flash', 'polarity': 'clear', 'aperture': 11, 'at': [1 + dx, 1 + dy]},
                    {
                        'type': 'line',
                        'polarity': 'dark',
                        'aperture': 11,
                        'start': [1 + dx, 1 + dy],
                        'end': [3 + dx, 1 + dy],
                    },
                ]
        expected.append({'type': 'flash', 'polarity': 'dark', 'aperture': 10, 'at': [20, 0]})
        self.assertEqual(layer['objects'], expected)
        self.assertEqual(layer['bounds'], [0.5, -0.5, 20.5, 5.5])

    def test_step_and_repeat_copies_arcs_and_regions_and_closes_at_the_end_of_the_file(self):
        text = (
            HEAD + '%ADD10C,0.1*%\nD10*\nG75*\n%SRX2Y1I10J0*%\n'
            f'{mm(2, 0)}D02*\nG03{mm(0, 2, -2, 0)}D01*\n'
            f'G36*\nG01{mm(0, 0)}D02*\n{mm(1, 0)}D01*\n{mm(0, 1)}D01*\n{mm(0, 0)}D01*\nG37*\nM02*\n'
        )
        arc, region, arc_copy, region_copy = pcb_import.gerber(text)['objects']
        self.assertEqual((arc_copy['start'], arc_copy['end'], arc_copy['centre']), ([12, 0], [10, 2], [10, 0]))
        self.assertPoints(arc_copy['points'], [[x + 10, y] for x, y in arc['points']])
        self.assertEqual(region['points'], [[0, 0], [1, 0], [0, 1], [0, 0]])
        self.assertEqual(region_copy['points'], [[10, 0], [11, 0], [10, 1], [10, 0]])
        # The older way to end a block: a step-and-repeat of one.
        older = HEAD + '%ADD10C,0.1*%\nD10*\n%SRX1Y3I0J2*%\nX0Y0D03*\n%SRX1Y1I0J0*%\nX5000000Y0D03*\nM02*\n'
        self.assertEqual([o['at'] for o in pcb_import.gerber(older)['objects']], [[0, 0], [0, 2], [0, 4], [5, 0]])

    def test_regions_with_several_contours_and_clear_polarity(self):
        text = (
            HEAD + 'G01*\nG36*\n'
            f'{mm(0, 0)}D02*\n{mm(4, 0)}D01*\n{mm(4, 3)}D01*\n{mm(0, 0)}D01*\n'
            f'{mm(10, 0)}D02*\n{mm(14, 0)}D01*\n{mm(14, 3)}D01*\n{mm(10, 0)}D01*\nG37*\n'
            f'%LPC*%\nG36*\n{mm(1, 0.5)}D02*\n{mm(3, 0.5)}D01*\n{mm(3, 1)}D01*\n{mm(1, 0.5)}D01*\nG37*\nM02*\n'
        )
        layer = pcb_import.gerber(text)
        self.assertEqual(
            layer['objects'],
            [
                {'type': 'region', 'polarity': 'dark', 'points': [[0, 0], [4, 0], [4, 3], [0, 0]]},
                {'type': 'region', 'polarity': 'dark', 'points': [[10, 0], [14, 0], [14, 3], [10, 0]]},
                {'type': 'region', 'polarity': 'clear', 'points': [[1, 0.5], [3, 0.5], [3, 1], [1, 0.5]]},
            ],
        )
        self.assertEqual(layer['bounds'], [0, 0, 14, 3])  # a region needs no aperture, and clear adds nothing
        self.assertEqual(layer['apertures'], [])

    def test_attributes_written_as_comments_and_a_negative_layer(self):
        text = 'G04 #@! TF.FileFunction,Soldermask,Top*\nG04 #@! TF.FilePolarity,Negative*\n' + HEAD + 'M02*\n'
        layer = pcb_import.gerber(text)
        self.assertEqual(layer['file_function'], 'Soldermask,Top')
        self.assertIn('negative image', layer['warnings'][0])
        self.assertEqual((layer['objects'], layer['bounds']), ([], None))

    def test_bytes_that_are_not_text_are_only_tolerated_in_comments(self):
        layer = pcb_import.gerber(b'G04 caf\xe9 \xff*\n%TF.Part,caf\xc3\xa9*%\n' + HEAD.encode() + b'M02*\n')
        self.assertEqual(layer['attributes'], {'.Part': 'caf??'})  # an answer holds printable ASCII only
        marked = pcb_import.gerber('\ufeff' + fixture('panel_sr.gbr'))  # a byte order mark before the first command
        self.assertEqual(marked, pcb_import.gerber(fixture('panel_sr.gbr')))
        drill = fixture('drill_inch.drl')
        self.assertEqual(pcb_import.excellon(b'\xef\xbb\xbf' + drill.encode()), pcb_import.excellon(drill))
        self.refused(lambda: pcb_import.gerber(HEAD.encode() + b'X1\xe9D02*\nM02*\n'), 'Line 3', 'not understood')

    def test_truncated_files_are_refused(self):
        whole = fixture('top_copper.gbr')
        self.refused(
            lambda: pcb_import.gerber(whole[: whole.index('M02')]), 'no end-of-file command (M02)', 'truncated'
        )
        self.refused(lambda: pcb_import.gerber(whole[: whole.index('D01*') + 2]), 'ends in the middle of a command')
        self.refused(lambda: pcb_import.gerber(whole[: whole.index('%ADD11') + 5]), 'ends inside an extended command')
        self.refused(lambda: pcb_import.gerber(whole[: whole.index('G37')] + 'M02*\n'), 'ends inside a region')
        self.refused(lambda: pcb_import.gerber(''), 'not a Gerber RS-274X file')
        self.refused(
            lambda: pcb_import.gerber('M48\nMETRIC\nT1C0.8\n%\nT1\nX1.0Y1.0\nM30\n'), 'not a Gerber RS-274X file'
        )

    def test_what_a_file_must_state_is_not_assumed(self):
        cases = {
            '%FSLAX46Y46*%\n%ADD10C,0.1*%\nM02*\n': 'an aperture is defined before the units',
            '%FSLAX46Y46*%\nX0Y0D02*\nM02*\n': 'coordinates appear before the units',
            '%MOMM*%\nX0Y0D02*\n%FSLAX46Y46*%\nM02*\n': 'coordinates appear before the format statement',
            HEAD + 'X0Y0D03*\nM02*\n': 'a flash is made before any aperture is selected',
            HEAD + 'D15*\nM02*\n': 'aperture D15 is selected but not defined',
            HEAD + '%ADD10THING,1*%\nM02*\n': 'uses the macro THING, which is not defined',
            HEAD + '%ADD10C,0.1*%\n%ADD10C,0.2*%\nM02*\n': 'aperture D10 is defined twice',
            HEAD + '%ADD10C,0.1*%\nD10*\nX0Y0D02*\nG03X1000000Y0I500000J0D01*\nM02*\n': 'before G74 or G75',
            HEAD + '%ADD10C,0.1*%\nD10*\nX0Y0D02*\nX1000000Y0*\nM02*\n': 'without an operation code',
            HEAD
            + 'G36*\nX0Y0D02*\nG01X1000000Y0D01*\nX1000000Y1000000D01*\nG37*\nM02*\n': 'region contour is not closed',
            HEAD + '%ADD10C,0.1*%\nD10*\nG36*\nX0Y0D03*\nG37*\nM02*\n': 'a flash (D03) inside a region',
            HEAD + '%ADD10C,0.1*%\nD10*\nG75*\nX0Y0D02*\nG02X5000000Y0I1000000J0D01*\nM02*\n': 'not the same distance',
            HEAD + '%ADD10C,0.1*%\nD10*\nG75*\nX0Y0D02*\nG02X5000000Y0D01*\nM02*\n': 'not the same distance',
            HEAD
            + '%ADD10R,1X1*%\nD10*\nG75*\nX0Y0D02*\nG02X2000000Y0I1000000J0D01*\nM02*\n': 'only a circle may be used',
            HEAD + '%ADD10O,1X2*%\nD10*\nG01*\nX0Y0D02*\nX2000000Y0D01*\nM02*\n': 'only a circle or a rectangle',
        }
        for text, reason in cases.items():
            with self.subTest(reason=reason):
                self.refused(lambda text=text: pcb_import.gerber(text), 'Line ', reason)

    def test_what_is_not_supported_is_declined_by_name(self):
        cases = {
            '%ABD12*%': 'block apertures (%AB) are not supported',
            '%LMX*%': 'aperture mirroring (%LM) is not supported',
            '%LR90*%': 'aperture rotation (%LR) is not supported',
            '%LS0.5*%': 'aperture scaling (%LS) is not supported',
            '%IPNEG*%': 'a negative image (%IPNEG) is not supported',
            '%ASAYBX*%': 'swapped axes (%AS) are not supported',
            '%IR90*%': 'image rotation (%IR) is not supported',
            '%MIA1B0*%': 'image mirroring (%MI) is not supported',
            '%OFA1.5B0*%': 'an image offset (%OF) is not supported',
            '%SFA2B2*%': 'an image scale factor (%SF) is not supported',
            '%KOC*%': 'the extended command %KO is not supported',
            '%ADD10C,1X0.2X0.2*%': 'aperture D10 has a rectangular hole, which is not supported',
            '%ADD10C,1X1.5*%': 'has a hole that does not fit inside it',
            '%ADD10P,1X13*%': 'needs 3 to 12 vertices',
            '%ADD10C,5000*%': 'has a diameter outside 0 to 1000.000 mm',
            '%AMM*6,0,0,5,0.5,0.5,2,0.1,6,0*%\n%ADD10M*%': 'uses the moire primitive (6), which is not supported',
            '%AMM*9,1,1*%\n%ADD10M*%': 'uses primitive 9, which is not known',
            '%AMM*1,2,1,0,0*%\n%ADD10M*%': 'has an exposure that is neither 0 nor 1',
            '%AMM*1,1,1/($1-2),0,0*%\n%ADD10M,2*%': 'divides by zero',
            '%AMM*1,1,1,0*%\n%ADD10M*%': 'gives primitive 1 the wrong number of parameters',
            '%AMM*4,1,3,0,0,1,0,1,1,0,0.5,0*%\n%ADD10M*%': 'has an outline whose last point is not its first',
            '%AMM*7,0,0,1,2,0.1,0*%\n%ADD10M*%': 'inner diameter is not smaller than its outer',
            '%AMM*1,1,1,0,0,*%\n%ADD10M*%': 'has an expression that is not understood',
            'G02X0Y0D02*\nG12*': 'the code G12 is not supported',
            'D05*': 'the code D5 is not supported',
            'M07*': 'the code M7 is not supported',
            'N10G01X0Y0D02*': 'is not understood',
            'X1.5Y0D02*': 'a decimal point, which Gerber does not allow',
            '%SRX2Y2I1J1*%\nG36*\n%SR*%': 'step-and-repeat inside a region',
            '%FSLAX46Y46*%': None,  # the same format again is harmless
            '%FSLAX24Y24*%': 'the coordinate format changes within the file',
        }
        for command, reason in cases.items():
            with self.subTest(command=command):
                text = HEAD + command + '\nM02*\n'
                if reason is None:
                    self.assertEqual(pcb_import.gerber(text)['objects'], [])
                else:
                    self.refused(
                        lambda text=text: pcb_import.gerber(text), f'Line {3 + command.count(chr(10))}: ', reason
                    )
        self.refused(lambda: pcb_import.gerber('%FSLAX46Y36*%\n%MOMM*%\nM02*\n'), 'X and Y different digits')
        self.refused(
            lambda: pcb_import.gerber('%FSLAN2X24Y24*%\n%MOMM*%\nM02*\n'), 'format statement', 'not understood'
        )
        self.refused(lambda: pcb_import.gerber('%FSLAX24Y24*%\n%MOCM*%\nM02*\n'), 'neither MM nor IN')

    def test_absurd_coordinates_are_refused(self):
        flash = HEAD + '%ADD10C,0.1*%\nD10*\n'
        self.refused(lambda: pcb_import.gerber(flash + 'X10000000001Y0D03*\nM02*\n'), 'outside -10000 to 10000 mm')
        self.refused(lambda: pcb_import.gerber(flash + 'X0Y-99999999999999D03*\nM02*\n'), 'outside -10000 to 10000 mm')
        self.refused(lambda: pcb_import.gerber(flash + 'X12345678901234567Y0D03*\nM02*\n'), 'more than 16 digits')
        self.assertEqual(
            pcb_import.gerber(flash + 'X10000000000Y-10000000000D03*\nM02*\n')['objects'][0]['at'], [10000, -10000]
        )
        # A copy that step-and-repeat would place out of range is refused too.
        far = flash + '%SRX2Y1I6000J0*%\nX5000000000Y0D03*\n%SR*%\nM02*\n'
        self.refused(lambda: pcb_import.gerber(far), 'outside -10000 to 10000 mm')

    def test_deep_macro_nesting_is_refused_without_running_out_of_stack(self):
        def nested(depth):
            return HEAD + '%AMDEEP*1,1,' + '(' * depth + '1' + ')' * depth + ',0,0*%\n%ADD10DEEP*%\nM02*\n'

        deepest = pcb_import.gerber(nested(32))
        self.assertEqual(deepest['apertures'][0]['parts'][0]['diameter'], 1)
        self.refused(
            lambda: pcb_import.gerber(nested(33)), 'Line 4: aperture macro DEEP nests brackets more than 32 deep'
        )
        self.refused(lambda: pcb_import.gerber(nested(30000)), 'nests brackets more than 32 deep')
        signs = HEAD + '%AMSIGNS*1,1,' + '-' * 20001 + '1+2,0,0*%\n%ADD10SIGNS*%\nM02*\n'  # signs need no stack
        self.assertEqual(pcb_import.gerber(signs)['apertures'][0]['parts'][0]['diameter'], 1)
        long_macro = HEAD + '%AMLONG*' + '1,1,1,0,0*' * 7000 + '%\n%ADD10LONG*%\nM02*\n'
        self.refused(lambda: pcb_import.gerber(long_macro), 'aperture macro LONG is longer than 65536 characters')
        many = HEAD + '%AMMANY*' + '1,1,1,0,0*' * 2001 + '%\n%ADD10MANY*%\nM02*\n'
        self.refused(lambda: pcb_import.gerber(many), 'makes more than 2000 shapes')
        overflow = HEAD + '%AMBIG*$1=99999x99999*$1=$1x$1*1,1,$1,0,0*%\n%ADD10BIG*%\nM02*\n'
        self.refused(lambda: pcb_import.gerber(overflow), 'computes a value that is out of range')

    def test_a_huge_step_and_repeat_is_refused_before_it_is_expanded(self):
        block = '%ADD10C,0.1*%\nD10*\n' + 'X0Y0D03*\n' * 11
        self.refused(
            lambda: pcb_import.gerber(HEAD + block.replace('D10*\n', 'D10*\n%SRX100Y100I1J1*%\n') + 'M02*\n'),
            'more than 100000 objects',
        )
        self.refused(
            lambda: pcb_import.gerber(HEAD + '%SRX100000Y2I1J1*%\nM02*\n'), 'more than 1000 copies along an axis'
        )
        self.refused(lambda: pcb_import.gerber(HEAD + '%SRX2.5Y2I1J1*%\nM02*\n'), 'whole numbers of at least 1')
        # 1000 by 1000 copies of an empty block cost nothing, and 1000 by 100 of one flash just fit.
        self.assertEqual(pcb_import.gerber(HEAD + '%SRX1000Y1000I1J1*%\n%SR*%\nM02*\n')['objects'], [])
        full = HEAD + '%ADD10C,0.1*%\nD10*\n%SRX1000Y100I1J1*%\nX0Y0D03*\n%SR*%\nM02*\n'
        self.assertEqual(pcb_import.gerber(full)['counts']['flash'], 100000)
        arcs = (
            HEAD
            + '%ADD10C,0.1*%\nD10*\nG75*\n%SRX1000Y1000I1J1*%\nX1000000Y0D02*\nG03X1000000Y0I-1000000J0D01*\nM02*\n'
        )
        self.refused(lambda: pcb_import.gerber(arcs), 'more than 100000 objects')
        points = arcs.replace('%SRX1000Y1000', '%SRX1000Y50')
        self.refused(lambda: pcb_import.gerber(points), 'Step-and-repeat would need more than 500000 points')

    def test_limits_on_size_and_counts(self):
        self.refused(
            lambda: pcb_import.gerber('G04 ' + ' ' * (16 << 20) + '*\n' + HEAD + 'M02*\n'), 'larger than 16 MiB'
        )
        flashes = HEAD + '%ADD10C,0.1*%\nD10*\n' + 'X0Y0D03*\n' * 100001 + 'M02*\n'
        self.refused(lambda: pcb_import.gerber(flashes), 'draws more than 100000 objects')
        circles = (
            HEAD
            + '%ADD10C,0.1*%\nD10*\nG75*\nX9000000000Y0D02*\n'
            + 'G03X9000000000Y0I-9000000000J0D01*\n' * 80
            + 'M02*\n'
        )
        self.refused(
            lambda: pcb_import.gerber(circles, arc_tolerance=0.0005), 'more than 500000 points', 'coarser arc tolerance'
        )
        apertures = HEAD + ''.join(f'%ADD{code}C,0.1*%\n' for code in range(10, 10011)) + 'M02*\n'
        self.refused(lambda: pcb_import.gerber(apertures), 'defines more than 10000 apertures')
        self.refused(
            lambda: pcb_import.gerber(HEAD + 'X' + '1' * 5000 + 'D02*\nM02*\n'),
            'a command is longer than 4096 characters',
        )
        self.refused(
            lambda: pcb_import.gerber(HEAD + '%ADD4294967306C,0.1*%\nM02*\n'),
            'aperture numbers run from D10 to D2147483647',
        )

    def test_requests_are_validated(self):
        self.refused(lambda: pcb_import.gerber(12), 'Give the board file as text')
        self.refused(lambda: pcb_import._call('gerber', 'M02*', {'arc_tolerance': float('nan')}), 'cannot be sent')
        self.refused(lambda: pcb_import._core_call('gerber', '[]'), 'The Gerber request must be an object')
        self.refused(lambda: pcb_import._core_call('gerber', '{"text": 5}'), 'needs the file as "text"')
        self.refused(lambda: pcb_import._core_call('gerber', '{"text":'), 'not valid JSON')
        self.refused(
            lambda: pcb_import._core_call('gerber', '{"text": "M02*", "arc_tolerence": 0.1}'),
            'The Gerber request has a field that is not known: arc_tolerence',
        )
        self.refused(
            lambda: pcb_import._core_call('excellon', '{"text": "M30", "offset": [1, 1]}'), 'not known: offset'
        )
        self.refused(lambda: pcb_import._core_call('gerber-panel', '{}'), 'Unknown command: gerber-panel')


@unittest.skipUnless(HAS_CORE, 'The compiled extension with the native core is not installed')
class OutlineTests(Base):
    def test_pieces_in_any_order_and_direction_are_joined_into_closed_paths(self):
        outline = pcb_import.outline_paths(fixture('edge_cuts.gbr'))
        self.assertEqual(len(outline['paths']), 3)
        for path in outline['paths']:
            self.assertEqual(path[0], path[-1])
        board, hole, square = outline['paths']  # largest first
        # 40 by 30 with corners of radius 3; a flattened arc cuts each corner slightly short.
        exact = 40 * 30 - (4 - math.pi) * 9
        self.assertLessEqual(abs(area(board)), exact + 0.004 * 24)  # one end is 0.004 mm off over a 24 mm side
        self.assertGreater(abs(area(board)), exact - 2 * math.pi * 3 * 0.005 - 0.004 * 24)
        self.assertGreater(abs(area(hole)), math.pi * 25 - 2 * math.pi * 5 * 0.005)
        self.assertLess(abs(area(hole)), math.pi * 25)
        for point in hole:
            self.assertAlmostEqual(math.dist(point, [120, -95]), 5, 5)
        self.assertSamePolygon(square, [[108, -100], [114, -100], [114, -104], [108, -104]])
        self.assertEqual(outline['bounds'], [100, -110, 140, -80])
        self.assertEqual(outline['line_width'], 0.1)
        self.assertEqual(
            outline['warnings'],
            [
                '1 repeated line was left out of the outline',
                'The outline has 3 closed loops; the largest is first and the others may be cut-outs inside it',
            ],
        )
        # Every corner of the board is on the path, whichever way its pieces were drawn.
        for corner in ([103, -110], [137, -110], [140, -107], [140, -83], [137, -80], [103, -80], [100, -107]):
            self.assertIn(corner, board)

    def test_the_outline_is_a_cut_out_job_the_generator_accepts(self):
        placed = pcb_import.outline_paths(fixture('edge_cuts.gbr'))
        offset = pcb_import.placement([placed['bounds']], margin=5)
        self.assertEqual(offset, [-95, 115])
        outline = pcb_import.outline_paths(fixture('edge_cuts.gbr'), offset=offset, width=60, height=40)
        self.assertEqual(outline['bounds'], [5, 5, 45, 35])
        self.assertPoints(outline['paths'][2], [[x - 95, y + 115] for x, y in placed['paths'][2]])
        job = generate({**SETTINGS, 'paths': outline['paths'], 'tool_diameter': 1})
        self.assertEqual(job['bounds'], [5, 5, 45, 35])
        self.assertAlmostEqual(
            job['cut_length'] / math.ceil(1.8 / 0.5), 2 * 34 + 2 * 24 + 6 * math.pi + 10 * math.pi + 20, 1
        )
        self.refused(
            lambda: pcb_import.outline_paths(fixture('edge_cuts.gbr'), offset=offset, width=40, height=40),
            'does not fit the work area with this offset',
            'X5.000 to X45.000',
        )
        self.refused(lambda: pcb_import.outline_paths(fixture('edge_cuts.gbr'), width=40), 'both the work width')
        self.refused(
            lambda: pcb_import.outline_paths(fixture('edge_cuts.gbr'), offset=[1]), 'Give the offset as [X, Y]'
        )
        self.refused(
            lambda: pcb_import.outline_paths(fixture('edge_cuts.gbr'), offset=[0, 1e6]), 'Offset Y must be between'
        )

    def test_an_open_outline_is_an_error_that_names_the_place(self):
        # The left side starts 0.004 mm from the corner arc: joined at the default 0.01 mm, open at 0.001 mm.
        self.refused(
            lambda: pcb_import.outline_paths(fixture('edge_cuts.gbr'), tolerance=0.001),
            'The board outline is open at X100.00',
            'the nearest other loose end is 0.004 mm away',
        )
        open_square = (
            HEAD
            + f'%ADD10C,0.1*%\nD10*\nG01*\n{mm(0, 0)}D02*\n{mm(10, 0)}D01*\n{mm(10, 10)}D01*\n{mm(0, 10)}D01*\nM02*\n'
        )
        self.refused(lambda: pcb_import.outline_paths(open_square), 'open at X0.000 Y', 'loose end is 10.000 mm away')
        closed = open_square.replace('M02*', f'{mm(0, 0)}D01*\nM02*')
        self.assertEqual(pcb_import.outline_paths(closed)['paths'], [[[0, 0], [10, 0], [10, 10], [0, 10], [0, 0]]])
        branch = closed.replace('M02*', f'{mm(10, 10)}D02*\n{mm(15, 15)}D01*\nM02*')
        self.refused(
            lambda: pcb_import.outline_paths(branch),
            'The board outline branches at X10.000 Y10.000: 3 lines meet there',
        )
        for bad in (0, 2, 'wide'):
            self.refused(lambda bad=bad: pcb_import.outline_paths(closed, tolerance=bad), 'Joining tolerance')

    def test_what_is_not_an_outline_is_left_out_and_said(self):
        text = (
            HEAD + '%ADD10C,0.15*%\n%ADD11C,1*%\nD10*\nG01*\n'
            f'{mm(0, 0)}D02*\n{mm(10, 0)}D01*\n{mm(10, 5)}D01*\n{mm(0, 0)}D01*\n{mm(0, 0)}D01*\n'
            f'D11*\n{mm(5, 2)}D03*\n'
            f'G36*\n{mm(20, 0)}D02*\n{mm(21, 0)}D01*\n{mm(21, 1)}D01*\n{mm(20, 0)}D01*\nG37*\n'
            f'%LPC*%\nD10*\n{mm(30, 0)}D02*\n{mm(31, 0)}D01*\nM02*\n'
        )
        outline = pcb_import.outline_paths(text)
        self.assertEqual(outline['paths'], [[[0, 0], [10, 0], [10, 5], [0, 0]]])
        self.assertEqual(outline['line_width'], 0.15)
        self.assertEqual(
            outline['warnings'],
            [
                '1 flash was left out of the outline',
                '1 filled region was left out of the outline',
                '1 clear line was left out of the outline',
                '1 line with no length was left out of the outline',
            ],
        )
        self.refused(
            lambda: pcb_import.outline_paths(HEAD + '%ADD11C,1*%\nD11*\nX0Y0D03*\nM02*\n'), 'holds no lines or arcs'
        )
        self.refused(lambda: pcb_import.outline_paths('not a board'), 'not a Gerber RS-274X file')

    def test_two_arcs_make_a_circle_and_tolerance_follows_the_request(self):
        halves = (
            HEAD
            + f'%ADD10C,0.1*%\nD10*\nG75*\n{mm(4, 0)}D02*\nG03{mm(-4, 0, -4, 0)}D01*\n{mm(-4, 0)}D02*\nG02{mm(4, 0, 4, 0)}D01*\nM02*\n'
        )
        coarse = pcb_import.outline_paths(halves, arc_tolerance=0.1)
        fine = pcb_import.outline_paths(halves, arc_tolerance=0.001)
        self.assertEqual(len(coarse['paths']), 1)
        self.assertGreater(len(fine['paths'][0]), len(coarse['paths'][0]))
        for point in fine['paths'][0]:
            self.assertAlmostEqual(math.dist(point, [0, 0]), 4, 5)
            self.assertGreaterEqual(point[1], -1e-9)  # both halves run over the top: the second was drawn clockwise
        self.assertEqual(fine['warnings'], ['1 repeated line was left out of the outline'] * 0)


@unittest.skipUnless(HAS_CORE, 'The compiled extension with the native core is not installed')
class ExcellonTests(Base):
    def test_decimal_coordinates_with_tools_slots_and_attributes(self):
        drill = pcb_import.excellon(fixture('drill_decimal.drl'))
        self.assertEqual(drill['format'], 'excellon')
        self.assertEqual(drill['units'], 'mm')
        self.assertEqual(
            drill['interpretation'],
            {
                'units': 'mm',
                'units_from': 'header',
                'integer': None,
                'decimal': None,
                'omitted_zeros': None,
                'digits_from': 'decimal points',
                'zeros_from': 'decimal points',
            },
        )
        self.assertTrue(drill['plated'])
        self.assertEqual(drill['file_function'], 'Plated,1,2,PTH')
        self.assertEqual(
            drill['tools'],
            [
                {'number': 1, 'diameter': 0.4, 'hits': [[105, -85], [110.5, -85.25]]},
                {'number': 2, 'diameter': 0.8, 'hits': [[120, -90], [122.54, -90], [125.08, -90]]},  # Y carries over
                {'number': 3, 'diameter': 1, 'hits': []},
                {'number': 4, 'diameter': 0.8, 'hits': [[104, -106]]},
            ],
        )
        self.assertEqual(
            drill['slots'],
            [
                {'tool': 3, 'diameter': 1, 'points': [[130, -100], [134, -100]]},  # routed: G00, M15, G01, M16
                {'tool': 3, 'diameter': 1, 'points': [[112, -104], [112, -100]]},  # G85
            ],
        )
        self.assertEqual(drill['counts'], {'hits': 6, 'slots': 2})
        # The hole edges: T4's hole on the left and at the bottom, the routed slot's end on the right, T1 at the top.
        self.assertEqual(drill['bounds'], [104 - 0.4, -106 - 0.4, 134 + 0.5, -85 + 0.2])
        self.assertEqual(drill['warnings'], [])

    def test_inches_with_leading_zeros_omitted_and_repeats(self):
        drill = pcb_import.excellon(fixture('drill_inch.drl'))
        self.assertEqual(
            drill['interpretation'],
            {
                'units': 'inch',
                'units_from': 'header',
                'integer': 2,
                'decimal': 4,
                'omitted_zeros': 'leading',
                'digits_from': 'comment',
                'zeros_from': 'header',
            },
        )
        first, second = drill['tools']
        self.assertEqual((first['diameter'], second['diameter']), (round(0.032 * 25.4, 6), round(0.04 * 25.4, 6)))
        row = [[inch(-2500 + 1000 * k), inch(12345)] for k in range(4)]  # the hole, then R3X1000
        self.assertPoints(first['hits'], [[inch(10000), inch(5000)]] + row)
        column = [[inch(20000), inch(-15000 - 500 * k)] for k in range(3)]  # the hole, then R2Y-500
        self.assertPoints(second['hits'], column + [[inch(20250), inch(-15750)]])  # then R1X250Y250
        self.assertEqual(drill['warnings'], [])
        self.assertIsNone(drill['plated'])

    def test_leading_zeros_kept_and_trailing_zeros_omitted(self):
        drill = pcb_import.excellon(fixture('drill_leading.drl'))
        self.assertEqual(drill['interpretation']['omitted_zeros'], 'trailing')
        self.assertEqual((drill['interpretation']['integer'], drill['interpretation']['decimal']), (3, 3))
        self.assertEqual(drill['interpretation']['digits_from'], 'header')
        self.assertFalse(drill['plated'])
        # 3.3 digits from the left: 0105 is 010.5, 00625 is 006.25, 012 is 012, 034567 is 034.567, -01 is -010.
        self.assertEqual(
            drill['tools'],
            [
                {'number': 1, 'diameter': 3.2, 'hits': [[10.5, -8.5], [110.5, 6.25]]},
                {'number': 2, 'diameter': 0.9, 'hits': [[12, 34.567], [0.5, -10]]},
            ],
        )
        self.assertEqual(drill['warnings'], [])
        long = fixture('drill_leading.drl').replace('X012Y034567', 'X012Y0345678')
        self.refused(lambda: pcb_import.excellon(long), 'Line 13', 'more digits than the 3.3 format allows')

    def test_an_unstated_format_is_reported_not_silently_assumed(self):
        text = fixture('drill_unstated.drl')
        drill = pcb_import.excellon(text)
        self.assertEqual(
            drill['interpretation'],
            {
                'units': 'inch',
                'units_from': 'header',  # M72
                'integer': 2,
                'decimal': 4,
                'omitted_zeros': 'trailing',
                'digits_from': 'default',
                'zeros_from': 'default',
            },
        )
        self.assertEqual(len(drill['warnings']), 2)
        self.assertIn('does not state its number format', drill['warnings'][0])
        self.assertIn('"integer" and "decimal"', drill['warnings'][0])
        self.assertIn('does not say which zeros', drill['warnings'][1])
        self.assertIn('"omitted_zeros"', drill['warnings'][1])
        # The default reading: digits from the left of a 2.4 number, so 5200 is 52 inches.
        self.assertPoints(drill['tools'][0]['hits'], [[52 * 25.4, 34 * 25.4], [15.2 * 25.4, 34 * 25.4]])
        # Stated outright, the same file reads as the half-inch grid it was drawn on, and one warning goes.
        stated = pcb_import.excellon(text, omitted_zeros='leading')
        self.assertPoints(stated['tools'][0]['hits'], [[inch(5200), inch(3400)], [inch(15200), inch(3400)]])
        self.assertPoints(stated['tools'][1]['hits'], [[25.4, 25.4]])
        self.assertEqual(stated['interpretation']['zeros_from'], 'request')
        self.assertEqual(len(stated['warnings']), 1)
        full = pcb_import.excellon(text, omitted_zeros='leading', integer=2, decimal=3, units='mm')
        self.assertEqual(full['tools'][0], {'number': 1, 'diameter': 0.024, 'hits': [[5.2, 3.4], [15.2, 3.4]]})
        self.assertEqual(full['interpretation']['units_from'], 'request')
        self.assertEqual(full['warnings'], ['The request\'s units differ from those the file states'])

    def test_units_are_never_assumed(self):
        text = 'M48\nT1C0.8\n%\nT1\nX1.5Y2.5\nM30\n'
        self.refused(lambda: pcb_import.excellon(text), 'does not state its units', 'give "units" in the request')
        self.refused(lambda: pcb_import.excellon('M48\nT1C0.8\n%\nM30\n'), 'does not state its units')
        self.assertEqual(
            pcb_import.excellon(text, units='mm')['tools'][0], {'number': 1, 'diameter': 0.8, 'hits': [[1.5, 2.5]]}
        )
        self.assertEqual(pcb_import.excellon(text, units='inch')['tools'][0]['hits'], [[38.1, 63.5]])
        self.assertEqual(pcb_import.excellon('M48\nM71\n' + text[4:])['interpretation']['units'], 'mm')

    def test_format_statements_of_several_design_programs(self):
        body = '%\nT1\nX012345Y-001\nM30\n'
        cases = [
            ('METRIC,TZ\n;FILE_FORMAT=4:2\n', [123.45, -0.01]),
            (';FILE_FORMAT=2:4\nINCH,TZ\n', [inch(12345), inch(-1)]),
            ('METRIC,LZ,0000.00\n', [123.45, -10]),
            ('METRIC,LZ\n; FORMAT={3:3/ absolute / metric / suppress trailing zeros}\n', [12.345, -1]),
            ('METRIC,000.000\n; format={3:3/ absolute / metric / keep zeros}\n', [12.345, -1]),
            ('INCH,LZ\n', [1.2345 * 25.4, -0.1 * 25.4]),
        ]
        for header, expected in cases:
            with self.subTest(header=header):
                drill = pcb_import.excellon('M48\n' + header + 'T1C0.5\n' + body)
                self.assertPoints(drill['tools'][0]['hits'], [expected])
        # A header and a comment that disagree: the header is used, and the answer says so.
        mixed = pcb_import.excellon('M48\nMETRIC,TZ,000.000\n;FILE_FORMAT=4:2\nT1C0.5\n' + body)
        self.assertPoints(mixed['tools'][0]['hits'], [[12.345, -0.001]])
        self.assertEqual(
            mixed['warnings'], ['A comment and the header give different number formats; the header\'s was used']
        )
        # Only the zeros matter where every number has all its digits.
        whole = pcb_import.excellon('M48\nMETRIC,000.000\nT1C0.5\n%\nT1\nX012345Y-001000\nM30\n')
        self.assertEqual((whole['tools'][0]['hits'], whole['warnings']), ([[12.345, -1]], []))

    def test_tools_defined_where_they_are_first_used(self):
        drill = pcb_import.excellon(
            'INCH,TZ\nT01C0.020\nX10000Y10000\nT02C0.040F200S65\nX20000Y10000\nT01\nX30000Y10000\nM30\n'
        )
        self.assertEqual([tool['number'] for tool in drill['tools']], [1, 2])
        self.assertPoints(drill['tools'][0]['hits'], [[25.4, 25.4], [76.2, 25.4]])
        self.assertPoints(drill['tools'][1]['hits'], [[50.8, 25.4]])

    def test_what_is_not_supported_is_declined_by_name(self):
        head = 'M48\nMETRIC\nT1C0.8\n%\nT1\nX1.0Y1.0\n'
        cases = {
            'T5\nX2.0Y2.0': 'Line 7 selects tool T5, which is not defined',
            'T0\nX2.0Y2.0': 'Line 8 drills before a tool is selected',
            'G91': 'selects incremental coordinates (G91), which are not supported',
            'G00X1.0Y1.0\nM15\nG02X2.0Y2.0A1.0': 'is not understood',
            'G02X2.0Y2.0': 'routes an arc (G02, G03), which is not supported',
            'G41': 'uses cutter compensation (G41, G42), which is not supported',
            'G81': 'uses a canned cycle (G81 to G84), which is not supported',
            'G93X1.0Y1.0': 'sets a zero offset (G93), which is not supported',
            'G12': 'uses the code G12, which is not supported',
            'M25': 'uses pattern and step-and-repeat codes',
            'M02X5.0Y0': 'uses pattern and step-and-repeat codes',
            'M97,TEXT': 'is not understood',
            'M98': 'drills text (M97, M98), which is not supported',
            '%': 'repeats a pattern (% in the body), which is not supported',
            'G01X2.0Y2.0': 'routes (G01) without the tool down (M15)',
            'M15': 'lowers the routing tool (M15) outside routing mode (G00)',
            'G00X2.0Y2.0\nM15\nG00X3.0Y3.0': 'makes a rapid move (G00) with the routing tool down',
            'G00X2.0Y2.0\nM15\nG05': 'returns to drilling with the routing tool down',
            'G00X2.0Y2.0\nM15\nT1': 'changes the tool while routing',
            'G00X2.0Y2.0\nM15\nG01X3.0Y3.0': 'ends with the routing tool still down',
            'G00X2.0Y2.0\nX2.0Y3.0G85X4.0Y3.0': 'drills a slot (G85) in routing mode',
            'X99999.0Y0': 'has a coordinate outside -10000 to 10000 mm',
            'X1e3Y0': 'is not understood',
            'X1.2.3Y0': 'has a number that is not understood',
            'X1.0X2.0': 'gives X twice',
            'R0X1.0': 'repeats a hole a number of times outside 1 to 10000',
            'R20000X1.0': 'repeats a hole a number of times outside 1 to 10000',
            'R5': 'repeats a hole (R) without a step',
            'T1\nR5X1.0': 'repeats a hole (R) with no hole before it',
            'R9000X2.0': 'has a coordinate outside -10000 to 10000 mm',
            'T1C0.9': 'defines tool T1 a second time with another diameter',
            'T12345': 'names a tool that is not understood',
            'T2C0': 'defines tool T2 with no diameter',
            'HELLO': 'is not understood',
            'INCH': 'changes the units after coordinates have been read',
            'X' + '1' * 1100: 'is longer than 1024 characters',
        }
        for lines, reason in cases.items():
            with self.subTest(lines=lines[:40]):
                self.refused(lambda lines=lines: pcb_import.excellon(head + lines + '\nM30\n'), reason)
        self.refused(
            lambda: pcb_import.excellon('M48\nFMAT,1\nMETRIC\n%\nM30\n'), 'Line 2 selects a format other than FMAT,2'
        )
        self.refused(lambda: pcb_import.excellon('M48\nICI,ON\nMETRIC\n%\nM30\n'), 'incremental coordinates (ICI)')
        self.refused(
            lambda: pcb_import.excellon('M48\nMETRIC\nT1C200\n%\nM30\n'), 'Tool T1 has a diameter of 200.000 mm'
        )
        self.refused(
            lambda: pcb_import.excellon(head.replace('METRIC', 'METRIC,TZ') + 'X15Y15\nMETRIC,LZ\nM30\n'),
            'changes the zero suppression',
        )
        self.refused(lambda: pcb_import.excellon(head + 'X15Y15\n;FILE_FORMAT=4:2\nM30\n'), 'changes the number format')

    def test_harmless_lines_warnings_and_limits(self):
        text = (
            'M48\nVER,1\nFMAT,2\nDETECT,ON\nATC,ON\nICI,OFF\nFROB,7\nMETRIC,TZ,BANANA\nT1C0.8\nT2\n%\n'
            'G90\nG05\nM47,Change the drill\nF100\nS500\nT1\nX1000Y2000\nM06\n'
        )
        drill = pcb_import.excellon(text)
        self.assertEqual(drill['tools'], [{'number': 1, 'diameter': 0.8, 'hits': [[1, 2]]}])
        self.assertEqual(
            drill['warnings'],
            [
                'Header line not understood and left out: FROB,7',
                'Header word not understood and left out: BANANA',
                'Tool T2 is named in the header without a diameter',
                'The file has no end-of-program code (M30); it may be truncated',
                'The file does not state its number format; coordinates without a decimal point were read with 3 digits '
                'before the point and 3 after. State "integer" and "decimal" in the request if this is wrong',
            ],
        )
        head = 'M48\nMETRIC\nT1C0.8\n%\nT1\n'
        self.assertEqual(pcb_import.excellon(head + 'X0Y0\n' * 100000 + 'M30\n')['counts']['hits'], 100000)
        self.refused(lambda: pcb_import.excellon(head + 'X0.0Y0.0\n' * 100001 + 'M30\n'), 'more than 100000 holes')
        self.refused(
            lambda: pcb_import.excellon(head + 'X0.0Y0.0\n' + 'R10000X0.01\n' * 11 + 'M30\n'), 'more than 100000 holes'
        )
        self.refused(
            lambda: pcb_import.excellon(head + 'X0.0Y0.0G85X1.0Y0.0\n' * 20001 + 'M30\n'), 'more than 20000 slots'
        )
        tools = 'M48\nMETRIC\n' + ''.join(f'T{n}C0.5\n' for n in range(1, 1002)) + '%\nM30\n'
        self.refused(lambda: pcb_import.excellon(tools), 'defines more than 1000 tools')
        self.refused(lambda: pcb_import.excellon(';' + ' ' * (8 << 20) + '\nM30\n'), 'larger than 8 MiB')
        self.assertEqual(pcb_import.excellon('')['tools'], [])  # nothing to drill, and nothing wrong
        for options, reason in (
            ({'units': 'cm'}, 'Units must be "mm" or "inch"'),
            ({'integer': 3}, 'Give both "integer" and "decimal" digits'),
            ({'integer': 3, 'decimal': 2.5}, 'must be whole numbers'),
            ({'integer': 9, 'decimal': 2}, 'Integer digits must be between 0 and 7'),
            ({'omitted_zeros': 'none'}, 'Omitted zeros must be "leading" or "trailing"'),
        ):
            self.refused(lambda options=options: pcb_import.excellon('M30\n', **options), reason)

    def test_holes_become_drilling_operations_grouped_by_diameter(self):
        text = fixture('drill_decimal.drl')
        offset = pcb_import.placement([pcb_import.excellon(text)['bounds']], margin=3.6)
        self.assertPoints([offset], [[-100, 110]])
        drills = pcb_import.drill_operations(text, offset=[-100, 110], width=60, height=40)
        marker = lambda x, y: [[x, y], [x, y]]  # a path that stays where it starts is a hole to drill
        self.assertEqual(
            drills['operations'],
            [
                {
                    'operation': 'drill',
                    'tool_shape': 'drill',
                    'tool_diameter': 0.4,
                    'paths': [marker(5, 25), marker(10.5, 24.75)],
                },
                {
                    'operation': 'drill',
                    'tool_shape': 'drill',
                    'tool_diameter': 0.8,
                    'paths': [marker(20, 20), marker(22.54, 20), marker(25.08, 20), marker(4, 4)],
                },
            ],
        )
        self.assertEqual(
            drills['groups'],
            [{'tool_diameter': 0.4, 'holes': 2, 'tools': [1]}, {'tool_diameter': 0.8, 'holes': 4, 'tools': [2, 4]}],
        )
        self.assertEqual(
            drills['slots'],
            [
                {'tool': 3, 'diameter': 1, 'points': [[30, 10], [34, 10]]},
                {'tool': 3, 'diameter': 1, 'points': [[12, 6], [12, 10]]},
            ],
        )
        self.assertEqual(drills['bounds'], [3.6, 3.6, 25.48, 25.2])  # of the holes; the slots are not drilled
        self.assertEqual(len(drills['warnings']), 2)
        self.assertIn('2 slots are not drilled', drills['warnings'][0])
        self.assertIn('2 drill diameters', drills['warnings'][1])
        # Each operation is a job the generator accepts, and it drills exactly these holes with this drill.
        for operation, expected in zip(
            drills['operations'], ([[5, 25], [10.5, 24.75]], [[20, 20], [22.54, 20], [25.08, 20], [4, 4]])
        ):
            job = generate({**SETTINGS, **operation})
            self.assertEqual(job['drill_holes'], [[x, y, operation['tool_diameter']] for x, y in expected])
            self.assertEqual(job['simulation_tool']['diameter'], operation['tool_diameter'])
            self.assertEqual(sum(line.startswith('G1 Z-1.800') for line in job['lines']), len(expected))
        # One job has one tool, so the two diameters cannot share an ordered job.
        self.refused(
            lambda: generate({**SETTINGS, 'operations': drills['operations']}), 'Tool identity or geometry differs'
        )

    def test_drill_conversion_checks_the_fit_and_splits_large_groups(self):
        text = fixture('drill_decimal.drl')
        self.refused(
            lambda: pcb_import.drill_operations(text, offset=[-100, 110], width=25, height=40),
            'The holes do not fit the work area with this offset',
            'X3.600 to X25.480',
        )
        self.refused(lambda: pcb_import.drill_operations(text, width=60, height=40), 'do not fit the work area')
        self.refused(
            lambda: pcb_import.drill_operations('M48\nMETRIC\nT1C0.8\n%\nM30\n'), 'The drill file holds no holes'
        )
        self.refused(
            lambda: pcb_import.drill_operations('M48\nMETRIC\nT1C0.005\n%\nT1\nX1.0Y1.0\nM30\n'), 'finer than 0.01 mm'
        )
        # 3500 holes of one diameter: two operations, which may share an ordered job because the drill is the same.
        grid = [[1 + 0.5 * (k % 70), 1 + 0.5 * (k // 70)] for k in range(3500)]
        many = 'M48\nMETRIC\nT1C0.3\n%\nT1\n' + ''.join(f'X{x}Y{y}\n' for x, y in grid) + 'M30\n'
        drills = pcb_import.drill_operations(many)
        self.assertEqual([len(operation['paths']) for operation in drills['operations']], [3000, 500])
        self.assertEqual([path[0] for operation in drills['operations'] for path in operation['paths']], grid)
        self.assertEqual(drills['groups'], [{'tool_diameter': 0.3, 'holes': 3500, 'tools': [1]}])
        job = generate({**SETTINGS, 'depth': 0.5, 'operations': drills['operations']})
        self.assertEqual(len(job['drill_holes']), 3500)
        # The number format can be stated here as it can when importing.
        stated = pcb_import.drill_operations(fixture('drill_unstated.drl'), omitted_zeros='leading')
        self.assertPoints(stated['operations'][0]['paths'][0], [[inch(5200), inch(3400)]] * 2)
        self.assertEqual(stated['interpretation']['zeros_from'], 'request')


@unittest.skipUnless(HAS_CORE, 'The compiled extension with the native core is not installed')
class DamagedFileTests(Base):
    """Fixtures with random damage: every one is read or refused with a reason, and none takes long."""

    ALPHABET = '0123456789XYIJDGM*%,.-+$=()xRTC\n ALPSOFNB/'

    def damaged(self, rng, text):
        characters = list(text)
        for _ in range(rng.randint(1, 6)):
            kind, at = rng.randint(0, 5), rng.randrange(len(characters) + 1)
            if kind == 0 and at < len(characters):
                characters[at] = rng.choice(self.ALPHABET)
            elif kind == 1:
                characters.insert(at, rng.choice(self.ALPHABET))
            elif kind == 2:
                del characters[at : at + rng.randint(1, 20)]
            elif kind == 3:
                source = rng.randrange(len(characters) + 1)
                characters[at:at] = characters[source : source + rng.randint(1, 40)]
            elif kind == 4:
                characters.insert(at, rng.choice(['0', '99999999999', '-1', '1e30', '1' * 30]))
            else:
                del characters[at:]
        return ''.join(characters)

    def test_damaged_files_are_read_or_refused(self):
        rng = random.Random(20261121)
        gerbers = [path.read_text() for path in sorted(FIXTURES.glob('*.gbr'))]
        drills = [path.read_text() for path in sorted(FIXTURES.glob('*.drl'))]
        self.assertEqual((len(gerbers), len(drills)), (11, 5))
        outcomes = {'read': 0, 'refused': 0}
        for _ in range(400):
            for texts, calls in (
                (gerbers, (pcb_import.gerber, pcb_import.outline_paths, lambda text: pcb_import.isolation(text, 0.2))),
                (drills, (pcb_import.excellon, pcb_import.drill_operations)),
            ):
                text = self.damaged(rng, rng.choice(texts))
                for call in calls:
                    try:
                        self.assertIsInstance(call(text), dict)
                        outcomes['read'] += 1
                    except ValueError as error:  # anything else, a RuntimeError included, fails the test
                        self.assertGreater(len(str(error)), 10)
                        self.assertNotIn('valid JSON', str(error))
                        outcomes['refused'] += 1
        self.assertGreater(outcomes['read'], 100)
        self.assertGreater(outcomes['refused'], 100)


class WrapperTests(Base):
    def test_without_the_extension_every_call_says_so(self):
        with mock.patch.object(pcb_import, '_core_call', None):
            self.assertFalse(pcb_import.available())
            for call in (pcb_import.gerber, pcb_import.excellon, pcb_import.drill_operations, pcb_import.outline_paths):
                self.refused(lambda call=call: call('M02*'), 'needs the compiled KerfKite extension')

    def test_an_extension_older_than_the_importers_is_not_available(self):
        def older(command, request):
            raise ValueError('Unknown command: ' + command)

        with mock.patch.object(pcb_import, '_core_call', older):
            self.assertFalse(pcb_import.available())

    def test_reading_a_file_is_bounded(self):
        self.assertIn('%TF.FileFunction,Profile,NP*%', fixture('edge_cuts.gbr'))
        self.refused(lambda: pcb_import.read(FIXTURES / 'edge_cuts.gbr', limit=100), 'The file is larger than 0 MiB')

    def test_placement_brings_the_lowest_corner_to_the_margin(self):
        self.assertEqual(
            pcb_import.placement([[100, -110, 140, -80], None, [103.6, -106.4, 134.5, -84.8]], margin=5), [-95, 115]
        )
        self.assertEqual(pcb_import.placement([[2, 3, 4, 5]]), [-2, -3])
        self.refused(lambda: pcb_import.placement([None]), 'There is nothing to place')


@unittest.skipUnless(pcb_import.available(), 'The compiled extension with the native core is not installed')
class LongNumberTests(unittest.TestCase):
    """Numbers with far more digits than a dimension needs are read, not run past the scale table."""

    def test_many_leading_fraction_zeros(self):
        tiny = '0.' + '0' * 60 + '7'
        drill = f'M48\nMETRIC\nT1C0.8\n%\nT1\nX12.5{"0" * 40}Y{tiny}\nX3.{"0" * 21}25Y1.{"9" * 30}\nM30\n'
        tool = pcb_import.excellon(drill)['tools'][0]
        self.assertEqual(tool['hits'], [[12.5, 0.0], [3.0, 2.0]])
        gerber = f'%FSLAX46Y46*%\n%MOMM*%\n%ADD10C,0.{"0" * 30}1*%\n%ADD11C,0.25{"0" * 50}*%\nD11*\nX1000000Y2000000D03*\nM02*\n'
        layer = pcb_import.gerber(gerber)
        self.assertEqual([aperture['diameter'] for aperture in layer['apertures']], [0.0, 0.25])
        self.assertEqual(layer['objects'][0]['at'], [1.0, 2.0])


if __name__ == '__main__':
    unittest.main()
