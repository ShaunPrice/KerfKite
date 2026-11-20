"""Read foreign drawing and toolpath files as editable artwork for layer-gated review."""

import io
import math
import re
from collections import Counter
from pathlib import Path

from flask import jsonify, request


UNITS = {'mm': 1.0, 'in': 25.4, 'inch': 25.4, 'cm': 10.0, 'm': 1000.0, 'ft': 304.8}
TOLERANCE = 0.025  # millimetres
MAX_POINTS = 100_000
MAX_PATHS = 3000
NUMBER = r'[-+]?(?:\d+(?:\.\d*)?|\.\d+)'


class Drawing:
    def __init__(self):
        self.objects = []
        self.layers = {}
        self.presets = []
        self.warnings = []
        self.mode = None
        self.points = 0

    def path(self, name, layer, points, depths=None):
        if len(points) < 2:
            return
        self.points += len(points)
        if self.points > MAX_POINTS or len(self.objects) >= MAX_PATHS:
            raise ValueError('Simplify the drawing to 100000 vertices and 3000 paths')
        if any(not math.isfinite(v) or abs(v) > 2000 for point in points for v in point):
            raise ValueError('Move or scale the drawing into the portable project ±2000 mm XY range')
        obj = {'name': name[:100], 'paths': [points], 'sourceLayer': layer}
        if depths is not None:
            if all(-50 <= z <= 0 for z in depths) and min(depths) < 0:
                obj['depths'] = [depths]
            else:
                obj['sourceZ'] = [depths]
        self.layers.setdefault(layer, {'key': layer, 'name': layer})
        self.objects.append(obj)

    def finish(self):
        if not self.objects:
            raise ValueError(
                'File contains no supported drawing paths' + ('. ' + ' '.join(self.warnings) if self.warnings else '')
            )
        # The sender accepts at most 100 ordered operations. Retain individually
        # selectable contours on small drawings and batch long consecutive runs
        # with identical layer/depth semantics on more densely posted artwork.
        if len(self.objects) > 100:
            groups = []
            for obj in self.objects:
                if groups and groups[-1]['sourceLayer'] == obj['sourceLayer'] and groups[-1].keys() == obj.keys():
                    group = groups[-1]
                    group['name'] = group['sourceLayer'][:80] + ' contours'
                    for key in ('paths', 'depths', 'sourceZ'):
                        if key in obj:
                            group[key].extend(obj[key])
                else:
                    groups.append(obj)
            self.objects = groups
        return {
            'objects': self.objects,
            'layers': list(self.layers.values()),
            'presets': self.presets,
            'devices': [],
            'warnings': self.warnings,
            'mode': self.mode,
        }


def _arc(start, centre, sweep, radius):
    if not 0 < radius <= 1e6 or not 0 < abs(sweep) <= math.tau + 1e-9:
        raise ValueError('Arc radius or sweep outside supported range')
    delta = min(math.pi / 8, 4 * math.asin(math.sqrt(min(1.0, TOLERANCE / (2 * radius)))))
    count = math.ceil(abs(sweep) / delta)
    if count > MAX_POINTS:
        raise ValueError('Arc is too large to flatten')
    angle = math.atan2(start[1] - centre[1], start[0] - centre[0])
    return [
        [
            centre[0] + radius * math.cos(angle + sweep * i / count),
            centre[1] + radius * math.sin(angle + sweep * i / count),
        ]
        for i in range(1, count + 1)
    ]


