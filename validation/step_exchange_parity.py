"""STEP exchange: the C++ server's Open CASCADE add-on (native/step) against the Flask reference (OCP).

    PYTHONPATH=$PWD python validation/step_exchange_parity.py --server build/native/kerfkite-studio [--parts 120]
        [--surfaces 120] [--seed 1]

The server must have kerfkite-step beside it (built when CMake finds Open CASCADE). Both servers answer the same
requests, and their answers must agree:

* Export (/api/solid-cad/step/export) of generated parts (validation/native_core_parity.py's parametric histories:
  boxes, cylinders, cones, spheres, lofts, sketch extrusions and revolutions with fillets and cubic edges, scripts,
  patterns, rotations and Booleans) and surfaces (validation/surface_mesh_check.py's patches, joined sheets, trims
  and outlines), and of documents written again from their content: the same status, and for a refusal the same
  message. Two documents are compared by reading both with the reference's Open CASCADE: the same faces, solids
  and closed-solid flag, the exact volume and area within 1e-9 of the shape's size, and the extent of a fine
  tessellation within three of its chord tolerances (Open CASCADE's optimal bounds of a trimmed spline face depend on
  where its outline starts, and can be millimetres beyond the face; the extent is that of the tessellation's nodes,
  without the deflection and tolerance Open CASCADE's bounding box adds). Documents from two releases may also
  differ by the shapes' tolerances. The documents' text is also compared, without
  the time of writing and the serial number Open CASCADE gives the product (counted, not required: the outline of a
  trimmed face can start at another point).
* Import (/api/solid-cad/step/import) and STL (/api/solid-cad/exchange-stl) of documents written, and of damaged
  ones (cut short, without the header or the end, lower case, too many entities, not base64, not text, too long,
  garbled, empty): the same refusals; for a document read, the same counts and flag, and the volume, area and bounds
  within 1e-9. With the same Open CASCADE release, the meshes and STL files must be identical. The reference reads
  damaged documents in a child process, because Open CASCADE can end the process it runs in.
* Solid features of kind step (/api/solid-cad): parts built from written documents with other features: the same
  refusals, or volume and bounds within 1e-6 of the size (the C++ kernel welds the tessellation where the
  reference's manifold3d merges it).

Where the reference fails with an unhandled exception (an OCP exception the reference does not catch, answered
500) or its process crashes, and the C++ server refuses the request with a message, the case is reported as a
reference fault. A refusal by which the native core declines what it does not yet read as the Python does is
reported as declined. When the
add-on links another Open CASCADE release than the reference's OCP, the meshes differ and so can Open CASCADE's own
verdict on a damaged document; those are reported as release differences, and the documents it writes must still
describe the same shapes, within 1e-6 of the size. The check exits 1 on any other difference.
"""

import argparse
import base64
import json
import math
import os
import pickle
import random
import re
import subprocess
import sys
from pathlib import Path

import OCP
from OCP.BRep import BRep_Tool
from OCP.BRepMesh import BRepMesh_IncrementalMesh
from OCP.ShapeAnalysis import ShapeAnalysis_ShapeTolerance
from OCP.TopAbs import TopAbs_FACE
from OCP.TopExp import TopExp_Explorer
from OCP.TopLoc import TopLoc_Location
from OCP.TopoDS import TopoDS

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'validation'))

from studio import cad_exchange  # noqa: E402
from validation.native_core_parity import solid_cad_cases  # noqa: E402
from validation.server_route_parity import JSON, Native, Reference  # noqa: E402
from validation.surface_mesh_check import surface as generated_surface  # noqa: E402

FILE_NAME = re.compile(rb"FILE_NAME\('[^']*','[^']*'")
# Open CASCADE names the product after the number of documents its process has written: always 1 in the add-on.
PRODUCT = re.compile(rb"(STEP translator [0-9.]+) [0-9]+'")


def text_of(document):
    """A document without the time of writing and the process's count of documents."""
    return PRODUCT.sub(rb"\1'", FILE_NAME.sub(b'', document))


