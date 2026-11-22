"""Time Studio's preview routes as the editor calls them, with digests of their answers.

    python validation/benchmark_routes.py

Through Flask's test client, so the JSON reading and writing of each request is counted: /api/relief for reliefs
from a synthetic 1024-point RGB image with a 6 mm and a 1.1 mm ball, and /api/generate and the core's JSON text
(studio.cam.job_json) for the 135,886-line relief job of validation/benchmark_gcode.py. The GPU is turned off, so
the reliefs run on the processor's threads. Best of seven or nine. The digests are of the answers as JSON gives
them back, keys sorted, so they are the same whichever code wrote the text; the job's digests differ between
platforms because its height map uses the platform's sine and cosine.
"""

import base64
import gc
import hashlib
import json
import os
import sys
import time
from pathlib import Path

os.environ['KERFKITE_GPU'] = 'off'
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from studio.cam import job_json  # noqa: E402
from studio.relief import prepare_relief  # noqa: E402
from studio.server import create_app  # noqa: E402
from validation.stress_cam import fixture  # noqa: E402


def best(run, count):
    times = []
    for _ in range(count):
        gc.collect()
        began = time.perf_counter()
        run()
        times.append(time.perf_counter() - began)
    return min(times) * 1000


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()[:16]


def main():
    side = 1024
    pixels = bytearray()
    for j in range(side):
        for i in range(side):
            ridge, noise = (3 * i + 2 * j) % 256, (i * 7919 + j * 104729) % 251
            pixels += bytes(((ridge * 3 + noise) % 256, (ridge + 2 * noise) % 256, (ridge * 5 + noise * 3) % 256))
    image = base64.b64encode(bytes(pixels)).decode()
    client = create_app().test_client()
    headers = {'X-Studio-Token': client.get('/api/session').json['token']}

    def post(path, body):
        answer = client.post(path, json=body, headers=headers)
        if answer.status_code != 200:
            raise SystemExit(f'{path}: {answer.status_code} {answer.data[:300]!r}')
        return answer

    rows = []
    for diameter in (6.0, 1.1):
        body = dict(
            heightmap_rgb=image, heightmap_size=[side, side], width=100.0, height=100.0, depth=4.0,
            tool_diameter=diameter, stepover=1.1, tool_shape='ball',
        )  # fmt: skip
        answer = post('/api/relief', body)
        rows.append(
            (f'/api/relief, 1024 map, {diameter} mm ball', best(lambda: post('/api/relief', body), 7), answer.json)
        )
    relief = prepare_relief(fixture())
    project = dict(
        mode='router', protocol='grbl', width=680, height=240, feed=1200, plunge=300, power=1000, safe=5, depth=6,
        step=3, tool_diameter=3.175, paths=relief['paths'], path_depths=relief['path_depths'],
    )  # fmt: skip
    text = job_json(project)
    if text is not None:
        rows.append(('job_json, relief job', best(lambda: job_json(project), 9), json.loads(text)))
    answer = post('/api/generate', project)
    rows.append(('/api/generate, relief job', best(lambda: post('/api/generate', project), 9), answer.json))
    for name, milliseconds, value in rows:
        print(f'{name:40} {milliseconds:8.1f} ms  {digest(value)}')


if __name__ == '__main__':
    main()