def _dxf_clip_polygon(clip, transform):
    """Project an ordinary planar spatial filter into the drawing's millimetre coordinates."""
    from ezdxf.math import BoundingBox2d, Matrix44, Z_AXIS
    from shapely.geometry import Polygon

    spatial = clip.get_spatial_filter()
    dxf = spatial.dxf
    if (
        clip.is_inverted_clip
        or dxf.has_front_clipping_plane
        or dxf.has_back_clipping_plane
        or not dxf.extrusion.isclose(Z_AXIS)
        or dxf.origin.x
        or dxf.origin.y
        or not all(math.isclose(a, b, abs_tol=1e-12) for a, b in zip(spatial.transform_matrix, Matrix44()))
    ):
        raise ValueError('unsupported inverted, 3D or custom-plane XCLIP')
    matrix = spatial.inverse_insert_matrix @ transform
    # Clipping is an infinite prism through XY. A tilted axis needs a 3D intersection.
    z = matrix.transform_direction(Z_AXIS)
    if not z.magnitude or math.hypot(z.x, z.y) > 1e-10 * z.magnitude:
        raise ValueError('unsupported tilted XCLIP')
    vertices = spatial.boundary_vertices
    if len(vertices) == 2:
        # Expand BEFORE rotation; the transformed diagonal alone loses two corners.
        vertices = BoundingBox2d(vertices).rect_vertices()
    if not 3 <= len(vertices) <= 256:
        raise ValueError('XCLIP needs 3–256 polygon vertices')
    xy = [(v.x, v.y) for v in matrix.transform_vertices(vertices)]
    if not all(math.isfinite(c) for point in xy for c in point):
        raise ValueError('nonfinite XCLIP boundary')
    polygon = Polygon(xy)
    if not polygon.is_valid or polygon.area <= 0:
        raise ValueError('invalid XCLIP boundary')
    return polygon


def _dxf_clipped_paths(points, mask):
    """Retain drawing order, retraces and gaps. The crop boundary is never a cutting edge."""
    from shapely.geometry import LineString

    if len(points) < 2:
        return []
    if mask is None:
        return [points]
    line = LineString(points)
    if mask.covers(line):
        return [points]
    if mask.disjoint(line):
        return []
    parts = []
    output_points = 0
    for start, end in zip(points, points[1:]):
        visible = mask.intersection(LineString([start, end]))
        # A concave window can retain several separate intervals from one source segment.
        members = getattr(visible, 'geoms', [visible])
        intervals = []
        for piece in members:
            if piece.geom_type != 'LineString' or piece.is_empty:
                continue
            a, b = list(piece.coords[0]), list(piece.coords[-1])
            if math.dist(start, a) > math.dist(start, b):
                a, b = b, a
            intervals.append((math.dist(start, a), a, b))
        for _, a, b in sorted(intervals):
            if parts and parts[-1][-1] == a:
                parts[-1].append(b)
                output_points += 1
            else:
                parts.append([a, b])
                output_points += 2
            if len(parts) > MAX_PATHS or output_points > MAX_POINTS:
                raise ValueError('Simplify the clipped DXF to 100000 vertices and 3000 paths')
    if len(parts) > 1 and points[0] == points[-1] and parts[-1][-1] == parts[0][0]:
        parts[0] = parts.pop() + parts[0][1:]
    return parts