# The refusals the servers make before Open CASCADE reads anything: the same whatever its release.
OWN_CHECKS = {
    'Select a STEP file up to 1.5 MB',
    'Invalid STEP file encoding',
    'Supply an ISO 10303-21 STEP file up to 1.5 MB',
    'Incomplete STEP document or STEP entity budget exceeded',
    'Choose one part, surface or imported STEP document',
}


def add_on_release(server):
    program = Path(server).with_name('kerfkite-step' + Path(server).suffix)
    answer = subprocess.run([str(program)], input=b'{"version": true}', capture_output=True, timeout=60, check=True)
    return json.loads(answer.stdout)['open_cascade']


def mesh_extent(shape):
    """The extent of a shape's tessellation: its nodes alone. Open CASCADE's bounding box adds to each face the
    deflection its mesher recorded and its tolerance, and the deflection depends on how a document describes the
    face: two descriptions of one surface, meshed alike, can differ there by tenths of a millimetre."""
    low, high = [math.inf] * 3, [-math.inf] * 3
    faces = TopExp_Explorer(shape, TopAbs_FACE)
    while faces.More():
        location = TopLoc_Location()
        triangulation = BRep_Tool.Triangulation_s(TopoDS.Face_s(faces.Current()), location)
        if triangulation is not None:
            transform = location.Transformation()
            for i in range(1, triangulation.NbNodes() + 1):
                point = triangulation.Node(i).Transformed(transform)
                for axis, value in enumerate((point.X(), point.Y(), point.Z())):
                    low[axis] = min(low[axis], value)
                    high[axis] = max(high[axis], value)
        faces.Next()
    if math.inf in low:
        raise ValueError('the shape has no tessellated face')
    return low + high


def measures(raw):
    """What the reference's Open CASCADE reads from a document: its counts, exact area and volume, and the extent
    of a fine tessellation. (The optimal bounds the import reports are not used here: on a trimmed spline face they
    depend on where the trim's outline starts, and can be millimetres beyond the face.)"""
    try:
        mesh = cad_exchange.inspect_step(raw)
        shape = cad_exchange._read_shape(raw)
        size = size_of(mesh['bounds'])
        BRepMesh_IncrementalMesh(shape, max(1e-4, size * 1e-4), False, 0.05, False)
        extent = mesh_extent(shape)
    except Exception as exc:  # noqa: BLE001 - any failure to read is the measure
        return {'error': str(exc)}
    out = {k: mesh[k] for k in ('faces', 'solids', 'solidOnly', 'volume', 'area', 'bounds')}
    out['extent'] = extent
    # Open CASCADE places a shape only to within its tolerances, which another release can set otherwise: the 3D
    # curves it fits to a trimmed face's outline, say.
    out['tolerance'] = ShapeAnalysis_ShapeTolerance().Tolerance(shape, 1)
    return out


def size_of(bounds):
    return max(1.0, math.dist(bounds[:3], bounds[3:]))


def agree(a, b, relative, release=False):
    """Two readings of a shape: counts equal, area and volume within relative of the shape's size, and the extent
    of a fine tessellation (or the bounds, for an import) within three of its chord tolerances. release: documents
    written by two Open CASCADE releases, whose extents may also differ by the shapes' own tolerances."""
    if set(a) != set(b):
        return f'keys {sorted(set(a) ^ set(b))}'
    if 'error' in a:
        return None if a['error'] == b['error'] else f'{a["error"]!r} against {b["error"]!r}'
    for key in ('faces', 'solids', 'solidOnly'):
        if a[key] != b[key]:
            return f'{key} {a[key]} against {b[key]}'
    size = size_of(a['bounds'])
    if 'extent' in a:
        allowed = 3e-4 * size + (a['tolerance'] + b['tolerance'] if release else 0)
        if any(abs(x - y) > allowed for x, y in zip(a['extent'], b['extent'])):
            return f'extent {a["extent"]} against {b["extent"]} (tolerances {a["tolerance"]:.3g}, {b["tolerance"]:.3g})'
    elif any(abs(x - y) > relative * size for x, y in zip(a['bounds'], b['bounds'])):
        return f'bounds {a["bounds"]} against {b["bounds"]}'
    if abs(a['area'] - b['area']) > relative * size * size:
        return f'area {a["area"]} against {b["area"]}'
    if abs(a['volume'] - b['volume']) > relative * size**3:
        return f'volume {a["volume"]} against {b["volume"]}'
    return None


