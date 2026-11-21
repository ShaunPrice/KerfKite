"""Parity of the public planar/rotary export and the packed stock/rotary paths."""

import math
import unittest
from unittest.mock import patch

from studio.cam import generate
from studio.cam_moves import PackedSegments
from studio.rotary import generate_rotary, wrap_job
from studio import simulation


def project(**changes):
    values = dict(
        protocol='grbl',
        mode='router',
        width=200,
        height=100,
        safe=5,
        feed=543.25,
        power=800,
        plunge=123.45,
        depth=2,
        step=1,
        paths=[[[0, 5], [31.41593, 5.001], [62.83185, 7]]],
    )
    return {**values, **changes}


class PackedCamTests(unittest.TestCase):
    def test_rotary_export_matches_posting_text(self):
        router = project(path_depths=[[-1.0411, -1.50, -0.0000001]])
        laser = project(mode='laser', path_powers=[[0, 231.5, 0]], passes=2)
        ring = [[[8, 8], [30, 8], [30, 30], [8, 30], [8, 8]]]
        ramp = project(paths=ring, router_entry='ramp', entry_angle=5)
        helix = project(paths=ring, operation='pocket', router_entry='helix', entry_radius=1, entry_angle=5)
        drill = project(paths=[[[20, 20], [20, 20]]], operation='drill', tool_shape='drill', drill_peck=0.5)
        layers = project(operations=[{'paths': ring}, {'paths': router['paths'], 'path_depths': router['path_depths']}])
        for p in (router, laser, ramp, helix, drill, layers):
            canonical = generate(p)
            packed = generate(p, packed_segments=True)
            self.assertIsInstance(packed['segments'], PackedSegments)
            self.assertEqual(list(packed['segments']), canonical['segments'])
            self.assertEqual(packed['lines'], canonical['lines'])
            for post, axis in [('xpro-v5', 'A'), ('grblhal', 'B'), ('linuxcnc', 'C')]:
                if p['mode'] == 'laser' and post == 'linuxcnc':
                    continue
                for wrapped in 'XY':
                    options = dict(
                        diameter=10,
                        wrap_axis=wrapped,
                        postprocessor=post,
                        output_axis=axis,
                        angle_offset=12,
                        direction=-1,
                        feed_reference='cut',
                    )
                    with self.subTest(mode=p['mode'], operation=p.get('operation'), post=post, axis=wrapped):
                        self.assertEqual(generate_rotary(p, options), wrap_job(canonical, options))

    def test_rotary_does_not_lose_signed_zero_in_posted_depth(self):
        p = project(path_depths=[[-1, -0.0, -1]])
        opts = dict(diameter=10, wrap_axis='X', postprocessor='grblhal')
        self.assertEqual(generate_rotary(p, opts)['lines'], wrap_job(generate(p), opts)['lines'])

    def test_fallback_handles_legacy_five_fields_and_packed_segments(self):
        elements = [[1, 1, 6, 6, -1], [2, 2, 2, 2, -1, {'ignored': True}]]
        data = dict(segments=elements, width=8, height=8)
        with patch.object(simulation, 'stock_heights', None):
            height = simulation.simulate(data)
            self.assertEqual(height, simulation.simulate(data, backend='python'))
            with self.assertRaisesRegex(ValueError, 'not installed'):
                simulation.simulate(data, backend='native')
            with self.assertRaises(ValueError):
                simulation.simulate(data, backend='invalid')
            packed = PackedSegments()
            for x, y, nx, ny, z, *_ in elements:
                packed.append([x, y, nx, ny, z, None, z])
            self.assertEqual(height, simulation.simulate({**data, 'segments': packed}))


@unittest.skipIf(simulation.stock_heights is None, 'Build the fast stock kernel to exercise native parity')
class NativeStockTests(unittest.TestCase):
    def test_full_grid_and_legacy_shapes(self):
        inputs = [
            ['1', 2, 5, 4, -0.3, None, -0.7],
            [0, 1, 7, 1, -1.2],
            [3, 3, 3, 3, -0.5, {'ignored': True}],
            [80, 90, 900, 1100, -0.5, 0, -1, 'extra'],
        ]
        for tool in ('flat', 'ball', 'v', 'drill'):
            data = dict(segments=inputs, x=1, y=1, width=8, height=7, tool_shape=tool, tool_diameter=2.7)
            native = simulation.simulate(data, backend='native')
            python = simulation.simulate(data, backend='python')
            for a, b in zip(native.pop('heights'), python.pop('heights')):
                self.assertTrue(all(math.isclose(i, j, rel_tol=0, abs_tol=1e-12) for i, j in zip(a, b)))
            self.assertEqual(native, python)

    def test_limits_and_non_finite_inputs(self):
        for item in (
            [0, 0, 1, 1, -20],
            [0, 0, 1, math.inf, 0],
            [0, 0, 0, 0, 0, 0, math.nan],
            [0, 0],
            [0, 0, 1, 1, object()],
        ):
            for backend in ('python', 'native'):
                with self.subTest(item=item, backend=backend), self.assertRaises((ValueError, TypeError)):
                    simulation.simulate(dict(segments=[item]), backend=backend)
        for backend in ('python', 'native'):
            with self.assertRaisesRegex(ValueError, 'numeric preview range'):
                simulation.simulate(dict(segments=[[1e308, 0, 1e308, 0, 0]], width=0.1), backend=backend)
        with self.assertRaisesRegex(ValueError, 'Simulation too large'):
            simulation.simulate(dict(segments=[[0, 0, 100, 100, -1]] * 1000, resolution=96), backend='native')
        call = simulation.stock_heights
        with self.assertRaises(ValueError):
            call(b'wrong stride', 48, 10.0, 10.0, 0.0, 0.0, 2.0, 0, 60.0, 10.0)
        with self.assertRaises(ValueError):
            call(bytes(7 * 8), 10**8, 10.0, 10.0, 0.0, 0.0, 2.0, 0, 60.0, 10.0)

    def test_in_process_geometry_reuses_packed_rows(self):
        p = project()
        packed = generate(p, packed_segments=True)['segments']
        buf = packed.buffer()
        self.assertIs(buf, packed.buffer())
        native = simulation.simulate(dict(segments=packed), backend='native')
        reference = simulation.simulate(dict(segments=generate(p)['segments']), backend='python')
        self.assertEqual(native, reference)
