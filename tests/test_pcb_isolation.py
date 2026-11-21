"""Isolation toolpaths through the native core, checked against Shapely as an independent oracle.

The copper image is rebuilt here from the importer's list of objects with Shapely, shape by shape, and compared
with the image the core returns. The contours are then measured against the copper: every point at the stated
distance, none nearer. Which islands a contour separates is worked out here from the contours themselves and
compared with the places the core reports as unreachable. The fixtures in tests/pcb are written by hand.
"""

import math
import unittest
from pathlib import Path

from shapely import affinity
from shapely.geometry import LineString, MultiPoint, Point, Polygon, box
from shapely.ops import unary_union
from shapely.validation import make_valid

from studio import pcb_import
from studio.cam import generate

HAS_CORE = pcb_import.isolation_available()
FIXTURES = Path(__file__).parent / 'pcb'
HEAD = '%FSLAX46Y46*%\n%MOMM*%\n%LPD*%\nG01*\nG75*\n'
ARC = 0.005  # the default arc tolerance: round shapes lie outside the true curve by no more
JOIN = 0.001  # the allowance of a contour's round corners
GRID = 2e-6  # a few nanometres of rounding
SETTINGS = dict(mode='router', protocol='grbl', width=60, height=40, feed=200, power=12000, depth=0.1, step=0.1)


def fixture(name):
    return pcb_import.read(FIXTURES / name)


def mm(*values):
    return ''.join(f'{letter}{round(value * 1e6)}' for letter, value in zip('XYIJ', values))


def polygonal(shape):
    """The polygons of a geometry, without the lines and points make_valid may add."""
    parts = [g for g in getattr(shape, 'geoms', [shape]) if g.geom_type in ('Polygon', 'MultiPolygon')]
    return unary_union(parts) if parts else Polygon()


def aperture_shape(aperture):
    """What an aperture draws at the origin, from its definition."""
    kind = aperture['shape']
    if kind == 'circle':
        shape = Point(0, 0).buffer(aperture['diameter'] / 2, quad_segs=128)
    elif kind == 'rectangle':
        shape = box(-aperture['width'] / 2, -aperture['height'] / 2, aperture['width'] / 2, aperture['height'] / 2)
    elif kind == 'obround':
        w, h = aperture['width'], aperture['height']
        r = min(w, h) / 2
        ends = [(-(w / 2 - r), -(h / 2 - r)), (w / 2 - r, h / 2 - r)]
        shape = (LineString(ends) if ends[0] != ends[1] else Point(0, 0)).buffer(r, quad_segs=128)
    elif kind == 'polygon':
        r, n = aperture['diameter'] / 2, aperture['vertices']
        turn = [math.radians(aperture['rotation'] + 360 * k / n) for k in range(n)]
        shape = Polygon([(r * math.cos(a), r * math.sin(a)) for a in turn])
    else:
        shape = Polygon()
        for part in aperture['parts']:
            if part['shape'] == 'circle':
                piece = Point(part['x'], part['y']).buffer(part['diameter'] / 2, quad_segs=128)
            else:
                piece = polygonal(make_valid(Polygon(part['points'])))
            shape = shape.union(piece) if part['exposure'] else shape.difference(piece)
        return shape
    hole = aperture.get('hole', 0)
    return shape.difference(Point(0, 0).buffer(hole / 2, quad_segs=128)) if hole else shape