class Tally:
    def __init__(self, same_release):
        self.cases = self.different = self.faults = self.same_text = self.documents = self.same_mesh = 0
        self.refused = self.release = self.declined = 0
        self.same_release = same_release

    def differ(self, label, problem):
        self.different += 1
        print(f'DIFFERENT {label}: {problem}')


def post(server, path, payload):
    status, content_type, body = server.request('POST', path, json.dumps(payload).encode(), JSON)
    return status, content_type.split(';')[0].strip(), body


def isolated(server, path, payload):
    """A request to the reference in a child process: its Open CASCADE runs in its process, and a damaged document
    can end that process (the C++ server's add-on runs apart, and the server refuses the request). Without fork
    (Windows) the request is made in this process."""
    if not hasattr(os, 'fork'):
        return post(server, path, payload)
    read, write = os.pipe()
    pid = os.fork()
    if pid == 0:  # pragma: no cover - the child
        os.close(read)
        try:
            data = pickle.dumps(post(server, path, payload))
        except BaseException:  # noqa: BLE001
            data = pickle.dumps((500, 'text/plain', b'exception'))
        with os.fdopen(write, 'wb') as out:
            out.write(data)
        os._exit(0)
    os.close(write)
    with os.fdopen(read, 'rb') as source:
        data = source.read()
    os.waitpid(pid, 0)
    return pickle.loads(data) if data else (599, 'text/plain', b'the reference process crashed')


def refusal(answer):
    status, kind, body = answer
    if kind == JSON:
        try:
            value = json.loads(body)
        except ValueError:
            return None
        if isinstance(value, dict):
            return value.get('error')
    return None


def declined(message):
    """A refusal by which the native core declines what it does not yet read as the Python does (NotPorted)."""
    return isinstance(message, str) and ('native core' in message or 'Python' in message)


def compare_status(tally, label, reference, native):
    """True when both wrote or read; otherwise records the comparison of their refusals and returns False."""
    (rs, _, rb), (ns, _, nb) = reference, native
    if rs == 200 and ns == 200:
        return True
    tally.refused += 1
    mine, theirs = refusal(native), refusal(reference)
    if rs in (500, 599) and ns == 400 and mine:
        tally.faults += 1
        crash = 'crashed' if rs == 599 else 'failed'
        print(f'reference fault {label}: the reference {crash}; the C++ server says {mine[:160]}')
    elif (rs, theirs) == (ns, mine):
        pass
    elif ns == 400 and declined(mine):
        tally.declined += 1
        print(f'declined  {label}: {mine[:160]}')
    elif not tally.same_release and mine not in OWN_CHECKS and theirs not in OWN_CHECKS:
        tally.release += 1  # Open CASCADE's own verdict, from another release
    elif rs != ns:
        tally.differ(label, f'status {rs} against {ns}: {rb[:160]!r} | {nb[:160]!r}')
    else:
        tally.differ(label, f'{theirs!r} against {mine!r}')
    return False


def compare_documents(tally, label, reference, native, reading=False):
    """Two written documents: their readings, and their text without the time of writing. reading: the add-on read
    a document the reference wrote, which another Open CASCADE release may read otherwise."""
    tally.documents += 1
    if text_of(reference) == text_of(native):
        tally.same_text += 1
    # Another release builds the same faces and Booleans to its own rounding: 1e-6 of the size, not 1e-9.
    problem = agree(
        measures(reference), measures(native), 1e-9 if tally.same_release else 1e-6, release=not tally.same_release
    )
    if problem and reading and not tally.same_release:
        tally.release += 1
    elif problem:
        tally.differ(label, problem)


