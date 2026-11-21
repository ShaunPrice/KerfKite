"""The machine is described from what its controller reports, and nothing is written to it."""

import time
import unittest
from collections import deque
from unittest.mock import patch

from studio.controller import Controller as Grbl
from studio.device import DeviceController
from studio.machine_setup import describe
from studio.marlin import Controller as Marlin
from studio.server import create_app

GRBL_SETTINGS = {'13': 0, '20': 1, '21': 0, '22': 1, '30': 12000, '31': 0, '32': 0, '110': 5000, '111': 4000,
                 '112': 600, '130': 300, '131': 180, '132': 45}  # fmt: skip


def grbl_status(**changes):
    status = dict(connected=True, simulated=False, error=None, ready=True, protocol='grbl', position_count=3,
                  banner="Grbl 1.1h ['$' for help]", identification=['[VER:1.1h.20190825:]', '[OPT:V,15,128]'],
                  settings=dict(GRBL_SETTINGS))  # fmt: skip
    status.update(changes)
    return status


class DescribeTests(unittest.TestCase):
    def test_grbl_readback_becomes_a_workspace(self):
        machine = describe(grbl_status())
        self.assertTrue(machine['available'])
        self.assertEqual(machine['firmware'], 'Grbl 1.1h.20190825')
        self.assertEqual(machine['name'], 'Grbl 1.1h.20190825 · 300 × 180 mm')
        self.assertEqual(machine['identity'], 'grbl|Grbl 1.1h.20190825|300|180|45')
        self.assertEqual(
            machine['environment'],
            dict(protocol='grbl', width=300, height=180, z_travel=45, feed_max=4000, plunge_max=600, power_max=12000,
                 laser_mode=False),
        )  # fmt: skip
        self.assertEqual((machine['homing'], machine['soft_limits'], machine['hard_limits']), (True, True, False))
        self.assertEqual((machine['missing'], machine['warnings']), ([], []))
        self.assertIn('Z travel 45 mm', machine['reported'])

    def test_what_the_controller_does_not_report_is_listed_and_not_invented(self):
        machine = describe(grbl_status(settings={'30': 1000, '32': 1}, identification=[], banner=None))
        self.assertTrue(machine['available'])
        self.assertEqual(machine['environment'], dict(protocol='grbl', power_max=1000, laser_mode=True))
        self.assertEqual(machine['missing'], ['X travel', 'Y travel', 'Z travel', 'maximum feed'])
        self.assertEqual((machine['name'], machine['identity'], machine['firmware']), (None, None, None))

    def test_values_outside_what_a_workspace_allows_are_not_applied(self):
        settings = {**GRBL_SETTINGS, '130': 2500, '110': 30000, '111': 25000, '13': 1, '132': float('nan')}
        machine = describe(grbl_status(settings=settings))
        self.assertNotIn('width', machine['environment'])
        self.assertEqual(machine['environment']['feed_max'], 10000)
        self.assertIsNone(machine['name'])
        self.assertIn('Z travel', machine['missing'])
        self.assertTrue(any('beyond the 2000 mm' in warning for warning in machine['warnings']))
        self.assertTrue(any('inches' in warning for warning in machine['warnings']))
        tall = describe(grbl_status(settings={**GRBL_SETTINGS, '132': 2400}))
        self.assertNotIn('z_travel', tall['environment'])
        self.assertNotIn('Z travel', tall['missing'])  # it was reported; it is refused, and the warning says so
        self.assertTrue(any('Z travel of 2400 mm is beyond' in warning for warning in tall['warnings']))

    def test_firmware_is_recognised_and_a_wrong_dialect_is_pointed_out(self):
        cases = [
            (dict(identification=['[VER:1.1f.20230414:]', '[FIRMWARE:grblHAL]'], protocol='grblhal'), 'grblHAL 1.1f.20230414', 'grblhal'),
            (dict(identification=['[VER:3.7.8 FluidNC v3.7.8:]'], banner=None, protocol='fluidnc'), '3.7.8 FluidNC v3.7.8', 'fluidnc'),
            (dict(identification=[], banner='FluidNC v3.9.1 [$ for help]', protocol='fluidnc'), 'FluidNC v3.9.1', 'fluidnc'),
            (dict(identification=['[VER:1.3a.20210424:]', '[MSG:Using machine:CNC_xPRO_V5_XYAZ_NC]'], protocol='xpro-v5'), 'Grbl_Esp32 1.3a.20210424 · xPRO V5', 'xpro-v5'),
            (dict(identification=[], banner='Grbl 0.9j [$ for help]'), 'Grbl 0.9j', 'grbl'),
        ]  # fmt: skip
        for changes, firmware, detected in cases:
            with self.subTest(firmware):
                machine = describe(grbl_status(**changes))
                self.assertEqual(
                    (machine['firmware'], machine['detected_protocol'], machine['warnings']), (firmware, detected, [])
                )
        wrong = describe(grbl_status(identification=['[VER:1.1f:]', '[FIRMWARE:grblHAL]']))
        self.assertIn('identifies as grblHAL', wrong['warnings'][0])
        self.assertEqual(wrong['environment']['protocol'], 'grbl')  # the connection is what the workspace follows

    def test_xpro_laser_power_limit_comes_from_its_own_setting(self):
        settings = {**GRBL_SETTINGS, '32': 1, 'Laser/FullPower': 255}
        self.assertEqual(describe(grbl_status(protocol='xpro-v5', settings=settings))['environment']['power_max'], 255)

    def test_marlin_readback(self):
        readback = dict(firmware='Marlin 2.1.2.1', machine_type='CNC', travel=[400.0, 300.0, 80.0], max_feed=[18000.0, 18000.0, 300.0], soft_endstops=True)  # fmt: skip
        machine = describe(
            dict(connected=True, simulated=False, error=None, ready=True, protocol='marlin', readback=readback)
        )
        self.assertEqual(machine['name'], 'Marlin 2.1.2.1 · CNC · 400 × 300 mm')
        self.assertEqual(
            machine['environment'],
            dict(protocol='marlin', width=400, height=300, z_travel=80, feed_max=10000, plunge_max=300),
        )
        self.assertEqual((machine['missing'], machine['warnings'], machine['detected_protocol']), ([], [], 'marlin'))
        loose = describe(
            dict(connected=True, ready=True, protocol='marlin', readback={**readback, 'soft_endstops': False})
        )
        self.assertIn('Soft endstops are off', loose['warnings'][0])
        silent = describe(dict(connected=True, ready=True, protocol='marlin', readback={}))
        self.assertEqual(silent['missing'], ['X travel', 'Y travel', 'Z travel', 'maximum feed'])

    def test_no_description_without_a_ready_machine(self):
        for status, reason, waiting in [
            (dict(connected=False), 'Connect a controller', False),
            (dict(connected=True, simulated=True, ready=True), 'simulator has no machine settings', False),
            (grbl_status(error='ALARM:1'), 'reported a fault', False),
            (grbl_status(ready=False), 'Waiting for the controller', True),
        ]:
            machine = describe(status)
            self.assertEqual((machine['available'], machine['waiting'], machine['environment']), (False, waiting, {}))
            self.assertIn(reason, machine['reason'])


