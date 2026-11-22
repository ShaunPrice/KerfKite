"""The native core's relief contact on a GPU, where the computer has one, against its threads bit for bit.

The device is chosen once per process from KERFKITE_GPU, so each mode runs in a process of its own. Without a
usable GPU the GPU comparison is skipped with the reason the runtime gives, and the threads must still answer.
"""

import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

try:
    from studio import _relief_native as native
except ImportError:
    native = None

ROOT = Path(__file__).resolve().parents[1]

# map rows, map columns, width, height, depth, diameter, allowance, stepover, shape
JOBS = [
    (3, 3, 1.0, 1.0, 1.0, 0.1, 0.0, 0.1, 'ball'),
    (41, 41, 10.0, 10.0, 2.0, 3.0, 0.0, 0.2, 'ball'),
    (121, 91, 60.0, 40.0, 3.0, 6.0, 0.2, 0.5, 'flat'),
    (256, 201, 100.0, 80.0, 5.0, 3.175, 0.0, 0.8, 'ball'),
    (18, 256, 5.0, 60.0, 1.0, 0.5, 0.1, 0.5, 'flat'),
    (301, 301, 90.0, 90.0, 4.0, 25.0, 0.3, 0.75, 'ball'),
]

SCRIPT = r'''
import json, random, sys
from studio import _relief_native as r
from studio.relief import prepare_relief
jobs = json.loads(sys.argv[1])
rng = random.Random(11)
results, paths = [], []
for rows, cols, w, h, d, diam, allow, step, shape in jobs:
    heightmap = [[rng.random() for _ in range(cols)] for _ in range(rows)]
    data = dict(heightmap=heightmap, width=w, height=h, depth=d, tool_diameter=diam, allowance=allow,
                stepover=step, tool_shape=shape)
    depths = prepare_relief(data, backend='native')['path_depths']
    results.append([[v.hex() for v in row] for row in depths])
    paths.append(r.last_contact())
print(json.dumps({'status': r.gpu_status(), 'results': results, 'paths': paths}))
'''


def run(mode, jobs=JOBS):
    env = {**os.environ, 'PYTHONPATH': str(ROOT)}
    env.pop('KERFKITE_GPU', None)
    if mode is not None:
        env['KERFKITE_GPU'] = mode
    done = subprocess.run(  # noqa: S603 - the test's own interpreter and script
        [sys.executable, '-c', SCRIPT, json.dumps(jobs)],
        capture_output=True,
        text=True,
        env=env,
        cwd=ROOT,
        timeout=600,
        check=False,
    )
    if done.returncode:
        raise AssertionError(done.stderr)
    return json.loads(done.stdout)


@unittest.skipUnless(native is not None and hasattr(native, 'gpu_status'), 'the compiled extension is not installed')
class GpuContactTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.threads = run('off')

    def test_off_keeps_every_job_on_the_threads(self):
        self.assertEqual(self.threads['status'], 'turned off by KERFKITE_GPU=off')
        self.assertTrue(all(path.startswith('CPU, ') for path in self.threads['paths']), self.threads['paths'])

    def test_a_gpu_gives_the_threads_bits(self):
        forced = run('on')
        if not any(path.startswith('GPU, ') for path in forced['paths']):
            self.assertTrue(all(path.startswith('CPU, ') for path in forced['paths']), forced['paths'])
            self.skipTest('no usable GPU: ' + forced['status'])
        self.assertEqual(forced['paths'], ['GPU, ' + forced['status']] * len(JOBS), forced['status'])
        self.assertEqual(forced['results'], self.threads['results'])

    def test_small_jobs_stay_on_the_threads_unless_asked(self):
        automatic = run(None, JOBS[:3])
        self.assertTrue(all(path.startswith('CPU, ') for path in automatic['paths']), automatic['paths'])
        self.assertEqual(automatic['results'], self.threads['results'][:3])
        self.assertTrue(automatic['status'])


if __name__ == '__main__':
    unittest.main()
