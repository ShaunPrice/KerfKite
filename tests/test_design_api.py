import json
import math
import unittest

from studio.operations import region
from studio.relief import prepare_relief, relief_json
from studio.server import create_app
from test_relief import sample as relief_sample
from test_solid_cad import plate
from test_tool_changes import project

SQUARE = [[0, 0], [20, 0], [20, 20], [0, 20], [0, 0]]
OVERLAP = [[10, 10], [30, 10], [30, 30], [10, 30], [10, 10]]


class DesignApiTests(unittest.TestCase):
    def setUp(self):
        self.client = create_app().test_client()
        self.headers = {'X-Studio-Token': self.client.get('/api/session').json['token']}

    def post(self, path, data):
        return self.client.post(path, json=data, headers=self.headers)

    def ok(self, path, data):
        result = self.post(path, data)
        self.assertEqual(result.status_code, 200, f'{path}: {result.get_json(silent=True)}')
        return result

    def rejected(self, path, data=None, **raw):
        result = self.client.post(path, headers=self.headers, **raw) if raw else self.post(path, data)
        self.assertEqual(result.status_code, 400, f'{path} {data!r}')
        self.assertIn('error', result.json)
        return result.json['error']

    def test_boolean_operations_return_expected_areas(self):
        def area(action, objects, **extra):
            paths = self.ok('/api/geometry', {'action': action, 'objects': objects, **extra}).json['paths']
            return region(paths).area

        both = [{'paths': [SQUARE]}, {'paths': [OVERLAP]}]
        self.assertAlmostEqual(area('union', both), 700)
        self.assertAlmostEqual(area('difference', both), 300)
        self.assertAlmostEqual(area('intersection', both), 100)
        grown = area('offset', [{'paths': [SQUARE]}], distance=1)
        self.assertAlmostEqual(grown, 400 + 80 + math.pi, delta=0.05)

    def test_geometry_rejects_malformed_objects(self):
        for objects in (None, [], [1], [{}], [{'paths': 5}], [{'paths': [5]}], [{'paths': [[1, 2, 3, 4]]}]):
            with self.subTest(objects=objects):
                self.rejected('/api/geometry', {'action': 'union', 'objects': objects})
        garbage = [['a', 'b'], [1, 2], [3, 4], ['a', 'b']]
        self.assertIn('XY', self.rejected('/api/geometry', {'action': 'union', 'objects': [{'paths': [garbage]}]}))
        self.rejected('/api/geometry', {'action': 'union', 'objects': [{'paths': [[[0, 0], [1, float('nan')]] * 2]}]})
        self.rejected('/api/geometry', {'action': 'slice', 'objects': [{'paths': [SQUARE]}]})
        disjoint = [{'paths': [SQUARE]}, {'paths': [[[50, 50], [60, 50], [60, 60], [50, 60], [50, 50]]]}]
        self.assertIn('empty', self.rejected('/api/geometry', {'action': 'intersection', 'objects': disjoint}))

    def test_extrude_builds_a_closed_prism(self):
        result = self.ok('/api/extrude', {'paths': [SQUARE], 'height': 4}).json
        self.assertEqual(result['height'], 4)
        self.assertGreaterEqual(result['triangles'], 12)
        self.assertTrue(result['stl'].startswith('solid '))
        self.rejected('/api/extrude', {'paths': 5})
        self.rejected('/api/extrude', {'paths': [SQUARE], 'height': 'tall'})

    def test_artwork_relief_protects_the_raised_region(self):
        base = relief_sample(paths=[SQUARE], x=0, y=0, width=40, height=40, resolution=16)
        base.pop('heightmap')
        result = self.ok('/api/artwork-relief', base).json
        self.assertTrue(any('rasterized' in warning for warning in result['warnings']))
        depths = [z for row in result['path_depths'] for z in row]
        # The 20 mm square stays at the surface while the surrounding stock is cut to depth.
        self.assertEqual(max(depths), 0)
        self.assertLess(min(depths), 0)
        recessed = self.ok('/api/artwork-relief', {**base, 'kind': 'recessed'}).json
        self.assertNotEqual(recessed['path_depths'], result['path_depths'])
        self.rejected('/api/artwork-relief', {**base, 'kind': 'embossed'})
        self.rejected('/api/artwork-relief', {**base, 'paths': 5})

    def test_relief_route_matches_the_library_and_reports_mesh_errors(self):
        response = self.ok('/api/relief', relief_sample())
        # The whole result, as JSON gives it back: with the extension the body is the core's own text.
        self.assertEqual(response.json, json.loads(json.dumps(prepare_relief(relief_sample()))))
        text = relief_json(relief_sample())
        if text is not None:
            self.assertEqual(response.data, text)
        self.assertIsNone(relief_json({'width': math.inf}))  # cannot cross unchanged: the reference answers
        self.assertIn('width', self.rejected('/api/relief', relief_sample(width='wide')))
        self.rejected('/api/relief', relief_sample(depth=True))
        self.rejected('/api/relief', relief_sample(content_base64='not an stl'))

    def test_solid_model_routes(self):
        rebuilt = self.ok('/api/solid-cad', {'model': plate()}).json
        # 40 x 30 x 6 plate less an 8 mm bore; the faceted cylinder removes slightly less than the exact volume.
        self.assertAlmostEqual(rebuilt['volume'], 7200 - math.pi * 16 * 6, delta=1)
        self.assertEqual(rebuilt['parameters']['width'], 40)
        section = self.ok('/api/solid-cad/section', {'model': plate(), 'z': 3}).json
        self.assertEqual(section['z'], 3)
        self.assertEqual(len(section['profiles']), 2)
        stl = self.ok('/api/solid-cad/stl', {'model': plate()})
        self.assertEqual(stl.mimetype, 'model/stl')
        self.assertEqual(len(stl.data), 84 + 50 * len(rebuilt['triangles']))
        for path in ('/api/solid-cad', '/api/solid-cad/section', '/api/solid-cad/stl'):
            with self.subTest(path=path):
                self.rejected(path, {'model': []})

    def test_script_preview_reports_two_and_three_dimensions(self):
        solid = self.ok('/api/cad-script', {'source': 'cube([4,5,6]);'}).json
        self.assertEqual(solid['dimension'], 3)
        self.assertAlmostEqual(solid['volume'], 120)
        flat = self.ok('/api/cad-script', {'source': 'square([4,5]);'}).json
        self.assertEqual(flat['dimension'], 2)
        self.assertAlmostEqual(flat['area'], 20)
        self.rejected('/api/cad-script', {'source': 'cube('})

    def test_sketch_points_resolve_expressions(self):
        result = self.ok(
            '/api/solid-cad/sketch-points',
            {'points': [[0, 0], ['w', 0], ['w', 5]], 'parameters': {'w': 12}},
        ).json
        self.assertEqual(result['points'][1][0], 12)
        self.rejected('/api/solid-cad/sketch-points', {'points': 'none'})
        self.assertIn('256', self.rejected('/api/solid-cad/sketch-points', {'points': [[0, 0]] * 257}))

    def test_project_routes(self):
        plan = self.ok('/api/tool-changes', {'project': project()}).json
        self.assertEqual(plan['format'], 'cnc-tool-change-plan/1')
        self.assertEqual(plan['tool_changes'], len(plan['stages']) - 1)
        self.rejected('/api/tool-changes', {'project': []})
        self.rejected('/api/rotary', {'project': []})
        preview = self.ok('/api/simulate', {}).json
        self.assertEqual(len(preview['heights']), preview['resolution'])
        protocols = self.client.get('/api/protocols').json
        self.assertIn('grbl', {entry['id'] for entry in protocols})

    def test_every_design_route_answers_non_object_bodies_with_a_json_400(self):
        routes = [
            '/api/solid-cad',
            '/api/cad-script',
            '/api/solid-cad/sketch-points',
            '/api/solid-cad/section',
            '/api/solid-cad/stl',
            '/api/tool-changes',
            '/api/rotary',
            '/api/geometry',
            '/api/simulate',
            '/api/extrude',
            '/api/artwork-relief',
            '/api/relief',
            '/api/pcb-import',
        ]
        for path in routes:
            for data in ([], 'text', 5):
                with self.subTest(path=path, data=data):
                    self.rejected(path, data)
            with self.subTest(path=path, data='not json'):
                self.rejected(path, data='not json', content_type='application/json')

    def test_region_accepts_tuples_and_rejects_non_numeric_points(self):
        self.assertAlmostEqual(region([tuple(map(tuple, SQUARE))]).area, 400)
        for paths in (5, [5], [[[0, 0], [1, True], [1, 1], [0, 0]]], [[[0, 0, 0]] * 4]):
            with self.subTest(paths=paths):
                with self.assertRaises(ValueError):
                    region(paths)


if __name__ == '__main__':
    unittest.main()
