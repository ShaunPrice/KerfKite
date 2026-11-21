"""Stock preview transfer under the local token and hosted login/CSRF guards."""

import json
import math
from pathlib import Path
import struct
import tempfile
import unittest

from studio.server import create_app as local_app
from studio.simulation import simulate
from studio.stock_wire import MIME
from studio.web_server import create_app as web_app
from studio.web_store import WebStore


def body(count, row, shape=0, magic=b'KKSTOCK1', resolution=16):
    return (
        struct.pack('<8sIII7d', magic, count, resolution, shape, 0, 0, 8, 8, 1, 60, 3)
        + struct.pack('<7d', *row) * count
    )


class StockAPITests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        store = WebStore(Path(temp.name))
        store.add_user('preview', 'Simulator test account passphrase!')
        origin = 'https://localhost:8443'
        web = web_app(store, origin).test_client()
        local = local_app().test_client()
        self.assertEqual(web.post('/api/simulate', base_url=origin, data=b'', content_type=MIME).status_code, 401)
        web.get('/login', base_url=origin)
        csrf = web.get_cookie('__Host-cnc-login', domain='localhost').value
        response = web.post(
            '/login',
            base_url=origin,
            headers={'Origin': origin},
            data=dict(username='preview', password='Simulator test account passphrase!', csrf=csrf),
        )
        self.assertEqual(response.status_code, 302)
        self.clients = []
        for client, url, headers in [(web, origin, {'Origin': origin}), (local, 'http://localhost', {})]:
            self.assertEqual(
                client.post('/api/simulate', base_url=url, data=b'', content_type=MIME, headers=headers).status_code,
                403,
            )
            session = client.get('/api/session', base_url=url).get_json()
            self.assertTrue(session['stock_wire_v1'])
            self.clients.append((client, dict(headers={**headers, 'X-Studio-Token': session['token']}, base_url=url)))

    def test_largest_wire_job_and_legacy_decimal_job_on_both_servers(self):
        # 200k outside-stock movements fit the work budget and exceed both original
        # app-wide byte limits. The final row crosses the stock to check the result.
        outside = [
            9.119891919192,
            10.911183838383,
            9.918811773311,
            11.911183838383,
            -0.435231239418,
            0,
            -0.143543123942,
        ]
        cut = [0.0, 2.0, 8.0, 2.0, -1.0, 0.0, -2.0]
        count = 200000
        packet = body(count, outside)[:-56] + struct.pack('<7d', *cut)
        self.assertGreater(len(packet), 10 * 1024 * 1024)
        options = dict(width=8, height=8, resolution=16, tool_diameter=1, stock_thickness=3)
        legacy = json.dumps({**options, 'segments': [outside] * (134268 - 1) + [cut]}, separators=(',', ':')).encode()
        self.assertGreater(len(legacy), 10 * 1024 * 1024)
        expected = simulate({**options, 'segments': [cut]}, backend='python')
        for client, auth in self.clients:
            for content, kind in [(packet, MIME), (legacy, 'application/json')]:
                with self.subTest(kind=kind, url=auth['base_url']):
                    response = client.post('/api/simulate', data=content, content_type=kind, **auth)
                    self.assertEqual(response.status_code, 200)
                    self.assertEqual(response.get_json(), expected)
            # Other design endpoints retain the global size setting.
            self.assertEqual(
                client.post('/api/generate', data=legacy, content_type='application/json', **auth).status_code, 413
            )

    def test_malformed_stock_returns_structured_errors(self):
        row = [0.0, 1.0, 1.0, 2.0, -1.0, math.nan, -1.0]
        packet = body(1, row)
        packets = [
            b'',
            packet[:-1],
            packet + b'bad',
            body(1, row, magic=b'KKOTHER0'),
            body(1, row, shape=99),
            body(1, row, resolution=15),
            body(1, [math.inf, *row[1:]]),
        ]
        for client, auth in self.clients:
            for malformed in packets:
                response = client.post('/api/simulate', data=malformed, content_type=MIME, **auth)
                self.assertEqual(response.status_code, 400)
                self.assertIn('error', response.get_json())
            for malformed in ('null', '[]', '{"segments":[[0,0,null,0,-1]]}'):
                response = client.post('/api/simulate', data=malformed, content_type='application/json', **auth)
                self.assertEqual(response.status_code, 400)
                self.assertIn('error', response.get_json())
