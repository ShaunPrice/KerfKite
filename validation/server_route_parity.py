"""Route parity: KerfKite Studio's C++ workstation server (native/server) against the Flask reference.

Each case is one request. The same method, path, body and content type go to studio.server.create_app()'s test
client and to a kerfkite-studio process, both in simulator mode, and their answers must agree: the status, and
the JSON body value for value (or the bytes of an STL). Machine-control cases run as scripts, step by step on
both servers, waiting for the simulated controller to finish where a step starts a job.

Two routes are compared by what they describe rather than value for value, because their geometry comes from a
different algorithm (the reference used Shapely/GEOS; Studio's own polygon and solid kernels replace it, see
docs/NATIVE.md): /api/geometry (the region of the answer, to a stated tolerance) and /api/extrude (a closed mesh
of the same volume). Answers the C++ server gives in place of a reference fallback are reported apart:
"declined" where the core declines an input the Python answered, "defined by the core" where the server
answered by one of the core's own rules (it names them in a header when started with KERFKITE_PARITY=1), and
"no STEP" where the reference used Open CASCADE, which the C++ server does not include.

Usage:
    PYTHONPATH=$PWD python validation/server_route_parity.py --server /tmp/kkb/kerfkite-studio [--seed 1] [--mutations 6]

It prints one line per difference and a summary, and exits 1 when any case differs.
"""

import argparse
import base64
import copy
import http.client
import json
import math
import os
import random
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests'))

from studio.server import create_app  # noqa: E402

PCB = ROOT / 'tests' / 'pcb'
NO_STEP = 'STEP exchange needs Open CASCADE, which this build of KerfKite Studio does not include'
NO_OCP = 'Install the Studio step extra to enable Open CASCADE STEP exchange'
JSON = 'application/json'
STOCK = 'application/vnd.kerfkite.stock-v1'


# ---- the two servers -----------------------------------------------------------------------------------


class Reference:
    """The Flask app in this process."""

    def __init__(self):
        self.client = create_app().test_client()
        self.token = self.client.get('/api/session').get_json()['token']

    def request(self, method, path, body=None, content_type=None, headers=None):
        headers = dict(headers or {})
        if method == 'POST' and 'X-Studio-Token' not in headers:
            headers['X-Studio-Token'] = self.token
        response = self.client.open(path, method=method, data=body, content_type=content_type, headers=headers)
        return response.status_code, response.headers.get('Content-Type', ''), response.get_data()


class Answer(tuple):
    """(status, content type, body), with the core's own rules the C++ server named for it (own), if any."""

    own = None


class Native:
    """A kerfkite-studio process on a free port."""

    def __init__(self, program):
        self.process = subprocess.Popen(
            [program, '--port', '0', '--static', str(ROOT / 'studio' / 'static')],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=dict(os.environ, KERFKITE_PARITY='1'),
        )
        line = self.process.stdout.readline()
        if 'http://127.0.0.1:' not in line:
            raise SystemExit(f'kerfkite-studio did not start: {line!r}')
        self.port = int(line.rsplit(':', 1)[1].strip().rstrip('/'))
        status, _, body = self.request('GET', '/api/session')
        self.token = json.loads(body)['token']

    def request(self, method, path, body=None, content_type=None, headers=None):
        headers = dict(headers or {})
        if method == 'POST' and 'X-Studio-Token' not in headers and getattr(self, 'token', None):
            headers['X-Studio-Token'] = self.token
        headers.setdefault('Host', f'127.0.0.1:{self.port}')
        if content_type:
            headers['Content-Type'] = content_type
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=120)
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            answer = Answer((response.status, response.getheader('Content-Type', ''), response.read()))
            answer.own = response.getheader('X-KerfKite-Own-Rule')
            return answer
        finally:
            connection.close()

    def close(self):
        self.process.terminate()
        try:
            self.process.wait(5)
        except subprocess.TimeoutExpired:
            self.process.kill()


# ---- comparing answers ---------------------------------------------------------------------------------


def media(content_type):
    return content_type.split(';')[0].strip().lower()


# Python's mimetypes takes the type of a .js file from the Windows registry, where some systems still give
# application/javascript; the C++ server always sends text/javascript (RFC 9239).
SAME_MEDIA = {'application/javascript': 'text/javascript'}


