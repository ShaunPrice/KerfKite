from importlib.util import find_spec
from pathlib import Path
from tempfile import TemporaryDirectory
from urllib.request import Request, urlopen
from uuid import uuid4
import json
import ssl
import unittest

from studio.controller import Controller
from studio.monitor import create_monitor_app
from studio.phone_jobs import PhoneJobs
from studio.phone_monitor_service import PhoneMonitor
from studio.server import create_app


class PendantTests(unittest.TestCase):
    def setUp(self):
        self.controller = Controller()
        self.controller.connect('simulator')
        self.addCleanup(self.controller.disconnect)
        self.jobs = PhoneJobs(self.controller)
        self.headers = {'Authorization': 'Bearer ' + 'x' * 43}
        self.client = create_monitor_app(self.controller, 'x' * 43, self.jobs).test_client()

    def post(self, action, **values):
        return self.client.post(
            '/api/pendant',
            json=dict(action=action, request_id=str(uuid4()), control_epoch=self.jobs.control_epoch, **values),
            headers=self.headers,
        )

    def test_step_disarms_and_duplicate_is_rejected(self):
        self.jobs.arm(['M5'])
        request = dict(action='jog', axis='X', distance=0.5, feed=100, request_id=str(uuid4()))
        request['control_epoch'] = self.jobs.control_epoch
        self.assertEqual(self.client.post('/api/pendant', json=request).status_code, 401)
        self.assertEqual(self.client.post('/api/pendant', json=request, headers=self.headers).status_code, 200)
        self.assertFalse(self.jobs.status()['armed'])
        self.assertIn('already', self.client.post('/api/pendant', json=request, headers=self.headers).json['error'])

    def test_active_job_and_invalid_commands(self):
        for data in [
            dict(action='raw'),
            dict(action='home', value='extra'),
            dict(action='jog', axis='A', distance=1, feed=200),
            dict(action='jog', axis='Y', distance=12, feed=100),
        ]:
            self.assertEqual(self.post(**data).status_code, 400)
        self.jobs.arm(['M5'] * 60)
        self.jobs.start()
        self.assertEqual(self.post('zero').status_code, 400)

    def test_local_pairing_controls_and_revocation(self):
        service = PhoneMonitor(self.controller, self.jobs, port=0)
        client = create_app(self.controller, self.jobs, phone_monitor=service).test_client()
        self.addCleanup(service.disable)
        request = lambda action, headers: client.post('/api/phone-monitor', json={'action': action}, headers=headers)
        self.assertEqual(request('enable', {}).status_code, 403)
        auth = {'X-Studio-Token': client.get('/api/session').json['token']}
        first = request('enable', auth).json
        self.assertTrue(first['enabled'])
        self.assertEqual(first['token'], request('status', auth).json['token'])
        old = create_monitor_app(self.controller, first['token'], self.jobs, service.revoked).test_client()
        bearer = {'Authorization': 'Bearer ' + first['token']}
        self.assertTrue(old.get('/api/status', headers=bearer).json['pendant'])
        self.assertFalse(request('disable', auth).json['enabled'])
        self.assertEqual(old.get('/api/status', headers=bearer).status_code, 401)
        self.assertNotEqual(first['token'], request('enable', auth).json['token'])

    @unittest.skipUnless(find_spec('cheroot') and find_spec('cryptography'), 'HTTPS web extra required')
    def test_trusted_https_request(self):
        from studio.web_server import certificate

        with TemporaryDirectory() as directory:
            cert = Path(directory) / 'server.pem'
            key = Path(directory) / 'server-key.pem'
            certificate(cert, key, ['127.0.0.1'])
            monitor = PhoneMonitor(self.controller, self.jobs, port=0, certificate=str(cert), key=str(key))
            try:
                pairing = monitor.enable()
                url = f"https://127.0.0.1:{pairing['port']}/api/status"
                request = Request(url, headers={'Authorization': 'Bearer ' + pairing['token']})
                with urlopen(request, context=ssl.create_default_context(cafile=cert), timeout=3) as response:
                    self.assertTrue(json.load(response)['simulated'])
            finally:
                monitor.disable()
