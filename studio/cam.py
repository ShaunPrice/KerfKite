"""Conservative centreline engraving CAM, dimensions in millimetres."""

import math
import os
from bisect import bisect_right
from .cam_moves import CutRun, PackedSegments, block_count
from .laser import cut_options, prepare
from .drilling import generate_drill
from .router_entry import generate_entry
from .protocols import postprocess, get_protocol
from .operations import prepare as prepare_operation, router_tool_shape

try:  # The native core's CAM, in the compiled extension: see docs/NATIVE.md.
    from ._relief_native import core_cam as _core_cam, core_cam_text as _core_cam_text
except ImportError:
    _core_cam = _core_cam_text = None


def _python_chosen():
    """Whether the environment asks for the Python CAM: KERFKITE_GCODE_BACKEND=python, or
    KERFKITE_POLYGON_BACKEND=shapely, since only the Python CAM can take offsets, pockets and fills from Shapely."""
    return (
        os.environ.get('KERFKITE_GCODE_BACKEND') == 'python' or os.environ.get('KERFKITE_POLYGON_BACKEND') == 'shapely'
    )


def cam_backend(backend, name='G-code'):
    """The native core's CAM to use, or None for this module's Python, which is the reference."""
    if backend not in ('auto', 'python', 'native'):
        raise ValueError(f'{name} backend must be auto, python or native')
    if backend == 'native' and _core_cam is None:
        raise ValueError(f'Native {name} backend is not installed')
    if backend == 'python' or (backend == 'auto' and _python_chosen()):
        return None
    return _core_cam


def core_answer(core, command, request, backend, packed=False):
    """The core's job, or None when the reference must answer: the request cannot cross to the core unchanged
    (a tuple, a non-finite number, a key that is not text), or the core does not reproduce what it asks. With
    backend='native' either of those raises NotImplementedError instead. A rejection is the reference's
    ValueError, with its message."""
    try:
        job = core(command, request, packed)
    except NotImplementedError:
        if backend == 'native':
            raise
        return None
    if job is None and backend == 'native':
        raise NotImplementedError('The request cannot be passed to the native core unchanged')
    return job


def _bounds(paths):
    """[min x, min y, max x, max y] over every vertex."""
    # One path at a time, so the working copy is the longest path and not every coordinate of the job.
    # min(a, b) and max(a, b) keep a on a tie, so the first vertex with the extreme value is still the one returned.
    low_x = low_y = high_x = high_y = None
    for path in paths:
        if not path:
            continue
        xs = [p[0] for p in path]
        ys = [p[1] for p in path]
        if low_x is None:
            low_x, low_y, high_x, high_y = min(xs), min(ys), max(xs), max(ys)
        else:
            low_x, low_y = min(low_x, min(xs)), min(low_y, min(ys))
            high_x, high_y = max(high_x, max(xs)), max(high_y, max(ys))
    if low_x is None:
        raise ValueError('min() arg is an empty sequence')
    return [low_x, low_y, high_x, high_y]


# Exact float and int only: bool, strings and other numeric types go through number().
_PLAIN = (float, int)


def number(value, name, low, high):
    if isinstance(value, bool):
        raise ValueError(f'{name} must be a number')
    try:
        value = float(value)
    except (TypeError, ValueError):
        raise ValueError(f'{name} must be a number') from None
    if not math.isfinite(value) or not low <= value <= high:
        raise ValueError(f'{name} must be between {low} and {high}')
    return value