def _dxf(text, default_scale):
    import ezdxf
    from ezdxf import colors, units
    from ezdxf.math import Matrix44
    from ezdxf.path import make_path
    from ezdxf.xclip import XClip

    try:
        doc = ezdxf.read(io.StringIO(text))
    except (ezdxf.DXFError, TypeError, ValueError, IndexError, StopIteration) as exc:
        raise ValueError('Cannot read this ASCII DXF drawing') from exc
    drawing = Drawing()
    if doc.units == 0:
        scale = default_scale
        drawing.warnings.append(f'Unitless DXF interpreted at {scale:g} mm per drawing unit.')
    else:
        try:
            scale = units.conversion_factor(doc.units, units.MM)
        except (TypeError, ValueError, IndexError) as exc:
            raise ValueError('Unrecognised DXF drawing units') from exc
    skipped = Counter()
    visited = 0
    source_points = clipped_instances = 0

    def read_entity(entity, transform, parent_layer='0', ancestry=(), mask=None):
        nonlocal visited, source_points, clipped_instances
        visited += 1
        if visited > 20_000:
            raise ValueError('Flattened DXF exceeds 20000 entities')
        kind = entity.dxftype()
        layer = entity.dxf.get('layer', '0')
        if layer == '0':
            layer = parent_layer
        if kind == 'INSERT':
            block = entity.dxf.name.casefold()
            if block in ancestry or len(ancestry) >= 12:
                raise ValueError('Recursive or excessively nested DXF block')
            if entity.mcount > 256:
                raise ValueError('DXF block array exceeds 256 instances')
            layout = entity.block()
            if layout is None:
                skipped['missing block definition'] += 1
                return
            clip = XClip(entity)
            copies = entity.multi_insert() if entity.mcount > 1 else (entity,)
            for instance in copies:
                frame = instance.matrix44() @ transform
                window = mask
                if clip.is_clipping_enabled:
                    try:
                        local = _dxf_clip_polygon(clip, frame)
                    except ValueError as exc:
                        skipped[f'block with {exc}; export its visible outlines'] += 1
                        continue
                    clipped_instances += 1
                    window = local if mask is None else mask.intersection(local)
                    if window.is_empty or not window.area:
                        continue
                # Traverse the original block to retain nested spatial filters.
                # virtual_entities() discards clipping during expansion.
                for child in layout:
                    read_entity(child, frame, layer, ancestry + (block,), window)
            return
        if kind not in {'LINE', 'LWPOLYLINE', 'POLYLINE', 'CIRCLE', 'ARC', 'ELLIPSE', 'SPLINE'}:
            skipped[kind] += 1
            return
        if kind == 'POLYLINE' and (entity.is_poly_face_mesh or entity.is_polygon_mesh):
            skipped['MESH'] += 1
            return
        path = make_path(entity).transform(transform)
        points = []
        z0 = None
        for p in path.flattening(distance=TOLERANCE, segments=4):
            z = p.z
            if z0 is None:
                z0 = z
            if not math.isfinite(z) or abs(z - z0) > 1e-6:
                skipped['non-XY contour'] += 1
                return
            xy = [p.x, p.y]
            if not all(math.isfinite(axis) for axis in xy):
                raise ValueError('DXF contains a nonfinite XY coordinate')
            if not points or points[-1] != xy:
                points.append(xy)
            if len(points) + source_points > MAX_POINTS:
                raise ValueError('Simplify DXF curves to at most 100000 vertices')
        source_points += len(points)
        for run in _dxf_clipped_paths(points, mask):
            drawing.path(f'{layer} · {kind.title()}', layer, run)
        if layer in drawing.layers and doc.layers.has_entry(layer):
            entry = doc.layers.get(layer)
            color = entry.rgb
            if color is None and 1 <= entry.color <= 255:
                color = colors.aci2rgb(entry.color)
            if color:
                drawing.layers[layer]['color'] = '#%02x%02x%02x' % tuple(color)
            drawing.layers[layer]['visible'] = not (entry.is_off() or entry.is_frozen())

    try:
        unit_transform = Matrix44.scale(scale)
        for entity in doc.modelspace():
            read_entity(entity, unit_transform)
    except ezdxf.DXFError as exc:
        raise ValueError('Malformed or unsupported DXF entity') from exc
    if clipped_instances:
        drawing.warnings.append(
            f'Applied XCLIP to {clipped_instances} block instance(s). Only visible contour segments are retained.'
        )
    if skipped:
        drawing.warnings.append('Skipped DXF entities: ' + ', '.join(f'{k} ({v})' for k, v in sorted(skipped.items())))
    drawing.warnings.append(
        'Imported model-space outlines in XY. Elevation, widths, paper layouts and plot styles are omitted.'
    )
    return drawing.finish()


