import unittest
from unittest.mock import patch
from studio.protocols import PROTOCOLS, postprocess
from studio.device import DeviceController
from studio.controller import Controller
from studio.server import create_app
from studio.cam import generate
from studio.tool_changes import plan_tool_changes
from studio.rotary import wrap_job

def sample(protocol='grbl', **kw):
    return dict(mode='router',tool_id=1,protocol=protocol,width=100,height=100,feed=600,power=1000,depth=1,step=1,plunge=100,paths=[[[1,1],[10,10]]],**kw)

class ProtocolCatalogTests(unittest.TestCase):
    def test_every_profile_export_and_energy_rejection(self):
        for id,p in PROTOCOLS.items():
            with self.subTest(id=id):
                job=generate(sample(id,dwell_units='milliseconds'))
                self.assertIn('M3 S1000',job['lines'])
                knife=generate({**sample(id,dwell_units='milliseconds'),'mode':'knife'})
                self.assertFalse(any(l.startswith(('M3','M4 ')) and l!='M30' for l in knife['lines']))
                if p.family!='grbl':
                    with self.assertRaises(ValueError):generate({**sample(id),'mode':'laser'})
    def test_configured_dwell_required_and_exact(self):
        for id in ('mach3','uccnc'):
            for unit,expected in [('seconds','G4 P2'),('milliseconds','G4 P2000')]:
                self.assertIn(expected,generate(sample(id,dwell_units=unit))['lines'])
            for unit in ('',None,True,'guess'):
                with self.assertRaises(ValueError):generate(sample(id,dwell_units=unit))
        for id in ('linuxcnc','mach4'):
            self.assertIn('G4 P2',generate(sample(id))['lines'])
    def test_ordered_jobs_terminate_once_and_reject_mixed_dwell(self):
        for id in ('linuxcnc','mach3','mach4','uccnc'):
            p=sample(id,dwell_units='seconds');p['operations']=[{'paths':p['paths']},{'paths':[[[20,20],[30,30]]]}]
            job=generate(p);self.assertEqual(job['lines'].count('M30'),1);self.assertEqual(job['lines'][-1],'M30')
            self.assertIn('G1 X30.000 Y30.000 F600.0',job['lines'])
            stages=plan_tool_changes(p)['stages'];self.assertEqual(stages[0]['lines'][-2:],['M5','M30'])
            p['operations'][1]['dwell_units']='milliseconds'
            with self.assertRaises(ValueError):generate(p)
            with self.assertRaises(ValueError):plan_tool_changes(p)
    def test_export_cannot_connect_or_run(self):
        c=DeviceController();self.addCleanup(c.disconnect)
        app=create_app(c);client=app.test_client();headers={'X-Studio-Token':client.get('/api/session').json['token']}
        self.assertEqual({p['id'] for p in client.get('/api/protocols').json},set(PROTOCOLS))
        for id in ('linuxcnc','mach3','mach4','uccnc'):
            self.assertEqual(client.post('/api/connect',json={'port':'simulator','protocol':id},headers=headers).status_code,400)
            self.assertEqual(client.post('/api/run',json={'confirmed':True,'project':sample(id,dwell_units='seconds')},headers=headers).status_code,400)
        self.assertFalse(c.status()['connected'])
    def test_aliases_keep_physical_settings_guards(self):
        for id in ('fluidnc','grblhal'):
            c=DeviceController();c.select_protocol(id);self.addCleanup(c.disconnect)
            app=create_app(c);client=app.test_client();headers={'X-Studio-Token':client.get('/api/session').json['token']}
            for settings in ({'30':1000,'32':1},{'30':float('nan'),'32':0},{'30':100,'32':0},{'30':1000}):
                c.driver.settings=settings
                result=client.post('/api/run',json={'confirmed':True,'project':sample(id)},headers=headers)
                self.assertEqual(result.status_code,400)
            self.assertEqual(c.protocol,id)
    def test_fluid_readbacks_acknowledged_before_ready(self):
        c=Controller(initialization_queries=('$30','$32','$I'));c.init_remaining.extend(c.initialization_queries);c.pending='$$'
        with patch.object(c,'_write') as write:
            c._receive('$30=1000');c._receive('ok');self.assertEqual(c.pending,'$30')
            c._receive('ok');self.assertEqual(c.pending,'$32')
            c._receive('$32=0');c._receive('<Idle|MPos:0,0,0>');self.assertFalse(c.ready)
            c._receive('ok');self.assertEqual(c.pending,'$I')
            c._receive('[VER:3.9:FluidNC]');c._receive('ok');c._receive('<Idle|MPos:0,0,0>');self.assertTrue(c.ready)
            self.assertIn('[VER:3.9:FluidNC]',c.status()['identification'])
    def test_bad_readback_and_idle_reboot_invalidate_ready(self):
        for text in ('$30=nan','$32=inf','GrblHAL 1.1f','FluidNC v3.9'):
            c=Controller();c.initialized=True;c.ready=True;c.settings={'30':1000,'32':0};c._receive(text)
            self.assertFalse(c.ready);self.assertIsNotNone(c.error)
    def test_linuxcnc_profile_can_wrap(self):
        job=wrap_job(generate(sample('linuxcnc')),dict(postprocessor='linuxcnc',diameter=20,wrap_axis='X'))
        self.assertTrue(any(' A' in l for l in job['lines']))