def _holding_tabs(project, paths, depth):
    """Flat holding tabs on final compensated cutter paths, at stock-relative Z."""
    from shapely.geometry import LineString

    if sum(map(len, paths)) > 100000:
        raise ValueError('Holding-tab source geometry exceeds 100000 vertices')
    config = project['tabs']
    if not isinstance(config, dict) or set(config) - {'width', 'height', 'count', 'positions'}:
        raise ValueError('Tabs require width, height and either count or positions')
    tab_width = number(config.get('width'), 'Tab width', 0.01, 100)
    tab_height = number(config.get('height'), 'Tab height', 0.01, 50)
    thickness = number(project.get('stock_thickness'), 'Stock thickness for tabs', 0.01, 50)
    diameter = number(project.get('tool_diameter', 3.175), 'Tab cutter diameter', 0.01, 100)
    if tab_height >= thickness:
        raise ValueError('Tab height must be less than stock thickness')
    if depth < thickness:
        raise ValueError('Holding tabs require a cut depth reaching the stock bottom')
    if 'count' in config and 'positions' in config:
        raise ValueError('Choose tab count or explicit positions, not both')
    if 'positions' in config:
        fractions = config['positions']
        if not isinstance(fractions, list) or not 1 <= len(fractions) <= 100:
            raise ValueError('Provide 1..100 tab positions as contour fractions in [0,1)')
        fractions = sorted(number(v, 'Tab position fraction', 0, 1) for v in fractions)
        if fractions[-1] == 1:
            raise ValueError('Tab positions must be less than 1; the contour seam is position 0')
    else:
        count = number(config.get('count'), 'Tab count', 1, 100)
        if not count.is_integer():
            raise ValueError('Tab count must be a whole number')
        fractions = [(i + 0.5) / int(count) for i in range(int(count))]
    plateau = tab_width + diameter
    raised_z = -(thickness - tab_height)
    result, depths = [], []
    total_vertices = 0
    for original in paths:
        points = [list(original[0])]
        for point in original[1:]:
            if math.dist(points[-1], point) > 1e-9:
                points.append(list(point))
        if len(points) < 4 or math.dist(points[0], points[-1]) > 1e-6:
            raise ValueError('Holding tabs require closed contours with at least three distinct vertices')
        points[-1] = points[0][:]
        if not LineString(points).is_simple or LineString(points).length < 1e-6:
            raise ValueError('Holding tabs require simple, non-crossing contours')
        distances = [0.0]
        for a, b in zip(points, points[1:]):
            distances.append(distances[-1] + math.dist(a, b))
        length = distances[-1]
        centers = [fraction * length for fraction in fractions]
        if any(
            (centers[(i + 1) % len(centers)] + (length if i == len(centers) - 1 else 0) - center) <= plateau + 1e-8
            for i, center in enumerate(centers)
        ):
            raise ValueError('Tabs overlap or cover the contour; reduce count/width or change positions')
        intervals = []
        for center in centers:
            lo, hi = center - plateau / 2, center + plateau / 2
            if lo < 0:
                intervals.extend([(0.0, hi), (length + lo, length)])
            elif hi > length:
                intervals.extend([(lo, length), (0.0, hi - length)])
            else:
                intervals.append((lo, hi))
        cuts = sorted(set(distances + [v for interval in intervals for v in interval]))

        def at(distance):
            if distance >= length:
                return points[0][:]
            index = min(len(points) - 2, bisect_right(distances, distance) - 1)
            ratio = (distance - distances[index]) / (distances[index + 1] - distances[index])
            return [points[index][axis] + (points[index + 1][axis] - points[index][axis]) * ratio for axis in (0, 1)]

        path, zs = [], []
        for lo, hi in zip(cuts, cuts[1:]):
            if hi - lo <= 1e-10:
                continue
            midpoint = (lo + hi) / 2
            z = raised_z if any(a <= midpoint <= b for a, b in intervals) else -depth
            if not path or zs[-1] != z:
                path.append(at(lo))
                zs.append(z)
            path.append(at(hi))
            zs.append(z)
        if zs[-1] != zs[0]:
            path.append(points[0][:])
            zs.append(zs[0])
        total_vertices += len(path)
        if total_vertices > 100000:
            raise ValueError('Holding-tab geometry exceeds 100000 vertices')
        result.append(path)
        depths.append(zs)
    warnings = [
        'Holding tabs retain material above the configured stock bottom; verify stock thickness and actual tool Z zero.',
        'Tab plateau length includes one cutter diameter of padding. Nominal tab width follows the contour; corners, nearby cuts and overlapping cutter footprints require inspection.',
        'Tabs apply to every closed contour in this operation, including holes. Flat tab shoulders use controlled vertical lifts and plunges; clamp stock and remove tabs after machining.',
    ]
    return result, depths, warnings


