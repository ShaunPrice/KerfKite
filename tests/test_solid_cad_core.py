"""The native core's parametric solid history against the Python reference, to the bit."""

import json
import os
import unittest
from unittest.mock import patch

from studio import solid_cad
from studio.server import create_app
from studio.solid_cad import horizontal_section, model_json, rebuild
from studio.solid_kernel import kernel_available

try:
    from studio import _relief_native as native
except ImportError:
    native = None

AVAILABLE = native is not None and hasattr(native, 'core_cam') and kernel_available()


def outcome(call, *args, **kwargs):
    """The answer as text that tells every bit, or the error; None when the core declines."""
    try:
        return json.dumps(call(*args, **kwargs), sort_keys=True)
    except ValueError as error:
        return 'ValueError: ' + str(error)
    except NotImplementedError:
        return None


def plate(**parameters):
    return {
        'version': 1,
        'parameters': {'w': 40, 'h': 'w / 2', 'r': 'h / 4 + 1', **parameters},
        'features': [
            {'id': 'base', 'kind': 'box', 'size': ['w', 'h', 10]},
            {'id': 'hole', 'kind': 'cylinder', 'op': 'cut', 'radius': 'r', 'height': 12, 'origin': ['w/2', 'h/2', -1]},
            {
                'id': 'row',
                'kind': 'cylinder',
                'op': 'cut',
                'radius': 2,
                'height': 12,
                'origin': [5, 5, -1],
                'pattern': {'kind': 'linear', 'count': 3, 'step': [10, 0, 0]},
            },
            {'id': 'dome', 'kind': 'sphere', 'radius': 4, 'origin': [20, 10, 10]},
            {
                'id': 'pegs',
                'kind': 'cylinder',
                'radius': 1.5,
                'height': 3,
                'topRadius': 0.5,
                'origin': [30, 10, 9],
                'rotation': [0, 0, 15],
                'pattern': {'kind': 'polar', 'count': 4, 'angle': 90, 'center': [20, 10, 0]},
            },
            {'id': 'trim', 'kind': 'box', 'op': 'intersect', 'size': [38, 30, 30], 'origin': [1, -5, -1]},
        ],
    }


def box(size, **more):
    return {'version': 1, 'features': [{'id': 'b', 'kind': 'box', 'size': size, **more}]}