def oracle_copper(layer, offset=(0, 0), mirror_x=None):
    """The copper image of an imported layer: each object's shape, dark and clear in file order."""
    apertures = {a['code']: a for a in layer['apertures']}
    stamps = {}
    image = Polygon()
    pending, pending_dark = [], True

    def settle():
        nonlocal image, pending
        if pending:
            shapes = unary_union(pending)
            image = image.union(shapes) if pending_dark else image.difference(shapes)
            pending = []

    for obj in layer['objects']:
        if obj['type'] == 'region':
            shape = polygonal(make_valid(Polygon(obj['points'])))
        else:
            aperture = apertures[obj['aperture']]
            if obj['aperture'] not in stamps:
                stamps[obj['aperture']] = aperture_shape(aperture)
            stamp = stamps[obj['aperture']]
            if stamp.is_empty:
                continue
            if obj['type'] == 'flash':
                shape = affinity.translate(stamp, *obj['at'])
            elif obj['type'] == 'line' and obj['start'] == obj['end']:
                shape = affinity.translate(stamp, *obj['start'])
            elif obj['type'] == 'line' and aperture['shape'] == 'rectangle':
                ends = [affinity.translate(stamp, *obj[end]) for end in ('start', 'end')]
                shape = MultiPoint([p for end in ends for p in end.exterior.coords]).convex_hull
            else:
                path = [obj['start'], obj['end']] if obj['type'] == 'line' else obj['points']
                shape = LineString(path).buffer(aperture['diameter'] / 2, quad_segs=128)
        dark = obj['polarity'] == 'dark'
        if dark != pending_dark:
            settle()
        pending_dark = dark
        pending.append(shape)
    settle()
    image = affinity.translate(image, *offset)
    if mirror_x is not None:
        image = affinity.scale(image, xfact=-1, origin=(mirror_x, 0))
    return image


def core_copper(answer):
    return [Polygon(island['shell'], island['holes']) for island in answer['copper']]


def passes_of(answer):
    """[(pass number, distance, paths)] in cutting order."""
    paths = pcb_import.isolation_paths(answer)
    out, first = [], 0
    for entry in answer['cut']:
        out.append((entry['pass'], entry['distance'], paths[first : first + entry['paths']]))
        first += entry['paths']
    assert first == len(paths) == answer['path_count']
    return out


def region_of_rings(rings):
    """The region closed contours bound: each ring flips what lies inside it."""
    region = Polygon()
    for ring in sorted(rings, key=lambda r: -Polygon(r).area):
        region = region.symmetric_difference(Polygon(ring))
    return region


def groups_by_contour(answer, islands):
    """Which islands the first contour encloses together, from the contours alone: {frozenset of islands}."""
    first = next(paths for number, _, paths in passes_of(answer) if number == 1)
    grown = region_of_rings(first)
    pieces = list(getattr(grown, 'geoms', [grown]))
    home = {}
    for index, island in enumerate(islands):
        inside = [k for k, piece in enumerate(pieces) if piece.contains(island.representative_point())]
        assert len(inside) == 1, (index, inside)
        home.setdefault(inside[0], set()).add(index)
    return {frozenset(members) for members in home.values()}


def groups_by_places(answer, count):
    parent = list(range(count))

    def find(i):
        while parent[i] != i:
            i = parent[i]
        return i

    for place in answer['unreachable']:
        a, b = (find(i) for i in place['islands'])
        parent[b] = a
    sets = {}
    for i in range(count):
        sets.setdefault(find(i), set()).add(i)
    return {frozenset(members) for members in sets.values()}


