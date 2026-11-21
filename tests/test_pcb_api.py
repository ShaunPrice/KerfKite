"""The circuit board import route: what it answers, what it refuses, and that it never drives a machine."""

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from shapely.geometry import Point, Polygon

from studio import pcb_api, pcb_import
from studio.cam import generate
from studio.controller import Controller
from studio.server import create_app
from studio.web_server import create_app as create_web_app
from studio.web_store import WebStore

HAS_CORE = pcb_import.isolation_available()
FIXTURES = Path(__file__).parent / 'pcb'
SETTINGS = dict(mode='router', protocol='grbl', width=60, height=40, feed=200, power=12000)


def fixture(name):
    return pcb_import.read(FIXTURES / name)


@unittest.skipUnless(HAS_CORE, 'the compiled extension with isolation is not installed')
class BoardRouteTests(unittest.TestCase):
    def setUp(self):
        self.controller = Controller()
        self.client = create_app(controller=self.controller).test_client()
        self.headers = {'X-Studio-Token': self.client.get('/api/session').json['token']}
        self.board = {
            'copper': fixture('board_top.gbr'),
            'outline': fixture('edge_cuts.gbr'),
            'drill': fixture('drill_decimal.drl'),
            'tool_width': 0.2,
            'width': 60,
            'height': 40,
        }

    def post(self, data):
        return self.client.post('/api/pcb-import', json=data, headers=self.headers)

    def ok(self, **changes):
        result = self.post({**self.board, **changes})
        self.assertEqual(result.status_code, 200, result.get_json(silent=True))
        return result.json

    def rejected(self, data=None, **raw):
        result = self.client.post('/api/pcb-import', headers=self.headers, **raw) if raw else self.post(data)
        self.assertEqual(result.status_code, 400, f'{data!r}'[:200])
        self.assertIn('error', result.json)
        return result.json['error']

    def test_the_session_advertises_the_import(self):
        self.assertIs(self.client.get('/api/session').json['pcb_import'], True)
        with mock.patch.object(pcb_import, '_core_call', None):
            self.assertIs(self.client.get('/api/session').json['pcb_import'], False)
            self.assertIn('compiled KerfKite extension', self.rejected(self.board))

    def test_a_whole_board_is_placed_and_interpreted(self):
        answer = self.ok()
        # The outline's lowest corner is brought to the margin, and every layer moves with it.
        self.assertEqual(answer['offset'], [-95.0, 115.0])
        self.assertEqual((answer['side'], answer['mirror_x']), ('top', None))
        self.assertEqual(answer['outline']['bounds'], [5.0, 5.0, 45.0, 35.0])
        self.assertEqual(len(answer['outline']['paths']), 3)  # the board and its two cut-outs
        self.assertEqual(answer['outline']['paths'][0][0], answer['outline']['paths'][0][-1])
        copper = answer['copper']
        self.assertEqual(copper['layer']['file_function'], 'Copper,L1,Top')
        self.assertEqual(copper['layer']['counts'], {'arc': 0, 'flash': 9, 'line': 10, 'region': 1})
        self.assertEqual((copper['tool_width'], copper['passes'], copper['unreachable']), (0.2, 1, []))
        self.assertEqual(copper['islands'], len(copper['copper']))
        self.assertEqual(copper['warnings'], [])
        # A via pad of the copper sits on its hole.
        drill = answer['drill']
        self.assertEqual([group['tool_diameter'] for group in drill['groups']], [0.4, 0.8])
        self.assertEqual([group['holes'] for group in drill['groups']], [2, 4])
        self.assertEqual(drill['operations'][0]['paths'][0], [[10.0, 30.0], [10.0, 30.0]])
        self.assertEqual(drill['interpretation']['units'], 'mm')
        self.assertEqual(drill['interpretation']['digits_from'], 'decimal points')
        self.assertIs(drill['plated'], True)
        self.assertEqual(len(drill['slots']), 2)
        self.assertTrue(any('slots are not drilled' in w for w in drill['warnings']))
        self.assertTrue(any('2 drill diameters' in w for w in drill['warnings']))
        self.assertEqual(answer['warnings'], [])
        for low, high in zip(answer['bounds'][:2], answer['bounds'][2:]):
            self.assertGreaterEqual(low, 0)
            self.assertLessEqual(high, 60)
        # Each part generates: isolation at the engraving depth, one drill job for each diameter, the cut-out.
        job = generate({**SETTINGS, 'depth': 0.1, 'step': 0.1, 'tool_shape': 'v', 'operations': copper['operations']})
        self.assertIn('Z-0.100', ' '.join(job['lines']))
        for operation in drill['operations']:
            self.assertTrue(generate({**SETTINGS, 'depth': 1.8, **operation})['drill_holes'])
        cut_out = generate({**SETTINGS, 'depth': 1.8, 'operation': 'line', 'paths': answer['outline']['paths']})
        self.assertGreater(cut_out['cut_length'], 100)

    def test_the_layers_of_one_board_register(self):
        # The outline, both copper layers and the holes of the fixture board: every hole has its pad on each side.
        board = {'outline': fixture('board_outline.gbr'), 'drill': fixture('board_drill.drl'), 'tool_width': 0.2}
        top = self.post({**board, 'copper': fixture('board_top.gbr'), 'width': 60, 'height': 40}).json
        under = self.post({**board, 'copper': fixture('board_bottom.gbr'), 'side': 'bottom'}).json
        self.assertEqual((top['warnings'], top['copper']['unreachable']), ([], []))
        self.assertEqual(top['outline']['bounds'], [5.0, 5.0, 45.0, 35.0])
        self.assertTrue(any('does not state its number format' in w for w in top['drill']['warnings']))
        self.assertEqual(top['drill']['interpretation']['digits_from'], 'default')
        for answer, expected in ((top, 7), (under, 6)):
            holes = [path[0] for operation in answer['drill']['operations'] for path in operation['paths']]
            self.assertEqual(len(holes), 7)
            pads = [Polygon(island['shell'], island['holes']) for island in answer['copper']['copper']]
            self.assertEqual(sum(any(pad.contains(Point(hole)) for pad in pads) for hole in holes), expected)
        self.assertEqual(under['mirror_x'], 25.0)

    def test_warnings_of_each_file_are_returned(self):
        tight = self.ok(tool_width=0.3)['copper']
        self.assertEqual(tight['unreachable_count'], 2)
        self.assertAlmostEqual(tight['smallest_gap'], 0.295, 6)
        self.assertTrue(any('cannot pass between' in w and '0.295 mm' in w for w in tight['warnings']))
        for place in tight['unreachable']:
            self.assertTrue(5 < place['at'][0] < 45 and 5 < place['at'][1] < 35)
        # A drill file that leaves its format to be guessed says so, and the request can state it.
        alone = dict(copper=None, outline=None, width=None, height=None)
        guessed = self.ok(drill=fixture('drill_inch.drl'), **alone)['drill']
        self.assertEqual(guessed['interpretation']['units'], 'inch')
        stated = self.ok(
            drill=fixture('drill_unstated.drl'),
            **alone,
            drill_format={'units': 'mm', 'integer': 3, 'decimal': 3},
        )['drill']
        self.assertEqual(stated['interpretation']['units_from'], 'request')
        self.assertEqual(stated['interpretation']['digits_from'], 'request')
        self.assertTrue(
            any(
                'digits' in w or 'format' in w
                for w in self.ok(drill=fixture('drill_unstated.drl'), **alone)['drill']['warnings']
            )
        )
        self.assertIn('give "units" in the request', self.rejected({'drill': 'M48\nT1C0.8\n%\nT1\nX1.0Y1.0\nM30\n'}))

    def test_layers_that_do_not_lie_on_one_board_are_reported(self):
        # Read as inches, the metric holes land far outside the outline: the core refuses them for the work area,
        # and without a work area the answer carries a warning.
        board = {k: v for k, v in self.board.items() if k not in ('width', 'height', 'copper', 'tool_width')}
        wrong = self.post({**board, 'drill': fixture('drill_leading.drl'), 'drill_format': {'units': 'inch'}})
        self.assertEqual(wrong.status_code, 200, wrong.json)
        self.assertTrue(any('Holes lie outside the board outline' in w for w in wrong.json['warnings']))
        self.assertIn('do not fit the work area', self.rejected({**self.board, 'drill_format': {'units': 'inch'}}))
        other = self.post({'copper': fixture('top_copper.gbr'), 'outline': fixture('edge_cuts.gbr'), 'tool_width': 0.2})
        self.assertEqual(other.status_code, 200, other.json)
        self.assertTrue(any('The copper reaches beyond the board outline' in w for w in other.json['warnings']))

    def test_the_underside_is_mirrored_about_the_middle_of_the_outline(self):
        top = self.ok()
        under = self.ok(copper=fixture('board_bottom.gbr'), side='bottom')
        self.assertEqual(under['mirror_x'], 25.0)  # the outline spans X5 to X45
        self.assertEqual(under['offset'], top['offset'])
        self.assertEqual(under['outline']['bounds'], top['outline']['bounds'])  # the board stays where it was
        self.assertTrue(any('mirrored about X25.000' in w for w in under['warnings']))
        # A hole at X10 on the top is at X40 from below, and the bottom copper's pad is there too.
        holes = {tuple(path[0]) for op in under['drill']['operations'] for path in op['paths']}
        self.assertEqual(holes, {(50 - x, y) for op in top['drill']['operations'] for (x, y), _ in op['paths']})
        self.assertIn((40.0, 30.0), holes)
        pads = under['copper']['copper']
        self.assertTrue(any(min(p[0] for p in pad['shell']) < 40 < max(p[0] for p in pad['shell']) for pad in pads))
        self.assertEqual(under['copper']['mirror_x'], 25.0)
        # Copper alone cannot say where the mirror line is; the line of the first import can be given instead.
        alone = {'copper': fixture('board_bottom.gbr'), 'tool_width': 0.2, 'side': 'bottom'}
        self.assertIn('so that both sides register', self.rejected(alone))
        again = self.post({**alone, 'offset': top['offset'], 'mirror_x': 25.0})
        self.assertEqual(again.json['copper']['operations'], under['copper']['operations'])
        self.assertIn('Only the underside is mirrored', self.rejected({**self.board, 'mirror_x': 25.0}))

    def test_tool_width_from_a_v_bit_and_clipping_to_the_outline(self):
        board = {k: v for k, v in self.board.items() if k != 'tool_width'}
        answer = self.post(
            {**board, 'v_bit': {'tip_width': 0.1, 'angle': 30, 'depth': 0.1}, 'passes': 2, 'clip': True}
        ).json
        self.assertEqual(answer['copper']['tool_width'], pcb_import.v_bit_width(0.1, 30, 0.1))
        self.assertEqual([entry['pass'] for entry in answer['copper']['cut']], [2, 1])
        # A header pad of this board sits on the edge of the round cut-out: its contours stop at the outline.
        self.assertEqual(answer['copper']['clipped'], {'cut': 4, 'dropped': 0})
        self.assertTrue(any('cut short at the clip outline' in w for w in answer['copper']['warnings']))
        self.assertIn('not both', self.rejected({**self.board, 'v_bit': {'tip_width': 0.1, 'angle': 30, 'depth': 0.1}}))
        self.assertIn('tip_width, angle and depth', self.rejected({**board, 'v_bit': {'angle': 30}}))
        self.assertIn('Give the tool width, or the V-bit', self.rejected(board))
        self.assertIn(
            'needs the board outline', self.rejected({'copper': self.board['copper'], 'tool_width': 0.2, 'clip': True})
        )

    def test_requests_are_validated(self):
        for data in ([], 'text', 5, None):
            with self.subTest(data=data):
                self.rejected(data)
        self.rejected(data='not json', content_type='application/json')
        self.assertIn('Choose a copper layer', self.rejected({}))
        self.assertIn('Choose a copper layer', self.rejected({'copper': '', 'tool_width': 0.2}))
        self.assertIn('not known: tool_diameter', self.rejected({**self.board, 'tool_diameter': 1}))
        self.assertIn('Send the copper layer as text', self.rejected({**self.board, 'copper': 5}))
        self.assertIn('Send the drill file as text', self.rejected({**self.board, 'drill': ['M48']}))
        self.assertIn('The board outline is larger than 4 MiB', self.rejected({'outline': 'G04 ' + 'x' * (4 << 20)}))
        self.assertIn('"top" or "bottom"', self.rejected({**self.board, 'side': 'left'}))
        self.assertIn('Give the offset as [X, Y]', self.rejected({**self.board, 'offset': [1]}))
        self.assertIn('Offset X must be a number', self.rejected({**self.board, 'offset': ['1', 2]}))
        self.assertIn('Margin must be between 0 and 1000', self.rejected({**self.board, 'margin': -1}))
        self.assertIn('Mirror X must be a number', self.rejected({**self.board, 'side': 'bottom', 'mirror_x': True}))
        self.assertIn('The drill format may state', self.rejected({**self.board, 'drill_format': {'digits': 3}}))
        self.assertIn('The drill format may state', self.rejected({**self.board, 'drill_format': [3]}))
        self.assertIn('Tool width must be between 0.01 and 10', self.rejected({**self.board, 'tool_width': 50}))
        self.assertIn('Isolation passes must be', self.rejected({**self.board, 'passes': 0}))
        self.assertIn('Give both the work width and the work height', self.rejected({**self.board, 'height': None}))
        self.assertIn('no format statement', self.rejected({**self.board, 'copper': 'X0Y0D03*\nM02*\n'}))
        self.assertIn('does not fit the work area', self.rejected({**self.board, 'margin': 30}))

    def test_importing_never_reaches_the_controller(self):
        # The route is not one of those that take the machine lock, and no controller method is called by it.
        with mock.patch.object(Controller, 'start') as start, mock.patch.object(Controller, 'command') as command:
            with mock.patch.object(Controller, '_write') as write, mock.patch.object(Controller, 'connect') as connect:
                before = self.controller.status()
                self.ok()
                self.ok(copper=fixture('board_bottom.gbr'), side='bottom')
                self.rejected({})
                self.assertEqual(self.controller.status(), before)
                for method in (start, command, write, connect):
                    method.assert_not_called()
        self.assertFalse(self.controller.connected)
        self.assertEqual((self.controller.total, len(self.controller.queue)), (0, 0))
        # The module can reach nothing but the importers: these are all it imports.
        source = Path(pcb_api.__file__).read_text().splitlines()
        imports = [line for line in source if line.startswith(('import ', 'from '))]
        self.assertEqual(imports, ['import math', 'from flask import jsonify, request', 'from . import pcb_import'])


