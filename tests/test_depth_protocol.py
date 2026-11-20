import unittest
from studio.cam import generate
from studio.protocols import postprocess


class DepthProtocolTests(unittest.TestCase):
    def job(self):
        return dict(
            mode='router',
            width=100,
            height=100,
            feed=600,
            power=1000,
            depth=1,
            step=0.5,
            plunge=100,
            paths=[[[1, 1], [2, 1], [3, 2]]],
            path_depths=[[-0.2, -0.8, -1.2]],
        )

    def test_varying_depth_steps_and_retract(self):
        result = generate(self.job())
        lines = result['lines']
        self.assertIn('G1 X2.000 Y1.000 F100.0 Z-0.500', lines)
        self.assertIn('G1 X3.000 Y2.000 F100.0 Z-1.200', lines)
        self.assertEqual(sum(l == 'G1 Z-0.200 F100.0' for l in lines), 3)
        for i, l in enumerate(lines):
            if l.startswith('G0 X'):
                self.assertEqual(lines[i - 1], 'G0 Z5.000')

    def test_invalid_depths_fail(self):
        for depths in [[[-1]], [[0.1, -1, -1]], [[float('nan'), -1, -1]]]:
            p = self.job()
            p['path_depths'] = depths
            with self.assertRaises(ValueError):
                generate(p)

    def test_marlin_export_has_its_own_dwell_and_no_grbl_coordinate_mode(self):
        p = self.job()
        p['protocol'] = 'marlin'
        lines = generate(p)['lines']
        self.assertIn('G4 P2000', lines)
        self.assertNotIn('G54', lines)
        with self.assertRaises(ValueError):
            postprocess(['M4 S500'], 'marlin', 'laser')
        with self.assertRaises(ValueError):
            postprocess([], 'unknown')
