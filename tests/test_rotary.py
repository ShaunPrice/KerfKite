import math
import unittest
from studio.rotary import wrap_job


def source(end=None):
    end = math.pi*10 if end is None else end
    return dict(mode='router', lines=['G21','G90','G17','G94','G54','M5','G0 Z5',
        'M3 S1000','G4 P2','G0 X0 Y0','G1 Z-1 F100',
        f'G1 X{end:.12f} Y0 F600','G0 Z5','M5'], warnings=[])


def options(**changes):
    return dict(diameter=10,wrap_axis='X',postprocessor='linuxcnc',**changes)


class RotaryTests(unittest.TestCase):
    def test_full_revolution_feed_depth_and_clearance(self):
        job=wrap_job(source(),options())
        self.assertIn('G93',job['lines'])
        cut=next(line for line in job['lines'] if line.startswith('G1 A'))
        words=dict((token[0],float(token[1:])) for token in cut.split()[1:])
        self.assertAlmostEqual(words['A'],360,places=6)
        self.assertAlmostEqual(words['F'],600/(math.pi*10),places=7)
        self.assertIn('G1 Z-1.000000 F16.666666667',job['lines'])
        self.assertEqual(job['lines'][-3:],['G0 Z5.000000','M5','G94 F600.000000'])
        self.assertTrue(job['rotary']['export_only'])

    def test_angles_are_continuous_beyond_seam_and_reversible(self):
        result=wrap_job(source(math.pi*20),options(angle_offset=15,direction=-1))
        self.assertAlmostEqual(result['rotary']['angle_bounds'][0],-705,places=6)
        self.assertEqual(result['rotary']['angle_bounds'][1],15)
        self.assertTrue(any('A-705.000000' in line for line in result['lines']))

    def test_y_wrap_and_coordinated_radial_feed(self):
        job=source();job['lines'][11]='G1 X3 Y4 Z-2 F600'
        opts=options();opts['wrap_axis']='Y'
        result=wrap_job(job,opts)
        line=next(line for line in result['lines'] if line.startswith('G1 X3'))
        words=dict((token[0],float(token[1:])) for token in line.split()[1:])
        self.assertAlmostEqual(words['A'],4*360/(math.pi*10),places=6)
        self.assertAlmostEqual(words['F'],600/math.sqrt(26),places=7)
        self.assertEqual(words['Z'],-2)


    def test_rounded_source_circumference_snaps_and_remaps_axial_coordinate(self):
        job=source(round(math.pi*10,3));job['lines'][11]=job['lines'][11].replace('Y0','Y7')
        result=wrap_job(job,options())
        line=next(line for line in result['lines'] if line.startswith('G1 A'))
        self.assertIn('A360.000000',line)
        self.assertIn('X7.000000',line)
        self.assertNotIn('Y',line)

    def test_zero_length_moves_skipped(self):
        job=source();job['lines'].insert(11,'G1 X0 Y0 F600')
        result=wrap_job(job,options())
        self.assertEqual(sum(line.startswith('G1 ') for line in result['lines']),2)

    def test_reject_unsupported_modes_dialects_and_commands(self):
        for updates in ({'postprocessor':'grbl'},{'postprocessor':'mach4'},{'wrap_axis':'A'},
                        {'diameter':0},{'diameter':float('inf')},{'direction':0}):
            opts=options();opts.update(updates)
            with self.subTest(updates=updates),self.assertRaises(ValueError):wrap_job(source(),opts)
        for command in ('G91','G2 X1 Y2 I0 J1','M4 S100','G1 X1 Y1 S20 F100',
                        'G1 X1 Y1','G1 X1 X2 F100','G1 Z-5 F100','G1 X1 Y1 F0',
                        'G1 Xnan Y1 F100','G1 X1 Y1 F100 ;comment','G20'):
            job=source();job['lines'][11]=command
            with self.subTest(command=command),self.assertRaises(ValueError):wrap_job(job,options())
        for mode in ('laser','knife'):
            job=source();job['mode']=mode
            with self.assertRaises(ValueError):wrap_job(job,options())
        job=source();job['lines'].remove('G0 X0 Y0')
        with self.assertRaises(ValueError):wrap_job(job,options())
        job=source();job['lines'].remove('G94')
        with self.assertRaises(ValueError):wrap_job(job,options())
        job=source();job['lines'].pop()
        with self.assertRaises(ValueError):wrap_job(job,options())


if __name__=='__main__':unittest.main()