def core_json(command, request):
    """The core's answer as UTF-8 JSON text, for a web response to send as it is, or None when the reference
    must answer it (no extension, the Python CAM chosen by the environment, a request that cannot cross
    unchanged, or a case the core does not reproduce). Writing a job of 200,000 moves this way takes about a quarter of the
    time that Python's json module takes. A rejection is the reference's ValueError."""
    if _core_cam_text is None or _python_chosen():
        return None
    try:
        return _core_cam_text(command, request)
    except NotImplementedError:
        return None


def job_json(project):
    """generate(project) as JSON text written by the core, or None: then generate(project, backend='python')."""
    return core_json('generate', project)


def generate(project, *, packed_segments=False, _rotary_blocks=False, backend='auto'):
    """The job for a project: its program lines, preview segments (packed when packed_segments), cut length
    and time, bounds and warnings, with the paths and depths it cut, or for an ordered job the number of
    operations, and for drilling the holes and the drill.

    backend='auto' runs the native core's generator when the extension is installed and Python otherwise, or
    when the core declines; 'python' runs this module, the reference; 'native' requires the core."""
    core = cam_backend(backend)
    if core is not None and not _rotary_blocks:
        job = core_answer(core, 'generate', project, backend, packed_segments)
        if job is not None:
            if packed_segments:
                job['segments'] = PackedSegments.from_native(job['segments'])
            return job
    return _reference(project, packed_segments, _rotary_blocks, 'python' if backend == 'native' else backend)


def _reference(project, packed_segments, _rotary_blocks, backend):
    """generate() in Python: the reference the native core is compared with."""
    if not isinstance(project, dict):
        raise ValueError('Project must be an object')
    if 'operations' in project:
        operations = project['operations']
        if not isinstance(operations, list) or not 1 <= len(operations) <= 100:
            raise ValueError('Use 1–100 ordered operations')
        jobs = []
        combined_lines = 0
        tool_signature = None
        common = {k: v for k, v in project.items() if k not in ('operations', 'paths', 'path_depths', 'path_powers')}
        for operation in operations:
            if not isinstance(operation, dict) or 'operations' in operation:
                raise ValueError('Nested operations are unsupported')
            if any(
                operation.get(k, project.get(k)) != project.get(k)
                for k in ('mode', 'protocol', 'width', 'height', 'dwell_units')
            ):
                raise ValueError('An ordered job must use one head, protocol and work area')
            merged = {**common, **operation}
            if project.get('mode') == 'router':
                shape = router_tool_shape(merged)
                angle = None
                if shape == 'v':
                    angle = merged.get('v_angle', 60)
                elif shape == 'drill':
                    angle = merged.get('drill_point_angle', 118)
                signature = (merged.get('tool_id'), merged.get('tool_diameter', 3.175), shape, angle)
                if tool_signature is not None and signature != tool_signature:
                    raise ValueError(
                        'Tool identity or geometry differs between operations; export separate jobs and change tools explicitly'
                    )
                tool_signature = signature
            jobs.append(
                generate(merged, packed_segments=packed_segments, _rotary_blocks=_rotary_blocks, backend=backend)
            )
            lines = jobs[-1]['lines']
            combined_lines += block_count(lines) if _rotary_blocks else len(lines)
            if combined_lines > 200000:
                raise ValueError('Combined job exceeds line limit')
        drill_data = {}
        holes = [hole for j in jobs for hole in j.get('drill_holes', [])]
        if holes:
            drill_data['drill_holes'] = holes
            tool = jobs[0].get('simulation_tool')
            if tool and all(j.get('simulation_tool') == tool for j in jobs):
                drill_data['simulation_tool'] = tool
        segments = PackedSegments() if packed_segments else []
        for job in jobs:
            segments.extend(job['segments'])
        return {
            'lines': [l for j in jobs for l in j['lines'] if l != 'M30']
            + (['M30'] if jobs[-1]['lines'][-1] == 'M30' else []),
            'segments': segments,
            'cut_length': round(sum(j['cut_length'] for j in jobs), 2),
            'cut_minutes': round(sum(j['cut_minutes'] for j in jobs), 2),
            'mode': project['mode'],
            'bounds': [
                min(j['bounds'][0] for j in jobs),
                min(j['bounds'][1] for j in jobs),
                max(j['bounds'][2] for j in jobs),
                max(j['bounds'][3] for j in jobs),
            ],
            'warnings': list(dict.fromkeys(w for j in jobs for w in j['warnings'])),
            'operations': len(jobs),
            **drill_data,
        }
    return _generate_single(project, packed_segments, _rotary_blocks)


