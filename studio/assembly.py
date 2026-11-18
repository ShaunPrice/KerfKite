"""Rigid assemblies of parametric solid parts, in millimetres and degrees."""
import itertools
import math

import numpy as np
from flask import jsonify, request

from .solid_cad import _build

CONTACT_MM = 1e-6
MAX_PARTS = 24
MAX_TRIANGLES = 150_000


def _vector(value):
    if (not isinstance(value, list) or len(value) != 3 or
            any(type(v) not in (int, float) or not -100_000 <= v <= 100_000 for v in value)):
        raise ValueError('Use three finite coordinates within ±100000')
    return np.array(value, dtype=float)


def _pose(frame):
    if not isinstance(frame, dict):
        raise ValueError('Each datum frame needs an origin and rotation')
    position = _vector(frame.get('origin', [0, 0, 0]))
    x, y, z = np.radians(_vector(frame.get('rotation', [0, 0, 0])))
    cx, cy, cz = math.cos(x), math.cos(y), math.cos(z)
    sx, sy, sz = math.sin(x), math.sin(y), math.sin(z)
    # Extrinsic XYZ orientation, equivalent to Rz @ Ry @ Rx.
    matrix = np.eye(4)
    matrix[:3, :3] = [
        [cz*cy, cz*sy*sx - sz*cx, cz*sy*cx + sz*sx],
        [sz*cy, sz*sy*sx + cz*cx, sz*sy*cx - cz*sx],
        [-sy, cy*sx, cy*cx],
    ]
    matrix[:3, 3] = position
    return matrix


def _joint(mate):
    kind = mate.get('kind')
    offset = _vector(mate.get('offset', [0, 0, 0]))
    rotation = _vector(mate.get('rotation', [0, 0, 0]))
    if kind == 'fixed':
        valid = not offset.any() and not rotation.any()
    elif kind == 'planar':
        valid = offset[2] == 0 and not rotation[:2].any()
    elif kind == 'concentric':
        valid = not offset[:2].any() and not rotation[:2].any()
    elif kind == 'point':
        valid = not offset.any()
    else:
        raise ValueError('Choose a fixed, planar, concentric or point mate')
    if not valid:
        raise ValueError(f'{kind} mate has a displacement or rotation on a locked axis')
    motion = _pose({'origin': offset.tolist(), 'rotation': rotation.tolist()})
    return _pose(mate.get('parentFrame', {})) @ motion @ np.linalg.inv(_pose(mate.get('childFrame', {})))


def solve(document, include_mesh=False):
    if not isinstance(document, dict) or type(document.get('version')) is not int or document['version'] != 1:
        raise ValueError('Expected a version 1 assembly')
    parts = document.get('parts')
    mates = document.get('mates', [])
    if not isinstance(parts, list) or not 1 <= len(parts) <= MAX_PARTS:
        raise ValueError(f'An assembly needs 1–{MAX_PARTS} parts')
    if not isinstance(mates, list) or len(mates) >= len(parts):
        raise ValueError('Provide a mate tree with at least one independent root part')

    free, parents, local = {}, {}, {}
    for part in parts:
        name = part.get('id') if isinstance(part, dict) else None
        if not isinstance(name, str) or not name.strip() or len(name) > 80 or name in free:
            raise ValueError('Each part needs a unique name of 1–80 characters')
        free[name] = _pose(part)
    for mate in mates:
        if not isinstance(mate, dict):
            raise ValueError('Each mate must specify a child and parent part')
        child, parent = mate.get('child'), mate.get('parent')
        if any(not isinstance(key, str) or key not in free for key in (child, parent)):
            raise ValueError('Mate refers to an unknown part')
        if child in parents:
            raise ValueError(f'{child} already has a driving mate')
        parents[child] = parent
        local[child] = _joint(mate)

    world = {}
    def resolve(name, visiting):
        if name in visiting:
            raise ValueError('Assembly mates contain a cycle')
        if name not in world:
            world[name] = (resolve(parents[name], visiting | {name}) @ local[name]
                           if name in parents else free[name])
        return world[name]
    for name in free:
        resolve(name, set())

    solids, result, contacts, collisions = {}, {}, [], []
    total_triangles = 0
    for part in parts:
        name = part['id']
        geometry, _, kernel = _build(part.get('model'))
        total_triangles += geometry.num_tri()
        if total_triangles > MAX_TRIANGLES:
            raise ValueError('Assembly exceeds the 150000 triangle budget')
        solid = geometry.transform(world[name][:3])
        bounds = solid.bounding_box()
        solids[name] = solid
        result[name] = {'matrix': world[name].tolist(), 'bounds': list(bounds)}
        if include_mesh:
            mesh = solid.to_mesh64()
            result[name].update(vertices=mesh.vert_properties[:, :3].tolist(), triangles=mesh.tri_verts.tolist())

    for a, b in itertools.combinations(solids, 2):
        ba, bb = result[a]['bounds'], result[b]['bounds']
        if any(ba[i+3] + CONTACT_MM < bb[i] or bb[i+3] + CONTACT_MM < ba[i] for i in range(3)):
            continue
        overlap = solids[a] ^ solids[b]
        if overlap.status() != kernel.Error.NoError:
            raise ValueError(f'Intersection calculation failed for {a} and {b}')
        volume = overlap.volume()
        if volume > 1e-9:
            collisions.append({'parts': [a, b], 'volume': volume})
        elif solids[a].min_gap(solids[b], 2 * CONTACT_MM) <= CONTACT_MM:
            contacts.append({'parts': [a, b]})
    return {'parts': result, 'contacts': contacts, 'interferences': collisions}


def example_assembly():
    """A shaft constrained to the bore of an upright bearing plate."""
    bore = [20, 15, 0]
    return {'version': 1, 'parts': [
        {'id': 'Bearing plate', 'model': {'version': 1, 'features': [
            {'id': 'plate', 'kind': 'box', 'size': [40, 30, 8]},
            {'id': 'bore', 'kind': 'cylinder', 'op': 'cut', 'height': 10, 'radius': 5, 'origin': [20, 15, -1]}]}},
        {'id': 'Shaft', 'model': {'version': 1, 'features': [
            {'id': 'shaft', 'kind': 'cylinder', 'radius': 4, 'height': 24}]}},
    ], 'mates': [{'child': 'Shaft', 'parent': 'Bearing plate', 'kind': 'concentric',
                  'parentFrame': {'origin': bore}, 'offset': [0, 0, -4]}]}


def register(app):
    @app.post('/api/assembly/solve')
    def solve_assembly():
        payload = request.get_json()
        if not isinstance(payload, dict) or type(payload.get('mesh', False)) is not bool:
            raise ValueError('Supply an assembly and a boolean mesh flag')
        return jsonify(solve(payload.get('assembly'), payload.get('mesh', False)))

    @app.get('/api/assembly/example')
    def assembly_example():
        return jsonify(example_assembly())