def decoded(answer):
    status, content_type, body = answer
    if media(content_type) == JSON:
        try:
            return status, json.loads(body)
        except ValueError:
            return status, ('unreadable JSON', body[:200])
    return status, (SAME_MEDIA.get(media(content_type), media(content_type)), body)


def ports_agree(a, b):
    """pyserial lists ports through a set, so its order changes from run to run; the C++ server sorts them."""
    if isinstance(a, list) and isinstance(b, list):
        key = lambda port: str(port.get('device')) if isinstance(port, dict) else str(port)  # noqa: E731
        return first_difference(sorted(a, key=key), sorted(b, key=key))
    return first_difference(a, b)


def same(a, b):
    """Value for value: numbers by value (1 == 1.0), everything else exactly."""
    if isinstance(a, bool) or isinstance(b, bool):
        return type(a) is type(b) and a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return a == b or (math.isnan(a) and math.isnan(b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    return a == b


def first_difference(a, b, where='$'):
    if same(a, b):
        return None
    if isinstance(a, dict) and isinstance(b, dict):
        if a.keys() != b.keys():
            return f'{where}: keys {sorted(set(a) ^ set(b))[:6]}'
        for k in a:
            found = first_difference(a[k], b[k], f'{where}.{k}')
            if found:
                return found
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return f'{where}: {len(a)} items against {len(b)}'
        for i, (x, y) in enumerate(zip(a, b)):
            found = first_difference(x, y, f'{where}[{i}]')
            if found:
                return found
    return f'{where}: {str(a)[:160]!s} against {str(b)[:160]!s}'


def region_of(paths):
    from studio.operations import region

    return region(paths)


def geometry_agrees(reference, native, distance=0.0):
    """The two answers describe the same region: the area between them is small beside the outline."""
    try:
        a, b = region_of(reference['paths']), region_of(native['paths'])
    except (ValueError, KeyError, TypeError) as error:
        return f'unreadable region: {error}'
    gap = a.symmetric_difference(b).area
    # Round corners: the kernel's arcs are circumscribed and Shapely's inscribed, 32 edges a quarter turn
    # each: pi^3 r^2 / 128^2 between them for a whole turn.
    allowance = 1e-3 * max(a.length, 1.0) + 0.002 * distance**2
    if gap > allowance:
        return f'regions differ by {gap:.6g} mm² (allowed {allowance:.3g})'
    return None


def stl_triangles(text):
    vertices = [tuple(map(float, line.split()[1:])) for line in text.splitlines() if line.startswith('vertex ')]
    return [vertices[i : i + 3] for i in range(0, len(vertices), 3)]


def mesh_volume(triangles):
    total = 0.0
    for a, b, c in triangles:
        total += (
            a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])
        )
    return total / 6


def closed(triangles):
    edges = {}
    for t in triangles:
        for i in range(3):
            edge = (t[i], t[(i + 1) % 3])
            edges[edge] = edges.get(edge, 0) + 1
    return all(edges.get((b, a), 0) == count for (a, b), count in edges.items())


def step_agrees(reference, native):
    """STEP answers of Open CASCADE on both sides (the reference's OCP, the C++ server's add-on): documents by what
    they describe and imports by their counts and measures, as validation/step_exchange_parity.py compares them."""
    from validation.step_exchange_parity import agree, measures

    if isinstance(reference, tuple) and isinstance(native, tuple):
        if reference[0] != native[0]:
            return f'{reference[0]} against {native[0]}'
        if reference[0] == 'model/step':
            return agree(measures(reference[1]), measures(native[1]), 1e-9)
        return None  # an STL of a document: the tessellation of the release in use
    keys = ('faces', 'solids', 'solidOnly', 'volume', 'area', 'bounds')
    if not (
        isinstance(reference, dict) and isinstance(native, dict) and all(k in reference and k in native for k in keys)
    ):
        return first_difference(reference, native)
    return agree({k: reference[k] for k in keys}, {k: native[k] for k in keys}, 1e-9)


def extrusion_agrees(reference, native):
    if not same(reference['height'], native['height']):
        return f'height {reference["height"]} against {native["height"]}'
    ours = stl_triangles(native['stl'])
    if len(ours) != native['triangles']:
        return 'the triangle count does not match the STL'
    if not closed(ours):
        return 'the mesh is not closed'
    expected, got = mesh_volume(stl_triangles(reference['stl'])), mesh_volume(ours)
    if abs(expected - got) > 1e-6 * max(1.0, abs(expected)):
        return f'volume {got} against {expected}'
    return None


