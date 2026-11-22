"""The native core's relief contact and stock preview give the same bits on any number of threads."""

import os
import random
import unittest
from contextlib import contextmanager

from studio import relief, simulation

threaded = relief._native('auto')


@contextmanager
def threads(count):
    """Run with KERFKITE_THREADS set to count (None: not set)."""
    old = os.environ.get('KERFKITE_THREADS')
    if count is None:
        os.environ.pop('KERFKITE_THREADS', None)
    else:
        os.environ['KERFKITE_THREADS'] = str(count)
    try:
        yield
    finally:
        if old is None:
            os.environ.pop('KERFKITE_THREADS', None)
        else:
            os.environ['KERFKITE_THREADS'] = old


def answer(call, *args, **kwargs):
    """The result in a form that tells every bit, or its error message, so that errors are compared too."""
    try:
        result = call(*args, **kwargs)
    except ValueError as error:
        return ('ValueError', str(error))
    result.pop('device', None)
    return repr(result)


def contact_jobs():
    rng = random.Random(2026)  # noqa: S311 - reproducible test data
    jobs = []
    for _ in range(12):
        rows, columns = rng.randrange(2, 90), rng.randrange(2, 90)
        depth = rng.choice([0.5, 3.0])
        diameter = rng.choice([0.5, 3.0, 9.0, 40.0])
        jobs.append(
            dict(
                heightmap=[[rng.random() for _ in range(columns)] for _ in range(rows)],
                width=rng.uniform(5, 80),
                height=rng.uniform(5, 80),
                depth=depth,
                tool_diameter=diameter,
                stepover=min(diameter, rng.choice([0.5, 1.0, 2.0])),
                allowance=rng.choice([0.0, depth / 3]),
                tool_shape=rng.choice(['ball', 'flat']),
            )
        )
    return jobs


def stock_jobs():
    rng = random.Random(4041)  # noqa: S311 - reproducible test data
    jobs = []
    for case in range(10):
        segments = []
        for _ in range(rng.choice([1, 40, 600])):
            ax, ay = rng.uniform(-10, 110), rng.uniform(-10, 110)
            bx, by = rng.uniform(-10, 110), rng.uniform(-10, 110)
            segment = [ax, ay, bx, by, -rng.uniform(0, 9)]
            if rng.random() < 0.3:
                segment += [0, -rng.uniform(0, 9)]
            segments.append(segment)
        if case == 3:
            segments[len(segments) // 2][4] = -50.0  # deeper than the stock: an error part-way through
        jobs.append(
            dict(
                segments=segments,
                resolution=rng.randrange(16, 97),
                width=100.0,
                height=100.0,
                tool_diameter=rng.choice([1.0, 6.0]),
                tool_shape=('flat', 'ball', 'v', 'drill')[case % 4],
                v_angle=60.0,
                stock_thickness=10.0,
            )
        )
    return jobs


@unittest.skipUnless(threaded, 'The compiled extension is not installed')
class ThreadCountTests(unittest.TestCase):
    def test_contact_is_the_same_on_any_number_of_threads(self):
        for job in contact_jobs():
            with threads(1):
                expected = answer(relief.prepare_relief, job, backend='native')
            for count in (2, 3, 7, None, 'many', 0):
                with self.subTest(threads=count), threads(count):
                    self.assertEqual(answer(relief.prepare_relief, job, backend='native'), expected)

    def test_stock_preview_is_the_same_on_any_number_of_threads(self):
        for job in stock_jobs():
            with threads(1):
                expected = answer(simulation.simulate, job, backend='native')
            for count in (2, 3, 7, None):
                with self.subTest(threads=count), threads(count):
                    self.assertEqual(answer(simulation.simulate, job, backend='native'), expected)

    def test_contact_caps(self):
        settings = dict(width=10.0, height=10.0, depth=2.0, tool_diameter=0.5, stepover=0.5, tool_shape='flat')
        for rows, columns, ok in ((1024, 2, True), (2, 1024, True), (1025, 2, False), (2, 1025, False)):
            data = dict(settings, heightmap=[[0.5] * columns for _ in range(rows)])
            with self.subTest(rows=rows, columns=columns):
                if ok:
                    self.assertEqual(relief.prepare_relief(data, backend='native')['surface']['rows'], rows)
                else:
                    with self.assertRaisesRegex(ValueError, '2..1024'):
                        relief.prepare_relief(data, backend='native')
        heavy = dict(
            settings, heightmap=[[0.0] * 1000 for _ in range(1000)], width=100.0, height=100.0, tool_diameter=100.0,
            stepover=2.0,
        )  # fmt: skip
        with self.assertRaisesRegex(ValueError, 'Relief contact calculation too large'):
            relief.prepare_relief(heavy, backend='native')


if __name__ == '__main__':
    unittest.main()
