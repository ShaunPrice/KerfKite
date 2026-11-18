import unittest
from studio.controller import Controller
from studio.monitor import create_monitor_app


class MonitorTests(unittest.TestCase):
    def setUp(self):
        self.ctl=Controller()
        self.addCleanup(self.ctl.disconnect)
        self.client=create_monitor_app(self.ctl,'a'*43).test_client()
        self.auth={'Authorization':'Bearer '+'a'*43}

    def test_requires_token(self):
        self.assertEqual(self.client.get('/api/status').status_code,401)
        self.assertEqual(self.client.get('/api/status',headers={'Authorization':'Bearer wrong'}).status_code,401)

    def test_authenticated_snapshot_is_read_only_and_filtered(self):
        self.ctl.connect('simulator')
        r=self.client.get('/api/status',headers=self.auth)
        self.assertEqual(r.status_code,200)
        self.assertTrue(r.json['simulated'])
        self.assertNotIn('settings',r.json)
        self.assertEqual(r.headers['Cache-Control'],'no-store')
        for route in ('/api/run','/api/command','/api/connect','/api/disconnect'):
            self.assertEqual(self.client.post(route,headers=self.auth,json={}).status_code,404)
        self.assertEqual(self.client.post('/api/status',headers=self.auth).status_code,405)

    def test_page_contains_no_secret(self):
        r=self.client.get('/')
        self.assertEqual(r.status_code,200)
        self.assertNotIn(b'a'*43,r.data)
        self.assertIn('frame-ancestors',r.headers['Content-Security-Policy'])
        r.close()
        for path in ('/monitor.js', '/monitor.css'):
            with self.client.get(path) as response:
                self.assertEqual(response.status_code,200)

    def test_weak_pairing_token_rejected(self):
        with self.assertRaises(ValueError):create_monitor_app(self.ctl,'short')
