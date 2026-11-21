from importlib.util import find_spec
from pathlib import Path
from tempfile import TemporaryDirectory
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from uuid import uuid4
import hashlib
import json
import socket
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

    @unittest.skipUnless(find_spec('cryptography'), 'cryptography required for the pinned listener')
    def test_listener_is_encrypted_and_pinned_by_default(self):
        monitor = PhoneMonitor(self.controller, self.jobs, port=0)
        self.addCleanup(monitor.disable)
        self.assertEqual(monitor.info()['encryption'], 'none')
        self.assertIsNone(monitor.info()['fragment'])
        pairing = monitor.enable()
        self.assertEqual((pairing['scheme'], pairing['encryption']), ('https', 'pinned'))
        self.assertRegex(pairing['fingerprint'], r'^[0-9a-f]{64}$')
        self.assertEqual(pairing['fragment'], f"token={pairing['token']}&fp={pairing['fingerprint']}")
        served = ssl.PEM_cert_to_DER_cert(ssl.get_server_certificate(('127.0.0.1', pairing['port'])))
        self.assertEqual(hashlib.sha256(served).hexdigest(), pairing['fingerprint'])
        # Trust exactly the served certificate, as Pocket does, then make an authorised request.
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.load_verify_locations(cadata=ssl.DER_cert_to_PEM_cert(served))
        url = f"https://127.0.0.1:{pairing['port']}/api/status"
        with urlopen(
            Request(url, headers={'Authorization': 'Bearer ' + pairing['token']}), context=context, timeout=3
        ) as response:
            self.assertTrue(json.load(response)['simulated'])
        with self.assertRaises(HTTPError) as refused:
            urlopen(Request(url, headers={'Authorization': 'Bearer wrong'}), context=context, timeout=3)
        self.assertEqual(refused.exception.code, 401)
        with self.assertRaises(OSError):
            urlopen(f"http://127.0.0.1:{pairing['port']}/api/status", timeout=3)
        self.assertEqual(monitor.enable()['fingerprint'], pairing['fingerprint'])
        self.assertIsNone(monitor.disable()['fingerprint'])
        self.assertNotEqual(monitor.enable()['fingerprint'], pairing['fingerprint'])

    @unittest.skipUnless(find_spec('cryptography'), 'cryptography required for the pinned listener')
    def test_stalled_connection_does_not_block_the_encrypted_listener(self):
        monitor = PhoneMonitor(self.controller, self.jobs, port=0)
        self.addCleanup(monitor.disable)
        pairing = monitor.enable()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        with socket.create_connection(('127.0.0.1', pairing['port'])):  # connects and never starts a handshake
            request = Request(
                f"https://127.0.0.1:{pairing['port']}/api/status",
                headers={'Authorization': 'Bearer ' + pairing['token']},
            )
            with urlopen(request, context=context, timeout=3) as response:
                self.assertEqual(response.status, 200)

    @unittest.skipUnless(find_spec('cryptography'), 'cryptography required for the pinned listener')
    def test_workstation_chooses_unencrypted_pairing_only_while_pairing_is_off(self):
        service = PhoneMonitor(self.controller, self.jobs, port=0)
        client = create_app(self.controller, self.jobs, phone_monitor=service).test_client()
        self.addCleanup(service.disable)
        auth = {'X-Studio-Token': client.get('/api/session').json['token']}
        request = lambda **data: client.post('/api/phone-monitor', json=data, headers=auth)
        status = request(action='status').json
        self.assertTrue(status['can_encrypt'])
        self.assertFalse(status['plaintext'])
        self.assertEqual(request(action='enable', plaintext='yes').status_code, 400)
        self.assertEqual(request(action='enable', plaintext=True).json['encryption'], 'none')
        refused = request(action='enable', plaintext=False)
        self.assertEqual(refused.status_code, 400)
        self.assertIn('Disable', refused.json['error'])
        request(action='disable')
        self.assertEqual(request(action='enable', plaintext=False).json['encryption'], 'pinned')
        self.assertEqual(request(action='enable').json['encryption'], 'pinned')

    def test_plaintext_option_and_missing_cryptography_fall_back_to_http(self):
        from unittest.mock import patch

        for label, monitor, hidden in (
            ('plaintext option', PhoneMonitor(self.controller, self.jobs, port=0, plaintext=True), False),
            ('cryptography missing', PhoneMonitor(self.controller, self.jobs, port=0), True),
        ):
            with self.subTest(label), patch('studio.monitor_tls.available', return_value=not hidden):
                try:
                    pairing = monitor.enable()
                    self.assertEqual((pairing['scheme'], pairing['encryption']), ('http', 'none'))
                    self.assertIsNone(pairing['fingerprint'])
                    self.assertEqual(pairing['fragment'], 'token=' + pairing['token'])
                    request = Request(
                        f"http://127.0.0.1:{pairing['port']}/api/status",
                        headers={'Authorization': 'Bearer ' + pairing['token']},
                    )
                    with urlopen(request, timeout=3) as response:
                        self.assertTrue(json.load(response)['simulated'])
                finally:
                    monitor.disable()

    @unittest.skipUnless(find_spec('cryptography'), 'cryptography required for the pinned listener')
    def test_self_signed_certificate_names_the_listener_addresses(self):
        from cryptography import x509
        from studio.monitor_tls import self_signed

        pem, key, fingerprint = self_signed(['workshop-pi', '192.168.1.20', '192.168.1.20', ''])
        certificate = x509.load_pem_x509_certificate(pem)
        names = certificate.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
        self.assertEqual(names.get_values_for_type(x509.DNSName), ['workshop-pi', 'localhost'])
        self.assertEqual([str(ip) for ip in names.get_values_for_type(x509.IPAddress)], ['192.168.1.20', '127.0.0.1'])
        self.assertEqual(fingerprint, certificate.fingerprint(certificate.signature_hash_algorithm).hex())
        self.assertIn(b'PRIVATE KEY', key)
        self.assertNotEqual(fingerprint, self_signed(['workshop-pi'])[2])

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
