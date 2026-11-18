"""Bounded STL-to-height-field conversion; no solid or undercut interpretation."""
import base64
import binascii
import math
import re
import struct

MAX_BYTES = 8 * 1024 * 1024
MAX_TRIANGLES = 50_000
MAX_CELL_CHECKS = 2_000_000
_NUMBER = r'[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?'
_TRIPLE = rf'({_NUMBER})\s+({_NUMBER})\s+({_NUMBER})'
_FACET = re.compile(rf'facet\s+normal\s+{_TRIPLE}\s+outer\s+loop\s+'
                    rf'vertex\s+{_TRIPLE}\s+vertex\s+{_TRIPLE}\s+vertex\s+{_TRIPLE}'
                    r'\s+endloop\s+endfacet', re.IGNORECASE)


def _triangles(raw):
    # Binary STL headers may start with "solid"; exact record length wins.
    if len(raw) >= 84:
        count = struct.unpack_from('<I', raw, 80)[0]
        if len(raw) == 84 + 50 * count:
            if not 1 <= count <= MAX_TRIANGLES:
                raise ValueError('STL must contain 1..50000 triangles')
            triangles = []
            for offset in range(84, len(raw), 50):
                numbers = struct.unpack_from('<12f', raw, offset)
                if not all(math.isfinite(n) for n in numbers):
                    raise ValueError('STL coordinates and normals must be finite')
                triangles.append([tuple(numbers[j:j+3]) for j in (3, 6, 9)])
            return triangles
    try:
        text = raw.decode('ascii').strip()
    except UnicodeDecodeError as exc:
        raise ValueError('Invalid binary STL record count or ASCII encoding') from exc
    lines = text.splitlines()
    if len(lines) < 3 or not re.fullmatch(r'solid(?:\s.*)?', lines[0], re.I) or not re.fullmatch(r'endsolid(?:\s.*)?', lines[-1], re.I):
        raise ValueError('ASCII STL requires solid and endsolid lines')
    body = '\n'.join(lines[1:-1])
    position, triangles = 0, []
    while position < len(body):
        while position < len(body) and body[position].isspace():
            position += 1
        if position == len(body):
            break
        match = _FACET.match(body, position)
        if not match:
            raise ValueError('Malformed ASCII STL facet')
        numbers = [float(value) for value in match.groups()]
        if not all(math.isfinite(n) for n in numbers):
            raise ValueError('STL coordinates and normals must be finite')
        triangles.append([tuple(numbers[j:j+3]) for j in (3, 6, 9)])
        if len(triangles) > MAX_TRIANGLES:
            raise ValueError('STL exceeds 50000 triangles')
        position = match.end()
    if not triangles:
        raise ValueError('STL contains no triangles')
    return triangles


def _clip(points, x, y):
    """Clip triangle projection to a closed raster cell, including degeneracies."""
    polygon = points
    for axis, boundary, direction in ((0, x, 1), (0, x+1, -1), (1, y, 1), (1, y+1, -1)):
        if not polygon:
            return []
        output = []
        previous = polygon[-1]
        previous_in = direction * (previous[axis]-boundary) >= -1e-10
        for current in polygon:
            current_in = direction * (current[axis]-boundary) >= -1e-10
            if current_in != previous_in:
                difference = current[axis]-previous[axis]
                if difference:
                    t = max(0, min(1, (boundary-previous[axis])/difference))
                    output.append(tuple(previous[i]+t*(current[i]-previous[i]) for i in range(len(current))))
            if current_in:
                output.append(current)
            previous, previous_in = current, current_in
        polygon = output
    return polygon


