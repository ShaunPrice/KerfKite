"""Coordinated cylindrical toolpaths and the normal simulator/phone job guards."""
import math
import time
import unittest

from studio.controller import Controller
from studio.device import DeviceController
from studio.rotary import controller_rotary_axes, generate_rotary, validate_rotary_machine
from studio.server import create_app


def project(post='grblhal', mode='router'):
    return dict(mode=mode, protocol=post, operation='line', width=100, height=100, feed=600,
                power=800, safe=3, depth=2, step=2, plunge=100, passes=1, stock_thickness=4,
                paths=[[[0,5],[5*math.pi,5],[10*math.pi,5]]])


def options(post='grblhal', **fields):
    return dict(postprocessor=post, diameter=10, wrap_axis='X', **fields)


class RotaryToolpathTests(unittest.TestCase):
    def test_laser_power_full_turn_and_axis_mapping(self):
        p = project(mode='laser')
        p['path_powers'] = [[0,200,650]]
        for drive, axial, radial in [('A','X','Z'),('B','Y','Z'),('C','Z','X')]:
            with self.subTest(axis=drive):
                job = generate_rotary(p,options(output_axis=drive,angle_offset=30,direction=-1))
                meta = job['rotary']
                self.assertEqual(meta['angle_bounds'], [-330,30])
                self.assertEqual((meta['axial_output_axis'], meta['radial_output_axis']),(axial,radial))
                self.assertIn('M4 S0',job['lines'])
                cut = [line for line in job['lines'] if line.startswith('G1 ')]
                self.assertEqual([line.split()[-1] for line in cut],['S200','S650'])
                self.assertIn(drive+'-330.000000',cut[-1])
                self.assertIn(axial+'5.000000',cut[-1])
                self.assertEqual(job['lines'][-2],'M5')

    def test_actual_cut_radius_and_simultaneous_depth(self):
        p = project()
        p['path_depths'] = [[-1,-1,-2]]
        surface = generate_rotary(p,options())
        cut = generate_rotary(p,options(feed_reference='cut',output_axis='C'))
        self.assertIn('X-2.000000',next(l for l in cut['lines'] if l.startswith('G1 C360.')))
        inv = lambda l: float(next(v[1:] for v in l.split() if v.startswith('F')))
        # Unwrapped source coordinates round to .001 mm. Surface radius is 5.
        surface_feed = inv(next(l for l in surface['lines'] if l.startswith('G1 A') and 'A360' not in l))
        cut_feed = inv(next(l for l in cut['lines'] if l.startswith('G1 C') and 'C360' not in l))
        self.assertAlmostEqual(cut_feed / surface_feed, 5/4, places=7)
        # Integrate the changing radius independently using midpoint samples.
        theta = (round(10*math.pi,3)-round(5*math.pi,3))/5
        length = sum(math.hypot(1,theta*(4-(i+.5)/10000)) for i in range(10000))/10000
        self.assertAlmostEqual(inv(next(l for l in cut['lines'] if l.startswith('G1 C360.'))),100/length,places=6)

    def test_xpro_rapids_use_units_per_minute(self):
        job = generate_rotary(project('xpro-v5'),options('xpro-v5'))
        mode = None
        for line in job['lines']:
            if line.startswith(('G93','G94')): mode = line[:3]
            if line.startswith('G0 '): self.assertEqual(mode,'G94')
            if line.startswith('G1 '): self.assertEqual(mode,'G93')
        self.assertTrue(any('A360.000000' in line for line in job['lines']))

    def test_fixture_and_unsupported_axes(self):
        p = project()
        for fixture in [{'axial_min':6,'axial_max':20,'min_radius':0}, {'axial_min':0,'axial_max':8,'min_radius':4},
                        {'axial_min':10,'axial_max':8,'min_radius':0}, {'axial_min':0,'axial_max':9}]:
            with self.subTest(fixture=fixture), self.assertRaises(ValueError):
                generate_rotary(p,options(fixture=fixture))
        for post, drive in [('xpro-v5','B'),('grbl','A'),('grblhal','U'),('marlin','A')]:
            with self.subTest(post=post,drive=drive), self.assertRaises(ValueError):
                generate_rotary(p,options(post,output_axis=drive))
        with self.assertRaises(ValueError):generate_rotary(project('marlin'),options())