@unittest.skipUnless(HAS_CORE, 'the compiled extension with isolation is not installed')
class HostedRouteTests(unittest.TestCase):
    def test_the_hosted_editor_offers_the_same_route_to_signed_in_users(self):
        with tempfile.TemporaryDirectory() as folder:
            store = WebStore(folder)
            store.add_user('alice', 'A secure chosen password!')
            base = 'https://localhost:8443'
            client = create_web_app(store, base).test_client()
            body = {'copper': fixture('board_top.gbr'), 'outline': fixture('edge_cuts.gbr'), 'tool_width': 0.2}
            self.assertEqual(client.post('/api/pcb-import', base_url=base, json=body).status_code, 401)
            client.get('/login', base_url=base)
            cookie = client.get_cookie('__Host-cnc-login', domain='localhost').value
            form = {'username': 'alice', 'password': 'A secure chosen password!', 'csrf': cookie}
            client.post('/login', base_url=base, headers={'Origin': base}, data=form)
            session = client.get('/api/session', base_url=base).json
            self.assertIs(session['pcb_import'], True)
            headers = {'Origin': base, 'X-Studio-Token': session['token']}
            answer = client.post('/api/pcb-import', base_url=base, headers=headers, json=body)
            self.assertEqual(answer.status_code, 200)
            self.assertEqual(answer.json['outline']['bounds'], [5.0, 5.0, 45.0, 35.0])
            refused = client.post('/api/pcb-import', base_url=base, headers=headers, json=[])
            self.assertEqual(refused.status_code, 400)
            self.assertIn('error', refused.json)


if __name__ == '__main__':
    unittest.main()