def _python_moves(lines, clean, laser_paths, depths, levels, laser, rotary_blocks, packed, safe, plunge, cut_feed):
    """The reference move loop: append each operation's moves to lines; return (segments, cut length)."""
    segments = PackedSegments() if packed else []
    # The loop below runs once per cutting move (up to 200,000): keep its lookups local.
    add = segments.appender() if packed else segments.append
    emit = lines.append
    sqrt = math.sqrt
    safe_line = f'G0 Z{safe:.3f}'
    feed_word = f' F{cut_feed:.1f}'
    length = 0
    for z in levels:
        for path_index, original in enumerate(clean):
            path = laser_paths[path_index] if laser else original
            x, y = path[0][:2]
            row_depths = depths[path_index] if depths is not None else None
            current_z = max(z, row_depths[0]) if row_depths is not None else z
            if not laser:
                lines += [safe_line, f'G0 X{x:.3f} Y{y:.3f}', f'G1 Z{current_z:.3f} F{plunge:.1f}']
            else:
                lines += ['M5', f'G0 X{x:.3f} Y{y:.3f}', f'M4 S{path[0][2]:.0f}']
            run_start = len(segments)
            start_z = 0 if current_z is None else current_z
            for vertex_index in range(1, len(path)):
                vertex = path[vertex_index]
                nx, ny = vertex[0], vertex[1]
                segment_power = vertex[2] if laser else None
                next_z = max(z, row_depths[vertex_index]) if row_depths is not None else current_z
                end_z = 0 if next_z is None else next_z
                if not rotary_blocks:
                    if laser:
                        emit(f'G1 X{nx:.3f} Y{ny:.3f}{feed_word} S{segment_power:.0f}')
                    elif row_depths is not None:
                        emit(f'G1 X{nx:.3f} Y{ny:.3f}{feed_word} Z{next_z:.3f}')
                    else:
                        emit(f'G1 X{nx:.3f} Y{ny:.3f}{feed_word}')
                if packed:
                    add(x, y, nx, ny, end_z, segment_power, start_z)
                else:
                    add([x, y, nx, ny, end_z, segment_power, start_z])
                length += sqrt((nx - x) ** 2 + (ny - y) ** 2 + (end_z - start_z) ** 2)
                current_z = next_z
                start_z = end_z
                x, y = nx, ny
            if rotary_blocks:
                emit(CutRun(segments, run_start, len(segments), cut_feed, depths is not None, laser))
            if laser:
                emit('M5')
            else:
                emit(safe_line)
    return segments, length


