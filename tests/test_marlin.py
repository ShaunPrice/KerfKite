import time
import unittest
from collections import deque
from unittest.mock import patch

from studio.marlin import Controller


class FakeSerial:
    def __init__(self, *args, **kwargs):
        self.writes = []
        self.responses = deque()
        self.closed = False

    def write(self, data):
        self.writes.append(data)
        return len(data)

    def readline(self, size=257):
        return self.responses.popleft() if self.responses else b''

    def close(self):
        self.closed = True


class MarlinTests(unittest.TestCase):
    def controller(self):
        c = Controller()
        c.connected = c.ready = True
        c.state = 'Idle'
        c.serial = FakeSerial()
        self.addCleanup(c.disconnect)
        return c

    def until(self, predicate):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.005)
        self.fail('Timed out waiting for fake transport')

    def test_checksum_and_only_one_outstanding_line(self):
        c = self.controller()
        frame = c.frame(1, 'G1 X10')
        payload, checksum = frame.strip().split(b'*')
        expected = 0
        for byte in payload:
            expected ^= byte
        self.assertEqual(int(checksum), expected)
        c.start(['G1 X10', 'M5'])
        c._send(*c.queue.popleft())
        self.assertEqual(c.acked, 0)
        self.assertEqual(len(c.queue), 2)  # Remaining M5 and completion fence.
        c._receive('ok N0 P15 B3')
        self.assertEqual(c.acked, 1)

    def test_completion_requires_m400_ack(self):
        c = self.controller()
        c.start(['G1 X10'])
        c._send(*c.queue.popleft())
        c._receive('ok')
        self.assertTrue(c.active)
        self.assertEqual(c.progress, 99)
        c._send(*c.queue.popleft())
        self.assertIn(b'M400', c.serial.writes[-1])
        self.assertTrue(c.active)
        c._receive('ok')
        self.assertEqual(c.progress, 100)
        self.assertFalse(c.active)

    def test_paused_inflight_ack_does_not_complete_before_fence(self):
        c = self.controller()
        c.start(['G1 X10'])
        c._send(*c.queue.popleft())
        c.command('hold')
        c._receive('ok')  # Buffered work may acknowledge during host pause.
        self.assertTrue(c.active)
        self.assertTrue(c.paused)
        self.assertEqual(c.progress, 99)
        self.assertEqual(len(c.serial.writes), 1)
        self.assertEqual(list(c.queue), [('M400', 'finish')])
        c.command('resume')
        c._send(*c.queue.popleft())
        c._receive('ok')
        self.assertFalse(c.active)
        self.assertEqual(c.progress, 100)

    def test_resend_consumes_handshake_ok_not_job_ack(self):
        c = self.controller()
        c.start(['G1 X10'])
        c._send(*c.queue.popleft())
        original = c.serial.writes[-1]
        c._receive('Error:checksum mismatch, Last Line: -1')
        c._receive('Resend: 0')
        c._receive('ok')  # Marlin resend handshake, not motion acknowledgement.
        self.assertEqual(c.serial.writes, [original, original])
        self.assertEqual(c.acked, 0)
        self.assertIsNotNone(c.pending)
        c._receive('ok N0')
        self.assertEqual(c.acked, 1)

    def test_resend_is_bounded_and_never_replays_old_motion(self):
        c = self.controller()
        c._send('G1 X1', 'job')
        for _ in range(c.MAX_RESENDS):
            c._receive('rs N0')
            c._receive('ok')
        c._receive('Resend: 0')
        self.assertEqual(c.state, 'Fault')
        self.assertEqual(c.serial.writes[-1], b'M112\n')
        c = self.controller()
        c.number = 2
        c._send('G1 X1', 'job')
        c._receive('Resend: 1')
        self.assertEqual(c.state, 'Fault')
        self.assertIn('refusing to replay', c.error)

    def test_commands_use_marlin_and_reset_requires_reconnect(self):
        c = self.controller()
        c.command('jog', axis='X', distance=1, feed=100)
        self.assertEqual([s for s, _ in c.queue], ['G21', 'G91', 'G1 X1.000 F100.0', 'G90', 'M400'])
        c.command('hold')
        self.assertTrue(c.paused)
        self.assertEqual(c.serial.writes, [])
        c.command('resume')
        self.assertFalse(c.paused)
        c.command('reset')
        self.assertEqual(c.serial.writes, [b'M112\n'])
        with self.assertRaises(RuntimeError):
            c.start(['M5'])
        for action, command in [('home', 'G28'), ('zero', 'G92 X0 Y0 Z0')]:
            controller = self.controller()
            controller.command(action)
            self.assertEqual(controller.queue[0][0], command)

    def test_invalid_inputs_and_number_reset_rejected(self):
        c = self.controller()
        for lines in [
            [],
            ['M5\nG1 X1'],
            ['N1 G1 X1*33'],
            ['M110 N0'],
            ['M110N0'],
            ['G1 X1\x18'],
            ['; only comment'],
            ['(comment)'],
            ['G1 X1'] * (c.MAX_LINES + 1),
        ]:
            with self.assertRaises(ValueError):
                c.start(lines)
        for kwargs in [
            dict(axis='A', distance=1, feed=100),
            dict(axis='X', distance=float('nan'), feed=100),
            dict(axis='Z', distance=11, feed=100),
        ]:
            with self.assertRaises(ValueError):
                c.command('jog', **kwargs)

    def test_real_factory_handshake_fragmented_read_timeout(self):
        fake = FakeSerial()
        c = Controller(serial_factory=lambda *a, **k: fake, timeout=60, boot_delay=0)
        self.addCleanup(c.disconnect)
        with patch.object(c, 'ports', return_value=[{'device': 'fake'}]):
            c.connect('fake')
        self.until(lambda: bool(fake.writes))
        self.assertIn(b'N0 M110 N0*', fake.writes[0])
        fake.responses.extend([b'o', b'k\n'])
        self.until(lambda: c.ready)
        c.start(['G1 X1'])
        self.until(lambda: len(fake.writes) == 2)
        with c.lock:
            self.assertEqual(len(fake.writes), 2)
            self.assertEqual(c.pending[2], 'job')
            self.assertEqual(list(c.queue), [('M400', 'finish')])
            c.command('hold')
            # Age the outstanding command explicitly. Scheduler delays must
            # not turn the no-ACK assertion into an accidental timeout race.
            c.sent_at = time.monotonic() - c.timeout - 1
        self.until(lambda: c.state == 'Fault')  # Paused outstanding commands still time out.
        self.assertEqual(fake.writes[-1], b'M112\n')
        self.assertFalse(c.ready)

    def test_simulator_pause_completion_reset(self):
        c = Controller()
        self.addCleanup(c.disconnect)
        c.connect('simulator')
        # Hold before the worker can send a line. Host pause deliberately
        # permits already buffered commands to drain, so racing start/hold
        # is not a valid test of a zero-progress paused queue.
        with c.lock:
            c.start(['G1 X1'] * 20)
            c.command('hold')
        time.sleep(0.04)
        self.assertEqual(c.progress, 0)
        c.command('resume')
        self.until(lambda: not c.active)
        self.assertEqual(c.progress, 100)
        self.assertIsNone(c.status()['position'])
        c.command('reset')
        with self.assertRaises(RuntimeError):
            c.start(['M5'])

    def test_unexpected_reset_or_wrong_ack_aborts(self):
        for response in ['start', 'ok N999', 'Error:Printer halted. kill() called!']:
            c = self.controller()
            c.start(['M5'])
            c._send(*c.queue.popleft())
            c._receive(response)
            self.assertEqual(c.state, 'Fault')
            self.assertFalse(c.active)


if __name__ == '__main__':
    unittest.main()
