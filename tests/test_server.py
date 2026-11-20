import unittest
from studio.server import create_app
from studio.controller import Controller
from test_cam import sample

class ServerTests(unittest.TestCase):
    def setUp(self):
        self.ctl=Controller();self.addCleanup(self.ctl.disconnect)
        self.client=create_app(self.ctl).test_client()
        self.token=self.client.get('/api/session').json['token']
        self.headers={'X-Studio-Token':self.token}
    def post(self,path,data):return self.client.post(path,json=data,headers=self.headers)
    def test_token_and_host_guards(self):
        self.assertEqual(self.client.post('/api/connect',json={'port':'simulator'}).status_code,403)
        self.assertEqual(self.client.get('/api/session',headers={'Host':'evil.example'}).status_code,403)
    def test_generate_and_run(self):
        self.assertEqual(self.post('/api/generate',sample()).status_code,200)
        self.post('/api/connect',{'port':'simulator'})
        self.assertEqual(self.post('/api/run',{'confirmed':False,'project':sample()}).status_code,400)
        self.assertEqual(self.post('/api/run',{'confirmed':True,'project':sample()}).status_code,200)
    def test_physical_mode_mismatch(self):
        self.ctl.connected=True;self.ctl.settings={'30':1000,'32':1}
        result=self.post('/api/run',{'confirmed':True,'project':sample()})
        self.assertEqual(result.status_code,400)

    def test_hardware_connection_blocked_until_commissioned(self):
        result=self.post('/api/connect',{'port':'/dev/ttyUSB0'})
        self.assertEqual(result.status_code,400)
        self.assertIn('commissioning',result.json['error'])


class MalformedRequestTests(unittest.TestCase):
    """Machine-control routes answer malformed bodies with a JSON 400, never a 500."""
    def setUp(self):
        self.ctl=Controller();self.addCleanup(self.ctl.disconnect)
        self.client=create_app(self.ctl).test_client()
        self.headers={'X-Studio-Token':self.client.get('/api/session').json['token']}
    def rejected(self,path,**body):
        result=self.client.post(path,headers=self.headers,**body)
        self.assertEqual(result.status_code,400,path)
        self.assertIn('error',result.json)
        return result.json['error']
    def test_non_object_bodies(self):
        for path in ('/api/connect','/api/run','/api/command','/api/head','/api/arm','/api/generate','/api/disarm','/api/arm/check','/api/arm/confirm','/api/phone-monitor'):
            with self.subTest(path=path):
                self.rejected(path,json=[])
                self.rejected(path,data='not json',content_type='application/json')
                self.rejected(path,data='x',content_type='text/plain')
    def test_connect_requires_a_port(self):
        self.assertIn('port',self.rejected('/api/connect',json={}))
        self.rejected('/api/connect',json={'port':7})
    def test_command_rejects_missing_action_and_unknown_fields(self):
        self.client.post('/api/connect',json={'port':'simulator'},headers=self.headers)
        self.assertIn('action',self.rejected('/api/command',json={'bogus':1}))
        self.assertIn('bogus',self.rejected('/api/command',json={'action':'hold','bogus':1}))
        self.rejected('/api/command',json={'action':['hold']})
        self.assertEqual(self.client.post('/api/command',json={'action':'hold'},headers=self.headers).status_code,200)
    def test_wrong_token_is_refused(self):
        self.assertEqual(self.client.post('/api/connect',json={'port':'simulator'},headers={'X-Studio-Token':'é-wrong'}).status_code,403)