class FakeSerial:
    """A serial port that answers each written line from a table."""

    def __init__(self, answers):
        self.answers, self.writes, self.incoming = answers, [], deque()

    def write(self, data):
        self.writes.append(data)
        for prefix, lines in self.answers.items():
            if prefix in data:
                self.incoming.extend(line.encode('ascii') + b'\n' for line in lines)
        return len(data)

    def readline(self, size=257):
        return self.incoming.popleft() if self.incoming else b''

    def close(self):
        pass


def until(predicate, seconds=8):
    deadline = time.monotonic() + seconds
    while not predicate() and time.monotonic() < deadline:
        time.sleep(0.01)
    return predicate()


class ReadbackTests(unittest.TestCase):
    def test_grbl_family_controllers_are_asked_to_identify_themselves(self):
        answers = {
            b'$$': [f'${key}={value}' for key, value in GRBL_SETTINGS.items()] + ['ok'],
            b'$I': ['[VER:1.1h.20190825:]', '[OPT:V,15,128]', 'ok'],
            b'?': ['<Idle|MPos:0.000,0.000,0.000|FS:0,0>'],
        }
        fake = FakeSerial(answers)
        device = DeviceController()
        device.driver.factory = lambda *a, **k: fake
        self.addCleanup(device.disconnect)
        fake.incoming.append(b"Grbl 1.1h ['$' for help]\n")
        with patch.object(device.driver, 'ports', return_value=[{'device': 'fake'}]):
            device.connect('fake')
        self.assertTrue(until(lambda: device.status()['ready'] and len(device.status()['identification']) == 2))
        self.assertTrue(
            all(not data.startswith(b'$') or data in (b'$$\n', b'$I\n') for data in fake.writes), fake.writes
        )
        machine = describe(device.status())
        self.assertEqual(machine['name'], 'Grbl 1.1h.20190825 · 300 × 180 mm')
        self.assertEqual(device.status()['banner'], "Grbl 1.1h ['$' for help]")
        app = create_app(device).test_client()
        self.assertEqual(app.get('/api/machine').json['environment']['z_travel'], 45)
        self.assertTrue(app.get('/api/session').json['machine_readback'])

    def marlin(self, answers):
        fake = FakeSerial({b'M110': ['ok'], **answers})
        c = Marlin(serial_factory=lambda *a, **k: fake, boot_delay=0)
        self.addCleanup(c.disconnect)
        with patch.object(c, 'ports', return_value=[{'device': 'fake'}]):
            c.connect('fake')
        return c, fake

    def test_marlin_reports_are_read_at_connection(self):
        c, fake = self.marlin({
            b'M115': ['FIRMWARE_NAME:Marlin 2.1.2.1 (Mar  1 2024 10:00:00) SOURCE_CODE_URL:github.com/MarlinFirmware/Marlin PROTOCOL_VERSION:1.0 MACHINE_TYPE:CNC EXTRUDER_COUNT:0', 'Cap:SERIAL_XON_XOFF:0', 'ok'],
            b'M211': ['echo:Soft endstops: ON  Min:  X0.00 Y0.00 Z-80.00   Max:  X400.00 Y300.00 Z0.00', 'ok'],
            b'M503': ['echo:; Maximum feedrates (units/s):', 'echo:  M203 X300.00 Y300.00 Z5.00', 'echo:  M201 X500.00 Y500.00 Z100.00', 'ok'],
        })  # fmt: skip
        self.assertTrue(until(lambda: c.ready))
        self.assertEqual(
            c.status()['readback'],
            dict(firmware='Marlin 2.1.2.1 (Mar  1 2024 10:00:00)', machine_type='CNC', soft_endstops=True,
                 travel=[400.0, 300.0, 80.0], max_feed=[18000.0, 18000.0, 300.0]),
        )  # fmt: skip
        sent = [data.split(b' ', 1)[1].split(b'*')[0] for data in fake.writes]
        self.assertEqual(sent, [b'M110 N0', b'M115', b'M211', b'M503'])  # reports only; nothing is set
        self.assertEqual(describe(c.status())['environment']['width'], 400)

    def test_marlin_without_those_reports_still_connects(self):
        c, fake = self.marlin({
            b'M115': ['echo:Unknown command: "M115"', 'ok'],
            b'M211': ['Error:M211 is not available', 'ok'],
            b'M503': ['x' * 300, 'ok'],
        })  # fmt: skip
        self.assertTrue(until(lambda: c.ready))
        self.assertEqual((c.status()['readback'], c.error, c.state), ({}, None, 'Idle'))

    def test_marlin_that_never_answers_a_report_is_ready_after_a_short_wait(self):
        with patch.object(Marlin, 'QUERY_TIMEOUT', 0.2):
            c, fake = self.marlin({})
            self.assertTrue(until(lambda: c.ready))
        self.assertEqual((c.error, c.state), (None, 'Idle'))
        self.assertIn('The firmware did not report its settings', c.status()['log'])
        self.assertEqual(len(fake.writes), 2)  # the remaining reports are not asked for


class SimulatorTests(unittest.TestCase):
    def test_the_simulator_reports_no_machine(self):
        controller = Grbl()
        self.addCleanup(controller.disconnect)
        controller.connect('simulator')
        app = create_app(controller).test_client()
        machine = app.get('/api/machine').json
        self.assertFalse(machine['available'])
        self.assertIn('simulator has no machine settings', machine['reason'])


if __name__ == '__main__':
    unittest.main()
