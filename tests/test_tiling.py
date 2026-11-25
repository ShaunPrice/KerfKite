"""Tiling an oversized job through `kerfkite-cam tile`, checked by following the programs here.

A tile's program is read back by a small interpreter written for the test: it tracks the tool, and collects every
cutting move with the depth at its ends. Moved back by the tile's origin, the cuts of all tiles must be the cuts of
the whole job, no more and no less; within a tile the tool must never travel across the work below the clearance,
never leave the tile, and never start a cut from somewhere it is not.
"""

import json
import math
import sys
import time
import unittest
import urllib.error

from kerfkite_core import CLI, ROOT, call, generate

sys.path.insert(0, str(ROOT))
from validation import native_servers  # noqa: E402 - after the path of the checkout

try:
    STUDIO = native_servers.program('kerfkite-studio', None, 'KERFKITE_STUDIO')
except SystemExit:
    STUDIO = None

NOT_BUILT = 'kerfkite-cam is not built: set KERFKITE_CAM or build native/ into build/native'


def follow(lines, test=None, box=None):
    """The cutting moves of a program as ((x0, y0, z0), (x1, y1, z1)), with the checks a tile must pass."""
    x = y = z = None
    clear = None
    cuts = []
    for line in lines:
        words = line.split()
        if words[0] not in ('G0', 'G1'):
            continue
        at = {w[0]: float(w[1:]) for w in words[1:] if w[0] in 'XYZ'}
        nx, ny, nz = at.get('X', x), at.get('Y', y), at.get('Z', z)
        across = 'X' in at or 'Y' in at
        if words[0] == 'G0':
            if 'Z' in at:
                clear = nz
            elif test:
                test.assertIsNotNone(clear, line)
                test.assertGreaterEqual(z, clear - 1e-9, f'a rapid across the work at Z{z}: {line}')
        elif across:
            if test:
                test.assertIsNotNone(x, f'a cut from nowhere: {line}')
            cuts.append(((x, y, z), (nx, ny, nz)))
        if test and box and across:
            test.assertTrue(-1e-6 <= nx <= box[0] + 1e-6 and -1e-6 <= ny <= box[1] + 1e-6, f'outside the tile: {line}')
        x, y, z = nx, ny, nz
    return cuts


def length(cuts):
    return sum(math.dist(a[:2], b[:2]) for a, b in cuts)


def on_a_cut(piece, whole):
    """Whether a piece of cutting lies along one of the whole job's cuts, at the depth that cut has there."""
    (ax, ay, az), (bx, by, bz) = piece
    for (px, py, pz), (qx, qy, qz) in whole:
        span = math.dist((px, py), (qx, qy))
        if span < 1e-9:
            continue
        ok = True
        for x, y, z in ((ax, ay, az), (bx, by, bz)):
            t = ((x - px) * (qx - px) + (y - py) * (qy - py)) / span**2
            near = math.dist((x, y), (px + t * (qx - px), py + t * (qy - py))) < 2e-3
            ok = ok and -1e-6 <= t <= 1 + 1e-6 and near and abs(z - (pz + t * (qz - pz))) < 2e-3
        if ok:
            return True
    return False