class Tally:
    def __init__(self):
        self.cases = self.differences = self.declined = self.no_step = self.reference_faults = self.defined = 0
        self.by_route = {}

    def record(self, label, reference, native, compare=None):
        self.cases += 1
        route = label.split(' ')[0]
        self.by_route[route] = self.by_route.get(route, 0) + 1
        (rs, rv), (ns, nv) = decoded(reference), decoded(native)
        if getattr(native, 'own', None):
            # Answered by the core's own rules, which the reference does not share.
            self.defined += 1
            print(f'defined   {label}: {native.own[:200]}')
            return
        if ns == 400 and isinstance(nv, dict) and str(nv.get('error')).endswith(NO_STEP):
            # A server without the STEP add-on; a solid feature of kind step names its feature first.
            self.no_step += 1
            return
        if rs == 400 and isinstance(rv, dict) and NO_OCP in str(rv.get('error')):
            # A reference without OCP (the step extra): validation/step_exchange_parity.py compares STEP.
            self.no_step += 1
            return
        if rs == 500 and ns == 400 and isinstance(nv, dict) and 'error' in nv:
            # The reference failed with an unhandled exception; the C++ server refuses the input with a message.
            self.reference_faults += 1
            print(f'reference fault {label}: the C++ server says {str(nv.get("error"))[:160]}')
            return
        if ns == 400 and rs == 200 and isinstance(nv, dict):
            # The core declines (NotPorted) an input the reference then answered itself.
            self.declined += 1
            print(f'declined  {label}: {str(nv.get("error"))[:200]}')
            return
        problem = None
        if (
            rs == ns
            and rs >= 400
            and isinstance(rv, tuple)
            and rv[0] == 'text/html'
            and isinstance(nv, dict)
            and 'error' in nv
        ):
            return  # Flask's own error page (a file that is not there, a body that is not JSON); the C++ server answers JSON
        if rs != ns:
            problem = f'status {rs} against {ns}: {str(rv)[:200]} | {str(nv)[:200]}'
        elif rs == 200 and compare is not None:
            problem = compare(rv, nv)
        else:
            problem = first_difference(rv, nv)
        if problem:
            self.differences += 1
            print(f'DIFFERENT {label}: {problem}')


# ---- the cases -----------------------------------------------------------------------------------------


def body(value):
    return json.dumps(value, separators=(',', ':'), allow_nan=False).encode()


def sample(**changes):
    project = dict(
        mode='router',
        width=100,
        height=100,
        feed=600,
        power=1000,
        safe=5,
        depth=1.2,
        step=0.5,
        plunge=100,
        passes=1,
        paths=[[[10, 10], [20, 10], [20, 20]]],
    )
    project.update(changes)
    return project


SQUARE = [[0, 0], [20, 0], [20, 20], [0, 20], [0, 0]]
OVERLAP = [[10, 10], [30, 10], [30, 30], [10, 30], [10, 10]]
HOLE = [[5, 5], [5, 15], [15, 15], [15, 5], [5, 5]]


def plate():
    return {
        'version': 1,
        'parameters': {'width': '2 * half', 'half': 20, 'thickness': 6},
        'features': [
            {'id': 'base', 'kind': 'box', 'size': ['width', 30, 'thickness']},
            {
                'id': 'bore',
                'kind': 'cylinder',
                'op': 'cut',
                'radius': 4,
                'height': 'thickness + 2',
                'origin': ['half', 15, -1],
            },
        ],
    }


def stock_packet(count, row, shape=0, magic=b'KKSTOCK1', resolution=16):
    return (
        struct.pack('<8sIII7d', magic, count, resolution, shape, 0, 0, 8, 8, 1, 60, 3)
        + struct.pack('<7d', *row) * count
    )


def places(value, path=()):
    """Every place in a payload a mutation can change: object members and list items, two levels deep."""
    out = []
    if len(path) >= 3:
        return out
    if isinstance(value, dict):
        for key, item in value.items():
            out.append(path + (key,))
            out.extend(places(item, path + (key,)))
    elif isinstance(value, list) and value and len(path) < 2:
        for index in {0, len(value) - 1}:
            out.append(path + (index,))
            out.extend(places(value[index], path + (index,)))
    return out


