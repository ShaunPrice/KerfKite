"""The native core's surface mesher against the Python reference (NumPy and Shapely)."""

import json
import random
import unittest

from studio import cad_exchange
from studio.cad_exchange import surface_mesh
from validation.surface_mesh_check import compare, surface

AVAILABLE = cad_exchange._core_cam is not None

PATCH = {'poles': [[[0, 0, 0], [0, 12, 3]], [[20, 0, 4], [20, 12, 0]]]}


def answer(given, backend):
    try:
        return surface_mesh(json.loads(json.dumps(given)), backend=backend)
    except ValueError as error:
        return 'refused: ' + str(error)
    except NotImplementedError:
        return None


@unittest.skipUnless(AVAILABLE, 'the compiled extension is not installed')
class SurfaceCoreTests(unittest.TestCase):
    def test_patches_and_sheets_match_the_reference(self):
        rng = random.Random(5)  # noqa: S311 - generated cases, not secrets
        meshed = 0
        for _ in range(150):
            given = surface(rng)
            core, reference = answer(given, 'native'), answer(given, 'python')
            if core is None:
                continue
            self.assertIsNone(compare(given, core, reference), json.dumps(given)[:300])
            meshed += isinstance(core, dict)
        self.assertGreater(meshed, 90)

    def test_untrimmed_patch_is_the_reference_to_rounding(self):
        core, reference = answer(PATCH, 'native'), answer(PATCH, 'python')
        self.assertEqual(core['triangles'], reference['triangles'])
        for mine, theirs in zip(core['vertices'], reference['vertices'], strict=True):
            for a, b in zip(mine, theirs, strict=True):
                self.assertAlmostEqual(a, b, places=12)

    def test_joined_sheet_resolves_its_joins_to_the_bit(self):
        child = {'poles': [[[20, 0, 4], [20, 12, 0]], [[40, 0, 1], [40, 12, 5]]], 'continuity': 'tangent'}
        sheet = {'kind': 'sheet', 'axis': 'u', 'patches': [PATCH, child]}
        core, reference = answer(sheet, 'native'), answer(sheet, 'python')
        self.assertEqual(
            json.dumps(core['resolvedPatches'], sort_keys=True),
            json.dumps(reference['resolvedPatches'], sort_keys=True),
        )

    def test_outline_with_a_hole(self):
        outline = {
            **PATCH,
            'boundary': [
                [[0.1, 0.1], [0.9, 0.15], [0.85, 0.9], [0.15, 0.8]],
                [[0.4, 0.4], [0.4, 0.6], [0.6, 0.6], [0.6, 0.4]],
            ],
        }
        core, reference = answer(outline, 'native'), answer(outline, 'python')
        self.assertIsNone(compare(outline, core, reference))
        self.assertAlmostEqual(core['area'], reference['area'], delta=1e-6 * reference['area'])

    def test_refusals_match_and_touching_loops_are_declined(self):
        for change in (
            {'poles': [[[0, 0, 0]]]},
            {'kind': 'nurbs'},
            {'weights': [[1, 1], [1, 0]]},
            {'trim': [0.5, 0.4, 0, 1]},
            {'boundary': [[[0.1, 0.1], [0.9, 0.9], [0.1, 0.9], [0.9, 0.1]]]},
            {'boundary': [[[0.1, 0.1], [0.4, 0.1], [0.4, 0.4]], [[0.6, 0.6], [0.8, 0.6], [0.8, 0.8]]]},
        ):
            given = {**PATCH, **change}
            self.assertEqual(answer(given, 'native'), answer(given, 'python'), change)
            self.assertTrue(answer(given, 'native').startswith('refused: '))
        touching = {
            **PATCH,
            'boundary': [[[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]], [[0.1, 0.1], [0.3, 0.2], [0.2, 0.3]]],
        }
        self.assertIsNone(answer(touching, 'native'))
        self.assertEqual(answer(touching, 'auto'), answer(touching, 'python'))

    def test_the_route_meshes_in_the_core(self):
        from studio.server import create_app

        client = create_app().test_client()
        headers = {'X-Studio-Token': client.get('/api/session').json['token']}
        reply = client.post('/api/solid-cad/surface', json={'surface': PATCH}, headers=headers)
        self.assertEqual(reply.status_code, 200)
        self.assertEqual(reply.get_json(), json.loads(json.dumps(answer(PATCH, 'native'))))


class SurfaceFallbackTests(unittest.TestCase):
    def test_python_answers_alone(self):
        self.assertEqual(len(surface_mesh(PATCH, backend='python')['triangles']), 1152)
        with self.assertRaises(ValueError):
            surface_mesh(PATCH, backend='other')


if __name__ == '__main__':
    unittest.main()