def compare_import(tally, label, raw, reference_server, native):
    payload = {'content': base64.b64encode(raw).decode()}
    tally.cases += 1
    r, n = (
        post(reference_server, '/api/solid-cad/step/import', payload),
        post(native, '/api/solid-cad/step/import', payload),
    )
    if compare_status(tally, label + ' import', r, n):
        rv, nv = json.loads(r[2]), json.loads(n[2])
        if rv == nv:
            tally.same_mesh += 1
        elif tally.same_release:
            tally.differ(
                label + ' import', f'{len(rv["triangles"])} triangles against {len(nv["triangles"])}, or other vertices'
            )
        keys = ('faces', 'solids', 'solidOnly', 'volume', 'area', 'bounds')
        problem = agree({k: rv[k] for k in keys}, {k: nv[k] for k in keys}, 1e-9)
        if problem and not tally.same_release:
            tally.release += 1  # another release reads the reference's document otherwise
        elif problem:
            tally.differ(label + ' import', problem)
    tally.cases += 1
    r, n = (
        post(reference_server, '/api/solid-cad/exchange-stl', payload),
        post(native, '/api/solid-cad/exchange-stl', payload),
    )
    if compare_status(tally, label + ' stl', r, n) and r[1:] != n[1:]:
        if tally.same_release:
            tally.differ(label + ' stl', f'{r[1]} {len(r[2])} bytes against {n[1]} {len(n[2])} bytes')
        else:
            tally.release += 1  # another tessellation