@unittest.skipUnless(HAS_CORE, 'the compiled extension with isolation is not installed')
class Base(unittest.TestCase):
    def check(self, text, tool_width, expect_islands=None, expect_places=None, **options):
        """Every property an isolation answer has to have, against the oracle. Returns the answer."""
        answer = pcb_import.isolation(text, tool_width, **options)
        tolerance = options.get('arc_tolerance') or ARC
        layer = pcb_import.gerber(text, arc_tolerance=options.get('arc_tolerance'))

        # The copper image is the oracle's, to the allowance of round shapes and the oracle's own chords.
        oracle = oracle_copper(layer, options.get('offset') or (0, 0), options.get('mirror_x'))
        islands = core_copper(answer)
        copper = unary_union(islands)
        for island in islands:
            self.assertTrue(island.is_valid)
        self.assertTrue(copper.buffer(-1e-4).within(oracle.buffer(tolerance + 1e-4)), 'copper beyond the oracle')
        # A round clear shape is drawn just outside its true curve as well, so copper beside it may be short.
        self.assertTrue(oracle.buffer(-1e-4).within(copper.buffer(tolerance + 1e-4)), 'copper short of the oracle')
        self.assertLess(copper.symmetric_difference(oracle).area, oracle.length * (tolerance + 2e-4))
        self.assertEqual(answer['islands'], len(islands))
        self.assertEqual(len(getattr(oracle, 'geoms', [oracle])), len(islands))
        if expect_islands is not None:
            self.assertEqual(len(islands), expect_islands)
        for got, want in zip(answer['copper_bounds'], copper.bounds):
            self.assertAlmostEqual(got, want, 6)

        # Every contour of pass k lies tool_width / 2 + k * step from the copper: never nearer, and further only
        # by the allowance of a round corner.
        step = round(tool_width * (1 - answer['overlap']), 6)
        self.assertEqual(answer['step'], step)
        seen = []
        for number, distance, paths in passes_of(answer):
            self.assertAlmostEqual(distance, tool_width / 2 + (number - 1) * step, 6)
            seen.append(number)
            for path in paths:
                if not answer['clipped']['cut']:
                    self.assertEqual(path[0], path[-1])
                line = LineString(path)
                self.assertGreaterEqual(line.distance(copper), distance - GRID)
                self.assertGreaterEqual(line.distance(oracle), distance - tolerance - 1e-4)
                for point in path:
                    self.assertLessEqual(Point(point).distance(copper), distance + JOIN + GRID)
                # Mid-edge points too: an edge cannot cut a corner.
                for a, b in zip(path, path[1:]):
                    middle = Point((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
                    self.assertLessEqual(middle.distance(copper), distance + JOIN + GRID)
        self.assertEqual(sorted(seen), list(range(1, answer['passes'] + 1)))
        self.assertEqual(seen, sorted(seen, reverse=answer['order'] == 'outer_first'))

        # Islands are separated by the first contour unless a reported place joins them, and every reported
        # place is a true one: its two islands share a contour and are as near as it says.
        self.assertEqual(answer['unreachable_count'], len(answer['unreachable']))
        if not answer['clipped']['cut'] and not answer['clipped']['dropped']:
            self.assertEqual(groups_by_contour(answer, islands), groups_by_places(answer, len(islands)))
        gaps = []
        for place in answer['unreachable']:
            first, second = (islands[i] for i in place['islands'])
            self.assertAlmostEqual(first.distance(second), place['gap'], 5)
            self.assertLessEqual(place['gap'], tool_width + 2 * JOIN + 2e-5)
            a, b = place['between']
            self.assertAlmostEqual(math.dist(a, b), place['gap'], 5)
            self.assertLess(Point(a).distance(first), 1e-5)
            self.assertLess(Point(b).distance(second), 1e-5)
            self.assertAlmostEqual(place['at'][0], (a[0] + b[0]) / 2, 5)
            gaps.append(place['gap'])
        # The oracle's own view: islands clearly nearer than the tool are reported, those clearly apart are not.
        pieces = list(getattr(oracle, 'geoms', [oracle]))
        near = sum(
            1
            for i, p in enumerate(pieces)
            for q in pieces[i + 1 :]
            if p.distance(q) < tool_width - 2 * tolerance - 1e-3
        )
        self.assertGreaterEqual(len(gaps), near)
        if gaps:
            self.assertEqual(answer['smallest_gap'], min(gaps))
            self.assertTrue(any('cannot pass between' in w and 'leave them joined' in w for w in answer['warnings']))
        else:
            self.assertIsNone(answer['smallest_gap'])
            self.assertFalse(any('cannot pass' in w for w in answer['warnings']))
        if expect_places is not None:
            self.assertEqual(len(gaps), expect_places)

        # Direction: copper on the right of travel for climb milling with a clockwise spindle.
        climb = answer['direction'] == 'climb'
        for _, distance, paths in passes_of(answer):
            for path in paths:
                (ax, ay), (bx, by) = max(zip(path, path[1:]), key=lambda edge: math.dist(*edge))
                length = math.dist((ax, ay), (bx, by))
                reach = min(distance / 2, 0.01)
                nx, ny = (by - ay) / length * reach, -(bx - ax) / length * reach  # to the right of travel
                middle = ((ax + bx) / 2, (ay + by) / 2)
                right = Point(middle[0] + nx, middle[1] + ny).distance(copper)
                left = Point(middle[0] - nx, middle[1] - ny).distance(copper)
                self.assertEqual(right < left, climb)

        points = [p for path in pcb_import.isolation_paths(answer) for p in path]
        self.assertEqual(answer['point_count'], len(points))
        self.assertEqual(answer['bounds'][:2], [min(p[0] for p in points), min(p[1] for p in points)])
        self.assertEqual(answer['bounds'][2:], [max(p[0] for p in points), max(p[1] for p in points)])
        for operation in answer['operations']:
            self.assertEqual(operation['operation'], 'line')
            self.assertLessEqual(len(operation['paths']), 3000)
            self.assertLessEqual(sum(map(len, operation['paths'])), 100000)
        return answer


class Fixtures(Base):
    def test_known_spacings(self):
        text = fixture('iso_spacing.gbr')
        answer = self.check(text, 0.2, expect_islands=17, expect_places=4)
        gaps = sorted(place['gap'] for place in answer['unreachable'])
        for got, want in zip(gaps, [0.15, 0.15, 0.2, 0.2]):
            self.assertAlmostEqual(got, want, 6)
        places = sorted(place['at'] for place in answer['unreachable'])
        self.assertAlmostEqual(places[0][0], 2.575, 6)  # midway between the two nearest round pads,
        self.assertLess(abs(places[0][1] - 2.0), 0.1)  # somewhere along the flat edges that face each other
        self.assertAlmostEqual(next(p for p in places if abs(p[1] - 5) < 0.5)[0], 2.7, 6)  # the rectangular pads
        self.assertEqual(answer['separating_tool_width'], 0.147)
        self.assertIn('the smallest gap is 0.150 mm', answer['warnings'][-1])
        self.assertIn('no wider than 0.147 mm', answer['warnings'][-1])
        # The width it names does separate everything, and a gap as wide as the tool exactly is not cut.
        self.check(text, answer['separating_tool_width'], expect_places=0)
        self.check(text, 0.199, expect_places=2)
        self.check(text, 0.26, expect_places=5)

    def test_ground_pour_with_clearances_and_thermals(self):
        text = fixture('iso_pour.gbr')
        answer = self.check(text, 0.2, expect_islands=11, expect_places=0, passes=2)
        self.assertEqual(answer['warnings'], [])
        # The pad on spokes is part of the pour: no island of its own.
        pour = max(core_copper(answer), key=lambda island: island.area)
        self.assertTrue(pour.contains(Point(5, 9)))
        self.assertTrue(pour.contains(Point(4.1, 9)))  # on a spoke
        self.assertFalse(pour.contains(Point(5.75, 9.75)))  # in a quarter of the relief
        # A tool wider than the 0.40 mm clearances cannot isolate the three pads or the thermal's quarters.
        wide = self.check(text, 0.45, expect_places=7)
        # Round shapes are many-sided, so a gap between two of them is within the arc tolerance of the design's.
        self.assertLess(abs(wide['smallest_gap'] - 0.4), ARC)
        self.assertEqual(wide['separating_tool_width'], math.floor((wide['smallest_gap'] - 0.00201) * 1000) / 1000)
        self.check(text, wide['separating_tool_width'], expect_places=0)

    def test_first_fixture_and_macros(self):
        self.check(fixture('top_copper.gbr'), 0.2, expect_islands=4, expect_places=0, passes=3, overlap=0.5)
        self.check(fixture('macros.gbr'), 0.1, passes=2)
        self.check(fixture('macros.gbr'), 0.3)
        self.check(fixture('legacy_inch.gbr'), 0.2)

    def test_panel(self):
        answer = self.check(fixture('iso_panel.gbr'), 0.2, expect_islands=19, expect_places=0)
        self.assertEqual(answer['path_count'], 19)
        self.check(fixture('panel_sr.gbr'), 0.15, passes=2)

    def test_two_sided_board(self):
        top = fixture('board_top.gbr')
        self.check(top, 0.2, expect_places=0)
        tight = self.check(top, 0.3, expect_places=2)  # the track between two header pads has 0.295 mm either side
        self.assertAlmostEqual(tight['smallest_gap'], 0.295, 6)
        self.check(fixture('board_bottom.gbr'), 0.2, expect_places=0, mirror_x=120.0)


class Options(Base):
    def test_offset_moves_every_coordinate_exactly(self):
        text = fixture('iso_pour.gbr')
        plain = pcb_import.isolation(text, 0.2, passes=2)
        moved = self.check(text, 0.2, passes=2, offset=[7.25, -3.125])
        self.assertEqual(moved['offset'], [7.25, -3.125])
        for a, b in zip(pcb_import.isolation_paths(plain), pcb_import.isolation_paths(moved)):
            self.assertEqual([[round(x + 7.25, 6), round(y - 3.125, 6)] for x, y in a], b)
        self.assertEqual(len(plain['copper']), len(moved['copper']))

    def test_mirror_turns_the_board_over_about_the_stated_x(self):
        text = fixture('top_copper.gbr')
        plain = pcb_import.isolation(text, 0.2)
        mirrored = self.check(text, 0.2, offset=[10, 0], mirror_x=25.0)
        self.assertEqual(mirrored['mirror_x'], 25.0)
        # The copper is the exact mirror image: X becomes 2 * 25 - (X + 10).
        before = unary_union(core_copper(plain))
        after = unary_union(core_copper(mirrored))
        expected = affinity.scale(affinity.translate(before, 10, 0), xfact=-1, origin=(25, 0))
        self.assertLess(after.symmetric_difference(expected).area, 1e-9)
        self.assertAlmostEqual(mirrored['copper_bounds'][0], 50 - (plain['copper_bounds'][2] + 10), 6)
        self.assertAlmostEqual(mirrored['copper_bounds'][2], 50 - (plain['copper_bounds'][0] + 10), 6)
        # The outline and the holes take the same mirror, so the sides register.
        outline = pcb_import.outline_paths(fixture('edge_cuts.gbr'), offset=[-95, 115])
        turned = pcb_import.outline_paths(fixture('edge_cuts.gbr'), offset=[-95, 115], mirror_x=25.0)
        self.assertEqual(
            [[round(50 - x, 6), y] for x, y in outline['paths'][0]], [[x, y] for x, y in turned['paths'][0]]
        )
        self.assertEqual(turned['bounds'], [5.0, 5.0, 45.0, 35.0])
        drills = pcb_import.drill_operations(fixture('drill_decimal.drl'), offset=[-95, 115])
        under = pcb_import.drill_operations(fixture('drill_decimal.drl'), offset=[-95, 115], mirror_x=25.0)
        for a, b in zip(drills['operations'], under['operations']):
            self.assertEqual([[round(50 - p[0][0], 6), p[0][1]] for p in a['paths']], [p[0] for p in b['paths']])
        self.assertEqual([[round(50 - x, 6), y] for x, y in drills['slots'][0]['points']], under['slots'][0]['points'])

    def test_direction_and_order(self):
        text = fixture('top_copper.gbr')
        climb = self.check(text, 0.2, passes=2)
        self.assertEqual((climb['direction'], climb['order']), ('climb', 'outer_first'))
        self.assertEqual([entry['pass'] for entry in climb['cut']], [2, 1])
        conventional = self.check(text, 0.2, passes=2, direction='conventional', order='inner_first')
        self.assertEqual([entry['pass'] for entry in conventional['cut']], [1, 2])
        forward = sorted([tuple(p) for p in path] for path in pcb_import.isolation_paths(climb))
        backward = sorted([tuple(p) for p in reversed(path)] for path in pcb_import.isolation_paths(conventional))
        self.assertEqual(forward, backward)  # the same contours, each run the other way

    def test_clip_to_the_board_outline(self):
        text = fixture('iso_pour.gbr')
        whole = pcb_import.isolation(text, 0.2)
        outline = [
            [[0.5, 0.5], [22, 0.5], [22, 13.05], [0.5, 13.05], [0.5, 0.5]],
            [[8, 3], [10, 3], [10, 5], [8, 5], [8, 3]],
        ]
        clip = Polygon(outline[0], [outline[1]])
        answer = self.check(text, 0.2, clip=outline)
        self.assertGreater(answer['clipped']['cut'], 0)
        self.assertGreater(answer['clipped']['dropped'], 0)  # the region beside the pour is outside the outline
        self.assertTrue(any('cut short at the clip outline' in w for w in answer['warnings']))
        lines = unary_union([LineString(path) for path in pcb_import.isolation_paths(whole)])
        kept = unary_union([LineString(path) for path in pcb_import.isolation_paths(answer)])
        self.assertAlmostEqual(kept.length, lines.intersection(clip).length, 4)
        self.assertTrue(kept.within(clip.buffer(1e-6)))
        self.assertLess(kept.hausdorff_distance(lines.intersection(clip)), 1e-5)
        # An outline that holds everything changes nothing.
        roomy = pcb_import.isolation(text, 0.2, clip=[[[-5, -5], [40, -5], [40, 20], [-5, 20]]])
        self.assertEqual(roomy['clipped'], {'cut': 0, 'dropped': 0})
        self.assertEqual(roomy['operations'], whole['operations'])
        with self.assertRaisesRegex(ValueError, 'No isolation contour lies inside the clip outline'):
            pcb_import.isolation(text, 0.2, clip=[[[50, 50], [51, 50], [51, 51]]])

    def test_copper_touching_at_a_point_is_reported(self):
        # One contour that touches itself: two squares that meet at a corner.
        corners = [(1, 1), (3, 1), (3, 3), (5, 3), (5, 5), (3, 5), (3, 3), (1, 3), (1, 1)]
        text = (
            HEAD
            + 'G36*\n'
            + mm(*corners[0])
            + 'D02*\n'
            + ''.join(mm(*p) + 'D01*\n' for p in corners[1:])
            + 'G37*\nM02*\n'
        )
        answer = self.check(text, 0.2, expect_islands=2, expect_places=1)
        self.assertEqual(answer['unreachable'][0]['gap'], 0)
        self.assertEqual(answer['unreachable'][0]['at'], [3.0, 3.0])
        self.assertIsNone(answer['separating_tool_width'])
        self.assertIn('No tool this program accepts is narrow enough', answer['warnings'][-1])

    def test_clear_polarity_is_applied_in_file_order(self):
        pad = '%ADD10C,2.000000*%\n%ADD11C,1.000000*%\n'
        # Dark, then clear over it, then dark again inside the hole: a ring and a dot.
        text = HEAD + pad + 'D10*\n' + mm(5, 5) + 'D03*\n%LPC*%\nD11*\n' + mm(5, 5) + 'D03*\n%LPD*%\n'
        ring = self.check(text + 'M02*\n', 0.2, expect_islands=1)
        self.assertEqual(len(ring['copper'][0]['holes']), 1)
        text += '%ADD12C,0.400000*%\nD12*\n' + mm(5, 5) + 'D03*\n'
        self.check(text + 'M02*\n', 0.2, expect_islands=2, expect_places=0)
        self.check(text + 'M02*\n', 0.35, expect_islands=2, expect_places=1)  # the gap is 0.30 mm
        # Clear before anything is drawn removes nothing.
        first = HEAD + pad + '%LPC*%\nD10*\n' + mm(5, 5) + 'D03*\n%LPD*%\nD11*\n' + mm(5, 5) + 'D03*\nM02*\n'
        self.assertAlmostEqual(unary_union(core_copper(self.check(first, 0.2))).area, math.pi * 0.25, 2)

    def test_objects_without_size_add_no_copper(self):
        text = HEAD + '%ADD10C,0*%\n%ADD11C,1.000000*%\nD10*\n' + mm(1, 1) + 'D03*\n' + mm(1, 1) + 'D02*\n'
        text += mm(4, 1) + 'D01*\nD11*\n' + mm(2, 5) + 'D03*\nM02*\n'
        answer = self.check(text, 0.2, expect_islands=1)
        self.assertIn('2 objects are drawn with an aperture of no size and add no copper', answer['warnings'])

    def test_arc_tolerance_sets_the_allowance(self):
        fine = self.check(fixture('top_copper.gbr'), 0.2, arc_tolerance=0.0005)
        coarse = self.check(fixture('top_copper.gbr'), 0.2, arc_tolerance=0.05)
        self.assertGreater(fine['point_count'], coarse['point_count'])


class Requests(Base):
    PAD = HEAD + '%ADD10C,1.000000*%\nD10*\n' + mm(5, 5) + 'D03*\nM02*\n'

    def refused(self, message, text=None, tool_width=0.2, **options):
        with self.assertRaisesRegex(ValueError, message):
            pcb_import.isolation(self.PAD if text is None else text, tool_width, **options)

    def test_limits_and_messages(self):
        self.refused('Tool width must be between 0.01 and 10', tool_width=0.005)
        self.refused('Tool width must be between 0.01 and 10', tool_width=11)
        self.refused('Tool width must be a number', tool_width=[1])
        self.refused('Isolation passes must be between 1 and 50', passes=0)
        self.refused('Isolation passes must be between 1 and 50', passes=51)
        self.refused('Isolation passes must be a whole number', passes=1.5)
        self.refused('Overlap must be between 0 and 0.9', overlap=0.95)
        self.refused('Overlap must be between 0 and 0.9', overlap=-0.1)
        self.refused('Direction must be "climb" or "conventional"', direction='up')
        self.refused('Order must be "outer_first" or "inner_first"', order='random')
        self.refused('Arc tolerance must be between 0.0005 and 0.1', arc_tolerance=1)
        self.refused('Give the offset as \\[X, Y\\]', offset=[1])
        self.refused('Mirror X must be between -10000 and 10000', mirror_x=1e6)
        self.refused('Give both the work width and the work height', width=100)
        self.refused('Give the clip as 1 to 3000 closed paths', clip=[])
        self.refused('Each clip path needs at least three', clip=[[[0, 0], [1, 1]]])
        self.refused('Clip X must be a number', clip=[[[None, 0], [1, 1], [2, 0]]])
        self.refused('The clip paths enclose no area', clip=[[[0, 0], [1, 1], [2, 2]]])
        self.refused('The layer draws no copper', text=HEAD + 'M02*\n')
        self.refused('no end-of-file command', text=HEAD)  # the importer's own refusals pass through
        with self.assertRaisesRegex(ValueError, 'Give "tool_width"'):
            pcb_import._call('gerber-isolation', self.PAD, {})
        with self.assertRaisesRegex(ValueError, 'a field that is not known: tool_diameter'):
            pcb_import._call('gerber-isolation', self.PAD, {'tool_width': 0.2, 'tool_diameter': 1})
        with self.assertRaisesRegex(ValueError, 'isolation request needs the file as "text"'):
            pcb_import._core_call('gerber-isolation', '{"tool_width": 0.2}')
        self.refused('larger than 16 MiB', text='G04 ' + 'x' * (16 << 20))

    def test_a_negative_image_is_refused(self):
        text = '%TF.FilePolarity,Negative*%\n' + self.PAD
        self.refused('negative image', text=text)

    def test_fit_is_checked_against_the_work_area(self):
        answer = pcb_import.isolation(self.PAD, 0.2, width=10, height=10)
        self.assertEqual(answer['bounds'], [4.4, 4.4, 5.6, 5.6])
        self.refused(
            'do not fit the work area with this offset: they span X-0.600 to X0.600',
            offset=[-5, 0],
            width=10,
            height=10,
        )
        self.refused('do not fit the work area', width=5.5, height=10)

    def test_passes_widen_the_isolation(self):
        answer = pcb_import.isolation(self.PAD, 0.2, passes=3, overlap=0.25)
        self.assertEqual(answer['step'], 0.15)
        self.assertEqual([(e['pass'], e['distance']) for e in answer['cut']], [(3, 0.4), (2, 0.25), (1, 0.1)])
        radii = [math.dist(path[0], (5, 5)) for path in pcb_import.isolation_paths(answer)]
        for radius, want in zip(radii, [0.9, 0.75, 0.6]):
            self.assertGreaterEqual(radius, want - 1e-6)
            self.assertLess(radius, want + ARC + JOIN + 1e-3)

    def test_v_bit_width(self):
        self.assertAlmostEqual(pcb_import.v_bit_width(0.1, 30, 0.1), 0.1 + 0.2 * math.tan(math.radians(15)), 6)
        self.assertEqual(pcb_import.v_bit_width(0.2, 90, 0.05), 0.3)
        self.assertEqual(pcb_import.v_bit_width(0, 60, 0.1), round(0.2 * math.tan(math.radians(30)), 6))
        for bad in [
            (0.1, 30, 0),
            (0.1, 180, 0.1),
            (-1, 30, 0.1),
            ('0.1', 30, 0.1),
            (0.1, float('nan'), 0.1),
            (True, 30, 0.1),
        ]:
            with self.assertRaises(ValueError):
                pcb_import.v_bit_width(*bad)

    def test_the_answer_is_the_same_every_time(self):
        text = fixture('iso_pour.gbr')
        self.assertEqual(pcb_import.isolation(text, 0.2, passes=2), pcb_import.isolation(text, 0.2, passes=2))


class Generator(Base):
    def test_operations_generate_as_router_line_jobs(self):
        outline = pcb_import.outline_paths(fixture('edge_cuts.gbr'))
        offset = pcb_import.placement([outline['bounds']], margin=5)
        answer = pcb_import.isolation(fixture('board_top.gbr'), 0.2, passes=2, offset=offset, width=60, height=40)
        self.assertEqual(answer['unreachable'], [])
        job = generate({**SETTINGS, 'tool_shape': 'v', 'operations': answer['operations']})
        cutting = [line for line in job['lines'] if line.startswith('G1') and 'Z' in line]
        self.assertTrue(cutting)
        self.assertEqual({line.split('Z')[1].split()[0] for line in cutting}, {'-0.100'})  # the engraving depth
        self.assertEqual(job['operations'], len(answer['operations']))
        for got, want in zip(job['bounds'], answer['bounds']):
            self.assertAlmostEqual(got, want, 3)
        # Many small contours are divided into operations the generator accepts.
        pads = ''.join(mm(1 + 0.9 * (k % 70), 1 + 0.9 * (k // 70)) + 'D03*\n' for k in range(3500))
        many = pcb_import.isolation(HEAD + '%ADD10C,0.500000*%\nD10*\n' + pads + 'M02*\n', 0.2)
        self.assertEqual(sum(len(op['paths']) for op in many['operations']), 3500)
        self.assertGreater(len(many['operations']), 1)
        self.assertEqual(many['islands'], 3500)
        self.assertEqual(many['unreachable_count'], 0)

    def test_travel_between_contours_is_short(self):
        # A row of pads is cut in order along the row, not in the order the file draws them.
        order = [0, 5, 2, 7, 1, 6, 3, 4]
        pads = ''.join(mm(2 + 3 * k, 2) + 'D03*\n' for k in order)
        answer = pcb_import.isolation(HEAD + '%ADD10C,1.000000*%\nD10*\n' + pads + 'M02*\n', 0.2)
        starts = [path[0][0] for path in pcb_import.isolation_paths(answer)]
        self.assertEqual(starts, sorted(starts))


if __name__ == '__main__':
    unittest.main()
