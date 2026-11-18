import unittest
from studio.controller import Controller
from studio.phone_jobs import PhoneJobs
from studio.monitor import create_monitor_app
from studio.server import create_app
from test_cam import sample


class PhoneJobTests(unittest.TestCase):
    def setUp(self):
        self.ctl=Controller();self.addCleanup(self.ctl.disconnect)
        self.jobs=PhoneJobs(self.ctl)
        self.phone=create_monitor_app(self.ctl,'x'*43,self.jobs).test_client()
        self.auth={'Authorization':'Bearer '+'x'*43}
        self.local=create_app(self.ctl,self.jobs).test_client()
        self.local_auth={'X-Studio-Token':self.local.get('/api/session').json['token']}
        self.ctl.connect('simulator')

    def action(self,action):return self.phone.post('/api/action',json={'action':action},headers=self.auth)

    def test_local_arm_phone_start_is_single_use(self):
        self.assertEqual(self.action('start').status_code,400)
        r=self.local.post('/api/arm',json={'project':sample(),'confirmed':True},headers=self.local_auth)
        self.assertEqual(r.status_code,200)
        self.assertTrue(self.phone.get('/api/status',headers=self.auth).json['armed'])
        self.assertEqual(self.action('start').status_code,200)
        self.assertFalse(self.jobs.status()['armed'])
        self.assertEqual(self.action('start').status_code,400)

    def test_pause_resume_abort(self):
        self.jobs.arm(['M5']*100);self.jobs.start()
        self.assertEqual(self.action('pause').status_code,200)
        self.assertEqual(self.ctl.status()['state'],'Hold')
        self.assertEqual(self.action('resume').status_code,200)
        self.assertEqual(self.action('stop').status_code,200)
        self.assertFalse(self.ctl.status()['active'])
        self.assertEqual(self.action('resume').status_code,400)

    def test_unauthenticated_and_arbitrary_actions_rejected(self):
        self.assertEqual(self.phone.post('/api/action',json={'action':'stop'}).status_code,401)
        self.assertEqual(self.action('home').status_code,400)
        self.assertEqual(self.action('G1 X100').status_code,400)

    def test_disarm_and_confirmation_required(self):
        self.assertEqual(self.local.post('/api/arm',json={'project':sample(),'confirmed':False},headers=self.local_auth).status_code,400)
        self.jobs.arm(['M5'])
        self.local.post('/api/disarm',json={},headers=self.local_auth)
        self.assertFalse(self.jobs.status()['armed'])

class ValidatorTests(unittest.TestCase):
 def test_validator_is_bound_to_armed_job_and_failed_start_consumes_it(self):
  from studio.controller import Controller
  from studio.phone_jobs import PhoneJobs
  ctl=Controller();ctl.connect('simulator')
  try:
   jobs=PhoneJobs(ctl)
   def invalid():raise ValueError('Changed setup')
   jobs.arm(['M5'],validator=invalid)
   with self.assertRaises(ValueError):jobs.start()
   self.assertFalse(jobs.status()['armed']);self.assertFalse(ctl.status()['active'])
  finally:ctl.disconnect()
