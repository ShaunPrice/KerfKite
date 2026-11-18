"""Conservative centreline engraving CAM, dimensions in millimetres."""
import math
from bisect import bisect_right
from .laser import prepare
from .protocols import postprocess, get_protocol
from .operations import prepare as prepare_operation


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
    tab_width = number(config.get('width'), 'Tab width', .01, 100)
    tab_height = number(config.get('height'), 'Tab height', .01, 50)
    thickness = number(project.get('stock_thickness'), 'Stock thickness for tabs', .01, 50)
    diameter = number(project.get('tool_diameter', 3.175), 'Tab cutter diameter', .01, 100)
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
        fractions = [(i + .5) / int(count) for i in range(int(count))]
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
        if any((centers[(i + 1) % len(centers)] + (length if i == len(centers)-1 else 0) - center) <= plateau + 1e-8
               for i, center in enumerate(centers)):
            raise ValueError('Tabs overlap or cover the contour; reduce count/width or change positions')
        intervals = []
        for center in centers:
            lo, hi = center - plateau/2, center + plateau/2
            if lo < 0:
                intervals.extend([(0.0, hi), (length+lo, length)])
            elif hi > length:
                intervals.extend([(lo, length), (0.0, hi-length)])
            else:
                intervals.append((lo, hi))
        cuts = sorted(set(distances + [v for interval in intervals for v in interval]))
        def at(distance):
            if distance >= length:
                return points[0][:]
            index = min(len(points)-2, bisect_right(distances, distance)-1)
            ratio = (distance-distances[index])/(distances[index+1]-distances[index])
            return [points[index][axis] + (points[index+1][axis]-points[index][axis])*ratio for axis in (0,1)]
        path, zs = [], []
        for lo, hi in zip(cuts, cuts[1:]):
            if hi-lo <= 1e-10:
                continue
            midpoint = (lo+hi)/2
            z = raised_z if any(a <= midpoint <= b for a,b in intervals) else -depth
            if not path or zs[-1] != z:
                path.append(at(lo));zs.append(z)
            path.append(at(hi));zs.append(z)
        if zs[-1] != zs[0]:
            path.append(points[0][:]);zs.append(zs[0])
        total_vertices += len(path)
        if total_vertices > 100000:
            raise ValueError('Holding-tab geometry exceeds 100000 vertices')
        result.append(path);depths.append(zs)
    warnings = [
        'Holding tabs retain material above the configured stock bottom; verify stock thickness and actual tool Z zero.',
        'Tab plateau length includes one cutter diameter of padding. Nominal tab width follows the contour; corners, nearby cuts and overlapping cutter footprints require inspection.',
        'Tabs apply to every closed contour in this operation, including holes. Flat tab shoulders use controlled vertical lifts and plunges; clamp stock and remove tabs after machining.',
    ]
    return result, depths, warnings