def _hpgl(text):
    drawing = Drawing()
    drawing.mode = 'knife'
    absolute, down, pen = True, False, 1
    position = [0.0, 0.0]
    stroke = []

    def flush():
        drawing.path(f'Pen {pen} stroke', f'Pen {pen}', stroke.copy())
        stroke.clear()

    def move(point):
        nonlocal position
        if down and pen > 0 and position != point:
            if not stroke:
                stroke.append(position)
            stroke.append(point)
            if drawing.points + len(stroke) > MAX_POINTS:
                raise ValueError('Simplify HP-GL pen strokes')
        position = point

    velocities, forces = {}, {}
    for idx, statement in enumerate(text.split(';'), 1):
        statement = statement.strip().upper()
        if not statement:
            continue
        op, body = statement[:2], statement[2:].strip()
        tokens = re.split(r'(?:\s*,\s*|\s+)', body) if body else []
        if any(not re.fullmatch(NUMBER, word) or abs(float(word)) > 40_000_000 for word in tokens):
            raise ValueError(f'Invalid plotter values at instruction {idx}')
        values = list(map(float, tokens))
        if op in ('IN', 'DF') and not values:
            flush()
            if op == 'IN':
                pen, position = 1, [0.0, 0.0]
            absolute, down = True, False
        elif op == 'SP' and len(values) <= 1:
            flush()
            chosen = values[0] if values else 0.0
            if not chosen.is_integer() or not 0 <= chosen <= 255:
                raise ValueError('Pen number must be between 0 and 255')
            pen = int(chosen)
        elif op in ('PA', 'PR', 'PU', 'PD') and len(values) % 2 == 0:
            if op in ('PA', 'PR'):
                absolute = op == 'PA'
            elif op == 'PU':
                flush()
                down = False
            else:
                down = True
            for x, y in zip(values[::2], values[1::2]):
                move([x / 40 + (0 if absolute else position[0]), y / 40 + (0 if absolute else position[1])])
        elif op in ('AA', 'AR') and len(values) in (3, 4):
            centre = [v / 40 + (position[i] if op == 'AR' else 0) for i, v in enumerate(values[:2])]
            for point in _arc(position, centre, math.radians(values[2]), math.dist(position, centre)):
                move(point)
        elif op == 'CI' and len(values) in (1, 2) and values[0] > 0:
            flush()
            radius = values[0] / 40
            first = [position[0] + radius, position[1]]
            circle = [first, *_arc(first, position, math.tau, radius)]
            circle[-1] = first.copy()
            if pen > 0:
                drawing.path(f'Pen {pen} circle', f'Pen {pen}', circle)
        elif op in ('VS', 'FS') and len(values) in (1, 2):
            selector = values[1] if len(values) == 2 else pen
            if not float(selector).is_integer() or not 0 <= selector <= 255 or values[0] < 0:
                raise ValueError('Invalid plotter pen setting')
            pen_id = int(selector)
            settings = velocities if op == 'VS' else forces
            settings.setdefault(pen_id, set()).add(values[0])
            if len(settings[pen_id]) > 16:
                raise ValueError('Too many different settings for a single pen')
        else:
            raise ValueError(f'Unsupported HP-GL instruction {idx}: {op}. Export plain, unscaled pen paths.')
    flush()
    for pen_id, speeds in velocities.items():
        for speed in sorted(speeds):
            feed = speed * 600  # HP-GL VS uses centimetres / second
            if 1 <= feed <= 10_000:
                drawing.presets.append({'name': f'Pen {pen_id} · {speed:g} cm/s', 'settings': {'feed': feed}})
        drawing.warnings.append(f'Pen {pen_id} velocity (cm/s): ' + ', '.join(f'{v:g}' for v in sorted(speeds)))
    for pen_id, pressures in forces.items():
        drawing.warnings.append(f'Pen {pen_id} force (source units): ' + ', '.join(f'{v:g}' for v in sorted(pressures)))
    drawing.warnings.append(
        'HP-GL uses 40 plotter units per mm. Pen travel is omitted. Confirm blade offset, drag depth and pressure on the target cutter.'
    )
    return drawing.finish()


def _arc_centre(start, end, radius, clockwise):
    chord = math.dist(start, end)
    if chord == 0 or chord > 2 * abs(radius) + 1e-8:
        raise ValueError('Ambiguous or impossible radius-format arc')
    offset = math.sqrt(max(0.0, radius**2 - chord**2 / 4))
    side = (-1 if clockwise else 1) * (-1 if radius < 0 else 1)
    dx, dy = (end[0] - start[0]) / chord, (end[1] - start[1]) / chord
    return [(start[0] + end[0]) / 2 - side * offset * dy, (start[1] + end[1]) / 2 + side * offset * dx]


