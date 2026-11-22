import json
import os
import unittest
from unittest.mock import patch

from studio import cam
from studio.tool_changes import plan_tool_changes, tool_changes_json


def operation(tool_id=1, **changes):
    return {
        'tool_id': tool_id,
        'tool_name': f'Tool {tool_id}',
        'paths': [[[10, 10], [20, 10]]],
        'tool_diameter': 3,
        'tool_shape': 'flat',
        **changes,
    }


def project(operations=None):
    return dict(
        mode='router',
        protocol='grbl',
        width=100,
        height=100,
        feed=600,
        power=1000,
        safe=5,
        depth=1,
        step=0.5,
        plunge=100,
        operation='line',
        operations=operations if operations is not None else [operation(), operation(2, tool_diameter=1)],
    )


class ManualToolChangeTests(unittest.TestCase):
    def test_two_tools_are_two_complete_independent_jobs(self):
        result = plan_tool_changes(project())
        self.assertTrue(result['manual_only'])
        self.assertEqual(result['tool_changes'], 1)
        self.assertEqual(len(result['stages']), 2)
        for index, stage in enumerate(result['stages'], 1):
            self.assertEqual(stage['tool']['tool_id'], index)
            self.assertEqual(stage['lines'][:2], ['G21', 'G90'])
            self.assertEqual(stage['lines'][-2:], ['G0 Z5.000', 'M5'])
            self.assertTrue(stage['filename'].endswith('.nc'))
            self.assertFalse(any(line.startswith(('M6', 'T')) for line in stage['lines']))
            self.assertTrue(stage['before_running'])

    def test_contiguous_groups_preserve_order_and_geometry_signatures(self):
        result = plan_tool_changes(
            project([operation(), operation(), operation(2), operation(), operation(tool_diameter=4)])
        )
        self.assertEqual([s['operation_indices'] for s in result['stages']], [[1, 2], [3], [4], [5]])
        self.assertEqual([s['tool']['diameter'] for s in result['stages']], [3, 3, 3, 4])
        shaped = plan_tool_changes(project([operation(), operation(tool_shape='ball')]))
        self.assertEqual(len(shaped['stages']), 2)

    def test_unknown_tools_heads_nested_and_invalid_geometry_rejected(self):
        for op in [
            operation(None),
            operation(0),
            operation(1.5),
            operation(True),
            operation(1000),
            operation(tool_name=''),
            operation(tool_shape='unknown'),
            operation(tool_diameter=0),
            operation(mode='laser'),
            operation(protocol='marlin'),
            operation(operations=[]),
            operation(tool_shape='v', v_angle=0),
        ]:
            with self.subTest(op=op), self.assertRaises(ValueError):
                plan_tool_changes(project([op]))
        for p in [None, {}, project([]), {**project(), 'mode': 'laser'}]:
            with self.assertRaises(ValueError):
                plan_tool_changes(p)

    def test_ordinary_run_rejects_different_tool_ids_with_equal_geometry(self):
        from unittest.mock import patch
        from studio.cam import generate
        from studio.controller import Controller
        from studio.server import create_app

        p = project([operation(1), operation(2)])
        with self.assertRaisesRegex(ValueError, 'Tool identity or geometry differs'):
            generate(p)
        controller = Controller()
        self.addCleanup(controller.disconnect)
        client = create_app(controller).test_client()
        token = client.get('/api/session').get_json()['token']
        with patch.object(controller, 'start') as start:
            response = client.post(
                '/api/run', json={'confirmed': True, 'project': p}, headers={'X-Studio-Token': token}
            )
            self.assertEqual(response.status_code, 400)
            self.assertIn('Tool identity or geometry differs', response.get_json()['error'])
            start.assert_not_called()
        self.assertEqual(len(plan_tool_changes(p)['stages']), 2)

    def test_same_v_bit_line_and_vcarve_share_one_stage(self):
        line = operation(4, tool_shape='v', v_angle=60)
        carve = operation(
            4,
            operation='vcarve',
            tool_shape='v',
            v_angle=60,
            v_resolution=1,
            paths=[[[10, 10], [20, 10], [20, 20], [10, 20], [10, 10]]],
        )
        result = plan_tool_changes(project([line, carve]))
        self.assertEqual(len(result['stages']), 1)
        self.assertEqual(result['stages'][0]['operation_indices'], [1, 2])
        self.assertEqual(result['stages'][0]['tool']['shape'], 'v')
        self.assertEqual(result['stages'][0]['tool']['v_angle'], 60)

    def test_single_tool_job_and_marlin_are_separate_export_paths(self):
        p = project()
        p.pop('operations')
        p.update(operation())
        self.assertEqual(len(plan_tool_changes(p)['stages']), 1)
        p['protocol'] = 'marlin'
        result = plan_tool_changes(p)
        self.assertIn('G4 P2000', result['stages'][0]['lines'])
        self.assertEqual(result['stages'][0]['lines'][-1], 'M5')


