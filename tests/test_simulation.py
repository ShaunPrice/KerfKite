import unittest
from studio.simulation import simulate


class SimulationTests(unittest.TestCase):
    def test_flat_cutter_removes_traversed_cells_only(self):
        r = simulate(
            dict(
                width=10,
                height=10,
                resolution=21,
                tool_diameter=2,
                stock_thickness=3,
                segments=[[1, 5, 9, 5, -1, None, -1]],
            )
        )
        self.assertEqual(r['heights'][10][10], -1)
        self.assertEqual(r['heights'][0][10], 0)

    def test_through_stock_rejected(self):
        with self.assertRaises(ValueError):
            simulate(dict(width=10, height=10, stock_thickness=1, segments=[[1, 5, 9, 5, -2, None, -2]]))