def _gcode(text, default_scale):
    drawing = Drawing()
    scale = default_scale
    absolute, absolute_centre = True, False
    motion = None
    tool = 'Toolpath'
    selected = None
    position = [None, None, None]
    stroke, heights = [], []
    feeds, speeds = set(), set()
    words = re.compile(r'([A-Z])\s*(' + NUMBER + r')')
    ended = assumed_start = used_fallback = False
    explicit_units = False

    def flush():
        if stroke:
            depth = heights.copy() if all(v is not None for v in heights) else None
            drawing.path(tool, tool, stroke.copy(), depth)
        stroke.clear()
        heights.clear()

    for index, raw in enumerate(text.splitlines(), 1):

        def fail(why):
            raise ValueError(f'G-code line {index}: {why}')

        raw = raw.split(';')[0].upper()
        raw = re.sub(r'\([^()]*\)', '', raw).strip()
        if not raw or raw == '%':
            continue
        if ended:
            fail('instructions after program end')
        cursor = 0
        args = {}
        gcodes, mcodes = [], []
        for match in words.finditer(raw):
            if raw[cursor : match.start()].strip():
                fail('unsupported expressions or syntax')
            cursor = match.end()
            key, number = match[1], float(match[2])
            if not math.isfinite(number) or abs(number) > 1e7:
                fail('word outside supported range')
            if key == 'G':
                gcodes.append(number)
            elif key == 'M':
                mcodes.append(number)
            else:
                if key not in 'NXYZIJRFSTP' or key in args:
                    fail(f'unsupported or duplicate {key} word')
                args[key] = number
        if raw[cursor:].strip():
            fail('unsupported expressions or syntax')
        groups = ((0, 1, 2, 3, 80), (20, 21), (90, 91), (90.1, 91.1))
        if any(sum(code in group for code in gcodes) > 1 for group in groups):
            fail('conflicting modal commands')
        for code in gcodes:
            if code in (0, 1, 2, 3):
                motion = int(code)
            elif code == 80:
                motion = None
            elif code in (20, 21):
                scale, explicit_units = (25.4 if code == 20 else 1.0), True
            elif code in (90, 91):
                absolute = code == 90
            elif code in (90.1, 91.1):
                absolute_centre = code == 90.1
            elif code not in (4, 17, 40, 49, 54, 61, 94):
                fail(f'unsupported G{code:g}; export linear or XY-plane arc toolpaths')
        if 'F' in args:
            feeds.add(round(args['F'] * scale, 5))
        if 'S' in args:
            speeds.add(args['S'])
        if 'T' in args:
            if args['T'] < 0 or not args['T'].is_integer():
                fail('invalid tool number')
            selected = int(args['T'])
        for m in mcodes:
            if m in (2, 30):
                ended = True
            elif m == 6:
                if selected is None:
                    fail('tool change without tool number')
                flush()
                tool = f'Tool {selected}'
            elif m not in (0, 1, 3, 4, 5, 7, 8, 9):
                fail(f'unsupported M{m:g}')
        if 'P' in args and 4 not in gcodes:
            fail('only dwell accepts P')
        if 4 in gcodes:
            if any(k in args for k in 'XYZIJR'):
                fail('unsupported coordinates on dwell')
            continue
        if not any(k in args for k in 'XYZIJR'):
            continue
        if motion is None:
            fail('missing motion mode')
        if any(k in args for k in 'IJR') and motion not in (2, 3):
            fail('arc parameters require G2 or G3')
        if not explicit_units:
            used_fallback = True
        target = position.copy()
        for i, k in enumerate('XYZ'):
            if k in args:
                if not absolute and position[i] is None:
                    fail('set an absolute starting position before incremental moves')
                target[i] = args[k] * scale + (0 if absolute else position[i])
                if abs(target[i]) > 1e6:
                    fail('coordinate outside the supported range')
        start, end = position[:2], target[:2]
        if motion == 0:
            flush()
        elif None in start or None in end:
            if motion in (2, 3):
                fail('arc has no established start position')
            assumed_start = True
        elif start != end or motion in (2, 3):
            if motion in (2, 3):
                if 'R' in args and any(k in args for k in 'IJ'):
                    fail('choose R or I/J for an arc')
                if 'R' in args:
                    centre = _arc_centre(start, end, args['R'] * scale, motion == 2)
                elif any(k in args for k in 'IJ'):
                    if absolute_centre and not {'I', 'J'} <= args.keys():
                        fail('absolute arc centres require I and J')
                    centre = [args.get(k, 0) * scale + (0 if absolute_centre else start[i]) for i, k in enumerate('IJ')]
                else:
                    fail('missing arc centre')
                radius = math.dist(start, centre)
                if abs(radius - math.dist(end, centre)) > max(0.005, radius * 0.0001):
                    fail('inconsistent arc endpoints and centre')
                angles = [math.atan2(p[1] - centre[1], p[0] - centre[0]) for p in (start, end)]
                sign = -1 if motion == 2 else 1
                sweep = ((angles[1] - angles[0]) * sign) % math.tau or math.tau
                points = _arc(start, centre, sign * sweep, radius)
                points[-1] = end
            else:
                points = [end]
            if not stroke:
                stroke.append(start)
                heights.append(position[2])
            if drawing.points + len(stroke) + len(points) > MAX_POINTS:
                fail('simplify paths to 100000 vertices')
            for i, point in enumerate(points, 1):
                stroke.append(point)
                if position[2] is None or target[2] is None:
                    heights.append(None)
                else:
                    heights.append(position[2] + (target[2] - position[2]) * i / len(points))
        elif target[2] != position[2]:
            # Start a new contour at the changed height instead of inventing a
            # sloping segment or attaching a drill plunge to unrelated artwork.
            flush()
        position = target
    flush()
    if any('depths' in obj for obj in drawing.objects):
        drawing.mode = 'router'
    for feed in sorted(feeds)[:32]:
        if 1 <= feed <= 10000:
            drawing.presets.append({'name': f'Posted feed · {feed:g} mm/min', 'settings': {'feed': feed}})
    if feeds:
        drawing.warnings.append(
            f'Source feed range {min(feeds):g}–{max(feeds):g} mm/min. Up to 32 feeds retained as presets.'
        )
    if speeds:
        drawing.warnings.append(
            f'Source S range {min(speeds):g}–{max(speeds):g}. Power/RPM requires a matching controller configuration.'
        )
    if assumed_start:
        drawing.warnings.append('Initial feed with unknown XY start excluded; subsequent positioned geometry retained.')
    if used_fallback:
        drawing.warnings.append(f'Motion before a units command interpreted at {default_scale:g} mm per unit.')
    drawing.warnings.append(
        'Feed contours imported. Rapids, stationary plunges, dwell and output commands are omitted. Compatible cutting Z is stored as depth; other known Z as source metadata. Regenerate with reviewed CAM settings.'
    )
    return drawing.finish()


