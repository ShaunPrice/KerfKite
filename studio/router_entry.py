"""Descend on a compensated contour or a pocket-contained helical entry."""

import math

from shapely.geometry import LineString, Point
from shapely.ops import nearest_points

from .operations import polygons, region, value
from .cam_moves import PackedSegments, CutRun
from .protocols import get_protocol, postprocess


def _pocket_entries(source, paths, bit_radius, radius):
    if radius > bit_radius:
        raise ValueError('Helix radius cannot exceed the cutter radius; a larger helix leaves a central core')
    stock = region(source)
    clearance = stock.buffer(-bit_radius, quad_segs=32)
    # Offset polygons approximate round corners. Keep an extra chord allowance and
    # verify each chosen circle against the original material/island boundaries.
    pad = 0.001 + (radius + bit_radius) * 0.0004
    centres = polygons(stock.buffer(-(bit_radius + radius + pad), quad_segs=64))
    if not centres:
        raise ValueError('The pocket has insufficient room for the cutter and helix radius')
    if len(centres) * len(paths) > 100000:
        raise ValueError('Too many disconnected pocket regions for helical entry')
    allowed = clearance.buffer(1e-8)
    result = []
    for path in paths:
        seam = Point(path[0])
        for patch in sorted(centres, key=lambda p: p.distance(seam)):
            center = nearest_points(patch, seam)[0]
            if stock.boundary.distance(center) < bit_radius + radius:
                continue
            angle = math.atan2(seam.y - center.y, seam.x - center.x)
            start = [center.x + radius * math.cos(angle), center.y + radius * math.sin(angle)]
            if allowed.covers(LineString([start, path[0]])):
                result.append((center.x, center.y, angle))
                break
        else:
            raise ValueError(
                'No clear helix and connecting feed to this pocket contour; reduce the helix radius or use a contour ramp'
            )
    return result