def damaged(raw, rng):
    """Documents the reference refuses before Open CASCADE reads them, and some it hands to Open CASCADE."""
    text = base64.b64encode(raw).decode()
    yield 'cut short', base64.b64encode(raw[: len(raw) // 2]).decode()
    yield 'no header', base64.b64encode(raw.replace(b'ISO-10303-21;', b'ISO-10303-22;', 1)).decode()
    yield 'no end', base64.b64encode(raw.replace(b'END-ISO-10303-21;', b'END;')).decode()
    yield 'lower case', base64.b64encode(b' \r\n' + raw.lower()).decode()
    yield 'entity budget', base64.b64encode(raw[:-30] + b'=' * 35001 + raw[-30:]).decode()
    yield 'not base64', text[:-4] + '!!!!'
    yield 'not text', 12
    yield 'too long', 'A' * 2000004
    middle = len(raw) // 2
    yield (
        'garbled',
        base64.b64encode(raw[:middle] + bytes(rng.randrange(256) for _ in range(40)) + raw[middle + 40 :]).decode(),
    )
    yield 'no data', base64.b64encode(b'ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\nENDSEC;\nEND-ISO-10303-21;\n').decode()


def step_part(rng, content):
    """A part with an imported solid: alone, or moved, patterned and combined with a primitive."""
    feature = {'id': 'imported', 'kind': 'step', 'content': content}
    if rng.random() < 0.5:
        feature['origin'] = [rng.choice([0, 3, -2]), rng.choice([0, 4]), 0]
    if rng.random() < 0.3:
        feature['rotation'] = [0, 0, rng.choice([0, 30, 90])]
    features = [feature]
    if rng.random() < 0.6:
        features.append(
            {
                'id': 'cutter',
                'kind': 'cylinder',
                'op': rng.choice(['cut', 'add']),
                'radius': 2,
                'height': 30,
                'origin': [5, 5, -5],
            }
        )
    if len(features) == 2 and rng.random() < 0.3:
        features.reverse()
        features[0].pop('op', None)
        features[1]['op'] = 'add'
    return {'version': 1, 'features': features}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--server', required=True, help='the kerfkite-studio program, with kerfkite-step beside it')
    parser.add_argument('--parts', type=int, default=120)
    parser.add_argument('--surfaces', type=int, default=120)
    parser.add_argument('--seed', type=int, default=1)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    theirs, mine = '.'.join(OCP.__version__.split('.')[:3]), add_on_release(args.server)
    print(f'Open CASCADE {theirs} in the reference, {mine} in the add-on')
    reference, native = Reference(), Native(args.server)
    tally = Tally(theirs == mine)
    written = []
    try:
        requests = []
        for _, request in solid_cad_cases(args.parts, args.seed):
            requests.append(('part', {'model': request['model']}))
        for _ in range(args.surfaces):
            requests.append(('surface', {'surface': generated_surface(rng)}))
        requests += [
            ('neither', {}),
            ('both', {'surface': requests[-1][1]['surface'], 'model': requests[0][1]['model']}),
            ('null surface', {'surface': None, 'model': requests[0][1]['model']}),
        ]
        for index, (kind, payload) in enumerate(requests):
            label = f'export {kind} {index}'
            print(label, file=sys.stderr, flush=True)
            tally.cases += 1
            r, n = (
                post(reference, '/api/solid-cad/step/export', payload),
                post(native, '/api/solid-cad/step/export', payload),
            )
            if compare_status(tally, label, r, n):
                compare_documents(tally, label, r[2], n[2])
                written.append((label, r[2]))
        # Documents written again from their content, read back, and refused when damaged.
        for label, raw in written[:: max(1, len(written) // 40)]:
            print(label, 'again', file=sys.stderr, flush=True)
            content = base64.b64encode(raw).decode()
            tally.cases += 1
            r, n = (
                post(reference, '/api/solid-cad/step/export', {'content': content}),
                post(native, '/api/solid-cad/step/export', {'content': content}),
            )
            if compare_status(tally, label + ' again', r, n):
                compare_documents(tally, label + ' again', r[2], n[2], reading=True)
            compare_import(tally, label, raw, reference, native)
        for label, raw in written[:3]:
            for damage, content in damaged(raw, rng):
                print(label, damage, file=sys.stderr, flush=True)
                for path in ('/api/solid-cad/step/import', '/api/solid-cad/step/export', '/api/solid-cad/exchange-stl'):
                    tally.cases += 1
                    payload = {'content': content}
                    r, n = isolated(reference, path, payload), post(native, path, payload)
                    compare_status(tally, f'{label} {damage} {path}', r, n)
        # Solid features of kind step.
        solids = [(label, raw) for label, raw in written if measures(raw).get('solidOnly')]
        for label, raw in solids[:: max(1, len(solids) // 25)]:
            print(label, 'as a feature', file=sys.stderr, flush=True)
            model = step_part(rng, base64.b64encode(raw).decode())
            tally.cases += 1
            r, n = post(reference, '/api/solid-cad', {'model': model}), post(native, '/api/solid-cad', {'model': model})
            if compare_status(tally, label + ' as a feature', r, n):
                rv, nv = json.loads(r[2]), json.loads(n[2])
                size = size_of(rv['bounds'])
                if (
                    any(abs(x - y) > 1e-6 * size for x, y in zip(rv['bounds'], nv['bounds']))
                    or abs(rv['volume'] - nv['volume']) > 1e-6 * size**3
                ):
                    if tally.same_release:
                        tally.differ(
                            label + ' as a feature',
                            f'{rv["bounds"]} {rv["volume"]} against {nv["bounds"]} {nv["volume"]}',
                        )
                    else:
                        tally.release += 1  # the add-on's release read the reference's document
    finally:
        native.close()
    print(
        f'{tally.cases} cases: {tally.different} different, {tally.refused} refused, {tally.faults} reference faults, '
        f'{tally.declined} declined by the core, '
        f'{tally.release} release differences; {tally.documents} documents compared ({tally.same_text} with the same '
        f'text), {tally.same_mesh} imports with the same mesh'
    )
    return 1 if tally.different else 0


if __name__ == '__main__':
    sys.exit(main())