def _linuxcnc_ini(text):
    known = {'TRAJ': {'LINEAR_UNITS', 'COORDINATES'}}
    for axis in 'XYZ':
        known['AXIS_' + axis] = {'MIN_LIMIT', 'MAX_LIMIT', 'MAX_VELOCITY', 'MAX_ACCELERATION'}
    values = {}
    section = ''
    for index, line in enumerate(text.splitlines(), 1):
        if re.match(r'^\s*#\s*INCLUDE\b', line, re.I):
            raise ValueError('Supply a self-contained LinuxCNC INI with resolved axis limits')
        entry = re.split(r'[;#]', line, maxsplit=1)[0].strip()
        if entry.startswith('['):
            section = entry.strip('[]').upper()
            continue
        if '=' not in entry or section not in known:
            continue
        key, value = [part.strip().upper() for part in entry.split('=', 1)]
        if key not in known[section]:
            continue
        if (section, key) in values:
            raise ValueError(f'Duplicate [{section}]{key} at line {index}')
        values[section, key] = value
    unit = values.get(('TRAJ', 'LINEAR_UNITS'))
    scale = {'MM': 1.0, 'METRIC': 1.0, 'IN': 25.4, 'INCH': 25.4, 'IMPERIAL': 25.4}.get(unit)
    if scale is None:
        raise ValueError('LinuxCNC INI needs explicit [TRAJ]LINEAR_UNITS in mm or inch')
    coords = values.get(('TRAJ', 'COORDINATES'))
    if coords is not None and ('X' not in coords or 'Y' not in coords):
        raise ValueError('This workspace requires LinuxCNC X and Y motion axes')
    spans = {}
    notes = []
    for axis in 'XYZ':
        section = 'AXIS_' + axis
        fields = {}
        for key in sorted(known[section]):
            if (section, key) not in values:
                continue
            raw = values[section, key]
            if not re.fullmatch(NUMBER + r'(?:E[+-]?\d+)?', raw):
                raise ValueError(f'[{section}]{key} needs an explicit numeric value')
            value = float(raw) * scale
            if not math.isfinite(value) or abs(value) > 1e7:
                raise ValueError(f'[{section}]{key} outside supported range')
            fields[key] = value
        if axis == 'Z' and not fields:
            continue
        if not {'MIN_LIMIT', 'MAX_LIMIT'} <= fields.keys():
            raise ValueError(f'Supply both [{section}]MIN_LIMIT and MAX_LIMIT')
        lo, hi = fields['MIN_LIMIT'], fields['MAX_LIMIT']
        span = hi - lo
        if not (0 < span <= 2000) or (axis in 'XY' and span < 1):
            raise ValueError(f'Invalid or unsupported {axis} travel span')
        spans[axis] = span
        notes.append(f'{axis} limits {lo:g} to {hi:g} mm; travel {span:g} mm.')
        for key, title, unit_label in [
            ('MAX_VELOCITY', 'velocity', 'mm/s'),
            ('MAX_ACCELERATION', 'acceleration', 'mm/s²'),
        ]:
            if key in fields:
                if fields[key] <= 0:
                    raise ValueError(f'[{section}]{key} must be positive')
                notes.append(f'{axis} maximum {title} {fields[key]:g} {unit_label}.')
    notes.append(
        'The workspace setup stores XY travel. Axis limits, velocity and acceleration are source references. HAL, routing, joint settings, homing and external files are omitted.'
    )
    return {
        'objects': [],
        'layers': [],
        'presets': [],
        'devices': [{'name': 'LinuxCNC workspace', 'width': spans['X'], 'height': spans['Y']}],
        'warnings': notes,
        'mode': None,
    }