def exact(value):
    """A form that tells 0 from 0.0 and -0.0 from 0.0."""
    return json.loads(json.dumps(value), parse_float=lambda text: ('f', text), parse_int=lambda text: ('i', text))


def answer(project, backend):
    try:
        return 'ok', exact(plan_tool_changes(project, backend=backend))
    except ValueError as error:
        return 'error', str(error)


@unittest.skipIf(cam._core_cam is None, 'Build the extension to compare the native core with the reference')
class NativeToolChangeTests(unittest.TestCase):
    """The native core's plans against this module's Python, the reference: every member, with its type."""

    def setUp(self):
        # The environment switches that choose the Python CAM or Shapely are for other tests.
        environment = patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        for key in ('KERFKITE_GCODE_BACKEND', 'KERFKITE_POLYGON_BACKEND'):
            os.environ.pop(key, None)

    def test_plans_and_refusals_are_the_reference(self):
        carve = operation(
            4,
            operation='vcarve',
            tool_shape='v',
            v_resolution=1,
            paths=[[[10, 10], [20, 10], [20, 20], [10, 20], [10, 10]]],
        )
        drill = operation(5, operation='drill', tool_shape='drill', paths=[[[30, 30], [30, 30]]])
        cases = [
            project(),
            project([operation(), operation(), operation(2), operation(), operation(tool_diameter=4.0)]),
            project([operation(4, tool_shape='v'), carve, drill, operation(5, tool_shape='drill', operation='drill',
                                                                          paths=[[[40, 40], [40, 40]]], drill_point_angle=135)]),
            project([operation(tool_name='  Spaced\u00a0name\u3000 '), operation(tool_name='caf\u00e9'), operation(tool_name=None)]),
            project([operation(tool_name='x' * 80), operation(tool_name='x' * 81)]),
            project([operation(width=100.0), operation(height=True)]),
            project([operation(dwell_units=None)]),
            {**project(), 'rotary': {}},
            {**project(), 'rotary': {'diameter': 10}},
            {**project(), 'operations': None},
            {key: value for key, value in project().items() if key != 'operations'} | operation(),
        ]  # fmt: skip
        for case in cases:
            with self.subTest(case=json.dumps(case)[:120]):
                self.assertEqual(answer(case, 'native'), answer(case, 'python'))

    def test_the_line_limit_counts_every_stage(self):
        long = [[[10 + (i % 2) * 0.5, 10 + i * 0.0015] for i in range(50_001)]]
        case = project([operation(1, paths=long), operation(2, paths=long)])
        found = answer(case, 'native')
        self.assertEqual(found, ('error', 'Tool-change plan exceeds 200000 total lines'))
        self.assertEqual(found, answer(case, 'python'))

    def test_route_sends_the_core_text(self):
        from studio.server import create_app

        client = create_app().test_client()
        headers = {'X-Studio-Token': client.get('/api/session').get_json()['token']}
        response = client.post('/api/tool-changes', json={'project': project()}, headers=headers)
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.data, tool_changes_json(project()))
        self.assertEqual(exact(response.get_json()), exact(plan_tool_changes(project(), backend='python')))
        refused = client.post('/api/tool-changes', json={'project': {**project(), 'mode': 'laser'}}, headers=headers)
        self.assertEqual(refused.status_code, 400)
        self.assertIn('router mode', refused.get_json()['error'])


if __name__ == '__main__':
    unittest.main()
