"""Solve point and straight-edge sketch relations before building a closed profile."""
import math


def _index(value, n):
    if type(value) is not int or not 0 <= value < n:
        raise ValueError('Sketch constraint refers to a missing vertex or edge')
    return value


def _pair(value, n):
    if not isinstance(value, list) or len(value) != 2:
        raise ValueError('A sketch relation requires two references')
    a, b = (_index(k, n) for k in value)
    if a == b:
        raise ValueError('Choose two different vertices or edges for this relation')
    return a, b


def _driven(expr):
    if not isinstance(expr, str):
        return False
    try:
        float(expr)
    except ValueError:
        return True
    return False


def solve_profile(feature, dimensions):
    raw = feature.get('points')
    if not isinstance(raw, list) or len(raw) > 256:
        raise ValueError('A sketch supports up to 256 vertices')
    coords = [dimensions.vector(pt, 2) for pt in raw]
    n = len(coords)
    locked = {(i, k) for i, pt in enumerate(raw) for k in range(2) if _driven(pt[k])}
    constraints = feature.get('constraints', [])
    if not isinstance(constraints, list) or len(constraints) > 128:
        raise ValueError('A sketch supports up to 128 geometric constraints')
    if not constraints:
        return coords, {'dof': 2*n - len(locked), 'redundant': 0, 'iterations': 0}

    import numpy as np
    xy = np.array(coords, dtype=float)
    used = set()
    relations = []
    redundant_fixed = 0
    for number, rel in enumerate(constraints, 1):
        if not isinstance(rel, dict):
            raise ValueError(f'Invalid sketch constraint {number}')
        kind = rel.get('kind')
        target = 0.
        if kind == 'fixed':
            i = _index(rel.get('point'), n)
            target_xy = dimensions.vector(rel.get('value'), 2)
            for axis, val in enumerate(target_xy):
                if (i, axis) in locked:
                    if abs(xy[i, axis] - val) > 1e-8:
                        raise ValueError(f'Fixed constraint {number} contradicts another dimension at point {i+1}')
                    redundant_fixed += 1
                locked.add((i, axis))
                xy[i, axis] = val
            continue
        if kind == 'distance':
            indices = _pair(rel.get('points'), n)
            target = dimensions.positive(rel.get('value'))
        elif kind in ('parallel', 'perpendicular', 'angle', 'equal'):
            a, b = _pair(rel.get('edges'), n)
            indices = (a, (a+1) % n, b, (b+1) % n)
            if kind == 'angle':
                target = math.radians(dimensions.evaluate(rel.get('value'))) % (2*math.pi)
            elif kind == 'perpendicular':
                target = math.pi/2
        else:
            raise ValueError(f'Unknown sketch constraint {number}')
        relations.append((number, kind, indices, target))
        used.update(indices)

    free = [(i,k) for i in sorted(used) for k in range(2) if (i,k) not in locked]
    columns = {slot:i for i,slot in enumerate(free)}
    free_count = 2*n-len(locked)

    def evaluate(points):
        residual = np.zeros(len(relations))
        jac = np.zeros((len(relations), len(free)))
        for row, (number, kind, idx, target) in enumerate(relations):
            i, j = idx[:2]
            a = points[j]-points[i]
            la = math.hypot(*a)
            if la < 1e-9:
                raise ValueError(f'Constraint {number} uses a collapsed edge or coincident reference points')
            da = a/la
            if kind == 'distance':
                residual[row] = la-target
                grads = (-da, da)
            else:
                k, l = idx[2:]
                b = points[l]-points[k]
                lb = math.hypot(*b)
                if lb < 1e-9:
                    raise ValueError(f'Constraint {number} uses a collapsed reference edge')
                if kind == 'equal':
                    db = -b/lb
                    residual[row] = la-lb
                else:
                    # Relative angle has a nonzero derivative even at right angles.
                    angle = math.atan2(a[0]*b[1]-a[1]*b[0], float(a @ b))
                    period = 2*math.pi if kind == 'angle' else math.pi
                    residual[row] = (angle-target+period/2) % period-period/2
                    da = np.array([a[1], -a[0]]) / la**2
                    db = np.array([-b[1], b[0]]) / lb**2
                grads = (-da, da, -db, db)
            for point, grad in zip(idx, grads):
                for axis in range(2):
                    column = columns.get((point,axis))
                    if column is not None:
                        jac[row,column] += grad[axis]
        return residual, jac

    for iteration in range(41):
        residual, jac = evaluate(xy)
        error = float(np.max(np.abs(residual), initial=0.))
        if error <= 1e-8:
            rank = int(np.linalg.matrix_rank(jac, tol=1e-10)) if jac.size else 0
            if not np.isfinite(xy).all() or np.max(np.abs(xy), initial=0.) > 100_000:
                raise ValueError('Solved sketch exceeds the Studio coordinate range')
            return xy.tolist(), {'dof': free_count-rank, 'redundant': redundant_fixed+len(relations)-rank,
                                 'iterations': iteration}
        if not free or iteration == 40:
            break
        step = np.linalg.lstsq(jac, -residual, rcond=1e-12)[0]
        delta = np.zeros_like(xy)
        for col, slot in enumerate(free):
            delta[slot] = step[col]
        objective = float(residual @ residual)
        for backtrack in range(11):
            candidate = xy + delta / (2**backtrack)
            try:
                next_residual, _ = evaluate(candidate)
            except ValueError:
                continue
            if float(next_residual @ next_residual) < objective:
                xy = candidate
                break
        else:
            break
    number, kind, _, _ = relations[int(np.argmax(np.abs(residual)))]
    raise ValueError(f'Cannot satisfy sketch constraint {number} ({kind}). Check conflicting dimensions or choose another starting geometry')