def _generate_single(project, packed_segments, rotary_blocks):
    mode = project.get('mode')
    if mode not in ('router', 'laser', 'knife'):
        raise ValueError('Choose router, laser or knife')
    tabs_enabled = project.get('tabs') is not None and project.get('tabs') is not False
    if tabs_enabled:
        if mode != 'router' or project.get('operation', 'line') not in ('line', 'inside', 'outside'):
            raise ValueError('Holding tabs require router line, inside or outside contour operations')
        if project.get('path_depths') is not None or project.get('tool_shape', 'flat') != 'flat':
            raise ValueError(
                'Holding tabs require a flat end mill and cannot combine with relief or per-vertex depth paths'
            )
    width = number(project.get('width'), 'Work width', 1, 2000)
    height = number(project.get('height'), 'Work height', 1, 2000)
    feed = number(project.get('feed'), 'Feed', 1, 10000)
    power = number(project.get('power'), 'Power / RPM', 1, 100000)
    safe = number(project.get('safe', 5), 'Clearance Z', 0.1, 100)
    depth = number(project.get('depth', 1), 'Cut depth', 0.01, 50)
    step = number(project.get('step', 0.5), 'Step down', 0.01, 10)
    plunge = number(project.get('plunge', 100), 'Plunge feed', 1, 1000)
    passes = number(project.get('passes', 1), 'Passes', 1, 100)
    if not passes.is_integer():
        raise ValueError('Passes must be a whole number')
    paths = project.get('paths')
    if not isinstance(paths, list) or not 1 <= len(paths) <= 3000:
        raise ValueError('Add between 1 and 3000 paths')
    clean = []
    count = 0
    for path in paths:
        if not isinstance(path, list) or len(path) < 2:
            raise ValueError('Each path needs at least two points')
        points = []
        for point in path:
            if not isinstance(point, list) or len(point) != 2:
                raise ValueError('Invalid XY point')
            px, py = point
            # Plain in-range numbers need no further checks; anything else takes the full validation.
            if type(px) in _PLAIN and type(py) in _PLAIN and 0 <= px <= width and 0 <= py <= height:
                points.append([float(px), float(py)])
            else:
                points.append([number(px, 'X', 0, width), number(py, 'Y', 0, height)])
        count += len(points)
        if count > 100000:
            raise ValueError('Too many vertices; simplify the design')
        clean.append(points)
    entry = project.get('router_entry', 'plunge')
    if mode == 'router' and entry != 'plunge' and project.get('operation') == 'drill':
        raise ValueError('Use the drill peck setting for drilling; select plunge as the router entry')
    if project.get('operation') == 'drill':
        return generate_drill(project, clean, number, packed_segments=packed_segments)
    if mode == 'router' and project.get('tool_shape') == 'drill':
        raise ValueError('Choose a milling cutter for a contour or pocket; a drill bit is for drilling')
    source = clean
    clean, prepared_depths, operation_warnings = prepare_operation(project, clean)
    if clean is not source:
        # The line operation returns the validated input unchanged; only compensated geometry needs rechecking.
        for path in clean:
            for point in path:
                px, py = point[0], point[1]
                if not (type(px) in _PLAIN and type(py) in _PLAIN and 0 <= px <= width and 0 <= py <= height):
                    number(px, 'Compensated X', 0, width)
                    number(py, 'Compensated Y', 0, height)
    if tabs_enabled:
        clean, prepared_depths, tab_warnings = _holding_tabs(project, clean, depth)
        operation_warnings += tab_warnings
    count = sum(map(len, clean))
    depths = prepared_depths if prepared_depths is not None else project.get('path_depths')
    if depths is not None:
        if mode == 'laser' or not isinstance(depths, list) or len(depths) != len(clean):
            raise ValueError('Depth paths must match router XY paths')
        for path, values in zip(clean, depths):
            if not isinstance(values, list) or len(values) != len(path):
                raise ValueError('Each vertex must have a matching depth')
            for value in values:
                if not (type(value) in _PLAIN and -50 <= value <= 0):
                    number(value, 'Vertex Z', -50, 0)
        depths = [[float(v) for v in path] for path in depths]
        depth = max(-min(path) for path in depths)
        if depth <= 0:
            raise ValueError('No contour cuts below the stock surface')
    if mode == 'router' and entry != 'plunge':
        return generate_entry(
            project,
            source,
            clean,
            depths,
            depth,
            step,
            safe,
            feed,
            plunge,
            power,
            operation_warnings,
            packed_segments=packed_segments,
            rotary_blocks=rotary_blocks,
        )
    laser_paths = []
    options = None
    if mode == 'laser':
        options = [
            number(project.get(key, default), label, low, high)
            for key, default, label, low, high in (
                ('lead_in', 0, 'Lead-in distance', 0, 100),
                ('lead_out', 0, 'Lead-out distance', 0, 100),
                ('ramp_in', 0, 'Start ramp distance', 0, 1000),
                ('ramp_out', 0, 'End ramp distance', 0, 1000),
                ('start_power', 100, 'Start power percent', 0, 100),
                ('end_power', 100, 'End power percent', 0, 100),
            )
        ]
        cut = cut_options(project, number)
        pixel_powers = project.get('path_powers')
        if pixel_powers is not None:
            if project.get('operation', 'line') != 'line' or any(options[:4]):
                raise ValueError('Raster powers require line operation with lead/ramp distances zero')
            if not isinstance(pixel_powers, list) or len(pixel_powers) != len(clean):
                raise ValueError('Pixel powers must match raster paths')
        expanded = 0
        for path_index, path in enumerate(clean):
            if pixel_powers is None:
                prepared = prepare(path, power, options, width, height, cut)
            else:
                powers = pixel_powers[path_index]
                if not isinstance(powers, list) or len(powers) != len(path):
                    raise ValueError('Pixel powers must match vertices')
                prepared = [(x, y, number(v, 'Pixel power', 0, power)) for (x, y), v in zip(path, powers)]
            expanded += len(prepared)
            if expanded * int(passes) > 200000:
                raise ValueError('Laser job exceeds 200,000 segments')
            laser_paths.append(prepared)
    levels = (
        [None] * int(passes)
        if mode == 'laser'
        else [-min(depth, step * (i + 1)) for i in range(math.ceil(depth / step))]
    )
    if count * len(levels) > 200000:
        raise ValueError('Job exceeds 200,000 cutting segments')
    lines = ['G21', 'G90', 'G17', 'G94', 'G54', 'M5']
    if mode != 'laser':
        lines += [f'G0 Z{safe:.3f}']
        if mode == 'router':
            lines += [f'M3 S{power:.0f}', 'G4 P2']
    packed = packed_segments or rotary_blocks
    laser = mode == 'laser'
    cut_feed = min(feed, plunge) if depths is not None else feed
    segments, length = _python_moves(
        lines, clean, laser_paths, depths, levels, laser, rotary_blocks, packed, safe, plunge, cut_feed
    )
    lines.append('M5')
    envelope = laser_paths if mode == 'laser' else clean
    warnings = (
        [get_protocol(project.get('protocol', 'grbl')).notes]
        + operation_warnings
        + [
            'Review tool geometry, stock and clearance before running.',
            'Time estimate excludes travel, plunge, acceleration and spindle startup.',
        ]
    )
    if get_protocol(project.get('protocol', 'grbl')).family == 'rs274':
        warnings.append(
            'Before loading, clear controller scaling, rotation and local offsets. Set G54 Z zero for the current tool tip with tool-length compensation off. Exact-stop output may run slower on dense curves.'
        )
    if mode == 'laser' and any(options[:4]):
        warnings.append(
            'Tangent leads are cutting moves and may mark outside the design. Power ramps use steps of at most 0.5 mm; M4 also scales power with speed.'
        )
    return {
        'lines': postprocess(lines, project.get('protocol', 'grbl'), mode, project.get('dwell_units', '')),
        'segments': segments,
        'cut_length': round(length, 2),
        'cut_minutes': round(length / (min(feed, plunge) if depths is not None else feed), 2),
        'mode': mode,
        'bounds': _bounds(envelope),
        'warnings': warnings,
        'prepared_paths': clean,
        'prepared_depths': depths,
    }