def generate(project):
    if not isinstance(project, dict):raise ValueError('Project must be an object')
    if 'operations' in project:
        operations=project['operations']
        if not isinstance(operations,list) or not 1<=len(operations)<=100:raise ValueError('Use 1–100 ordered operations')
        jobs=[]
        tool_signature=None
        for operation in operations:
            if not isinstance(operation,dict) or 'operations' in operation:raise ValueError('Nested operations are unsupported')
            if any(operation.get(k,project.get(k))!=project.get(k) for k in ('mode','protocol','width','height','dwell_units')):raise ValueError('An ordered job must use one head, protocol and work area')
            if project.get('mode')=='router':
                shape = 'v' if operation.get('operation', project.get('operation')) == 'vcarve' else operation.get('tool_shape', project.get('tool_shape', 'flat'))
                signature = (operation.get('tool_id', project.get('tool_id')),
                             operation.get('tool_diameter', project.get('tool_diameter', 3.175)), shape,
                             operation.get('v_angle', project.get('v_angle', 60)) if shape == 'v' else None)
                if tool_signature is not None and signature != tool_signature:
                    raise ValueError('Tool identity or geometry differs between operations; export separate jobs and change tools explicitly')
                tool_signature=signature
            jobs.append(generate({**{k:v for k,v in project.items() if k not in ('operations','paths','path_depths','path_powers')},**operation}))
            if sum(len(j['lines']) for j in jobs)>200000:raise ValueError('Combined job exceeds line limit')
        return {'lines':[l for j in jobs for l in j['lines'] if l != 'M30'] + (['M30'] if jobs[-1]['lines'][-1]=='M30' else []), 'segments':[s for j in jobs for s in j['segments']],
                'cut_length':round(sum(j['cut_length'] for j in jobs),2),'cut_minutes':round(sum(j['cut_minutes'] for j in jobs),2),
                'mode':project['mode'],'bounds':[min(j['bounds'][0] for j in jobs),min(j['bounds'][1] for j in jobs),max(j['bounds'][2] for j in jobs),max(j['bounds'][3] for j in jobs)],
                'warnings':list(dict.fromkeys(w for j in jobs for w in j['warnings'])),'operations':len(jobs)}
    return _generate_single(project)


