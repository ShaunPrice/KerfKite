"""Fit additional assembly mates using only the degrees of freedom of their driving joints."""
import math

import numpy as np

FREEDOMS = {'fixed': (), 'planar': (0, 1, 5), 'concentric': (2, 5), 'point': (3, 4, 5)}
TOLERANCE = 1e-6


def fit(world, tree, roots, constraints, pose):
    if not isinstance(constraints, list) or len(constraints) > 48:
        raise ValueError('Use a list of up to 48 additional assembly mates')
    if not constraints:
        return world

    targets = []
    involved = set()
    for item in constraints:
        if not isinstance(item, dict) or not isinstance(item.get('kind'), str) or item['kind'] not in FREEDOMS:
            raise ValueError('Choose a fixed, planar, concentric or point additional mate')
        child, parent = item.get('child'), item.get('parent')
        if any(not isinstance(name, str) or name not in world for name in (child, parent)) or child == parent:
            raise ValueError('An additional mate must connect two different existing parts')
        targets.append((child, parent, item['kind'], pose(item.get('childFrame', {})), pose(item.get('parentFrame', {}))))
        for name in (child, parent):
            while name in tree:
                involved.add(name)
                name = tree[name]['parent']

    drives = {}
    variables = []
    start = []
    for name, joint in tree.items():
        values = np.array([*joint.get('offset', [0]*3), *joint.get('rotation', [0]*3)], dtype=float)
        values[3:] = np.radians(values[3:])
        drives[name] = (joint['parent'], pose(joint.get('parentFrame', {})),
                        np.linalg.inv(pose(joint.get('childFrame', {}))), values)
        if name in involved:
            for axis in FREEDOMS[joint['kind']]:
                variables.append((name, axis))
                start.append(values[axis])

    # The driving tree already satisfies its own mates. This parameterisation
    # preserves those equations, including locked axes, throughout the solve.
    def evaluate(parameters):
        solved = dict(roots)
        motion = {name: entry[3].copy() for name, entry in drives.items()}
        for (name, axis), val in zip(variables, parameters):
            motion[name][axis] = val
        def place(name):
            if name not in solved:
                parent, a, b, _ = drives[name]
                values = motion[name]
                transform = pose({'origin': values[:3].tolist(), 'rotation': np.degrees(values[3:]).tolist()})
                solved[name] = place(parent) @ a @ transform @ b
            return solved[name]
        for name in world:
            place(name)
        rows = []
        for child, parent, kind, a, b in targets:
            first = solved[child] @ a
            second = solved[parent] @ b
            axis = second[:3, 2]
            delta = first[:3, 3] - second[:3, 3]
            if kind in ('fixed', 'point'):
                rows.extend(delta)
            elif kind == 'planar':
                rows.append(np.dot(delta, axis))
            else:
                rows.extend(delta - np.dot(delta, axis)*axis)
            if kind == 'fixed':
                rows.extend((first[:3, :3] - second[:3, :3]).ravel())
            elif kind != 'point':
                rows.extend(first[:3, 2] - axis)
        return np.asarray(rows), solved

    x = np.array(start)
    residual, solved = evaluate(x)
    for _ in range(40):
        if np.max(np.abs(residual)) <= TOLERANCE:
            return solved
        if not variables:
            break
        columns = []
        for i in range(len(x)):
            trial = x.copy()
            trial[i] += 1e-6
            columns.append((evaluate(trial)[0] - residual) / 1e-6)
        delta = np.linalg.lstsq(np.column_stack(columns), -residual, rcond=1e-9)[0]
        if not np.isfinite(delta).all() or np.linalg.norm(delta) < 1e-12:
            break
        old_norm = np.linalg.norm(residual)
        scale = min([1.] + [math.pi / abs(step) for step, (_, axis) in zip(delta, variables)
                            if axis >= 3 and abs(step) > math.pi])
        for _ in range(16):
            trial = x + delta * scale
            if all(abs(value) <= (1e5 if axis < 3 else math.radians(1e5)) for value, (_, axis) in zip(trial, variables)):
                candidate, positions = evaluate(trial)
                if np.linalg.norm(candidate) < old_norm:
                    x, residual, solved = trial, candidate, positions
                    break
            scale *= .5
        else:
            break
    raise ValueError('Additional mates could not be satisfied from these joint positions. Check the datum frames, independent placements and conflicting constraints.')