def stl_heightmap(data):
    """Normalize STL top-access envelope to 0..1, with conservative cell heights.

    resolution is the vertex count on each axis (2..128). Every intersected
    cell is raised to the maximum Z of its clipped triangle intersections.
    Its four vertices are at least this high, so bilinear interpolation cannot
    miss thin peaks. Vertices adjacent to empty cells stay at stock top (1).
    The result may leave substantial extra stock and is not a solid model.
    """
    if not isinstance(data, dict):
        raise ValueError('STL input must be an object')
    resolution = data.get('resolution', 64)
    if isinstance(resolution, bool) or not isinstance(resolution, int) or not 2 <= resolution <= 128:
        raise ValueError('resolution must be an integer in 2..128')
    content = data.get('content_base64')
    if not isinstance(content, str) or len(content) > 4*((MAX_BYTES+2)//3):
        raise ValueError('STL content must be base64, at most 8 MiB decoded')
    try:
        raw = base64.b64decode(content, validate=True)
    except (ValueError, binascii.Error) as exc:
        raise ValueError('Invalid STL base64 data') from exc
    if not raw or len(raw) > MAX_BYTES:
        raise ValueError('STL must contain 1 byte..8 MiB')
    triangles = _triangles(raw)
    points = [point for triangle in triangles for point in triangle]
    minimum = [min(p[i] for p in points) for i in range(3)]
    maximum = [max(p[i] for p in points) for i in range(3)]
    spans = [maximum[i]-minimum[i] for i in range(3)]
    if any(not math.isfinite(v) or abs(v) > 1e12 for v in minimum+maximum+spans):
        raise ValueError('STL coordinates exceed supported numeric range')
    if spans[0] < 1e-9 or spans[1] < 1e-9:
        raise ValueError('STL must have nonzero X and Y extent')
    cells = resolution-1
    levels = [[None]*cells for _ in range(cells)]
    checks = 0
    prepared = []
    for triangle in triangles:
        projected = [((p[0]-minimum[0])/spans[0]*cells, (p[1]-minimum[1])/spans[1]*cells,
                      1.0 if spans[2] == 0 else (p[2]-minimum[2])/spans[2]) for p in triangle]
        # One neighboring cell is included at lower boundaries for robustness.
        x0 = max(0, math.floor(min(p[0] for p in projected))-1)
        x1 = min(cells-1, math.floor(max(p[0] for p in projected)))
        y0 = max(0, math.floor(min(p[1] for p in projected))-1)
        y1 = min(cells-1, math.floor(max(p[1] for p in projected)))
        checks += (x1-x0+1)*(y1-y0+1)
        if checks > MAX_CELL_CHECKS:
            raise ValueError('STL raster workload too large; reduce resolution or simplify the mesh')
        prepared.append((projected, x0, x1, y0, y1))
    for projected, x0, x1, y0, y1 in prepared:
        for y in range(y0, y1+1):
            for x in range(x0, x1+1):
                clipped = _clip(projected, x, y)
                if clipped:
                    level = max(0.0, min(1.0, max(p[2] for p in clipped)))
                    levels[y][x] = level if levels[y][x] is None else max(levels[y][x], level)
    heightmap = []
    for y in range(resolution):
        row = []
        for x in range(resolution):
            adjacent = [levels[j][i] for j in (y-1, y) for i in (x-1, x)
                        if 0 <= i < cells and 0 <= j < cells]
            row.append(1.0 if any(v is None for v in adjacent) else max(adjacent))
        heightmap.append(row)
    return {'heightmap': heightmap,
            'source_bounds': {'min': minimum, 'max': maximum, 'size': spans,
                              'triangles': len(triangles), 'units': 'unspecified'},
            'warnings': [
                'STL units are unspecified. Set output width, height and relief depth explicitly; normalization can change proportions.',
                'Conservative top-access envelope: clipped triangle maxima raise intersected cells, leaving extra stock at finite raster resolution. Thin peaks are retained, not sampled away.',
                'Empty exterior cells remain at stock top. Cells partly intersecting the mesh use its conservative envelope; exterior portions within those boundary cells are not protected.',
                'No solid, watertightness, undercut, fixture or holder analysis. Review the height field and toolpaths before machining.',
            ]}