class RotaryMachineTests(unittest.TestCase):
    def test_controller_axes_require_current_readback(self):
        driver = Controller()
        driver.connected = True
        driver._receive('[FIRMWARE:grblHAL]')
        driver._receive('[AXS:5:XYZAB]')
        driver._receive('$376=2')
        for key in (103,104,113,114,123,124,130,131,132,133,134):
            driver._receive(f'${key}=1000')
        driver._receive('<Idle|WPos:0,0,0,0,15>')
        state = driver.status() | {'protocol':'grblhal'}
        self.assertEqual(controller_rotary_axes('grblhal', state), ['B'])
        self.assertEqual(state['position']['b'],15)
        meta = generate_rotary(project(), options(output_axis='B'))['rotary']
        validate_rotary_machine(meta,state)
        with self.assertRaises(ValueError):
            validate_rotary_machine(meta, state | {'settings':state['settings'] | {'20':1,'134':100}})
        driver._receive('<Idle|WPos:0,0,0>')
        self.assertEqual(controller_rotary_axes('grblhal',driver.status()),[])
        driver._receive('[MSG:Using machine:CNC_xPRO_V5_XYYZ_NC]')
        driver._receive('<Idle|WPos:0,0,0,0>')
        self.assertEqual(controller_rotary_axes('xpro-v5',driver.status()),[])
        driver._receive('[MSG:Using machine:CNC_xPRO_V5_XYAZ_NC]')
        self.assertEqual(controller_rotary_axes('xpro-v5',driver.status()),['A'])
        driver._receive('Grbl 1.1')
        self.assertEqual(controller_rotary_axes('xpro-v5',driver.status()),[])

    def test_stock_check_includes_start_of_ascending_cut_for_both_posts(self):
        ctl = DeviceController()
        ctl.select_protocol('grblhal')
        self.addCleanup(ctl.disconnect)
        driver = ctl.driver
        driver.connected = True
        for line in ['[FIRMWARE:grblHAL]','[AXS:4:XYZA]','<Idle|WPos:0,0,0,0>','$376=1',
                     '$30=1000','$32=0','$103=50','$113=720','$123=90','$130=500','$132=100']:
            driver._receive(line)
        client = create_app(ctl).test_client()
        token = client.get('/api/session').json['token']
        p = project() | {'paths':[[[0,5],[10,5]]],'path_depths':[[-4,-1]],'depth':4,'step':4,'stock_thickness':2}
        for setup in [{},{'rotary':options(),'rotary_confirmed':True}]:
            with self.subTest(rotary=bool(setup)):
                response = client.post('/api/run',headers={'X-Studio-Token':token},
                                       json={'project':p,'confirmed':True,**setup})
                self.assertEqual(response.status_code,400)
                self.assertIn('Cut exceeds entered stock thickness',response.json['error'])
                self.assertFalse(driver.active)
        driver.connected = False

    def test_xpro_laser_uses_firmware_laser_power_scale(self):
        ctl = DeviceController()
        ctl.select_protocol('xpro-v5')
        self.addCleanup(ctl.disconnect)
        drv = ctl.driver
        drv.connected = True
        for line in ['[MSG:Using machine:CNC_xPRO_V5_XYAZ_NO]', '<Idle|WPos:0,0,0,0>', '$32=1',
                     '$30=20000','$103=10','$113=360','$123=90','$130=500','$132=100', '$Laser/FullPower=255']:
            drv._receive(line)
        client = create_app(ctl).test_client()
        headers = {'X-Studio-Token':client.get('/api/session').json['token']}
        payload = dict(project=project('xpro-v5','laser'),rotary=options('xpro-v5'),confirmed=True,rotary_confirmed=True)
        result = client.post('/api/run',headers=headers,json=payload)
        self.assertIn('Spindle/Type',result.json['error'])
        drv._receive('$Spindle/Type=LASER')
        result = client.post('/api/run',headers=headers,json=payload)
        self.assertIn('full-power limit',result.json['error'])
        self.assertFalse(drv.active)
        drv.connected = False

    def test_streamed_simulator_router_and_laser_and_normal_confirmations(self):
        for protocol,mode in [('grblhal','router'),('xpro-v5','laser')]:
            with self.subTest(protocol=protocol):
                ctl = DeviceController()
                self.addCleanup(ctl.disconnect)
                client = create_app(ctl).test_client()
                session = client.get('/api/session').json
                self.assertTrue(session['rotary_stream'])
                headers = {'X-Studio-Token':session['token']}
                post = lambda route, payload: client.post('/api/'+route,json=payload,headers=headers)
                self.assertEqual(post('connect',{'port':'simulator','protocol':protocol}).status_code,200)
                self.assertIn('A',client.get('/api/status').json['rotary_axes'])
                p = project(protocol,mode)
                if mode == 'laser':p['path_powers']=[[0,75,305]]
                else:p['path_depths']=[[-.5,-1,-2]]
                opts = options(protocol)
                payload = {'project':p,'rotary':opts,'confirmed':True,'rotary_confirmed':True}
                preview = post('rotary',{'project':p,'options':opts}).json
                self.assertFalse(ctl.status()['active'])
                for edit in [{'confirmed':False},{'rotary_confirmed':False},{'rotary':options('linuxcnc')},
                             {'project':{'lines':['G1 X1']}},{'rotary':dict(opts,postprocessor='grbl')}]:
                    self.assertEqual(post('run',payload | edit).status_code,400)
                self.assertEqual(post('arm',payload | {'phone_arm_challenge':True,'rotary_confirmed':False}).status_code,400)
                arm = post('arm',payload | {'phone_arm_challenge':True})
                self.assertEqual(arm.status_code,200,arm.json)
                post('disarm',{'arm_id':arm.json['arm_id']})
                self.assertEqual(post('run',payload).status_code,200)
                deadline = time.monotonic()+3
                while ctl.status()['active'] and time.monotonic()<deadline:
                    time.sleep(.01)
                state = ctl.status()
                self.assertEqual(state['state'],'Idle')
                self.assertEqual(state['progress'],100)
                self.assertEqual(state['position']['a'],360)
                self.assertEqual(state['log'][-len(preview['lines']):],preview['lines'])
                self.assertIn('S305' if mode=='laser' else 'Z-2.000000',' '.join(state['log']))
                ctl.disconnect()
