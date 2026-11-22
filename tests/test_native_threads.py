"""The compiled relief contact and stock preview give the same bits on any number of threads."""

import os
import random
import struct
import unittest
from contextlib import contextmanager

from studio import relief

native = relief._relief_native
threaded = native is not None and hasattr(native, 'contact_packed')


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


def answer(call, *args):
    """The call's result, or its error message, so that errors are compared too."""
    try:
        return call(*args)
    except ValueError as error:
        return ('ValueError', str(error))


def packed(maxima):
    return b''.join(struct.pack(f'={len(row)}d', *row) for row in maxima)


def contact_jobs():
    rng = random.Random(2026)  # noqa: S311 - reproducible test data
    jobs = []
    for _ in range(12):
        rows = rng.randrange(1, 90)
        cols = rng.randrange(1, 90)
        depth = rng.choice([0.5, 3.0])
        maxima = [[-depth * rng.random() for _ in range(cols)] for _ in range(rows)]
        nx = rng.randrange(1, 200)
        ny = rng.randrange(1, 200)
        while (nx + 1) * (ny + 1) > 100000:
            ny //= 2
        diameter = rng.choice([0.5, 3.0, 9.0, 40.0])
        allowance = rng.choice([0.0, depth / 3])
        shape = rng.choice(['ball', 'flat'])
        jobs.append((maxima, rng.uniform(5, 80), rng.uniform(5, 80), depth, diameter, allowance, nx, ny, shape))
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
            (segments, rng.randrange(16, 97), 100.0, 100.0, 0.0, 0.0, rng.choice([1.0, 6.0]), case % 4, 60.0, 10.0)
        )
    return jobs


@unittest.skipUnless(threaded, 'The threaded C++ extension is not installed')
class ThreadCountTests(unittest.TestCase):
    def test_contact_is_the_same_on_any_number_of_threads(self):
        for job in contact_jobs():
            with threads(1):
                expected = answer(native.contact, *job)
            for count in (2, 3, 7, None, 'many', 0):
                with self.subTest(threads=count), threads(count):
                    self.assertEqual(answer(native.contact, *job), expected)

    def test_packed_maxima_give_what_lists_give(self):
        for maxima, *rest in contact_jobs():
            with self.subTest(rows=len(maxima), columns=len(maxima[0])):
                self.assertEqual(
                    answer(native.contact_packed, packed(maxima), len(maxima), len(maxima[0]), *rest),
                    answer(native.contact, maxima, *rest),
                )

    def test_stock_preview_is_the_same_on_any_number_of_threads(self):
        for job in stock_jobs():
            with threads(1):
                expected = answer(native.stock_heights, *job)
            for count in (2, 3, 7, None):
                with self.subTest(threads=count), threads(count):
                    self.assertEqual(answer(native.stock_heights, *job), expected)

    def test_contact_caps(self):
        settings = (10.0, 10.0, 2.0, 0.5, 0.0, 4, 4, 'flat')
        for rows, cols, ok in ((1023, 1, True), (1, 1023, True), (1024, 1, False), (1, 1024, False)):
            maxima = [[-1.0] * cols for _ in range(rows)]
            with self.subTest(rows=rows, cols=cols):
                for call in (
                    lambda: native.contact(maxima, *settings),
                    lambda: native.contact_packed(packed(maxima), rows, cols, *settings),
                ):
                    if ok:
                        self.assertEqual(len(call()), 5)
                    else:
                        with self.assertRaisesRegex(ValueError, '1..1023'):
                            call()
        with self.assertRaisesRegex(ValueError, 'bytes holding'):
            native.contact_packed(packed([[-1.0] * 3]), 1, 4, *settings)
        with self.assertRaisesRegex(ValueError, 'bytes holding'):
            native.contact_packed([[-1.0] * 3], 1, 3, *settings)
        for value in (1.0, -3.0, float('nan'), float('inf')):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'cell maximum is out of range'):
                native.contact_packed(packed([[-1.0, value]]), 1, 2, *settings)
        with self.assertRaisesRegex(ValueError, 'exceeds 2000000000 checks'):
            native.contact([[0.0] * 1000 for _ in range(1000)], 100.0, 100.0, 2.0, 100.0, 0.0, 315, 315, 'flat')


if __name__ == '__main__':
    unittest.main()