@unittest.skipUnless(CLI, NOT_BUILT)
class TilingTests(unittest.TestCase):
    def job(self, **changes):
        # A sign 500 × 260 mm: a border, a diagonal brace that crosses tiles at an angle, and a ramped cut.
        project = dict(
            mode='router', protocol='grbl', width=600, height=300, feed=900, plunge=200, power=12000, safe=5, depth=1.5, step=1.5,
            paths=[
                [[20, 20], [520, 20], [520, 280], [20, 280], [20, 20]],
                [[40, 40], [500, 260]],
                [[60, 250], [300, 250], [300, 60]],
            ],
            path_depths=[[-1.5] * 5, [-1.5, -1.5], [-0.5, -1.5, -1.0]],
        )  # fmt: skip
        project.update(changes)
        return generate(project)

    def check(self, job, width, height):
        answer = call('tile', {'lines': job['lines'], 'tile_width': width, 'tile_height': height})
        whole = follow(job['lines'])
        total = 0
        for tile in answer['tiles']:
            cuts = follow(tile['lines'], self, (width, height))
            self.assertAlmostEqual(length(cuts), tile['cut_length'], delta=0.01 + 0.002 * len(cuts))
            ox, oy = tile['origin']
            self.assertEqual((ox, oy), (tile['column'] * width, tile['row'] * height))
            for a, b in cuts:
                piece = ((a[0] + ox, a[1] + oy, a[2]), (b[0] + ox, b[1] + oy, b[2]))
                self.assertTrue(
                    on_a_cut(piece, whole), f'tile {tile["column"]},{tile["row"]} cuts {piece}, which the job does not'
                )
            # The program ends as the job does: retracted, spindle off.
            self.assertEqual(tile['lines'][-2:], job['lines'][-2:])
            self.assertEqual(tile['lines'][:6], job['lines'][:6])
            total += tile['cut_length']
        # Nothing is cut twice and nothing is left out: the tiles' cutting adds up to the job's.
        self.assertAlmostEqual(total, length(whole), delta=0.05)
        return answer

    def test_the_tiles_cut_the_whole_job_and_nothing_else(self):
        job = self.job()
        answer = self.check(job, 200, 150)
        self.assertEqual((answer['columns'], answer['rows']), (3, 2))
        self.assertEqual(len(answer['tiles']), 6)
        self.assertEqual(answer['bounds'], [20, 20, 520, 280])
        for width, height in ((130, 90), (260, 300), (97.5, 61.25), (520, 140)):
            with self.subTest(tile=(width, height)):
                self.check(job, width, height)

    def test_a_tile_that_holds_the_whole_job_is_the_job(self):
        job = self.job()
        answer = call('tile', {'lines': job['lines'], 'tile_width': 600, 'tile_height': 300})
        self.assertEqual((answer['columns'], answer['rows'], len(answer['tiles'])), (1, 1, 1))
        self.assertEqual(answer['tiles'][0]['lines'], job['lines'])

    def test_tiles_without_cutting_are_left_out(self):
        job = self.job(paths=[[[20, 20], [80, 20]], [[450, 250], [500, 250]]], path_depths=None)
        answer = self.check(job, 200, 150)
        self.assertEqual((answer['columns'], answer['rows']), (3, 2))
        self.assertEqual([(t['column'], t['row']) for t in answer['tiles']], [(0, 0), (2, 1)])

    def test_a_cut_re_entering_a_tile_comes_down_at_the_plunge_feed(self):
        # One long cut across three tiles at a constant depth, in two passes of depth.
        job = self.job(paths=[[[20, 50], [520, 50]]], path_depths=None, depth=3, step=1.5)
        answer = call('tile', {'lines': job['lines'], 'tile_width': 200, 'tile_height': 150})
        middle = answer['tiles'][1]['lines']
        self.assertIn('G0 X0.000 Y50.000', middle)  # the cut enters the middle tile at its left edge
        down = [line for line in middle if line.startswith('G1 Z')]
        self.assertEqual(down, ['G1 Z-1.500 F200.0', 'G1 Z-3.000 F200.0'])
        self.assertEqual([line for line in middle if line.startswith('G1 X')], ['G1 X200.000 Y50.000 F900.0'] * 2)
        self.assertEqual(middle.count('G0 Z5.000'), job['lines'].count('G0 Z5.000') + 2)  # and leaves it retracted
        stated = call('tile', {'lines': job['lines'], 'tile_width': 200, 'tile_height': 150, 'plunge': 80})
        self.assertIn('G1 Z-1.500 F80.0', stated['tiles'][1]['lines'])

    def test_what_cannot_be_tiled_is_refused_with_its_line(self):
        lines = self.job()['lines']
        bad = lambda extra, **more: call(
            'tile', {'lines': lines[:9] + extra + lines[9:], 'tile_width': 200, 'tile_height': 150, **more}
        )
        with self.assertRaisesRegex(
            ValueError, r'Line 10 cannot be tiled \(only straight moves in absolute millimetres are handled\): G2'
        ):
            bad(['G2 X10 Y10 I5 J0'])
        with self.assertRaisesRegex(ValueError, 'cannot be tiled'):
            bad(['G91'])
        with self.assertRaisesRegex(ValueError, 'not laser power'):
            bad(['G1 X10 Y10 S500'])
        with self.assertRaisesRegex(ValueError, 'below the clearance height'):
            call(
                'tile',
                {
                    'lines': ['G21', 'G90', 'G0 Z5.000', 'G0 X1 Y1', 'G1 Z-1 F100', 'G0 X50 Y1', 'G1 X60 Y1 F300'],
                    'tile_width': 20,
                    'tile_height': 20,
                },
            )
        with self.assertRaisesRegex(ValueError, 'X0 Y0 or beyond'):
            call(
                'tile',
                {
                    'lines': ['G21', 'G90', 'G0 Z5', 'G0 X-5 Y1', 'G1 Z-1 F100', 'G1 X30 Y1 F300'],
                    'tile_width': 20,
                    'tile_height': 20,
                },
            )
        with self.assertRaisesRegex(ValueError, 'no cutting moves'):
            call('tile', {'lines': ['G21', 'G90', 'G0 Z5', 'M5'], 'tile_width': 20, 'tile_height': 20})
        with self.assertRaisesRegex(ValueError, 'more than 100 tiles'):
            call('tile', {'lines': lines, 'tile_width': 10, 'tile_height': 10})
        with self.assertRaisesRegex(ValueError, 'Tile width must be between 10'):
            call('tile', {'lines': lines, 'tile_width': 5, 'tile_height': 150})
        with self.assertRaisesRegex(ValueError, 'needs the program lines'):
            call('tile', {'tile_width': 200, 'tile_height': 150})
        with self.assertRaisesRegex(ValueError, 'Every program line must be text'):
            call('tile', {'lines': [5], 'tile_width': 200, 'tile_height': 150})


