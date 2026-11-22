"""The native core's reader of foreign drawings against the Python reference, to the bit."""

import json
import unittest
from unittest.mock import patch

from studio import program_import
from studio.program_import import import_json, import_program
from studio.server import create_app

try:
    from studio import _relief_native as native
except ImportError:
    native = None

AVAILABLE = native is not None and hasattr(native, 'core_cam')

GCODE = '\n'.join([
    'G21 G90 (millimetres)',
    'T2 M6',
    'G0 X0 Y0 Z5',
    'G1 Z-1.5 F300',
    'G1 X20 Y0 F1200.123456',
    'G2 X30 Y10 I0 J10',
    'G3 X20 Y20 R10',
    'G1 X0 Y20 Z-2',
    'G0 Z5',
    'G0 X40 Y40',
    'G1 X45 Y45 F250',
    'M3 S12000',
    'G4 P0.5',
    'M30',
])  # fmt: skip
HPGL = 'IN;SP1;VS10;VS2.5,2;FS40;PU0,0;PD400,0,400,400;PU;AA200,200,90;SP2;PU800,800;PD;PR40,0,0,40;CI80;PU;'
INI = '\n'.join([
    '[TRAJ]',
    'LINEAR_UNITS = inch',
    'COORDINATES = X Y Z',
    '[AXIS_X]',
    'MIN_LIMIT = -0.5',
    'MAX_LIMIT = 24.5 ; travel',
    'MAX_VELOCITY = 2.5',
    '[AXIS_Y]',
    'MIN_LIMIT = 0',
    'MAX_LIMIT = 1.8E1',
    'MAX_ACCELERATION = 30',
])  # fmt: skip


def outcome(payload, backend):
    """The answer as text that tells every bit, or the error; None when the core declines."""
    try:
        return json.dumps(import_program(payload, backend=backend), sort_keys=True)
    except ValueError as error:
        return 'ValueError: ' + str(error)
    except NotImplementedError:
        return None


