import time
import unittest
from unittest.mock import patch
from studio.controller import Controller


class ControllerTests(unittest.TestCase):
    def test_simulator_lifecycle(self):
        c=Controller();self.addCleanup(c.disconnect)
        with self.assertRaises(RuntimeError):c.start(['M5'])
        c.connect('simulator');c.start(['M5']*100)
        with self.assertRaises(RuntimeError):c.command('zero')
        c.command('hold');before=c.status()['progress'];time.sleep(.04)
        self.assertEqual(c.status()['progress'],before)
        c.command('reset');self.assertFalse(c.status()['active'])
        c.start(['M5'])
        deadline=time.monotonic()+5
        while c.status()['active'] and time.monotonic()<deadline:time.sleep(.005)
        self.assertEqual(c.status()['progress'],100)
        self.assertEqual(c.status()['state'],'Idle')

    def test_ack_is_not_completion(self):
        c=Controller();c.active=True;c.total=1;c.pending='M5'
        # Windows and other clocks can report one tick for several serial
        # responses. Completion follows message order, not clock resolution.
        with patch('studio.controller.time.monotonic', return_value=42.0):
            c._receive('<Idle|MPos:1,2,3>');self.assertTrue(c.active)
            c._receive('ok')
            self.assertTrue(c.active);self.assertLess(c.progress,100)
            c._receive('<Run|MPos:1,2,3>');self.assertTrue(c.active)
            c._receive('<Idle|MPos:1,2,3>');self.assertFalse(c.active)
        self.assertEqual(c.progress,100)
        self.assertEqual(c.position['basis'],'MPos')

    def test_error_discards_queue_and_latches_fault(self):
        c=Controller();c.connected=c.ready=True;c.state='Idle';c.last_status=time.monotonic()
        c.start(['M5','G1 X10']);c._receive('error:20')
        self.assertEqual(len(c.queue),0);self.assertFalse(c.active)
        c._receive('<Idle|MPos:0,0,0>')
        with self.assertRaises(RuntimeError):c.start(['M5'])

    def test_reset_banner_aborts(self):
        c=Controller();c.active=True;c._receive('Grbl 1.1h [help]')
        self.assertIsNotNone(c.error)
        self.assertFalse(c.active)

    def test_invalid_jogs(self):
        c=Controller();self.addCleanup(c.disconnect);c.connect('simulator')
        for axis,distance,feed in [('A',1,100),('X',float('nan'),100),('Y',11,100),('Z',1,1001)]:
            with self.subTest(axis=axis),self.assertRaises(ValueError):c.command('jog',axis=axis,distance=distance,feed=feed)

    def test_settings_and_status_required(self):
        c=Controller();c.connected=True;c.state='Idle';c.last_status=time.monotonic()
        c._receive('$30=1000');c._receive('$32=0');c.pending='$$'
        c._receive('<Idle|MPos:0,0,0>');self.assertFalse(c.ready)
        c._receive('ok');c._receive('<Idle|MPos:0,0,0>');self.assertTrue(c.ready)
