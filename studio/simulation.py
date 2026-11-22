"""Bounded sampled stock-removal preview, not a collision certification."""

import math
from .operations import value
from .cam_moves import PackedSegments

try:  # The native core's stock preview, in the compiled extension.
    from ._relief_native import core_cam as _core_cam
except ImportError:
    _core_cam = None


def simulate(data, *, backend='auto'):
    """The stock-removal preview of a job's segments.

    backend='auto' runs the native core when the extension is installed and Python otherwise, or when the core
    declines; 'python' runs this module, the reference; 'native' requires the core."""
    if backend not in ('auto', 'python', 'native'):
        raise ValueError('Stock backend must be auto, python or native')
    if backend == 'native' and _core_cam is None:
        raise ValueError('Native stock backend is not installed')
    if not isinstance(data, dict):
        raise ValueError('Stock preview requires an object')
    if backend != 'python' and _core_cam is not None:
        from .cam import core_answer

        segments = data.get('segments')
        if isinstance(segments, PackedSegments):
            # Packed rows cross as their bytes.
            rest = {key: value for key, value in data.items() if key != 'segments'}
            answer = core_answer(
                lambda command, request, packed: _core_cam(command, request, packed, segments.buffer()),
                'stock',
                rest,
                backend,
            )
        else:
            answer = core_answer(_core_cam, 'stock', data, backend)
        if answer is not None:
            return answer
    try:
        return _simulate(data)
    except (TypeError, OverflowError) as exc:
        raise ValueError('Stock geometry and dimensions must be numeric') from exc


def _simulate(data):
    segments = data.get('segments', [])
    if not isinstance(segments, (list, PackedSegments)) or len(segments) > 200000:
        raise ValueError('Invalid segment count')
    origin_x = value(data, 'x', 0, 0, 2000)
    origin_y = value(data, 'y', 0, 0, 2000)
    width = value(data, 'width', 100, 0.1, 2000)
    height = value(data, 'height', 100, 0.1, 2000)
    n = int(value(data, 'resolution', 48, 16, 96))
    diameter = value(data, 'tool_diameter', 3.175, 0.01, 100)
    radius = diameter / 2
    shape = data.get('tool_shape', 'flat')
    angle = value(data, 'v_angle', 60, 10, 170)
    if shape not in ('flat', 'ball', 'v', 'drill'):
        raise ValueError('Unknown simulated cutter')
    stock = value(data, 'stock_thickness', 10, 0.01, 200)
    surface = _python_heights(segments, n, width, height, origin_x, origin_y, radius, shape, angle, stock)
    return {
        'heights': surface,
        'x': origin_x,
        'y': origin_y,
        'width': width,
        'height': height,
        'stock_thickness': stock,
        'resolution': n,
        'warnings': [
            'Sampled stock preview: features smaller than a grid cell may disappear. Variable-depth cutter contact is approximate.',
            'No holder, fixture, machine acceleration, deflection or physical-clearance validation is performed.',
        ],
    }


def _python_heights(segments, n, width, height, origin_x, origin_y, radius, shape, angle, stock):
    surface = [[0.0 for _ in range(n)] for _ in range(n)]
    checks = 0
    dx = width / (n - 1)
    dy = height / (n - 1)
    for s in segments:
        if not isinstance(s, list) or len(s) < 5:
            raise ValueError('Invalid preview segment')
        ax, ay, bx, by, z = map(float, s[:5])
        ax -= origin_x
        bx -= origin_x
        ay -= origin_y
        by -= origin_y
        start_z = float(s[6]) if len(s) > 6 else z
        if not all(math.isfinite(v) for v in (ax, ay, bx, by, z, start_z)):
            raise ValueError('Non-finite segment')
        if min(z, start_z) < -stock:
            raise ValueError('Cut exceeds simulated stock thickness')
        try:
            x0 = max(0, math.floor((min(ax, bx) - radius) / dx))
            x1 = min(n - 1, math.ceil((max(ax, bx) + radius) / dx))
            y0 = max(0, math.floor((min(ay, by) - radius) / dy))
            y1 = min(n - 1, math.ceil((max(ay, by) + radius) / dy))
        except OverflowError:
            raise ValueError('Segment exceeds numeric preview range') from None
        if x1 < x0 or y1 < y0:
            continue
        checks += (x1 - x0 + 1) * (y1 - y0 + 1)
        if checks > 8000000:
            raise ValueError('Simulation too large; lower preview resolution')
        vx = bx - ax
        vy = by - ay
        length_sq = vx * vx + vy * vy
        for j in range(y0, y1 + 1):
            for i in range(x0, x1 + 1):
                x = i * dx
                y = j * dy
                t = max(0, min(1, ((x - ax) * vx + (y - ay) * vy) / length_sq)) if length_sq else 0
                distance = math.hypot(x - ax - t * vx, y - ay - t * vy)
                if distance > radius:
                    continue
                sag = (
                    0
                    if shape == 'flat'
                    else radius - math.sqrt(max(0, radius * radius - distance * distance))
                    if shape == 'ball'
                    else distance / math.tan(math.radians(angle / 2))
                )
                # Visual approximation uses depth at closest XY point on the move.
                contact = start_z + t * (z - start_z) if length_sq else min(start_z, z)
                surface[j][i] = min(surface[j][i], contact + sag)
    return surface
