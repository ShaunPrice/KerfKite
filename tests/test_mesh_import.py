import base64
import json
import math
import os
import random
import struct
import unittest
from unittest.mock import patch

from studio import cam
from studio.mesh_import import stl_heightmap


def ascii_mesh(triangles):
    facets = []
    for triangle in triangles:
        vertices = '\n'.join('vertex ' + ' '.join(map(str, point)) for point in triangle)
        facets.append('facet normal 0 0 1\nouter loop\n' + vertices + '\nendloop\nendfacet')
    return ('solid test\n' + '\n'.join(facets) + '\nendsolid test').encode()


def binary_mesh(triangles):
    header = b'solid binary header'.ljust(80, b'\0')
    return (
        header
        + struct.pack('<I', len(triangles))
        + b''.join(
            struct.pack('<12fH', 0, 0, 1, *(coordinate for point in triangle for coordinate in point), 0)
            for triangle in triangles
        )
    )


def convert(raw, resolution=9):
    return stl_heightmap({'content_base64': base64.b64encode(raw).decode(), 'resolution': resolution})


RAMP = [[(0, 0, 0), (10, 0, 10), (10, 10, 10)], [(0, 0, 0), (10, 10, 10), (0, 10, 0)]]


class MeshImportTests(unittest.TestCase):
    def test_ascii_binary_ramp_conservative_and_not_flat(self):
        results = [convert(encoder(RAMP)) for encoder in (ascii_mesh, binary_mesh)]
        self.assertEqual(results[0], results[1])
        result = results[0]
        self.assertEqual(result['source_bounds']['size'], [10, 10, 10])
        self.assertEqual(result['source_bounds']['triangles'], 2)
        matrix = result['heightmap']
        self.assertLess(matrix[4][0], 0.3)
        for row in matrix:
            for x, value in enumerate(row):
                self.assertGreaterEqual(value + 1e-10, x / 8)
                self.assertLessEqual(value, 1)
        # Linear interpolation within every cell also bounds the exact plane.
        for row in matrix:
            for x in range(8):
                self.assertGreaterEqual((row[x] + row[x + 1]) / 2 + 1e-10, (x + 0.5) / 8)

    def test_thin_peak_between_raster_vertices_is_retained(self):
        base = [[(0, 0, 0), (10, 0, 0), (10, 10, 0)], [(0, 0, 0), (10, 10, 0), (0, 10, 0)]]
        tiny_peak = [(5.21, 5.21, 0), (5.23, 5.21, 0), (5.22, 5.23, 10)]
        matrix = convert(ascii_mesh(base + [tiny_peak]))['heightmap']
        # Peak lies inside cell (4,4), not on any raster sample.
        for y in (4, 5):
            for x in (4, 5):
                self.assertEqual(matrix[y][x], 1)
        self.assertEqual(matrix[0][0], 0)

    def test_empty_exterior_cells_are_stock_top(self):
        triangle = [[(0, 0, 0), (10, 0, 10), (0, 10, 0)]]
        matrix = convert(ascii_mesh(triangle))['heightmap']
        self.assertEqual(matrix[8][8], 1)
        self.assertEqual(matrix[7][7], 1)
        self.assertLess(matrix[1][1], 0.5)

    def test_flat_surface_normalizes_to_stock_top(self):
        triangle = [[(0, 0, 7), (10, 0, 7), (0, 10, 7)]]
        result = convert(ascii_mesh(triangle))
        self.assertEqual(result['source_bounds']['size'][2], 0)
        self.assertTrue(all(v == 1 for row in result['heightmap'] for v in row))

    def test_reject_malformed_and_degenerate(self):
        invalid = [
            b'',
            b'solid test\nendsolid test',
            b'not stl',
            ascii_mesh(RAMP).replace(b'vertex', b'vertices', 1),
            binary_mesh(RAMP)[:-1],
            ascii_mesh([[(0, 0, 0), (0, 1, 1), (0, 2, 2)]]),
            ascii_mesh(RAMP).replace(b'10', b'1e999', 1),
        ]
        for raw in invalid:
            with self.subTest(raw=raw[:20]), self.assertRaises(ValueError):
                convert(raw)
        for resolution in (0, 1, 129, True, 8.5):
            with self.assertRaises(ValueError):
                convert(ascii_mesh(RAMP), resolution)
        for content in ('@@@', None, 'A' * (12 * 1024 * 1024)):
            with self.assertRaises(ValueError):
                stl_heightmap({'content_base64': content})
        with self.assertRaises(ValueError):
            convert(binary_mesh([[(0, 0, 0), (1, 0, math.inf), (0, 1, 0)]]))

    def test_work_and_triangle_caps(self):
        with self.assertRaises(ValueError):
            convert(binary_mesh(RAMP * 100), resolution=128)
        with self.assertRaises(ValueError):
            convert(binary_mesh(RAMP * 25001))


