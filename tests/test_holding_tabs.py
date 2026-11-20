import unittest
from studio.cam import generate


def sample(**changes):
    result = dict(
        mode='router',
        operation='line',
        width=100,
        height=100,
        feed=600,
        power=1000,
        safe=5,
        depth=10,
        step=3,
        plunge=100,
        stock_thickness=10,
        tool_diameter=2,
        paths=[[[10, 10], [30, 10], [30, 30], [10, 30], [10, 10]]],
        tabs={'width': 4, 'height': 2, 'count': 4},
    )
    result.update(changes)
    return result


class HoldingTabTests(unittest.TestCase):
    def test_tab_plateaus_preserved_at_every_pass_and_safe_feed(self):
        job = generate(sample())
        self.assertEqual(set(job['prepared_depths'][0]), {-10, -8})
        # Count-based positions are side midpoints; 4mm nominal + 2mm cutter.
        self.assertIn([17, 10], job['prepared_paths'][0])
        self.assertIn([23, 10], job['prepared_paths'][0])
        crossings = [s for s in job['segments'] if s[:4] == [17, 10, 23, 10]]
        self.assertEqual([s[4] for s in crossings], [-3, -6, -8, -8])
        self.assertTrue(any(s[4] == -10 for s in job['segments']))
        self.assertTrue(all(' F100.0 Z' in line for line in job['lines'] if line.startswith('G1 X')))
        self.assertEqual(job['lines'][-2:], ['G0 Z5.000', 'M5'])
        for i, line in enumerate(job['lines']):
            if line.startswith('G0 X'):
                self.assertEqual(job['lines'][i - 1], 'G0 Z5.000')

    def test_stock_bottom_height_is_independent_of_overcut(self):
        job = generate(sample(depth=11, step=5))
        self.assertEqual(set(job['prepared_depths'][0]), {-11, -8})

    def test_explicit_tab_at_seam_is_continuous(self):
        job = generate(sample(tabs={'width': 4, 'height': 2, 'positions': [0]}))
        points, zs = job['prepared_paths'][0], job['prepared_depths'][0]
        self.assertEqual(points[0], points[-1])
        self.assertEqual(zs[0], -8)
        self.assertEqual(zs[-1], -8)
        self.assertIn([13, 10], points)
        self.assertIn([10, 13], points)
        self.assertIn(-10, zs)

    def test_profile_offsets_get_tabs_after_compensation(self):
        for operation in ('outside', 'inside'):
            job = generate(sample(operation=operation))
            self.assertEqual(set(job['prepared_depths'][0]), {-10, -8})
            self.assertTrue(any('cutter diameter' in warning for warning in job['warnings']))

    def test_ordered_operation_can_override_tab_settings(self):
        p = sample()
        p['operations'] = [
            {'paths': p['paths'], 'tabs': None},
            {'paths': p['paths'], 'tabs': {'width': 4, 'height': 3, 'count': 2}},
        ]
        job = generate(p)
        self.assertEqual(job['operations'], 2)
        self.assertTrue(any(s[4] == -7 for s in job['segments']))
        self.assertTrue(any(s[4] == -10 for s in job['segments']))

    def test_inappropriate_operations_and_open_relief_paths_fail(self):
        for changes in (
            {'mode': 'laser'},
            {'mode': 'knife'},
            {'operation': 'pocket'},
            {'operation': 'vcarve'},
            {'operation': 'fill'},
            {'tool_shape': 'ball'},
            {'path_depths': [[-1] * 5]},
            {'paths': [[[10, 10], [30, 10]]]},
            {'paths': [[[10, 10], [30, 30], [10, 30], [30, 10], [10, 10]]]},
        ):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                generate(sample(**changes))

    def test_invalid_tab_parameters_fail_without_ignoring(self):
        invalid = [
            {},
            True,
            0,
            {'width': 0, 'height': 2, 'count': 4},
            {'width': 4, 'height': 0, 'count': 4},
            {'width': 4, 'height': 10, 'count': 4},
            {'width': 4, 'height': 2, 'count': 0},
            {'width': 4, 'height': 2, 'count': 2.5},
            {'width': 4, 'height': 2, 'count': 1000},
            {'width': 4, 'height': 2, 'count': 20},
            {'width': 4, 'height': 2, 'positions': []},
            {'width': 4, 'height': 2, 'positions': [1]},
            {'width': 4, 'height': 2, 'positions': [0.2, 0.2]},
            {'width': 4, 'height': 2, 'positions': [0.2, 0.21]},
            {'width': 4, 'height': 2, 'positions': [float('nan')]},
            {'width': 4, 'height': 2, 'count': 4, 'positions': [0.2]},
            {'width': 4, 'height': 2, 'count': 4, 'typo': True},
        ]
        for config in invalid:
            with self.subTest(config=config), self.assertRaises(ValueError):
                generate(sample(tabs=config))
        for changes in ({'stock_thickness': None}, {'stock_thickness': 11}, {'stock_thickness': 0}):
            with self.assertRaises(ValueError):
                generate(sample(**changes))

    def test_disabled_tabs_do_not_change_plain_contours(self):
        for disabled in (None, False):
            result = generate(sample(tabs=disabled))
            self.assertIsNone(result['prepared_depths'])
            self.assertEqual(result['prepared_paths'], sample()['paths'])


if __name__ == '__main__':
    unittest.main()
