import io
import math
import unittest

import ezdxf
from ezdxf import units

from studio.cam import generate
from studio.program_import import import_program


def read(name, text, unit='mm'):
    return import_program({'name': name, 'text': text, 'unit': unit})


def drawing(doc):
    stream = io.StringIO()
    doc.write(stream)
    return stream.getvalue()


class ProgramImportTests(unittest.TestCase):
    def test_dxf_units_transformed_block_bulge_layers_and_skips(self):
        doc = ezdxf.new(units=units.IN)
        doc.layers.new('Cut', dxfattribs={'color': 1})
        block = doc.blocks.new('Curved')
        block.add_lwpolyline([(0, 0, 1.), (1, 0, 0.)], format='xyb')
        msp = doc.modelspace()
        msp.add_blockref('Curved', (2, 1), dxfattribs={'layer': 'Cut', 'rotation': 90})
        msp.add_circle((4, 5), 1)
        msp.add_line((0, 0, 0), (0, 1, 1))
        msp.add_text('Label')
        src = read('example.dxf', drawing(doc))
        self.assertEqual(len(src['objects']), 2)
        self.assertEqual(src['layers'][0]['color'], '#ff0000')
        self.assertEqual(src['layers'][0]['key'], 'Cut')
        path = src['objects'][0]['paths'][0]
        self.assertAlmostEqual(path[0][0], 50.8)
        self.assertAlmostEqual(path[0][1], 25.4)
        self.assertAlmostEqual(path[-1][1], 50.8)
        self.assertGreater(len(path), 8)
        self.assertIn('non-XY contour', ' '.join(src['warnings']))
        self.assertIn('TEXT', ' '.join(src['warnings']))

    def test_unitless_dxf_and_recursive_block(self):
        doc = ezdxf.new(units=0)
        doc.modelspace().add_line((0, 0), (1, 2))
        src = read('unitless.dxf', drawing(doc), 'cm')
        self.assertEqual(src['objects'][0]['paths'][0][-1], [10, 20])
        block = doc.blocks.new('Cycle')
        block.add_blockref('Cycle', (0, 0))
        doc.modelspace().add_blockref('Cycle', (0, 0))
        with self.assertRaisesRegex(ValueError, 'Recursive'):
            read('bad.dxf', drawing(doc))

    def test_hpgl_knife_layers_pen_lifts_incremental_force_and_velocity(self):
        src = read('example.plt', 'IN;SP2;VS2;FS5;PU40,80;PD80,80;PR40,40;PU;PA0,0;SP4;PD40,0;PU;')
        self.assertEqual(src['mode'], 'knife')
        self.assertEqual(src['objects'][0]['paths'], [[[1, 2], [2, 2], [3, 3]]])
        self.assertEqual(src['objects'][1]['paths'], [[[0, 0], [1, 0]]])
        self.assertEqual([layer['key'] for layer in src['layers']], ['Pen 2', 'Pen 4'])
        self.assertEqual(src['presets'][0]['settings']['feed'], 1200)
        self.assertIn('force (source units): 5', ' '.join(src['warnings']))

    def test_hpgl_arc_circle_and_transform_refusal(self):
        src = read('circle.hpg', 'IN;SP1;PA400,400;CI40;PU440,400;PD;AA400,400,90;PU;')
        circle, arc = (o['paths'][0] for o in src['objects'])
        self.assertEqual(circle[0], circle[-1])
        self.assertAlmostEqual(arc[-1][0], 10)
        self.assertAlmostEqual(arc[-1][1], 11)
        with self.assertRaisesRegex(ValueError, 'SC'):
            read('bad.plt', 'IN;SC0,10,0,10;PD40,40;')

    def test_gcode_stock_program_preserves_gaps_depths_and_feeds(self):
        posted = generate({'mode': 'router', 'protocol': 'grbl', 'width': 150, 'height': 100,
                           'feed': 400, 'power': 8000, 'depth': 2, 'step': 2,
                           'safe': 3, 'paths': [[[1, 2], [3, 4]], [[11, 12], [13, 14]]],
                           'path_depths': [[-.5,-2],[-1,-1.5]]})
        result = read('generated.nc', '\n'.join(posted['lines']))
        self.assertEqual(result['mode'], 'router')
        self.assertEqual(len(result['objects']), 2)
        self.assertEqual(result['objects'][0]['paths'], [[[1, 2], [3, 4]]])
        self.assertEqual(result['objects'][0]['depths'], [[-.5,-2]])
        self.assertEqual(result['objects'][1]['depths'], [[-1,-1.5]])
        self.assertTrue(result['presets'])

    def test_gcode_incremental_inch_arcs_and_helical_z(self):
        text = '%\nG90 G20 G17\nN1G0X1Y1Z-.1\nG91G3X1Y1I0J1Z-.1F3\nG90G0X3Y3\nG1X3.1Y3F2\nM30\n%'
        src = read('arc.ngc', text)
        arc = src['objects'][0]
        p = arc['paths'][0]
        self.assertEqual(p[0], [25.4, 25.4])
        self.assertAlmostEqual(p[-1][0], 50.8)
        self.assertAlmostEqual(arc['depths'][0][-1], -5.08)
        self.assertTrue(all(abs(math.dist((25.4, 50.8), xy)-25.4)<1e-7 for xy in p))
        self.assertEqual({p['settings']['feed'] for p in src['presets']}, {50.8, 76.2})

    def test_gcode_modal_xy_arc_radius_major_full_and_absolute_centre(self):
        src = read('arc.tap','G21G90\nG0X1Y0\nG3X0Y1R-1\nG0X1Y0\nG90.1G3I0J0\nM2')
        major, circle = [o['paths'][0] for o in src['objects']]
        self.assertTrue(any(x>1.5 for x,y in major))
        self.assertEqual(circle[0], circle[-1])
        self.assertTrue(all(abs(math.hypot(*p)-1)<1e-7 for p in circle))

    def test_gcode_disallow_unhandled_state_words(self):
        for cmd in ('G92 X0', 'G38.2 Z-1', 'G0 A90', 'G2 X4 Y0 R1', 'G18',
                    'M98 P001', 'G10 L2 P1 X20', 'G1 X#3', 'G91 G90 X10'):
            with self.subTest(cmd=cmd), self.assertRaises(ValueError):
                read('unsupported.nc','G21 G90\nG0X0Y0\nG1X1Y1\n'+cmd)

    def test_bad_files(self):
        for name, text in [('bad.dxf', 'BOGUS'), ('bad.nc','G21\nM5'),
                           ('bad.plt','PU;PD42;'), ('bad.plt','PU1,0;PD80001,0;')]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                read(name,text)

    def test_linuxcnc_signed_extents_inch_units_and_numerical_fields_only(self):
        source = """[TRAJ]
LINEAR_UNITS=inch
COORDINATES = X Y Z
[AXIS_X]
MIN_LIMIT=-2
MAX_LIMIT=3
MAX_VELOCITY=1.5
[AXIS_Y]
MIN_LIMIT=1
MAX_LIMIT=2
[AXIS_Z]
MIN_LIMIT=-1
MAX_LIMIT=0
[DISPLAY]
PROGRAM_PREFIX=ignored-source-path
"""
        parsed = read('reference.ini', source)
        device = parsed['devices'][0]
        self.assertAlmostEqual(device['width'], 127)
        self.assertAlmostEqual(device['height'], 25.4)
        self.assertIn('38.1 mm/s', ' '.join(parsed['warnings']))
        self.assertNotIn('ignored-source-path', str(parsed))
        for invalid in (source.replace('MAX_LIMIT=3','MAX_LIMIT=-4'),
                        source.replace('MIN_LIMIT=-2', 'MIN_LIMIT=invalid'),
                        source.replace('LINEAR_UNITS=inch', ''), source + '#INCLUDE more.ini'):
            with self.assertRaises(ValueError):
                read('bad.ini', invalid)

    def test_batch_dense_runs_preserves_separate_paths(self):
        source = 'IN;SP1;' + ''.join(f'PU{4*i},0;PD{4*i+2},4;' for i in range(150))
        src = read('dense.plt', source)
        self.assertEqual(len(src['objects']), 1)
        paths = src['objects'][0]['paths']
        self.assertEqual(len(paths), 150)
        self.assertEqual(paths[1], [[.1, 0], [.15, .1]])
