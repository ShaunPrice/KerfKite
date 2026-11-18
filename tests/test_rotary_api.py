"""Rotary export integration through both guarded design API surfaces."""
import math
import tempfile
import unittest

from studio.controller import Controller
from studio.server import create_app as local_app
from studio.web_server import create_app as web_app
from studio.web_store import WebStore


def payload(mode='router'):
    paths=[[[0,5],[math.pi*10,5]]]
    if mode=='knife':paths=[[[10,10],[20,10]]]
    return {'project': {'mode':mode,'operation':'line','protocol':'grbl',
            'width':100,'height':100,'feed':600,'power':1000,'safe':5,
            'depth':1,'step':1,'plunge':100,'passes':1,'paths':paths},
            'options': {'diameter':10,'wrap_axis':'X','postprocessor':'linuxcnc'}}


def assert_full_turn(case,response):
    case.assertEqual(response.status_code,200,response.get_json())
    result=response.get_json()
    case.assertEqual(result['rotary']['angle_bounds'],[0,360])
    case.assertTrue(result['rotary']['export_only'])
    case.assertTrue(any('A360.000000 X5.000000' in line for line in result['lines']))
    case.assertIn('G93',result['lines'])
    case.assertEqual(result['lines'][-2],'M5')
    case.assertTrue(result['lines'][-1].startswith('G94 F'))
    case.assertTrue(all(' F' in line for line in result['lines'] if line.startswith('G1 ')))


class LocalRotaryAPITests(unittest.TestCase):
    def setUp(self):
        self.controller=Controller();self.addCleanup(self.controller.disconnect)
        self.client=local_app(self.controller).test_client()
        token=self.client.get('/api/session').get_json()['token']
        self.headers={'X-Studio-Token':token}

    def test_export_without_machine_connection_and_token_guard(self):
        self.assertEqual(self.client.post('/api/rotary',json=payload()).status_code,403)
        response=self.client.post('/api/rotary',json=payload(),headers=self.headers)
        assert_full_turn(self,response)
        self.assertFalse(self.controller.connected)

    def test_unsupported_heads_and_dialect_are_explicit_errors(self):
        for mode in ('laser','knife'):
            response=self.client.post('/api/rotary',json=payload(mode),headers=self.headers)
            self.assertEqual(response.status_code,400,response.get_json())
            self.assertIn('router jobs only',response.get_json()['error'])
        data=payload();data['options']['postprocessor']='grbl'
        response=self.client.post('/api/rotary',json=data,headers=self.headers)
        self.assertEqual(response.status_code,400)
        self.assertIn('LinuxCNC',response.get_json()['error'])


class HTTPSRotaryAPITests(unittest.TestCase):
    def setUp(self):
        directory=tempfile.TemporaryDirectory();self.addCleanup(directory.cleanup)
        store=WebStore(directory.name);store.add_user('rotary-tester','Rotary integration test passphrase!')
        self.base='https://localhost:8443'
        self.client=web_app(store,self.base).test_client()

    def test_authenticated_export_requires_session_and_csrf(self):
        self.assertEqual(self.client.post('/api/rotary',base_url=self.base,json=payload()).status_code,401)
        self.client.get('/login',base_url=self.base)
        csrf=self.client.get_cookie('__Host-cnc-login',domain='localhost').value
        login=self.client.post('/login',base_url=self.base,headers={'Origin':self.base},
              data={'username':'rotary-tester','password':'Rotary integration test passphrase!','csrf':csrf})
        self.assertEqual(login.status_code,302)
        self.assertEqual(self.client.post('/api/rotary',base_url=self.base,json=payload()).status_code,403)
        token=self.client.get('/api/session',base_url=self.base).get_json()['token']
        headers={'Origin':self.base,'X-Studio-Token':token}
        response=self.client.post('/api/rotary',base_url=self.base,json=payload(),headers=headers)
        assert_full_turn(self,response)
        response=self.client.post('/api/rotary',base_url=self.base,json=payload('laser'),headers=headers)
        self.assertEqual(response.status_code,400)
        self.assertIn('router jobs only',response.get_json()['error'])


if __name__=='__main__':unittest.main()
