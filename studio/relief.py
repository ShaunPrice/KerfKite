"""Conservative, top-access height-field finishing paths (millimetres).

The input is a bilinear height field, not a mesh or a volumetric stock model.
Each cell is conservatively replaced by its highest corner for collision
calculation. This deliberately leaves stock on coarse maps. Both the whole
linear move and the cutter footprint are considered; checking only vertices
would miss collisions between them. Holder, clamps and material outside the
height-field rectangle are not modeled.
"""
import math

try:
    from . import _relief_native
except ImportError:
    _relief_native = None

MAX_VERTICES = 100_000
MAX_CONTACT_CHECKS = 10_000_000


def _number(data, key, default=None, *, minimum=0, positive=False, maximum=10000):
    value = data.get(key, default)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f'{key} must be a finite number')
    value = float(value)
    if not math.isfinite(value) or value < minimum or value > maximum or (positive and value < 0.000001):
        raise ValueError(f'{key} is outside its supported range')
    return value


def prepare_relief(data, *, backend="auto"):
    """Return XY paths and corresponding (nonpositive) cutter-tip Z values.

    heightmap rows advance in +Y and columns in +X. Values 0..1 map to
    -depth..0. A nonnegative allowance leaves extra stock, capped at Z=0
    (the stock top). Separate raster rows require the caller to retract
    before traversing, and apply its normal step-down, feed and machine bounds
    checks. This function emits geometry only, never machine commands.
    """
    if backend not in ('auto', 'python', 'native'):
        raise ValueError('Relief backend must be auto, python or native')
    if backend == 'native' and _relief_native is None:
        raise ValueError('Native relief backend is not installed')
    if not isinstance(data, dict):
        raise ValueError('Relief input must be an object')
    width = _number(data, 'width', positive=True)
    height = _number(data, 'height', positive=True)
    depth = _number(data, 'depth', positive=True, maximum=1000)
    x = _number(data, 'x', 0)
    y = _number(data, 'y', 0)
    diameter = _number(data, 'tool_diameter', positive=True, maximum=1000)
    step = _number(data, 'stepover', positive=True)
    allowance = _number(data, 'allowance', 0, maximum=1000)
    if step > diameter:
        raise ValueError('stepover must not exceed tool_diameter')
    if allowance > depth:
        raise ValueError('allowance must not exceed depth')
    shape = data.get('tool_shape', 'ball')
    if shape not in ('flat', 'ball'):
        raise ValueError('tool_shape must be flat or ball')
    matrix = data.get('heightmap')
    if not isinstance(matrix, list) or not 2 <= len(matrix) <= 256:
        raise ValueError('heightmap must contain 2..256 rows')
    columns = len(matrix[0]) if isinstance(matrix[0], list) else 0
    if not 2 <= columns <= 256:
        raise ValueError('heightmap must contain 2..256 columns')
    values = []
    for row in matrix:
        if not isinstance(row, list) or len(row) != columns:
            raise ValueError('heightmap must be rectangular')
        converted = []
        for value in row:
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not 0 <= value <= 1:
                raise ValueError('heightmap values must be finite numbers in 0..1')
            converted.append((float(value) - 1) * depth)
        values.append(converted)
    rows = len(values)
    dx, dy = width / (columns - 1), height / (rows - 1)
    # Sampling no coarser than the input X grid or requested stepover.
    nx = math.ceil(width / min(dx, step))
    ny = math.ceil(height / step)
    if (nx + 1) * (ny + 1) > MAX_VERTICES:
        raise ValueError('Relief exceeds 100000 vertices; increase stepover or reduce map size')
    radius = diameter / 2
    # Bound work before allocating paths. Actual checks are capped as well.
    cells_x = min(columns - 1, math.ceil((width / nx + diameter) / dx) + 2)
    cells_y = min(rows - 1, math.ceil(diameter / dy) + 2)
    if nx * (ny + 1) * cells_x * cells_y > MAX_CONTACT_CHECKS:
        raise ValueError('Relief contact calculation too large; reduce map resolution or tool diameter')
    maxima = [[max(values[j][i], values[j][i+1], values[j+1][i], values[j+1][i+1])
               for i in range(columns - 1)] for j in range(rows - 1)]
    native_depths = None
    if backend != 'python' and _relief_native is not None:
        native_depths = _relief_native.contact(maxima, width, height, depth, diameter, allowance, nx, ny, shape)
    paths, depths = [], []
    checks = 0
    for raster in range(ny + 1):
        py = height * raster / ny
        xs = [width * i / nx for i in range(nx + 1)]
        if native_depths is not None:
            z = native_depths[raster]
        else:
            contact = []
            for start, end in zip(xs, xs[1:]):
                best = -depth
                lo_i = max(0, math.floor((start-radius) / dx))
                hi_i = min(columns-2, math.floor((end+radius) / dx))
                lo_j = max(0, math.floor((py-radius) / dy))
                hi_j = min(rows-2, math.floor((py+radius) / dy))
                for j in range(lo_j, hi_j+1):
                    yd = max(j*dy-py, py-(j+1)*dy, 0)
                    for i in range(lo_i, hi_i+1):
                        checks += 1
                        if checks > MAX_CONTACT_CHECKS:
                            raise ValueError('Relief contact calculation exceeds supported work limit')
                        xd = max(i*dx-end, start-(i+1)*dx, 0)
                        distance_sq = xd*xd + yd*yd
                        if distance_sq > radius*radius:
                            continue
                        sag = 0 if shape == 'flat' else radius - math.sqrt(max(0, radius*radius-distance_sq))
                        best = max(best, maxima[j][i] - sag)
                contact.append(min(0.0, best + allowance))
            z = [contact[0]] + [max(contact[i-1], contact[i]) for i in range(1, nx)] + [contact[-1]]
        path = [[x+px, y+py] for px in xs]
        if raster % 2:
            path.reverse()
            z.reverse()
        paths.append(path)
        depths.append(z)
    return {
        'paths': paths,
        'path_depths': depths,
        'surface': {'kind': 'bilinear-heightmap', 'width': width, 'height': height,
                    'depth': depth, 'x': x, 'y': y, 'rows': rows, 'columns': columns,
                    'sample_spacing': [dx, dy], 'actual_stepover': height/ny,
                    'tool_shape': shape, 'tool_diameter': diameter, 'allowance': allowance,
                    'compensation': 'conservative-cell-envelope',
                    'backend': 'cpp' if native_depths is not None else 'python'},
        'warnings': [
            'Top-access finishing geometry only; caller must apply safe retracts, step-downs and machine limits.',
            'Compensation bounds the supplied bilinear height field using cell maxima. Coarse samples leave extra stock; features absent from the heightmap cannot be recovered.',
            'Raster centers reach the map edges: clear the cutter-radius overhang outside the map. Outside stock, clamps, holder and undercuts are not modeled.',
            'Allowance is capped at the stock top (Z=0). Ball-cutter raster spacing can leave scallops between passes.',
        ],
    }
