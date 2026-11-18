import base64
import math
import struct
import unittest

from studio.mesh_import import stl_heightmap


def ascii_mesh(triangles):
    facets = []
    for triangle in triangles:
        vertices = '\n'.join('vertex '+ ' '.join(map(str, point)) for point in triangle)
        facets.append('facet normal 0 0 1\nouter loop\n'+vertices+'\nendloop\nendfacet')
    return ('solid test\n'+'\n'.join(facets)+'\nendsolid test').encode()


def binary_mesh(triangles):
    header = b'solid binary header'.ljust(80, b'\0')
    return header+struct.pack('<I', len(triangles))+b''.join(
        struct.pack('<12fH', 0, 0, 1, *(coordinate for point in triangle for coordinate in point), 0)
        for triangle in triangles)


def convert(raw, resolution=9):
    return stl_heightmap({'content_base64': base64.b64encode(raw).decode(), 'resolution': resolution})


RAMP = [[(0, 0, 0), (10, 0, 10), (10, 10, 10)],
        [(0, 0, 0), (10, 10, 10), (0, 10, 0)]]


class MeshImportTests(unittest.TestCase):
    def test_ascii_binary_ramp_conservative_and_not_flat(self):
        results = [convert(encoder(RAMP)) for encoder in (ascii_mesh, binary_mesh)]
        self.assertEqual(results[0], results[1])
        result = results[0]
        self.assertEqual(result['source_bounds']['size'], [10, 10, 10])
        self.assertEqual(result['source_bounds']['triangles'], 2)
        matrix = result['heightmap']
        self.assertLess(matrix[4][0], .3)
        for row in matrix:
            for x, value in enumerate(row):
                self.assertGreaterEqual(value+1e-10, x/8)
                self.assertLessEqual(value, 1)
        # Linear interpolation within every cell also bounds the exact plane.
        for row in matrix:
            for x in range(8):
                self.assertGreaterEqual((row[x]+row[x+1])/2+1e-10, (x+.5)/8)

    def test_thin_peak_between_raster_vertices_is_retained(self):
        base = [[(0,0,0), (10,0,0), (10,10,0)], [(0,0,0), (10,10,0), (0,10,0)]]
        tiny_peak = [(5.21, 5.21, 0), (5.23, 5.21, 0), (5.22, 5.23, 10)]
        matrix = convert(ascii_mesh(base+[tiny_peak]))['heightmap']
        # Peak lies inside cell (4,4), not on any raster sample.
        for y in (4,5):
            for x in (4,5):
                self.assertEqual(matrix[y][x], 1)
        self.assertEqual(matrix[0][0], 0)

    def test_empty_exterior_cells_are_stock_top(self):
        triangle = [[(0,0,0), (10,0,10), (0,10,0)]]
        matrix = convert(ascii_mesh(triangle))['heightmap']
        self.assertEqual(matrix[8][8], 1)
        self.assertEqual(matrix[7][7], 1)
        self.assertLess(matrix[1][1], .5)

    def test_flat_surface_normalizes_to_stock_top(self):
        triangle = [[(0,0,7), (10,0,7), (0,10,7)]]
        result = convert(ascii_mesh(triangle))
        self.assertEqual(result['source_bounds']['size'][2], 0)
        self.assertTrue(all(v == 1 for row in result['heightmap'] for v in row))

    def test_reject_malformed_and_degenerate(self):
        invalid = [b'', b'solid test\nendsolid test', b'not stl',
                   ascii_mesh(RAMP).replace(b'vertex', b'vertices', 1),
                   binary_mesh(RAMP)[:-1],
                   ascii_mesh([[(0,0,0), (0,1,1), (0,2,2)]]),
                   ascii_mesh(RAMP).replace(b'10', b'1e999', 1)]
        for raw in invalid:
            with self.subTest(raw=raw[:20]), self.assertRaises(ValueError):
                convert(raw)
        for resolution in (0, 1, 129, True, 8.5):
            with self.assertRaises(ValueError):
                convert(ascii_mesh(RAMP), resolution)
        for content in ('@@@', None, 'A'*(12*1024*1024)):
            with self.assertRaises(ValueError):
                stl_heightmap({'content_base64': content})
        with self.assertRaises(ValueError):
            convert(binary_mesh([[(0,0,0), (1,0,math.inf), (0,1,0)]]))

    def test_work_and_triangle_caps(self):
        with self.assertRaises(ValueError):
            convert(binary_mesh(RAMP*100), resolution=128)
        with self.assertRaises(ValueError):
            convert(binary_mesh(RAMP*25001))


if __name__ == '__main__':
    unittest.main()
