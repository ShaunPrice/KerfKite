"""Artwork relief: closed artwork contours as a raised or recessed relief (/api/artwork-relief).

Each sample of a resolution x resolution grid stands for a cell reaching one grid step to each side. A raised
relief keeps material where the cell meets the artwork, its outline included; a recessed relief keeps it where
the artwork does not cover the whole cell. The height field of 0s and 1s then goes through prepare_relief.

The native core does this on KerfKite's own polygon kernel (native/core/src/artwork_relief.cpp). The Shapely
version below is the reference, and answers when the compiled extension is missing or the environment asks for
the Python CAM. The two agree except where the artwork's outline passes within a millionth of a millimetre of
a cell's edge, or where contours cross one another and GEOS rounds the crossing points; see
docs/NATIVE.md#artwork-reliefs.
"""

from .cam import cam_backend, core_answer, core_json
from .operations import region, value
from .relief import prepare_relief

WARNING = 'Artwork relief is conservatively rasterized; increase map resolution for small glyph details.'


def artwork_relief(data, *, backend='auto'):
    """The relief of the artwork in data: prepare_relief's answer with the route's warning added."""
    core = cam_backend(backend, 'artwork relief')
    if core is not None:
        answer = core_answer(core, 'artwork-relief', data, backend)
        if answer is not None:
            return answer
    result = prepare_relief({**data, 'heightmap': raster(data, backend='python')})
    result['warnings'].append(WARNING)
    return result


def artwork_relief_json(data):
    """artwork_relief(data) as JSON text written by the core, or None when the reference must answer."""
    return core_json('artwork-relief', data)


def raster(data, *, backend='auto'):
    """The height field: resolution rows of resolution 0s and 1s, row 0 at y."""
    core = cam_backend(backend, 'artwork relief')
    if core is not None:
        answer = core_answer(core, 'artwork-raster', data, backend)
        if answer is not None:
            return answer
    return _reference_raster(data)


def _reference_raster(data):
    from shapely.geometry import box

    if not isinstance(data, dict):
        raise ValueError('Supply closed artwork paths')
    g = region(data.get('paths', []))
    width = value(data, 'width', 100, 0.1, 2000)
    height = value(data, 'height', 100, 0.1, 2000)
    x = value(data, 'x', 0, 0, 2000)
    y = value(data, 'y', 0, 0, 2000)
    n = int(value(data, 'resolution', 64, 16, 128))
    kind = data.get('kind', 'raised')
    if kind not in ('raised', 'recessed'):
        raise ValueError('Choose raised or recessed relief')
    # Protect raised material wherever its contour intersects a cell neighbourhood.
    matrix = []
    for row in range(n):
        values = []
        for col in range(n):
            px = x + col * width / (n - 1)
            py = y + row * height / (n - 1)
            cell = box(px - width / (n - 1), py - height / (n - 1), px + width / (n - 1), py + height / (n - 1))
            values.append(1 if (g.intersects(cell) if kind == 'raised' else not g.covers(cell)) else 0)
        matrix.append(values)
    return matrix