def import_program(payload):
    if not isinstance(payload, dict):
        raise ValueError('Provide a drawing filename and its text')
    text, name = payload.get('text'), payload.get('name')
    if not isinstance(text, str) or not 1 <= len(text) <= 4_000_000 or '\0' in text:
        raise ValueError('Provide a text drawing up to 4 MB')
    if not isinstance(name, str) or len(name) > 1024:
        raise ValueError('Invalid drawing filename')
    unit = payload.get('unit', 'mm')
    if not isinstance(unit, str) or unit not in UNITS:
        raise ValueError('Choose millimetres, inches, centimetres, metres or feet')
    suffix = Path(name).suffix.lower()
    text = text.lstrip('\ufeff')
    if suffix == '.ini':
        return _linuxcnc_ini(text)
    if suffix == '.dxf':
        return _dxf(text, UNITS[unit])
    if suffix in ('.plt', '.hpgl', '.hpg'):
        return _hpgl(text)
    if suffix in ('.gcode', '.nc', '.ngc', '.cnc', '.tap'):
        return _gcode(text, UNITS[unit])
    raise ValueError('Choose an ASCII DXF, G-code or HP-GL drawing')


def register(app):
    @app.post('/api/pdf-import')
    def review_pdf():
        from .pdf_import import import_pdf

        return jsonify(import_pdf(request.get_json()))

    @app.post('/api/program-import')
    def program_import():
        return jsonify(import_program(request.get_json()))
