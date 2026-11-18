import unittest
import threading
import time
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

    def action(self,action):return self.phone.post('/api/action',json={'action':action,'control_epoch':self.jobs.control_epoch},headers=self.auth)

    def arm_editor(self):
        response = self.local.post('/api/arm', json={'project': sample(), 'confirmed': True,
                                  'phone_arm_challenge': True}, headers=self.local_auth)
        self.assertEqual(response.status_code, 200, response.json)
        return response.json['arm_id']

    def editor(self, path, arm_id, **extras):
        return self.local.post('/api/' + path, json={'arm_id':arm_id, **extras}, headers=self.local_auth)

    def begin_start(self, arm_id):
        results = []
        worker = threading.Thread(target=lambda: results.append(self.action('start')))
        worker.start()
        self.addCleanup(worker.join, 3)
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            challenge = self.editor('arm/check', arm_id).json.get('challenge')
            if challenge:
                return worker, results, challenge
            time.sleep(.005)
        self.fail('Phone Start did not reach the waiting state.')

    def test_local_arm_phone_start_is_single_use(self):
        self.assertEqual(self.action('start').status_code,400)
        arm_id = self.arm_editor()
        self.assertTrue(self.phone.get('/api/status',headers=self.auth).json['armed'])
        worker, results, challenge = self.begin_start(arm_id)
        self.assertFalse(self.ctl.status()['active'])
        self.assertEqual(self.action('start').status_code, 400)
        self.assertEqual(self.editor('arm/confirm', arm_id, challenge=challenge).status_code, 200)
        worker.join(1)
        self.assertEqual(results[0].status_code,200)
        self.assertFalse(self.jobs.status()['armed'])
        self.assertEqual(self.editor('arm/confirm', arm_id, challenge=challenge).status_code, 400)
        self.assertEqual(self.action('start').status_code,400)

    def test_missing_owner_times_out_and_stale_confirm_cannot_start(self):
        self.jobs.confirmation_timeout = .25
        arm_id = self.arm_editor()
        worker, results, challenge = self.begin_start(arm_id)
        self.assertFalse(self.editor('arm/check', 'another-editor').json['armed'])
        self.assertEqual(self.editor('arm/confirm','another-editor',challenge=challenge).status_code,400)
        worker.join(1)
        self.assertEqual(results[0].status_code,400)
        self.assertFalse(self.jobs.status()['armed'])
        self.assertEqual(self.editor('arm/confirm', arm_id, challenge=challenge).status_code,400)
        self.assertFalse(self.ctl.status()['active'])

    def test_new_arm_survives_delayed_disarm_from_previous_page(self):
        first = self.arm_editor()
        worker, results, challenge = self.begin_start(first)
        replacement = self.arm_editor()
        worker.join(1)
        self.assertEqual(results[0].status_code,400)
        self.assertNotEqual(first,replacement)
        self.assertFalse(self.editor('disarm', first).json['removed'])
        self.assertEqual(self.editor('arm/confirm', replacement, challenge=challenge).status_code,400)
        self.assertTrue(self.editor('arm/check', replacement).json['armed'])
        self.assertFalse(self.ctl.status()['active'])

    def test_stop_cancels_start_while_editor_has_not_answered(self):
        arm_id = self.arm_editor()
        worker, results, challenge = self.begin_start(arm_id)
        started = time.monotonic()
        self.assertEqual(self.action('stop').status_code,200)
        self.assertLess(time.monotonic()-started, self.jobs.confirmation_timeout)
        worker.join(1)
        self.assertEqual(results[0].status_code,400)
        self.assertEqual(self.editor('arm/confirm', arm_id, challenge=challenge).status_code,400)
        self.assertFalse(self.ctl.status()['active'])

    def test_delayed_start_and_jog_after_stop_use_outdated_epoch(self):
        from uuid import uuid4
        old = self.phone.get('/api/status',headers=self.auth).json['control_epoch']
        self.assertNotIn('control_epoch', self.local.get('/api/status').json)
        self.assertEqual(self.phone.post('/api/action',json={'action':'stop'},headers=self.auth).status_code,200)
        current = self.phone.get('/api/status',headers=self.auth).json['control_epoch']
        self.assertNotEqual(old, current)
        self.arm_editor()
        self.assertEqual(self.phone.post('/api/action',json={'action':'start','control_epoch':old},
                                         headers=self.auth).status_code,400)
        jog = {'action':'jog','axis':'X','distance':.1,'feed':20,'request_id':str(uuid4()),'control_epoch':old}
        self.assertEqual(self.phone.post('/api/pendant',json=jog,headers=self.auth).status_code,400)
        self.assertTrue(self.jobs.status()['armed'])
        self.assertFalse(self.ctl.status()['active'])
        self.assertNotIn('arm_id',self.phone.get('/api/status',headers=self.auth).json)
        legacy=self.phone.post('/api/action',json={'action':'resume'},headers=self.auth)
        self.assertEqual(legacy.status_code,400)
        self.assertIn('Update',legacy.json['error'])

    def test_revoking_pairing_cancels_outstanding_start(self):
        from unittest.mock import Mock
        from studio.phone_monitor_service import PhoneMonitor
        service = PhoneMonitor(self.ctl,self.jobs)
        service.listener, service.worker, service.revoked = Mock(), Mock(), threading.Event()
        arm_id = self.arm_editor()
        worker, results, challenge = self.begin_start(arm_id)
        service.disable()
        worker.join(1)
        self.assertEqual(results[0].status_code,400)
        self.assertEqual(self.editor('arm/confirm',arm_id,challenge=challenge).status_code,400)
        self.assertFalse(self.ctl.status()['active'])

    def test_absent_owner_expires_and_late_heartbeat_cannot_revive_preview(self):
        arm_id = self.arm_editor()
        with self.jobs.lock:
            self.jobs.owner_seen -= 9
        self.assertFalse(self.editor('arm/check',arm_id).json['armed'])
        self.assertFalse(self.phone.get('/api/status',headers=self.auth).json['armed'])
        self.assertEqual(self.action('start').status_code,400)

    def test_machine_revalidated_after_owner_confirms(self):
        arm_id = self.arm_editor()
        worker, results, challenge = self.begin_start(arm_id)
        with self.ctl.lock:
            self.ctl.state = 'Alarm'
        self.assertEqual(self.editor('arm/confirm', arm_id, challenge=challenge).status_code,200)
        worker.join(1)
        self.assertEqual(results[0].status_code,400)
        self.assertFalse(self.jobs.status()['armed'])
        self.assertFalse(self.ctl.status()['active'])

    def test_legacy_editor_neither_arms_nor_disarms_another_tab(self):
        self.assertEqual(self.local.post('/api/arm',json={'project':sample(),'confirmed':True},
                                         headers=self.local_auth).status_code,400)
        arm_id = self.arm_editor()
        self.assertEqual(self.local.post('/api/disarm',json={},headers=self.local_auth).status_code,400)
        self.assertTrue(self.editor('arm/check', arm_id).json['armed'])

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
        arm_id = self.arm_editor()
        self.assertTrue(self.editor('disarm',arm_id).json['removed'])
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