def generate_entry(
    project,
    source,
    paths,
    caps,
    depth,
    step,
    safe,
    feed,
    plunge,
    rpm,
    warnings,
    *,
    packed_segments=False,
    rotary_blocks=False,
):
    strategy = project['router_entry']
    op = project.get('operation', 'line')
    if strategy not in ('ramp', 'helix'):
        raise ValueError('Choose plunge, contour ramp or pocket helix')
    if (
        project.get('tool_shape', 'flat') != 'flat'
        or op not in ('line', 'inside', 'outside', 'pocket')
        or project.get('path_depths') is not None
    ):
        raise ValueError(
            'Ramp and helix entries require flat end mills on closed profiles or pockets with one target depth'
        )
    if project.get('stock_thickness') is not None and depth > value(project, 'stock_thickness', 10, 0.01, 200) + 1e-8:
        raise ValueError('Entry depth exceeds the entered stock thickness')
    angle = value(project, 'entry_angle', 3, 0.25, 15)
    slope = math.tan(math.radians(angle))
    bit_radius = value(project, 'tool_diameter', 3.175, 0.01, 100) / 2
    if any(
        x - bit_radius < -1e-8
        or y - bit_radius < -1e-8
        or x + bit_radius > float(project['width']) + 1e-8
        or y + bit_radius > float(project['height']) + 1e-8
        for path in paths
        for x, y in path
    ):
        raise ValueError('Router entry cutter footprint extends outside the work area')
    lens = []
    for path in paths:
        if len(path) < 4 or math.dist(path[0], path[-1]) > 1e-6:
            raise ValueError('Contour ramp and helical entry require closed machining contours')
        line = LineString(path)
        if not line.is_simple or line.length < 1e-5:
            raise ValueError('Repair crossing or degenerate machining contours before using a ramp')
        lens.append([math.dist(a, b) for a, b in zip(path, path[1:])])
    helices = None
    if strategy == 'helix':
        if op != 'pocket' or caps is not None:
            raise ValueError('Helical entry requires a pocket with confirmed cutter clearance')
        radius = value(project, 'entry_radius', 1, 0.05, 50)
        helices = _pocket_entries(source, paths, bit_radius, radius)
        chords = max(36, math.ceil(math.pi / math.acos(max(-1, 1 - 0.005 / radius))))
        turn_length = 2 * radius * math.sin(math.pi / chords) * chords
    protocol = project.get('protocol', 'grbl')
    lines = ['G21', 'G90', 'G17', 'G94', 'G54', 'M5', f'G0 Z{safe:.3f}', f'M3 S{rpm:.0f}', 'G4 P2']
    segments = PackedSegments() if packed_segments or rotary_blocks else []
    line_count = len(lines)
    length = minutes = 0.0
    bounds = [math.inf, math.inf, -math.inf, -math.inf]
    state = None

    def move(x, y, z, rate):
        nonlocal state, length, minutes, line_count
        if line_count >= 199996:
            raise ValueError('Entry exceeds the 200000-line limit; simplify the contour or adjust the depth and angle')
        ax, ay, az = state
        if [x, y, z] == state:
            return
        if rotary_blocks:
            if isinstance(lines[-1], CutRun) and lines[-1].feed == rate:
                lines[-1].stop += 1
            else:
                lines.append(CutRun(segments, len(segments), len(segments) + 1, rate, True, False))
        else:
            lines.append(f'G1 X{x:.3f} Y{y:.3f} Z{z:.3f} F{rate:.1f}')
        line_count += 1
        segments.append([ax, ay, x, y, z, None, az])
        d = math.dist((ax, ay, az), (x, y, z))
        length += d
        minutes += d / rate
        bounds[0] = min(bounds[0], ax, x)
        bounds[1] = min(bounds[1], ay, y)
        bounds[2] = max(bounds[2], ax, x)
        bounds[3] = max(bounds[3], ay, y)
        state = [x, y, z]

    previous = 0
    for level in range(1, math.ceil(depth / step) + 1):
        bottom = -min(depth, level * step)
        drop = previous - bottom
        if drop < 1e-10:
            continue
        for i, path in enumerate(paths):
            lengths = lens[i]
            perimeter = sum(lengths)
            limit = caps[i] if caps is not None else None
            if helices is None:
                x, y = path[0]
            else:
                cx, cy, start_angle = helices[i]
                x = cx + radius * math.cos(start_angle)
                y = cy + radius * math.sin(start_angle)
            lines += [f'G0 Z{safe:.3f}', f'G0 X{x:.3f} Y{y:.3f}']
            line_count += 2
            state = [x, y, safe]
            move(x, y, max(previous, limit[0]) if limit else previous, plunge)
            if helices is None:
                turns = max(1, math.ceil(drop / (slope * perimeter)))
                if line_count + turns * (len(path) - 1) > 200000:
                    raise ValueError('Contour ramp exceeds the path segment limit')
                total = turns * perimeter
                along = 0
                for _ in range(turns):
                    for j, ((px, py), edge) in enumerate(zip(path[1:], lengths), 1):
                        along += edge
                        z = max(bottom, previous - drop * along / total)
                        move(px, py, max(z, limit[j]) if limit else z, min(feed, plunge))
            else:
                turns = max(1, math.ceil(drop / (slope * turn_length)))
                count = turns * chords
                if line_count + count > 200000:
                    raise ValueError('Helix exceeds the path segment limit')
                for j in range(1, count + 1):
                    theta = start_angle + 2 * math.pi * (j % chords) / chords
                    move(
                        cx + radius * math.cos(theta),
                        cy + radius * math.sin(theta),
                        previous - drop * j / count,
                        min(feed, plunge),
                    )
                move(*path[0], bottom, feed)
            # Finish a complete lap at this level, including raised tab plateaux.
            for j, (px, py) in enumerate(path[1:], 1):
                z = max(bottom, limit[j]) if limit else bottom
                move(px, py, z, min(feed, plunge) if limit else feed)
            lines.append(f'G0 Z{safe:.3f}')
            line_count += 1
        previous = bottom
    lines.append('M5')
    return dict(
        lines=postprocess(lines, protocol, 'router', project.get('dwell_units', '')),
        segments=segments,
        cut_length=round(length, 2),
        cut_minutes=round(minutes, 2),
        mode='router',
        bounds=bounds,
        warnings=[
            get_protocol(protocol).notes,
            *warnings,
            'Entry angle is a maximum. Entry and feed plunges use the configured plunge feed limit. Inspect stock, fixtures and tool ramp capability.',
        ],
        prepared_paths=paths,
        prepared_depths=caps,
    )