@unittest.skipUnless(AVAILABLE, 'the compiled extension with the solid kernel is not installed')
class NativeSolidCadTests(unittest.TestCase):
    def setUp(self):
        environment = patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop('KERFKITE_SOLID_BACKEND', None)

    def same(self, model, z=None):
        if z is None:
            native_answer, reference = (outcome(rebuild, model, backend=b) for b in ('native', 'python'))
        else:
            native_answer, reference = (outcome(horizontal_section, model, z, backend=b) for b in ('native', 'python'))
        self.assertIsNotNone(native_answer, 'the core declined')
        self.assertEqual(native_answer, reference)
        return native_answer

    def test_a_model_of_every_ported_feature_is_the_reference_to_the_bit(self):
        answer = json.loads(self.same(plate()))
        self.assertGreater(len(answer['triangles']), 1000)
        self.assertEqual(answer['parameters'], {'w': 40.0, 'h': 20.0, 'r': 6.0})
        for z in (0, 5, 9.99, 'h / 3', 10, 30, -1):
            with self.subTest(z=z):
                self.same(plate(), z)

    def test_errors_name_the_feature_as_the_reference_does(self):
        cases = [
            box([1, 1, 0]),
            box([1, '1/0', 1]),
            box([1, 'missing', 1]),
            box([1, 1]),
            box([1, 1, 1], rotation=None),
            box([1, 1, 1], pattern={'kind': 'polar', 'count': 3, 'angle': 360}),
            box([1, 1, 1], pattern={'kind': 'linear', 'count': 2.5, 'step': [1, 0, 0]}),
            box([1, 1, 1], pattern={'kind': 'linear', 'count': 2, 'step': [0, 0, 0]}),
            box([1, 1, 1], pattern={'kind': 'spiral', 'count': 2}),
            box([1, 1, 1], pattern=5),
            box([1, 1, 1], enabled='yes'),
            box([1, 1, 1], op='cut'),
            {'version': 2, 'features': []},
            {'version': 1.0, 'features': []},
            {'version': 1, 'features': []},
            {'version': 1, 'parameters': None, 'features': [{'id': 'b', 'kind': 'box', 'size': [1, 1, 1]}]},
            {'version': 1, 'parameters': {'9x': 1}, 'features': [{'id': 'b', 'kind': 'box', 'size': [1, 1, 1]}]},
            {'version': 1, 'features': [{'id': 'b', 'kind': 'cone'}]},
            {'version': 1, 'features': [{'id': ' ', 'kind': 'box', 'size': [1, 1, 1]}]},
            {'version': 1, 'features': [box([1, 1, 1])['features'][0], box([2, 2, 2])['features'][0]]},
            {'version': 1, 'features': [box([1, 1, 1])['features'][0] | {'enabled': False}]},
            {'version': 1, 'features': ['b']},
            {
                'version': 1,
                'features': [
                    {'id': 'a', 'kind': 'box', 'size': [1, 1, 1]},
                    {'id': 'b', 'kind': 'box', 'op': 'cut', 'size': [3, 3, 3], 'origin': [-1, -1, -1]},
                ],
            },
        ]
        for model in cases:
            with self.subTest(model=model):
                self.same(model)

    def test_parameters_resolve_in_the_order_of_the_dictionary(self):
        # Python names the parameter it started from; JSON objects in the core are in the order of their names.
        for parameters in ({'b': 'a', 'a': 'b'}, {'a': 'b', 'b': 'a'}):
            model = {'version': 1, 'parameters': parameters, 'features': box([1, 1, 1])['features']}
            self.assertIn(f'reference: {next(iter(parameters))}', self.same(model))

    def test_dimension_expressions(self):
        expressions = [
            '-(-3)', '+-+3', '1_000 / 8', '0.5e1', '.5', '1.', '00', '3 * (2 + 1)', '((1))', '7 - 2 - 1',
            '8 / 4 / 2', '2 * 3 + 4 * 5', ' 2\t+ 3 ', 'cross_x(0, 0, 10, 10, 0, 10, 10, 0)',
            'cross_y(0, 0, 10, 10, 0, 10, 10, 0)', 'cross_x(0, 0, 1, 0, 0, 1, 1, 1)', 'cross_x(1, 2, 3)',
            'cross_x(0, 0, 1, 0, 2, 1, 2, 2)', 'f(1)', 'f()', 'True', 'None', '1/0', '1/-0.0', '1e400', '100001',
            '100000', '-100000', '1e5 * 2', 'unknown', '', '   ', 'x' * 161, '1+' * 16 + '1', '1+' * 17 + '1',
            'w+' * 15 + 'w', '-' * 30 + '1', '2.5e-3', '1_0.0_5',
        ]  # fmt: skip
        compared = 0
        for text in expressions:
            with self.subTest(text=text):
                model = {'version': 1, 'parameters': {'w': 2}, 'features': box([10, 10, text])['features']}
                native_answer, reference = (outcome(rebuild, model, backend=b) for b in ('native', 'python'))
                if native_answer is not None:
                    compared += 1
                    self.assertEqual(native_answer, reference)
        self.assertGreater(compared, 30)

    def test_what_the_core_does_not_read_is_declined_to_the_reference(self):
        for text in ('w ** 2', 'w // 2', 'w if 1 else 2', '(w, 1)', 'w.real', '0x10', '1j', 'w # note', 'é'):
            model = {'version': 1, 'parameters': {'w': 2}, 'features': box([10, 10, text])['features']}
            with self.subTest(text=text):
                self.assertIsNone(outcome(rebuild, model, backend='native'))
                self.assertIsNotNone(outcome(rebuild, model))
        sketch = {
            'version': 1,
            'features': [{'id': 's', 'kind': 'extrude', 'height': 2, 'profile': {'points': [[0, 0], [5, 0], [0, 5]]}}],
        }
        with self.assertRaises(NotImplementedError):
            rebuild(sketch, backend='native')
        self.assertIsNone(model_json(sketch))

    def test_the_route_sends_the_core_text(self):
        client = create_app().test_client()
        headers = {'X-Studio-Token': client.get('/api/session').json['token']}
        with patch.object(solid_cad, 'rebuild', wraps=solid_cad.rebuild) as reference:
            answer = client.post('/api/solid-cad', json={'model': plate()}, headers=headers)
        self.assertEqual(answer.status_code, 200)
        reference.assert_not_called()
        self.assertEqual(json.dumps(answer.get_json(), sort_keys=True), outcome(rebuild, plate(), backend='python'))
        refused = client.post('/api/solid-cad', json={'model': box([1, 1, 0])}, headers=headers)
        self.assertEqual(refused.status_code, 400)
        self.assertIn('b: Box dimensions must be positive', refused.get_data(as_text=True))


if __name__ == '__main__':
    unittest.main()
