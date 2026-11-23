"""The native core's reader of foreign drawings against the Python reference, to the bit."""

import io
import json
import math
import unittest
from unittest.mock import patch

import ezdxf
from ezdxf import units

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

    def test_text_that_is_not_ascii_is_read_as_the_reference_reads_it(self):
        drawings = [
            {'name': 'part.nc', 'text': 'G21 (Ä)\nG0 X0 Y0\nG1 X3 Y4 F100'},
            {'name': 'part.nc', 'text': 'G21\u2028G0 X0 Y0\r\u2028G1 X\u0663 Y4\u00a0F100\nG1 X1 Q1'},
            {'name': 'cutter.plt', 'text': 'IN;SP1;PD400,0;\u00c4B1;'},
            {'name': 'machine.ini', 'text': '#\u0130NCLUDE other.ini\n[TRAJ]\nLINEAR_UNITS=mm'},
            {'name': 'machine.ini', 'text': '[TRAJ]\nLINEAR_UNITS=mm # für\n[ax\u0131s_x]\nMIN_LIMIT=-\u0661\nMAX_LIMIT=300\n'
             '[AXIS_Y]\nMIN_LIMIT=0\nMAX_LIMIT=200'},
        ]  # fmt: skip
        for payload in drawings:
            with self.subTest(text=payload['text']):
                self.assertIsNotNone(outcome(payload, 'native'))
                self.assertEqual(outcome(payload, 'native'), outcome(payload, 'python'))

    def test_dxf_drawings_are_read_by_the_core_to_the_tolerance(self):
        doc = ezdxf.new(units=units.MM)
        doc.layers.new('Cut', dxfattribs={'color': 1})
        doc.layers.new('Hidden', dxfattribs={'color': -5, 'flags': 1})
        msp = doc.modelspace()
        msp.add_lwpolyline(
            [(0, 0, 0.5), (40, 0, 0), (40, 30, -1), (0, 30, 0)], format='xyb', close=True, dxfattribs={'layer': 'Cut'}
        )
        msp.add_circle((20, 15), 6, dxfattribs={'layer': 'Hidden'})
        msp.add_arc((20, 15), 10, 30, 150)
        msp.add_ellipse((60, 15), major_axis=(10, 4), ratio=0.4)
        msp.add_open_spline([(70, 0), (80, 20), (90, 0), (100, 20)])
        msp.add_text('Label')
        block = doc.blocks.new('Hole', base_point=(1, 1))
        block.add_circle((1, 1), 2)
        msp.add_blockref(
            'Hole', (10, 50), dxfattribs={'rotation': 30, 'xscale': 2, 'yscale': -1, 'row_count': 2, 'row_spacing': 8}
        )
        stream = io.StringIO()
        doc.write(stream)
        payload = {'name': 'plate.DXF', 'text': stream.getvalue()}
        self.assertIsNotNone(import_json(payload))
        core, reference = import_program(payload, backend='native'), import_program(payload, backend='python')
        self.assertEqual(core['layers'], reference['layers'])
        self.assertEqual(core['warnings'], reference['warnings'])
        self.assertEqual([o['name'] for o in core['objects']], [o['name'] for o in reference['objects']])
        for mine, theirs in zip(core['objects'], reference['objects']):
            for a, b in zip(mine['paths'], theirs['paths']):
                self.assertLess(math.dist(a[0], b[0]), 1e-6)
                self.assertLess(math.dist(a[-1], b[-1]), 1e-6)
                for point in a:
                    nearest = min(math.dist(point, q) for q in b)
                    self.assertLess(nearest, 1.0)
        self.assertEqual(core['layers'][0], {'key': 'Cut', 'name': 'Cut', 'color': '#ff0000', 'visible': True})
        self.assertFalse(core['layers'][1]['visible'])
        # The core draws the true curves: a circle's vertices lie on it.
        circle = next(o for o in core['objects'] if o['name'] == 'Hidden \u00b7 Circle')['paths'][0]
        for x, y in circle:
            self.assertAlmostEqual(math.hypot(x - 20, y - 15), 6, places=9)
        self.assertEqual(circle[0], circle[-1])

    def test_a_spline_given_only_by_fit_points_is_interpolated_as_cad_programs_do(self):
        doc = ezdxf.new(units=units.MM)
        spline = doc.modelspace().add_spline([(0, 0), (10, 10), (20, 0)])
        tool = spline.construction_tool()
        stream = io.StringIO()
        doc.write(stream)
        payload = {'name': 'fit.dxf', 'text': stream.getvalue()}
        path = import_program(payload, backend='native')['objects'][0]['paths'][0]
        self.assertEqual(path[0], [0.0, 0.0])
        self.assertLess(math.dist(path[-1], [20.0, 0.0]), 1e-9)
        # Every vertex on the curve ezdxf makes from the fit points, within the chord tolerance.
        curve = [tool.point(t / 2000) for t in range(2001)]
        for point in path:
            self.assertLess(min(math.dist(point, (q.x, q.y)) for q in curve), 0.03)
        # A fit point given twice: the reference cannot interpolate it (a division by zero); the core's own rule
        # passes through it once, and with the reference's rules in force (the extension) it is declined.
        doc = ezdxf.new(units=units.MM)
        doc.modelspace().add_spline([(0, 0), (0, 0), (10, 10), (20, 0)])
        stream = io.StringIO()
        doc.write(stream)
        with self.assertRaises(NotImplementedError):
            import_program({'name': 'twice.dxf', 'text': stream.getvalue()}, backend='native')

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