@unittest.skipUnless(AVAILABLE, 'the compiled extension is not installed')
class NativeProgramImportTests(unittest.TestCase):
    def same(self, payload):
        native_answer = outcome(payload, 'native')
        self.assertIsNotNone(native_answer, 'the core declined')
        self.assertEqual(native_answer, outcome(payload, 'python'))

    def test_drawings_are_read_as_the_reference_reads_them(self):
        for name, text, unit in (
            ('part.nc', GCODE, 'mm'),
            ('part.NGC', GCODE.replace('G21 G90', 'G90'), 'in'),
            ('part.tap', GCODE.replace('G21 G90', 'G20 G91.1'), 'mm'),
            ('cutter.plt', HPGL, 'mm'),
            ('cutter.HPGL', HPGL.replace('IN;', ''), 'ft'),
            ('machine.ini', INI, 'mm'),
            ('machine.INI', INI.replace('inch', 'mm').replace('1.8E1', '450'), 'mm'),
            ('dense.plt', 'IN;SP1;' + ''.join(f'PU{4 * i},0;PD{4 * i + 2},4;' for i in range(150)), 'mm'),
            ('dense.nc', 'G21\n' + '\n'.join(f'G0 X{i} Y0\nG1 Z-1 F100\nG1 Y3\nG0 Z2' for i in range(120)), 'mm'),
            ('dir/sub/part.cnc', '﻿G21\r\nG0 X0 Y0\r\nG1 X3 Y4 F100\r\n', 'cm'),
        ):
            with self.subTest(name=name):
                self.same({'name': name, 'text': text, 'unit': unit})

    def test_refusals_carry_the_reference_messages(self):
        for payload in (
            {'name': 'part.nc', 'text': 'G21\nG1 X1 Y1\n', 'unit': 'yard'},
            {'name': 'part.nc', 'text': ''},
            {'name': 'part.nc', 'text': 'G1 X1\0'},
            {'name': 'part.svg', 'text': 'G1 X1'},
            {'name': 'part.', 'text': 'G1 X1'},
            {'name': 'x' * 1025 + '.nc', 'text': 'G1 X1'},
            {'name': 7, 'text': 'G1 X1'},
            {'name': 'part.nc', 'text': 'G1 X1', 'unit': None},
            [],
            {'name': 'part.nc', 'text': 'G21\nG0 X0 Y0\nG2 X10 Y0 I3 J0'},
            {'name': 'part.nc', 'text': 'G21\nG1 X1 X2'},
            {'name': 'part.nc', 'text': 'G21\nG0 X0 Y0\nG1 X' + '9' * 400},
            {'name': 'part.nc', 'text': 'M30\nG1 X1'},
            {'name': 'part.nc', 'text': 'G21 G0 X0 Y0\nG18'},
            {'name': 'part.nc', 'text': 'G21\nG0 X0 Y0\nG1 Z-1 F100'},
            {'name': 'part.plt', 'text': 'IN;SP1;LB text;'},
            {'name': 'part.plt', 'text': ';'.join(['IN', 'SP1'] + [f'VS{v}' for v in range(17)])},
            {'name': 'part.plt', 'text': 'IN;SP1;PU0,0;PD90000,0;'},
            {'name': 'machine.ini', 'text': '#INCLUDE other.inc\n[TRAJ]\nLINEAR_UNITS = mm'},
            {'name': 'machine.ini', 'text': '[TRAJ]\nLINEAR_UNITS = mm\n[AXIS_X]\nMIN_LIMIT = 0\nMAX_LIMIT = 0'},
            {'name': 'machine.ini', 'text': '[TRAJ]\nLINEAR_UNITS = furlong'},
        ):
            with self.subTest(payload=payload):
                self.same(payload)

    def test_dxf_and_text_that_is_not_ascii_are_answered_by_the_reference(self):
        for payload, expected in (
            ({'name': 'part.dxf', 'text': '0\nEOF\n'}, 'ValueError'),
            ({'name': 'part.nc', 'text': 'G21 (Ä)\nG0 X0 Y0\nG1 X3 Y4 F100'}, '{'),
        ):
            with self.subTest(payload=payload):
                self.assertIsNone(import_json(payload))
                with self.assertRaises(NotImplementedError):
                    import_program(payload, backend='native')
                self.assertTrue(outcome(payload, 'auto').startswith(expected))
                self.assertEqual(outcome(payload, 'auto'), outcome(payload, 'python'))

    def test_the_route_sends_the_core_text(self):
        client = create_app().test_client()
        headers = {'X-Studio-Token': client.get('/api/session').json['token']}
        payload = {'name': 'part.nc', 'text': GCODE, 'unit': 'mm'}
        with patch.object(program_import, 'import_program', wraps=program_import.import_program) as reference:
            response = client.post('/api/program-import', json=payload, headers=headers)
        reference.assert_not_called()
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.get_data(), import_json(payload))
        self.assertEqual(response.get_json(), import_program(payload, backend='python'))
        failing = client.post('/api/program-import', json={'name': 'part.svg', 'text': 'G1'}, headers=headers)
        self.assertEqual(failing.status_code, 400)
        self.assertEqual(failing.get_json()['error'], 'Choose an ASCII DXF, G-code or HP-GL drawing')
        declined = client.post(
            '/api/program-import', json={'name': 'part.nc', 'text': 'G21 (é)\nG0 X0 Y0\nG1 X1 Y1'}, headers=headers
        )
        self.assertEqual(declined.status_code, 200)
        self.assertEqual(
            declined.get_json(),
            import_program({'name': 'part.nc', 'text': 'G21 (é)\nG0 X0 Y0\nG1 X1 Y1'}, backend='python'),
        )

    def test_backend_names_are_checked(self):
        with self.assertRaisesRegex(ValueError, 'auto, python or native'):
            import_program({'name': 'a.nc', 'text': 'G1'}, backend='fast')


if __name__ == '__main__':
    unittest.main()
