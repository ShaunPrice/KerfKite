"""Conservative, top-access height-field finishing paths (millimetres).

The input is a bilinear height field, not a mesh or a volumetric stock model.
Each cell is conservatively replaced by its highest corner for collision
calculation. This deliberately leaves stock on coarse maps. Both the whole
linear move and the cutter footprint are considered; checking only vertices
would miss collisions between them. Holder, clamps and material outside the
height-field rectangle are not modeled.
"""

import base64
import binascii
import json
import math
import threading
import time

try:
    from . import _relief_native
except ImportError:
    _relief_native = None

MAX_VERTICES = 100_000
# Without the compiled extension the contact runs in Python, so its maps and work stay small.
MAX_CONTACT_CHECKS = 10_000_000
MAX_MAP = 256
# The native core's contact shares the work between threads, and between them and a capable GPU. Its largest
# job is about 4.5 s on one thread of a 2026 laptop core and a quarter of a second on 24 threads.
NATIVE_MAX_CONTACT_CHECKS = 2_000_000_000
NATIVE_MAX_MAP = 1024


def _native(backend):
    """Whether the native core, with its larger limits, will run this job."""
    return backend != 'python' and _relief_native is not None and hasattr(_relief_native, 'core_cam')


def map_limit():
    """The most points a side of an image heightmap may have in this installation."""
    return NATIVE_MAX_MAP if _native('auto') else MAX_MAP


def relief_json(data):
    """prepare_relief(data) as UTF-8 JSON text written by the native core, for a web response to send as it is, or
    None when the reference must answer it: no extension, or a request that cannot cross to the core unchanged.
    Python's json module then does not encode every point of the result. A rejection is the reference's ValueError."""
    if not _native('auto') or not hasattr(_relief_native, 'core_cam_text'):
        return None
    try:
        return _relief_native.core_cam_text('relief', data)
    except NotImplementedError:
        return None


def gpu_status():
    """The GPU the compiled contact uses for large jobs, or why there is none. Looks for it if not yet done."""
    if _relief_native is None or not hasattr(_relief_native, 'gpu_status'):
        return 'no compiled extension'
    return _relief_native.gpu_status()


_calibration = {'thread': None, 'report': None}


def calibrate_in_background(delay=2.0):
    """Measure this computer's relief contact once, on a daemon thread, after `delay` seconds: the processor's
    threads, any GPU fit to use, and both at once. Large jobs then go the way the measurements favour (see
    kerfkite::relief_calibrate). Studio starts this when it starts, so the measurement follows the computer it
    runs on today; until it finishes, jobs follow the uncalibrated rule. Returns the thread, or None when there is
    no compiled extension or a calibration has already been started."""
    if not _native('auto') or not hasattr(_relief_native, 'calibrate') or _calibration['thread'] is not None:
        return None

    def measure():
        time.sleep(delay)
        try:
            _calibration['report'] = json.loads(_relief_native.calibrate())
        except (RuntimeError, ValueError):
            _calibration['report'] = None

    thread = threading.Thread(target=measure, name='relief-calibration', daemon=True)
    _calibration['thread'] = thread
    thread.start()
    return thread


def calibration():
    """The last calibration report, or None before one has finished."""
    return _calibration['report']


def _rgb_heights(data, depth, limit):
    """Heights from an image's RGB bytes, as the editor computes them, and the map's size.

    heightmap_rgb is base64 of columns x rows x 3 bytes, the image's top row first, as a canvas gives them;
    heightmap_size is [columns, rows]. Luminance is (0.2126 R + 0.7152 G + 0.0722 B) / 255, evaluated in the
    same order as the editor's JavaScript, so a value here is the value the editor would have sent as a number.
    Rows are turned over so that row 0 is the bottom of the image, as in heightmap.
    """
    import numpy as np

    size = data.get('heightmap_size')
    if (
        not isinstance(size, list)
        or len(size) != 2
        or not all(isinstance(v, int) and not isinstance(v, bool) for v in size)
        or not all(2 <= v <= limit for v in size)
    ):
        raise ValueError(f'heightmap_size must be [columns, rows], each 2..{limit}')
    columns, rows = size
    text = data.get('heightmap_rgb')
    if not isinstance(text, str) or len(text) > 4 * ((columns * rows * 3 + 2) // 3):
        raise ValueError('heightmap_rgb must be base64 of columns x rows x 3 bytes')
    try:
        raw = base64.b64decode(text, validate=True)
    except (ValueError, binascii.Error) as exc:
        raise ValueError('heightmap_rgb must be base64 of columns x rows x 3 bytes') from exc
    if len(raw) != columns * rows * 3:
        raise ValueError('heightmap_rgb must be base64 of columns x rows x 3 bytes')
    pixels = np.frombuffer(raw, dtype=np.uint8).reshape(rows, columns, 3)[::-1].astype(np.float64)
    luminance = (0.2126 * pixels[..., 0] + 0.7152 * pixels[..., 1] + 0.0722 * pixels[..., 2]) / 255
    return (luminance - 1) * depth, rows, columns


def _number(data, key, default=None, *, minimum=0, positive=False, maximum=10000):
    value = data.get(key, default)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f'{key} must be a finite number')
    value = float(value)
    if not math.isfinite(value) or value < minimum or value > maximum or (positive and value < 0.000001):
        raise ValueError(f'{key} is outside its supported range')
    return value