def mutations(payload, rng, count):
    """Variants of a payload with one member or item removed or replaced."""
    odd = [None, True, False, 0, -1, 1e9, -1e-9, 'x', '', [], {}, [1], {'a': 1}, 2.5, '12', [[0, 0]], [0, 0, 0]]
    spots = places(payload)
    out = []
    for _ in range(count):
        variant = copy.deepcopy(payload)
        spot = rng.choice(spots)
        parent = variant
        for step in spot[:-1]:
            parent = parent[step]
        if isinstance(parent, dict) and rng.random() < 0.25:
            del parent[spot[-1]]
        else:
            parent[spot[-1]] = copy.deepcopy(rng.choice(odd))
        out.append(variant)
    return out


def design_cases(rng, count):
    """(label, path, payload) for the design routes, valid and mutated."""
    from studio.assembly import example_assembly
    from test_relief import sample as relief_sample
    from test_tool_changes import project as tool_project

    rotary = {
        'project': {
            **sample(),
            'operation': 'line',
            'protocol': 'grbl',
            'depth': 1,
            'step': 1,
            'paths': [[[0, 5], [math.pi * 10, 5]]],
        },
        'options': {'diameter': 10, 'wrap_axis': 'X', 'postprocessor': 'linuxcnc'},
    }
    sketch = {
        'points': [[0, 0], ['w', 0], ['w', 'h'], [0, 'h']],
        'parameters': {'w': 30, 'h': 'w / 2'},
        'fillets': [0, 2, 2, 0],
    }
    constrained = {
        'points': [[0, 0], [10, 0.5], [10.5, 9], [0.2, 10]],
        'parameters': {'side': 12},
        'constraints': [
            {'kind': 'fixed', 'point': 0, 'value': [0, 0]},
            {'kind': 'horizontal', 'edge': 0},
            {'kind': 'vertical', 'edge': 1},
            {'kind': 'length', 'edge': 0, 'value': 'side'},
        ],
    }
    curved = {
        'points': [[0, 0], [20, 0], [20, 10], [0, 10]],
        'curves': [None, None, {'controls': [[15, 18], [5, 18]], 'tangentStart': False}, None],
    }
    assembly = {'assembly': example_assembly(), 'mesh': True}
    fixture = {
        'start': [0, 0, 4],
        'lines': ['G21', 'G90', 'G0 X100'],
        'tool': {'diameter': 2, 'stickout': 4, 'holder_diameter': 12, 'holder_length': 15},
        'fixtures': [{'name': 'Bridge clamp', 'min': [30, 1, 7], 'max': [32, 3, 10]}],
    }
    board = {
        'copper': (PCB / 'board_top.gbr').read_text(),
        'outline': (PCB / 'edge_cuts.gbr').read_text(),
        'drill': (PCB / 'drill_decimal.drl').read_text(),
        'tool_width': 0.2,
        'width': 60,
        'height': 40,
    }
    surface = {'surface': {'kind': 'bspline', 'poles': [[[0, 0, 0], [0, 12, 3]], [[20, 0, 4], [20, 12, 0]]]}}
    net = [[[10 * i, 10 * j, (i * j) % 3] for j in range(3)] for i in range(3)]
    degrees = {'surface': {'kind': 'bspline', 'degrees': [1, 2], 'poles': net}}
    program = {'filename': 'part.nc', 'content': 'G21\nG90\nG0 X10 Y10\nG1 Z-1 F100\nG1 X20 F600\nM2\n'}
    relief_settings = relief_sample()
    from validation.pdf_import_workflow import example_pdf

    pdf = base64.b64encode(example_pdf()).decode('ascii')
    bundle = zip_text([('devices/one.lbdev', '{}'), ('materials/ref.clb', '<library/>'), ('artwork.lbart', b'123')])
    stl_box = base64.b64encode(open_stl_box()).decode()
    families = {
        '/api/generate': [sample(), sample(mode='laser', power=500), sample(operation='pocket', paths=[SQUARE])],
        '/api/solid-cad': [{'model': plate()}],
        '/api/solid-cad/section': [{'model': plate(), 'z': 3}],
        '/api/solid-cad/sketch-points': [sketch, constrained, curved, dict(sketch, validate=True)],
        '/api/cad-script': [{'source': 'cube([10, 20, 5]);'}, {'source': 'w = 4;\nsquare([w, 2 * w]);'}],
        '/api/tool-changes': [{'project': tool_project()}],
        '/api/rotary': [rotary],
        '/api/geometry': [
            {'action': 'union', 'objects': [{'paths': [SQUARE]}, {'paths': [OVERLAP]}]},
            {'action': 'difference', 'objects': [{'paths': [SQUARE]}, {'paths': [OVERLAP]}]},
            {'action': 'intersection', 'objects': [{'paths': [SQUARE]}, {'paths': [OVERLAP]}]},
            {'action': 'offset', 'distance': 1.5, 'objects': [{'paths': [SQUARE, HOLE]}]},
            {'action': 'offset', 'distance': -2, 'objects': [{'paths': [SQUARE]}]},
        ],
        '/api/simulate': [
            {'segments': [[1, 1, 7, 7, -1, 0, -1]], 'width': 8, 'height': 8, 'resolution': 16, 'tool_diameter': 1}
        ],
        '/api/extrude': [{'paths': [SQUARE, HOLE], 'height': 4}, {'paths': [SQUARE]}],
        '/api/artwork-relief': [{'paths': [SQUARE], 'depth': 1, 'resolution': 0.5}],
        '/api/relief': [
            relief_settings,
            dict(relief_settings, heightmap=None, content_base64=stl_box, width=10, height=10),
        ],
        '/api/assembly/solve': [assembly, dict(assembly, mesh=False)],
        '/api/fixture-collision': [fixture],
        '/api/program-import': [program, {'filename': 'part.svg', 'content': SVG}],
        '/api/pdf-import': [{'content': pdf}, {'content': pdf, 'page': 2}],
        '/api/import-bundle': [{'content': bundle}, {'content': bundle, 'index': 1}],
        '/api/pcb-import': [board, dict(board, side='bottom'), dict(board, copper=None, tool_width=None)],
        '/api/solid-cad/surface': [surface, degrees],
        '/api/solid-cad/exchange-stl': [surface, {'model': plate()}],
        '/api/solid-cad/stl': [{'model': plate()}],
        '/api/solid-cad/step/export': [surface, {'model': plate()}],
        '/api/solid-cad/step/import': [{'content': base64.b64encode(b'ISO-10303-21;\nEND-ISO-10303-21;\n').decode()}],
    }
    cases = []
    for path, payloads in families.items():
        for payload in payloads:
            cases.append((path, payload))
            for variant in mutations(payload, rng, count):
                cases.append((path, variant))
    return cases


