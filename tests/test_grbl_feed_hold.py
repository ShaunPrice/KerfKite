"""Exercise the GRBL serial loop against scripted firmware responses and a virtual clock."""
import time
import unittest
from unittest.mock import Mock, patch

from studio.controller import Controller


class FeedHoldTests(unittest.TestCase):
    def setUp(self):
        self.controller = Controller()
        self.controller.connected = True
        self.controller.serial = Mock()
        self.now = 10.0

    def run_serial(self, steps, lines=None):
        c = self.controller
        def start():
            c.start(['M5'] if lines is None else lines)
        # The real driver requests $$ after boot and needs both settings plus Idle.
        sequence = iter([(2.1, ''), (.1, '$30=1000'), (.1, '$32=0'), (.1, 'ok'),
                         (.1, '<Idle>'), (.1, '<Run>', start), *steps])

        def next_response(_):
            step = next(sequence, None)
            if step is None:
                return True
            elapsed, response, *actions = step
            self.now += elapsed
            c.serial.readline.return_value = (response + '\n').encode('ascii') if response else b''
            for action in actions:
                if action == 'hold' or action == 'resume':
                    c.command(action)
                else:
                    action()
            return False

        c.stop.wait = next_response
        with patch('studio.controller.time.monotonic', side_effect=lambda: self.now):
            c._loop()
        self.writes = [call.args[0] for call in c.serial.write.call_args_list]

    def assert_finished(self):
        state = self.controller.status()
        self.assertIsNone(state['error'])
        self.assertEqual((state['state'],state['progress'],state['active']), ('Idle',100,False))
        self.assertNotIn(b'\x18', self.writes)

    def test_pending_ack_survives_confirmed_hold_and_resumes(self):
        self.run_serial([(1,'<Hold:1>','hold'), (1,'<Hold:0>'), *[(1,'<Hold:0>')]*20,
                         (.1,'<Run>','resume'), (1,'ok'), (1,'<Idle>')])
        self.assert_finished()
        self.assertEqual(self.writes.count(b'M5\n'), 1)

    def test_drain_before_long_hold_waits_for_idle(self):
        self.run_serial([(1,'ok'), (1,'<Hold:0>','hold'), *[(1,'<Hold:0>')]*125,
                         (.1,'<Run>','resume'), (1,'<Idle>')])
        self.assert_finished()

    def test_ack_during_long_hold_starts_new_drain_interval(self):
        self.run_serial([(1,'<Hold:0>','hold'), *[(1,'<Hold:0>')]*20, (1,'ok'),
                         *[(1,'<Hold:0>')]*125, (.1,'<Run>','resume'),
                         *[(1,'<Run>')]*119, (.1,'<Idle>')])
        self.assert_finished()

    def test_resume_keeps_remaining_completion_interval(self):
        self.run_serial([(1,'ok'), *[(1,'<Run>')]*110, (1,'<Hold:0>','hold'),
                         *[(1,'<Hold:0>')]*125, (.1,'<Run>','resume'), *[(1,'<Run>')]*10])
        self.assertIn('Completion timeout', self.controller.error)
        self.assertIn(b'\x18', self.writes)

    def test_resume_keeps_only_remaining_ack_interval(self):
        self.run_serial([*[(1,'<Run>')]*10, (1,'<Hold:0>','hold'), *[(1,'<Hold:0>')]*18,
                         (.1,'<Run>','resume'), *[(1,'<Run>')]*5])
        self.assertIn('Acknowledgement timeout', self.controller.error)
        self.assertIn(b'\x18', self.writes)

    def test_late_hold_cannot_mask_pending_ack_timeout(self):
        self.run_serial([(1,'<Hold:1>','hold'), *[(1,'<Hold:1>')]*14, (1,'<Hold:0>')])
        self.assertIn('Acknowledgement timeout', self.controller.error)
        self.assertNotIn(b'~', self.writes)

    def test_unconfirmed_hold_times_out_with_no_ack_pending(self):
        self.run_serial([(.1,'ok','hold'), *[(1,'<Run>')]*16], lines=['M5','M5'])
        self.assertIn('Feed hold was not confirmed', self.controller.error)
        self.assertEqual(self.writes.count(b'M5\n'), 1)

    def test_stale_link_during_hold_rejects_late_status(self):
        self.run_serial([(1,'<Hold:0>','hold'), (4,'<Hold:0>')])
        self.assertIn('Status timeout', self.controller.error)
        self.assertIn(b'\x18', self.writes)

    def test_decelerating_resume_is_rejected_and_queued_motion_waits_for_run(self):
        observed = []
        def cannot_resume():
            observed.append(self.controller.state)
            with self.assertRaisesRegex(RuntimeError, 'fully stopped'):
                self.controller.command('resume')
        def capture():
            observed.append((self.controller.state, self.controller.queue.copy()))
        self.run_serial([
            (.1,'<Hold:1>','hold'), (.1,'ok',cannot_resume), (.1,'<Hold:0>'),
            (.1,'<Hold:0>','resume'), (.1,'<Hold:0>',capture), (.1,'<Run>',capture),
            (.1,'ok'), (.1,'<Idle>')], lines=['M5','M5'])
        self.assertEqual(observed[0], 'Decelerating')
        self.assertEqual([(state,list(lines)) for state,lines in observed[1:]],
                         [('Resuming',['M5']),('Resuming',['M5'])])
        self.assertEqual(self.writes.count(b'~'),1)
        self.assert_finished()

    def test_unacknowledged_resume_cannot_send_the_next_line(self):
        self.run_serial([(.1,'<Hold:0>','hold'),(.1,'ok'),(.1,'<Hold:0>','resume'),
                         *[(1,'<Hold:0>')]*16],lines=['M5','M5'])
        self.assertIn('Resume was not confirmed', self.controller.error)
        self.assertEqual(self.writes.count(b'M5\n'),1)

    def test_idle_feed_hold_clears_before_the_next_job(self):
        c = self.controller
        c.ready, c.state, c.last_status = True, 'Idle', time.monotonic()
        c.command('hold')
        self.assertEqual(c.state,'Hold requested')
        with self.assertRaisesRegex(RuntimeError,'idle'):
            c.start(['M5'])
        c._receive('<Idle>')
        self.assertFalse(c.paused)
        c.start(['M5'])
        self.assertTrue(c.active)
        self.assertFalse(c.paused)

    def test_stale_resume_writes_no_cycle_start(self):
        results = []
        def try_resume():
            with self.assertRaises(RuntimeError):
                self.controller.command('resume')
            results.append(self.controller.error)
        self.run_serial([(1,'<Hold:0>','hold'), (4,'<Hold:0>',try_resume)])
        self.assertIn('Status timeout', results[0])
        self.assertNotIn(b'~', self.writes)

    def test_alarm_during_confirmed_hold_aborts(self):
        self.run_serial([(1,'<Hold:0>','hold'), (1,'ALARM:1')])
        self.assertEqual(self.controller.error, 'ALARM:1')
        self.assertEqual(self.controller.state, 'Fault')
        self.assertIn(b'\x18', self.writes)
