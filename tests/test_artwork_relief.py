"""Artwork reliefs from the native core against the Shapely reference, and the route that serves them."""

import json
import math
import os
import unittest
from unittest.mock import patch

from studio import artwork_relief
from studio.artwork_relief import _reference_raster, raster
from studio.server import create_app

try:
    from studio import _relief_native as native
except ImportError:
    native = None


def regular(cx, cy, r, sides, turn=0.0):
    points = [
        [cx + r * math.cos(turn + 2 * math.pi * k / sides), cy + r * math.sin(turn + 2 * math.pi * k / sides)]
        for k in range(sides)
    ]
    return points + [points[0]]


def square(low, high):
    return [[low, low], [high, low], [high, high], [low, high], [low, low]]


# Outlines that pass nowhere near a cell's edge, where the kernel and Shapely must agree cell for cell.
CASES = [
    dict(paths=[regular(50.3, 49.1, 23.7, 48)], width=100, height=100, resolution=64),
    dict(
        paths=[regular(31.1, 22.2, 17.3, 40), regular(31.1, 22.2, 8.9, 40, 0.1)],
        width=63.3,
        height=47.7,
        x=2.2,
        y=1.3,
        resolution=37,
    ),
    dict(
        paths=[regular(20.13, 20.17, 12.3, 5, 0.3), regular(47.71, 25.37, 9.1, 7, 0.2)],
        width=70.7,
        height=48.9,
        resolution=128,
    ),
]

REFUSALS = [
    dict(paths=[[[0, 0], [1, 0], [1, 1]]]),
    dict(paths=[[[0, 0], [2, 2], [2, 0], [0, 2], [0, 0]]]),
    dict(paths=[square(1, 5), square(1, 5)]),
    dict(),
    dict(paths=5),
    dict(paths=[square(1, 5)], width='wide'),
    dict(paths=[square(1, 5)], resolution=129),
    dict(paths=[square(1, 5)], kind='embossed'),
]


def post(data):
    client = create_app().test_client()
    headers = {'X-Studio-Token': client.get('/api/session').json['token']}
    return client.post('/api/artwork-relief', json=data, headers=headers)


def refusal(make, data):
    try:
        make(data)
    except ValueError as error:
        return str(error)
    return None


@unittest.skipUnless(native is not None and hasattr(native, 'core_cam'), 'the compiled extension is not installed')
class NativeArtworkReliefTests(unittest.TestCase):
    def setUp(self):
        environment = patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop('KERFKITE_GCODE_BACKEND', None)
        os.environ.pop('KERFKITE_POLYGON_BACKEND', None)

    def test_cells_away_from_the_outline_agree_with_shapely(self):
        for data in CASES:
            for kind in ('raised', 'recessed'):
                with self.subTest(case=CASES.index(data), kind=kind):
                    request = {**data, 'kind': kind}
                    self.assertEqual(raster(request, backend='native'), _reference_raster(request))

    def test_refusals_are_the_reference_messages(self):
        for data in REFUSALS:
            with self.subTest(data=data):
                expected = refusal(_reference_raster, data)
                self.assertIsNotNone(expected)
                self.assertEqual(refusal(lambda d: raster(d, backend='native'), data), expected)

    def test_an_outline_on_a_cell_edge_touches_that_cell(self):
        # 21 points over 100 mm: cells 10 mm wide centred every 5 mm. The square's edges lie on cell edges, and
        # the kernel, whose grid holds them exactly, counts a cell that only touches the square as meeting it.
        request = dict(paths=[square(30, 70)], width=100, height=100, resolution=21)
        self.assertEqual(sum(map(sum, raster(request, backend='native'))), 11 * 11)
        recessed = raster({**request, 'kind': 'recessed'}, backend='native')
        self.assertEqual(sum(map(sum, recessed)), 21 * 21 - 7 * 7)

    def test_the_route_sends_the_core_relief(self):
        data = {**CASES[0], 'depth': 2, 'tool_diameter': 3, 'stepover': 1.5}
        with patch('studio.design_api.artwork_relief', wraps=artwork_relief.artwork_relief) as reference:
            answer = post(data)
        self.assertEqual(answer.status_code, 200)
        reference.assert_not_called()
        result = answer.get_json()
        self.assertEqual(result['surface']['backend'], 'cpp')
        self.assertEqual(result['warnings'][-1], artwork_relief.WARNING)
        expected = artwork_relief.artwork_relief(data, backend='python')
        self.assertEqual(result['path_depths'], expected['path_depths'])
        self.assertEqual(result['paths'], json.loads(json.dumps(expected['paths'])))
        rejected = post({**data, 'kind': 'embossed'})
        self.assertEqual(rejected.status_code, 400)
        self.assertIn('Choose raised or recessed relief', rejected.get_data(as_text=True))

    def test_the_python_cam_uses_the_reference(self):
        os.environ['KERFKITE_POLYGON_BACKEND'] = 'shapely'
        self.assertIsNone(artwork_relief.artwork_relief_json(CASES[0]))
        answer = post({**CASES[0], 'depth': 2, 'tool_diameter': 3, 'stepover': 1.5})
        self.assertEqual(answer.status_code, 200)
        self.assertEqual(answer.get_json()['warnings'][-1], artwork_relief.WARNING)


if __name__ == '__main__':
    unittest.main()
