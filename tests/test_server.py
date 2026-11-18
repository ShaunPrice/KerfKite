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