def _generate_single(project):
    mode = project.get('mode')
    if mode not in ('router', 'laser', 'knife'):
        raise ValueError('Choose router, laser or knife')
    tabs_enabled = project.get('tabs') is not None and project.get('tabs') is not False
    if tabs_enabled:
        if mode != 'router' or project.get('operation', 'line') not in ('line', 'inside', 'outside'):
            raise ValueError('Holding tabs require router line, inside or outside contour operations')
        if project.get('path_depths') is not None or project.get('tool_shape', 'flat') != 'flat':
            raise ValueError('Holding tabs require a flat end mill and cannot combine with relief or per-vertex depth paths')
    width = number(project.get('width'), 'Work width', 1, 2000)
    height = number(project.get('height'), 'Work height', 1, 2000)
    feed = number(project.get('feed'), 'Feed', 1, 10000)
    power = number(project.get('power'), 'Power / RPM', 1, 100000)
    safe = number(project.get('safe', 5), 'Clearance Z', 0.1, 100)
    depth = number(project.get('depth', 1), 'Cut depth', 0.01, 50)
    step = number(project.get('step', .5), 'Step down', .01, 10)
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
            points.append([number(point[0], 'X', 0, width), number(point[1], 'Y', 0, height)])
        count += len(points)
        if count > 100000:
            raise ValueError('Too many vertices; simplify the design')
        clean.append(points)
    clean, prepared_depths, operation_warnings = prepare_operation(project, clean)
    for path in clean:
        for point in path:
            number(point[0], 'Compensated X', 0, width)
            number(point[1], 'Compensated Y', 0, height)
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
            for value in values:number(value, 'Vertex Z', -50, 0)
        depths = [[float(v) for v in path] for path in depths]
        depth = max(-v for path in depths for v in path)
        if depth <= 0:raise ValueError('No contour cuts below the stock surface')
    laser_paths = []
    options = None
    if mode == 'laser':
        options = [number(project.get(key, default), label, low, high) for key, default, label, low, high in (
            ('lead_in', 0, 'Lead-in distance', 0, 100), ('lead_out', 0, 'Lead-out distance', 0, 100),
            ('ramp_in', 0, 'Start ramp distance', 0, 1000), ('ramp_out', 0, 'End ramp distance', 0, 1000),
            ('start_power', 100, 'Start power percent', 0, 100), ('end_power', 100, 'End power percent', 0, 100))]
        pixel_powers = project.get('path_powers')
        if pixel_powers is not None:
            if project.get('operation','line')!='line' or any(options[:4]):raise ValueError('Raster powers require line operation with lead/ramp distances zero')
            if not isinstance(pixel_powers,list) or len(pixel_powers)!=len(clean):raise ValueError('Pixel powers must match raster paths')
        expanded = 0
        for path_index, path in enumerate(clean):
            if pixel_powers is None:prepared = prepare(path, power, options, width, height)
            else:
                powers=pixel_powers[path_index]
                if not isinstance(powers,list) or len(powers)!=len(path):raise ValueError('Pixel powers must match vertices')
                prepared=[(x,y,number(v,'Pixel power',0,power)) for (x,y),v in zip(path,powers)]
            expanded += len(prepared)
            if expanded * int(passes) > 200000:
                raise ValueError('Laser job exceeds 200,000 segments')
            laser_paths.append(prepared)
    levels = [None] * int(passes) if mode == 'laser' else [-min(depth, step * (i + 1)) for i in range(math.ceil(depth / step))]
    if count * len(levels) > 200000:
        raise ValueError('Job exceeds 200,000 cutting segments')
    lines = ['G21', 'G90', 'G17', 'G94', 'G54', 'M5']
    if mode != 'laser':
        lines += [f'G0 Z{safe:.3f}']
        if mode == 'router':lines += [f'M3 S{power:.0f}', 'G4 P2']
    segments = []
    length = 0
    for z in levels:
        for path_index, original in enumerate(clean):
            path = laser_paths[path_index] if mode == 'laser' else [(x, y, None) for x, y in original]
            x, y = path[0][:2]
            current_z = max(z, depths[path_index][0]) if depths is not None else z
            if mode != 'laser':
                lines += [f'G0 Z{safe:.3f}', f'G0 X{x:.3f} Y{y:.3f}', f'G1 Z{current_z:.3f} F{plunge:.1f}']
            else:
                lines += ['M5', f'G0 X{x:.3f} Y{y:.3f}', f'M4 S{path[0][2]:.0f}']
            for vertex_index, (nx, ny, segment_power) in enumerate(path[1:], 1):
                suffix = f' S{segment_power:.0f}' if mode == 'laser' else ''
                next_z = max(z, depths[path_index][vertex_index]) if depths is not None else current_z
                if depths is not None:suffix += f' Z{next_z:.3f}'
                lines.append(f'G1 X{nx:.3f} Y{ny:.3f} F{min(feed, plunge) if depths is not None else feed:.1f}' + suffix)
                segments.append([x, y, nx, ny, next_z or 0, segment_power, current_z or 0])
                length += math.sqrt((nx-x)**2 + (ny-y)**2 + ((next_z or 0)-(current_z or 0))**2)
                current_z = next_z
                x, y = nx, ny
            if mode == 'laser':
                lines.append('M5')
            else:
                lines.append(f'G0 Z{safe:.3f}')
    lines.append('M5')
    envelope = laser_paths if mode == 'laser' else clean
    warnings = [get_protocol(project.get('protocol','grbl')).notes] + operation_warnings + ['Review tool geometry, stock and clearance before running.', 'Time estimate excludes travel, plunge, acceleration and spindle startup.']
    if get_protocol(project.get('protocol','grbl')).family=='rs274':
        warnings.append('Before loading, clear controller scaling, rotation and local offsets. Set G54 Z zero for the current tool tip with tool-length compensation off. Exact-stop output may run slower on dense curves.')
    if mode == 'laser' and any(options[:4]):
        warnings.append('Straight tangent leads are cutting moves and may mark outside the design. Power ramps use steps of at most 0.5 mm; M4 also scales power with speed.')
    return {'lines': postprocess(lines, project.get('protocol', 'grbl'), mode, project.get('dwell_units','')), 'segments': segments, 'cut_length': round(length, 2),
            'cut_minutes': round(length / (min(feed, plunge) if depths is not None else feed), 2), 'mode': mode,
            'bounds': [min(p[0] for path in envelope for p in path), min(p[1] for path in envelope for p in path), max(p[0] for path in envelope for p in path), max(p[1] for path in envelope for p in path)],
            'warnings': warnings, 'prepared_paths': clean, 'prepared_depths': depths}