@unittest.skipUnless(CLI and STUDIO, NOT_BUILT)
class TileRouteTests(unittest.TestCase):
    """The route that divides a project's job, and a tile run on the simulator."""

    PROJECT = dict(
        mode='router', protocol='grbl', width=600, height=300, feed=900, plunge=200, power=1000, safe=5, depth=1.5, step=1.5,
        stock_thickness=6, paths=[[[20, 20], [520, 20], [520, 280], [20, 280], [20, 20]], [[40, 40], [500, 260]]],
    )  # fmt: skip

    @classmethod
    def setUpClass(cls):
        cls.running = native_servers.studio(STUDIO)
        cls.server = cls.running.__enter__()

    @classmethod
    def tearDownClass(cls):
        cls.running.__exit__(None, None, None)

    def refused(self, route, payload, fragment):
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.server.post(route, payload)
        self.assertEqual(caught.exception.code, 400)
        self.assertIn(fragment, json.loads(caught.exception.read())['error'])

    def rest(self):
        for _ in range(1500):
            status = self.server.status()
            if not status['active']:
                return status
            time.sleep(0.02)
        self.fail('the simulator did not finish the tile')

    def test_the_route_answers_as_the_tool_does(self):
        answer = self.server.post('api/tile', {'project': self.PROJECT, 'tile_width': 200, 'tile_height': 150})
        direct = call('tile', {'lines': generate(self.PROJECT)['lines'], 'tile_width': 200, 'tile_height': 150})
        self.assertEqual(answer, direct)
        self.assertEqual(len(answer['tiles']), 6)
        self.refused(
            'api/tile',
            {'project': {**self.PROJECT, 'mode': 'laser'}, 'tile_width': 200, 'tile_height': 150},
            'router jobs',
        )
        self.refused('api/tile', {'tile_width': 200, 'tile_height': 150}, 'Provide a project and a tile size')
        self.refused('api/tile', {'project': self.PROJECT, 'tile_width': 200}, 'Tile height')
        self.refused('api/tile', {'project': self.PROJECT, 'tile_width': 5, 'tile_height': 150}, 'Tile width')

    def test_one_tile_runs_on_the_simulator_and_the_others_do_not(self):
        self.server.post('api/disconnect', {})
        self.server.post('api/connect', {'port': 'simulator'})
        tiles = self.server.post('api/tile', {'project': self.PROJECT, 'tile_width': 200, 'tile_height': 150})['tiles']
        wanted = next(t for t in tiles if (t['column'], t['row']) == (1, 1))
        tile = {'width': 200, 'height': 150, 'column': 1, 'row': 1}
        answer = self.server.post('api/run', {'project': self.PROJECT, 'confirmed': True, 'tile': tile})
        self.assertEqual(answer['lines'], len(wanted['lines']))
        status = self.rest()
        self.assertIsNone(status['error'])
        self.assertEqual(status['progress'], 100)
        ran = status['log'][-len(wanted['lines']) :] if len(wanted['lines']) <= len(status['log']) else None
        if ran is not None:
            self.assertEqual(ran, wanted['lines'])
        # The simulated tool never left the tile.
        self.assertTrue(0 <= status['position']['x'] <= 200 and 0 <= status['position']['y'] <= 150, status['position'])
        self.refused('api/run', {'project': self.PROJECT, 'tile': tile}, 'Confirm work zero')
        self.refused(
            'api/run', {'project': self.PROJECT, 'confirmed': True, 'tile': {**tile, 'column': 7}}, 'no cutting in it'
        )
        self.refused(
            'api/run', {'project': self.PROJECT, 'confirmed': True, 'tile': {**tile, 'column': -1}}, 'column and row'
        )
        self.refused(
            'api/run', {'project': self.PROJECT, 'confirmed': True, 'tile': 'first'}, 'width, height, column and row'
        )
        self.refused(
            'api/run',
            {'project': self.PROJECT, 'confirmed': True, 'tile': tile, 'leveled': True},
            'cannot follow a probed surface',
        )
        self.refused(
            'api/run', {'project': {**self.PROJECT, 'mode': 'laser'}, 'confirmed': True, 'tile': tile}, 'router jobs'
        )
        self.server.post('api/disconnect', {})


if __name__ == '__main__':
    unittest.main()