def exact(value):
    """A form that tells 0 from 0.0 and -0.0 from 0.0."""
    return json.loads(json.dumps(value), parse_float=lambda text: ('f', text), parse_int=lambda text: ('i', text))


def answer(data, backend):
    try:
        return 'ok', exact(stl_heightmap(data, backend=backend))
    except ValueError as error:
        return 'error', str(error)


@unittest.skipIf(cam._core_cam is None, 'Build the extension to compare the native core with the reference')
class NativeMeshImportTests(unittest.TestCase):
    """The native core's height fields against this module's Python, the reference: to the bit."""

    def setUp(self):
        environment = patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop('KERFKITE_GCODE_BACKEND', None)

    def test_height_fields_and_refusals_are_the_reference(self):
        rng = random.Random(31)
        bumps = [[tuple(rng.uniform(-20, 20) for _ in range(3)) for _ in range(3)] for _ in range(40)]
        meshes = [ascii_mesh(RAMP), binary_mesh(RAMP), ascii_mesh(bumps), binary_mesh(bumps)]
        meshes.append(
            b'  SOLID x\r\nFACET\tnormal 0 0 1 outer loop vertex 0 0 0 vertex 1.e1 0 1E1 vertex +.5 10 -0\n'
            b'endloop endfacet\x0cendsolid\n\n'
        )
        meshes += [
            b'solid\nfacet normal 0 0 1e400\nendsolid',
            b'solid x\nendsolid',
            b'solid\nfacet\nendsolid',
            b'\xff' * 90,
        ]
        for raw in meshes:
            for resolution in (2, 9, 64, 128):
                data = {'content_base64': base64.b64encode(raw).decode(), 'resolution': resolution}
                with self.subTest(raw=raw[:40], resolution=resolution):
                    self.assertEqual(answer(data, 'native'), answer(data, 'python'))
        for data in (
            {'content_base64': 'AAAA', 'resolution': 64.0},
            {'content_base64': 'AA=A'},
            {'content_base64': '\U0001f600' * 1000},
            {'content_base64': 'AAAA\u00e9AAA'},
            {},
            None,
        ):
            with self.subTest(data=data):
                self.assertEqual(answer(data, 'native'), answer(data, 'python'))

    def test_padding_after_whole_groups_goes_to_the_reference(self):
        # Python 3.12.3 reads "AAAA=" as three bytes and 3.13 refuses it: the core declines, so the Python in use answers.
        data = {'content_base64': 'AAAA=', 'resolution': 8}
        with self.assertRaises(NotImplementedError):
            stl_heightmap(data, backend='native')
        self.assertEqual(answer(data, 'auto'), answer(data, 'python'))

    def test_the_work_limit_matches_the_reference(self):
        wide = [[(0, 0, 0), (100, 0, 1), (100, 100, 2)]] * 130
        data = {'content_base64': base64.b64encode(binary_mesh(wide)).decode(), 'resolution': 128}
        self.assertEqual(answer(data, 'native'), answer(data, 'python'))
        self.assertEqual(answer(data, 'native')[0], 'error')


if __name__ == '__main__':
    unittest.main()