def prepare_relief(data, *, backend="auto", _native_limits=False):
    """Return XY paths and corresponding (nonpositive) cutter-tip Z values.

    heightmap rows advance in +Y and columns in +X. Values 0..1 map to
    -depth..0. A nonnegative allowance leaves extra stock, capped at Z=0
    (the stock top). Separate raster rows require the caller to retract
    before traversing, and apply its normal step-down, feed and machine bounds
    checks. This function emits geometry only, never machine commands.

    backend='auto' runs the native core when the extension is installed and Python otherwise, or when the core
    declines; 'python' runs this module, the reference, with its smaller limits ('_native_limits' gives it the
    core's, for comparison); 'native' requires the core.
    """
    if backend not in ('auto', 'python', 'native'):
        raise ValueError('Relief backend must be auto, python or native')
    if backend == 'native' and not _native('native'):
        raise ValueError('Native relief backend is not installed')
    if _native(backend):
        from .cam import core_answer

        answer = core_answer(_relief_native.core_cam, 'relief', data, backend)
        if answer is not None:
            return answer
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
    native = _native_limits
    limit = NATIVE_MAX_MAP if native else MAX_MAP
    # A map the compiled extension could take, but which must run in Python here.
    hint = '' if native else '; larger maps need the compiled extension'
    grid = None
    if 'heightmap_rgb' in data:
        if data.get('heightmap') is not None:
            raise ValueError('Give heightmap or heightmap_rgb, not both')
        size = data.get('heightmap_size')
        if (
            not native
            and isinstance(size, list)
            and len(size) == 2
            and any(isinstance(v, int) and MAX_MAP < v <= NATIVE_MAX_MAP for v in size)
        ):
            raise ValueError(f'heightmap_size must be [columns, rows], each 2..{MAX_MAP}{hint}')
        grid, rows, columns = _rgb_heights(data, depth, limit)
        values = grid.tolist()
    else:
        matrix = data.get('heightmap')
        if not isinstance(matrix, list) or not 2 <= len(matrix) <= limit:
            extra = hint if isinstance(matrix, list) and limit < len(matrix) <= NATIVE_MAX_MAP else ''
            raise ValueError(f'heightmap must contain 2..{limit} rows{extra}')
        columns = len(matrix[0]) if isinstance(matrix[0], list) else 0
        if not 2 <= columns <= limit:
            extra = hint if limit < columns <= NATIVE_MAX_MAP else ''
            raise ValueError(f'heightmap must contain 2..{limit} columns{extra}')
        values = []
        for row in matrix:
            if not isinstance(row, list) or len(row) != columns:
                raise ValueError('heightmap must be rectangular')
            converted = []
            for value in row:
                if (
                    isinstance(value, bool)
                    or not isinstance(value, (int, float))
                    or not math.isfinite(value)
                    or not 0 <= value <= 1
                ):
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
    if nx * (ny + 1) * cells_x * cells_y > (NATIVE_MAX_CONTACT_CHECKS if native else MAX_CONTACT_CHECKS):
        raise ValueError('Relief contact calculation too large; reduce map resolution or tool diameter')
    maxima = [
        [max(values[j][i], values[j][i + 1], values[j + 1][i], values[j + 1][i + 1]) for i in range(columns - 1)]
        for j in range(rows - 1)
    ]
    work_limit = NATIVE_MAX_CONTACT_CHECKS if native else MAX_CONTACT_CHECKS
    paths, depths = [], []
    checks = 0
    for raster in range(ny + 1):
        py = height * raster / ny
        xs = [width * i / nx for i in range(nx + 1)]
        contact = []
        for start, end in zip(xs, xs[1:]):
            best = -depth
            lo_i = max(0, math.floor((start - radius) / dx))
            hi_i = min(columns - 2, math.floor((end + radius) / dx))
            lo_j = max(0, math.floor((py - radius) / dy))
            hi_j = min(rows - 2, math.floor((py + radius) / dy))
            for j in range(lo_j, hi_j + 1):
                yd = max(j * dy - py, py - (j + 1) * dy, 0)
                for i in range(lo_i, hi_i + 1):
                    checks += 1
                    if checks > work_limit:
                        raise ValueError('Relief contact calculation exceeds supported work limit')
                    xd = max(i * dx - end, start - (i + 1) * dx, 0)
                    distance_sq = xd * xd + yd * yd
                    if distance_sq > radius * radius:
                        continue
                    sag = 0 if shape == 'flat' else radius - math.sqrt(max(0, radius * radius - distance_sq))
                    best = max(best, maxima[j][i] - sag)
            contact.append(min(0.0, best + allowance))
        z = [contact[0]] + [max(contact[i - 1], contact[i]) for i in range(1, nx)] + [contact[-1]]
        path = [[x + px, y + py] for px in xs]
        if raster % 2:
            path.reverse()
            z.reverse()
        paths.append(path)
        depths.append(z)
    return {
        'paths': paths,
        'path_depths': depths,
        # Where the contact was computed: 'gpu', 'cpu' (C++ threads) or 'python'. Not part of the surface, which is
        # saved with the design: the same design gives the same surface on any computer.
        'device': 'python',
        'surface': {
            'kind': 'bilinear-heightmap',
            'width': width,
            'height': height,
            'depth': depth,
            'x': x,
            'y': y,
            'rows': rows,
            'columns': columns,
            'sample_spacing': [dx, dy],
            'actual_stepover': height / ny,
            'tool_shape': shape,
            'tool_diameter': diameter,
            'allowance': allowance,
            'compensation': 'conservative-cell-envelope',
            'backend': 'python',
        },
        'warnings': [
            'Top-access finishing geometry only; caller must apply safe retracts, step-downs and machine limits.',
            'Compensation bounds the supplied bilinear height field using cell maxima. Coarse samples leave extra stock; features absent from the heightmap cannot be recovered.',
            'Raster centers reach the map edges: clear the cutter-radius overhang outside the map. Outside stock, clamps, holder and undercuts are not modeled.',
            'Allowance is capped at the stock top (Z=0). Ball-cutter raster spacing can leave scallops between passes.',
        ],
    }