SVG = (
    '<svg xmlns="http://www.w3.org/2000/svg" width="40mm" height="20mm" viewBox="0 0 40 20">'
    '<path d="M 2 2 L 38 2 L 38 18 Z" fill="none" stroke="black"/><circle cx="10" cy="10" r="4"/></svg>'
)


def zip_text(items):
    import io
    import zipfile

    blob = io.BytesIO()
    with zipfile.ZipFile(blob, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, value in items:
            archive.writestr(name, value)
    return base64.b64encode(blob.getvalue()).decode()


def open_stl_box():
    """A 10 mm cube as binary STL, for the relief route's mesh input."""
    from studio.solid_cad import model_stl

    return model_stl({'version': 1, 'features': [{'id': 'b', 'kind': 'box', 'size': [10, 10, 4]}]})


POST_ROUTES = [
    '/api/connect',
    '/api/disconnect',
    '/api/run',
    '/api/command',
    '/api/head',
    '/api/arm',
    '/api/arm/check',
    '/api/arm/confirm',
    '/api/disarm',
    '/api/generate',
    '/api/phone-monitor',
    '/api/level/probe',
    '/api/level/collect',
    '/api/level/program',
    '/api/solid-cad',
    '/api/cad-script',
    '/api/solid-cad/sketch-points',
    '/api/solid-cad/section',
    '/api/solid-cad/stl',
    '/api/tool-changes',
    '/api/rotary',
    '/api/geometry',
    '/api/simulate',
    '/api/extrude',
    '/api/artwork-relief',
    '/api/relief',
    '/api/assembly/solve',
    '/api/fixture-collision',
    '/api/program-import',
    '/api/pdf-import',
    '/api/import-bundle',
    '/api/pcb-import',
    '/api/solid-cad/surface',
    '/api/solid-cad/exchange-stl',
]


def malformed_cases():
    out = []
    for path in POST_ROUTES:
        for raw, kind in (
            (b'[]', JSON),
            (b'not json', JSON),
            (b'x', 'text/plain'),
            (b'null', JSON),
            (b'5', JSON),
            (b'{}', JSON),
        ):
            out.append((f'{path} malformed {raw[:8]!r} {kind}', path, raw, kind))
    for raw in (b'', stock_packet(1, [0, 1, 1, 2, -1, 0, -1])[:-1], stock_packet(1, [0, 1, 1, 2, -1, 0, -1]) + b'bad'):
        out.append((f'/api/simulate wire {len(raw)} bytes', '/api/simulate', raw, STOCK))
    for packet in (
        stock_packet(1, [0, 1, 1, 2, -1, 0, -1], magic=b'KKOTHER0'),
        stock_packet(1, [0, 1, 1, 2, -1, 0, -1], shape=99),
        stock_packet(1, [0, 1, 1, 2, -1, 0, -1], resolution=15),
        stock_packet(3, [0.5, 2, 7.5, 6, -1, 0, -0.5], shape=1),
        stock_packet(2, [0, 0, 8, 8, -2, 0, -2], shape=2),
        stock_packet(1, [math.inf, 1, 1, 2, -1, 0, -1]),
    ):
        out.append(('/api/simulate wire packet', '/api/simulate', packet, STOCK))
    return out


def get_cases():
    return [
        '/api/session-keys',
        '/api/protocols',
        '/api/profiles',
        '/api/assembly/example',
        '/api/level',
        '/api/machine',
        '/api/ports',
        '/',
        '/index.html',
        '/app.js',
        '/protocols.json',
        '/no-such-file.js',
        '/../server.py',
    ]


# ---- machine scripts -----------------------------------------------------------------------------------


def wait_idle(server, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        _, _, data = server.request('GET', '/api/status')
        state = json.loads(data)
        if not state.get('active'):
            return state
        time.sleep(0.05)
    return None


ENGRAVING = dict(depth=0.1, step=0.1, stock_thickness=1.6, paths=[[[10, 10], [60, 10], [60, 40], [10, 10]]])


def machine_script():
    """Steps run on both servers in turn: (label, method, path, payload or None, wait for the controller)."""
    project = sample()
    probe = dict(project=sample(**ENGRAVING), confirmed=True, spacing=20, margin=2, clearance=2, depth=3, feed=300)
    steps = [
        ('run before connect', 'POST', '/api/run', {'confirmed': True, 'project': project}, False),
        ('connect no port', 'POST', '/api/connect', {}, False),
        ('connect bad port', 'POST', '/api/connect', {'port': 7}, False),
        ('connect hardware', 'POST', '/api/connect', {'port': '/dev/ttyUSB0'}, False),
        ('connect unknown protocol', 'POST', '/api/connect', {'port': 'simulator', 'protocol': 'tinyg'}, False),
        ('connect', 'POST', '/api/connect', {'port': 'simulator'}, True),
        ('status', 'GET', '/api/status', None, False),
        ('machine', 'GET', '/api/machine', None, False),
        ('run unconfirmed', 'POST', '/api/run', {'confirmed': False, 'project': project}, False),
        ('run levelled not bool', 'POST', '/api/run', {'confirmed': True, 'leveled': 'yes', 'project': project}, False),
        ('run', 'POST', '/api/run', {'confirmed': True, 'project': project}, True),
        ('status after run', 'GET', '/api/status', None, False),
        ('command none', 'POST', '/api/command', {'bogus': 1}, False),
        ('command unknown field', 'POST', '/api/command', {'action': 'hold', 'bogus': 1}, False),
        ('command not text', 'POST', '/api/command', {'action': 5}, False),
        ('command jog', 'POST', '/api/command', {'action': 'jog', 'axis': 'X', 'distance': 5, 'feed': 600}, True),
        (
            'command jog bad axis',
            'POST',
            '/api/command',
            {'action': 'jog', 'axis': 'Q', 'distance': 5, 'feed': 600},
            False,
        ),
        (
            'command jog far',
            'POST',
            '/api/command',
            {'action': 'jog', 'axis': 'X', 'distance': 5000, 'feed': 600},
            False,
        ),
        ('command home', 'POST', '/api/command', {'action': 'home'}, True),
        ('command zero', 'POST', '/api/command', {'action': 'zero'}, True),
        ('command unlock', 'POST', '/api/command', {'action': 'unlock'}, True),
        ('command frobnicate', 'POST', '/api/command', {'action': 'frobnicate'}, False),
        ('status after commands', 'GET', '/api/status', None, False),
        ('head unconfirmed', 'POST', '/api/head', {'mode': 'laser'}, False),
        ('head laser', 'POST', '/api/head', {'mode': 'laser', 'confirmed': True}, True),
        ('head router', 'POST', '/api/head', {'mode': 'router', 'confirmed': True}, True),
        ('arm unconfirmed', 'POST', '/api/arm', {'project': project}, False),
        ('arm no challenge', 'POST', '/api/arm', {'project': project, 'confirmed': True}, False),
        ('arm', 'POST', '/api/arm', {'project': project, 'confirmed': True, 'phone_arm_challenge': True}, False),
        ('arm check', 'POST', '/api/arm/check', {'arm_id': '$arm_id'}, False),
        ('arm confirm without a phone', 'POST', '/api/arm/confirm', {'arm_id': '$arm_id', 'challenge': 'x'}, False),
        ('run while armed', 'POST', '/api/run', {'confirmed': True, 'project': project}, True),
        ('arm check after run', 'POST', '/api/arm/check', {'arm_id': '$arm_id'}, False),
        ('arm again', 'POST', '/api/arm', {'project': project, 'confirmed': True, 'phone_arm_challenge': True}, False),
        ('disarm', 'POST', '/api/disarm', {'arm_id': '$arm_id'}, False),
        ('disarm again', 'POST', '/api/disarm', {'arm_id': '$arm_id'}, False),
        ('arm check no id', 'POST', '/api/arm/check', {}, False),
        ('arm check unknown', 'POST', '/api/arm/check', {'arm_id': 'nope'}, False),
        ('arm confirm unknown', 'POST', '/api/arm/confirm', {'arm_id': 'nope', 'challenge': 'x'}, False),
        ('disarm unknown', 'POST', '/api/disarm', {'arm_id': 'nope'}, False),
        ('level', 'GET', '/api/level', None, False),
        ('level probe unconfirmed', 'POST', '/api/level/probe', dict(probe, confirmed=False), False),
        ('level probe laser', 'POST', '/api/level/probe', dict(probe, project=sample(mode='laser')), False),
        ('level collect first', 'POST', '/api/level/collect', {}, False),
        ('level program first', 'POST', '/api/level/program', {'project': sample(**ENGRAVING)}, False),
        ('level probe', 'POST', '/api/level/probe', probe, True),
        ('level after probe', 'GET', '/api/level', None, False),
        ('level collect', 'POST', '/api/level/collect', {}, False),
        ('level program', 'POST', '/api/level/program', {'project': sample(**ENGRAVING)}, False),
        (
            'run levelled',
            'POST',
            '/api/run',
            {'confirmed': True, 'leveled': True, 'project': sample(**ENGRAVING)},
            True,
        ),
        ('status after levelled run', 'GET', '/api/status', None, False),
        ('level clear', 'POST', '/api/level/clear', {}, False),
        ('phone monitor bad', 'POST', '/api/phone-monitor', {'action': 'open'}, False),
        ('phone monitor status', 'POST', '/api/phone-monitor', {'action': 'status'}, False),
        ('disconnect', 'POST', '/api/disconnect', {}, True),
        ('connect marlin', 'POST', '/api/connect', {'port': 'simulator', 'protocol': 'marlin'}, True),
        ('status marlin', 'GET', '/api/status', None, False),
        ('run marlin mismatch', 'POST', '/api/run', {'confirmed': True, 'project': project}, False),
        ('run marlin', 'POST', '/api/run', {'confirmed': True, 'project': sample(protocol='marlin')}, True),
        ('status after marlin run', 'GET', '/api/status', None, False),
        ('disconnect marlin', 'POST', '/api/disconnect', {}, True),
    ]
    return steps


VOLATILE = {'fragment', 'token', 'url', 'urls', 'addresses', 'arm_id', 'challenge', 'expires'}
# Members the C++ server answers differently on purpose, with the reason; reported, not counted as differences.
KNOWN = {
    'can_encrypt': 'the C++ server encrypts pairing through its TLS relay (kerfkite-pendant-tls), when it is built',
}


def steady(value):
    """An answer without the values that are random on each server (tokens and the like)."""
    if isinstance(value, dict):
        return {k: ('…' if k in VOLATILE or k in KNOWN else steady(v)) for k, v in value.items()}
    if isinstance(value, list):
        return [steady(v) for v in value]
    return value


def substituted(value, captured):
    if isinstance(value, str) and value.startswith('$'):
        return captured.get(value[1:], value)
    if isinstance(value, dict):
        return {k: substituted(v, captured) for k, v in value.items()}
    if isinstance(value, list):
        return [substituted(v, captured) for v in value]
    return value


def run_machine_script(reference, native, tally):
    captured = {id(reference): {}, id(native): {}}
    for label, method, path, payload, wait in machine_script():
        kind = JSON if payload is not None else None
        answers = []
        for server in (reference, native):
            own = captured[id(server)]
            data = body(substituted(payload, own)) if payload is not None else None
            answer = server.request(method, path, data, kind)
            if media(answer[1]) == JSON:
                value = json.loads(answer[2])
                if isinstance(value, dict) and isinstance(value.get('arm_id'), str):
                    own['arm_id'] = value['arm_id']  # each server's own handle for the preview it armed
            if wait:
                wait_idle(server)
            answers.append(answer)
        (rs, rt, rb), (ns, nt, nb) = answers
        if media(rt) == JSON:
            rb = json.dumps(steady(json.loads(rb))).encode()
        if media(nt) == JSON:
            nb = json.dumps(steady(json.loads(nb))).encode()
        tally.record(f'{path} [{label}]', (rs, rt, rb), (ns, nt, nb))


# ---- main ----------------------------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--server', required=True, help='the kerfkite-studio program')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--mutations', type=int, default=6, help='variants of each valid design payload')
    args = parser.parse_args()
    rng = random.Random(args.seed)
    reference, native = Reference(), Native(args.server)
    tally = Tally()
    try:
        # The workstation's guards.
        for headers in ({'Host': 'evil.example'}, {'Host': 'localhost:8765'}):
            tally.record(
                f'/api/session host {headers["Host"]}',
                reference.request('GET', '/api/session', headers=headers),
                native.request('GET', '/api/session', headers=headers),
                compare=lambda a, b: first_difference(steady(a), steady(b)),
            )
        tally.record(
            '/api/connect without token',
            reference.request('POST', '/api/connect', body({'port': 'simulator'}), JSON, {'X-Studio-Token': ''}),
            native.request('POST', '/api/connect', body({'port': 'simulator'}), JSON, {'X-Studio-Token': ''}),
        )
        for path in get_cases():
            compare = ports_agree if path == '/api/ports' else None
            tally.record(f'{path} GET', reference.request('GET', path), native.request('GET', path), compare)
        for label, path, raw, kind in malformed_cases():
            tally.record(label, reference.request('POST', path, raw, kind), native.request('POST', path, raw, kind))
        for path, payload in design_cases(rng, args.mutations):
            data = body(payload)
            compare = None
            if path == '/api/extrude':
                compare = extrusion_agrees
            elif path.startswith('/api/solid-cad/step/') or (
                path == '/api/solid-cad/exchange-stl' and 'content' in payload
            ):
                compare = step_agrees
            elif path == '/api/geometry':
                try:
                    distance = float(payload.get('distance', 0)) if payload.get('action') == 'offset' else 0.0
                except (TypeError, ValueError):
                    distance = 0.0
                compare = lambda a, b, d=distance: geometry_agrees(a, b, d)
            tally.record(
                f'{path} {str(payload)[:100]}',
                reference.request('POST', path, data, JSON),
                native.request('POST', path, data, JSON),
                compare,
            )
        run_machine_script(reference, native, tally)
    finally:
        native.close()
    for member, reason in KNOWN.items():
        print(f'known: {member}: {reason}')
    print(
        f'{tally.cases} cases: {tally.differences} different, {tally.declined} declined by the core, '
        f'{tally.defined} defined by the core, {tally.no_step} STEP requests, {tally.reference_faults} reference faults'
    )
    for route, count in sorted(tally.by_route.items()):
        print(f'  {route}: {count}')
    return 1 if tally.differences else 0


if __name__ == '__main__':
    sys.exit(main())
