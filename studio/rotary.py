"""Export-only cylindrical wrapping for LinuxCNC's A axis and G93 mode.

Dialect reference: https://www.linuxcnc.org/docs/stable/html/gcode/g-code.html#gcode:g93-g94-g95
No controller transport is exposed. Ordinary three-axis GRBL is not a target.
"""
import math
import re

_WORD = re.compile(r'([A-Z])([+-]?(?:\d+(?:\.\d*)?|\.\d+))')


def _number(value, label, low, high):
    if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value) or not low <= value <= high:
        raise ValueError(f'{label} must be a finite number in {low}..{high}')
    return float(value)


def wrap_job(job, options):
    """Wrap generated router G-code from planar X or Y to unbounded A degrees.

    Requires explicit postprocessor='linuxcnc'. The remaining planar axis is
    axial travel and is emitted as X (canonical A rotates around X); Z remains
    stock-relative radial depth. A0 must be configured
    at the intended stock seam; the physical arrangement must match the chosen
    wrap axis. Feed timing uses nominal unwrapped XYZ path length, avoiding
    ambiguous degree/mm feed mixing. No modulo is applied at 360 degrees.
    """
    if not isinstance(job, dict) or not isinstance(options, dict):
        raise ValueError('Rotary job and options must be objects')
    if job.get('mode') != 'router':
        raise ValueError('Rotary export currently supports router jobs only; dynamic laser and knife are unsupported')
    if options.get('postprocessor') != 'linuxcnc':
        raise ValueError('Select the LinuxCNC rotary export postprocessor; GRBL and other dialects are unsupported')
    axis = options.get('wrap_axis')
    if axis not in ('X', 'Y'):
        raise ValueError('wrap_axis must explicitly be X or Y')
    diameter = _number(options.get('diameter'), 'Diameter', 1, 2000)
    offset = _number(options.get('angle_offset', 0), 'Angle offset', -360000, 360000)
    direction = _number(options.get('direction', 1), 'Direction', -1, 1)
    if direction not in (-1, 1):
        raise ValueError('Direction must be -1 or 1')
    source = job.get('lines')
    if not isinstance(source, list) or not 1 <= len(source) <= 200000:
        raise ValueError('Rotary source must contain 1..200000 G-code lines')
    if any(not isinstance(line, str) or len(line) > 256 for line in source):
        raise ValueError('Invalid or oversized source G-code line')
    if source[-1:] == ['M30']:
        source = source[:-1]
    scale = direction * 360 / (math.pi*diameter)
    state = dict.fromkeys('XYZ')
    units = absolute = feed_mode = False
    output, angles = [], []
    last_feed = None
    moves = 0
    for raw in source:
        tokens = raw.strip().split()
        if not tokens:
            raise ValueError('Blank source lines are unsupported')
        words = {}
        for token in tokens:
            match = _WORD.fullmatch(token)
            if not match or match[1] in words:
                raise ValueError('Rotary requires canonical generated G-code without comments or duplicate words')
            value = float(match[2])
            if not math.isfinite(value) or abs(value) > 1e9:
                raise ValueError('Source coordinate or command exceeds supported range')
            words[match[1]] = value
        command = tokens[0]
        if command in ('G21', 'G90', 'G17', 'G54', 'G94', 'M5', 'G40', 'G49', 'G80', 'G61') and len(tokens) == 1:
            if command == 'G21': units = True
            if command == 'G90': absolute = True
            if command == 'G94': feed_mode = True
            output.append('G93' if command == 'G94' else command)
            continue
        if command == 'M3' and set(words) == {'M', 'S'} and words['S'] > 0:
            output.append(raw.strip())
            continue
        if command == 'G4' and set(words) == {'G', 'P'} and 0 <= words['P'] <= 60:
            output.append(raw.strip())
            continue
        if command not in ('G0', 'G1'):
            raise ValueError('Unsupported rotary source command; arcs, incremental motion, offsets and laser modes are rejected')
        if not (units and absolute and feed_mode):
            raise ValueError('Rotary source must establish G21 G90 G94 before motion')
        allowed = {'G', 'X', 'Y', 'Z'} | ({'F'} if command == 'G1' else set())
        if set(words)-allowed or not (set(words)&set('XYZ')):
            raise ValueError('Unsupported words in rotary motion')
        coordinates = {key: words[key] for key in 'XYZ' if key in words}
        if 'Z' in coordinates and coordinates['Z'] <= -diameter/2:
            raise ValueError('Cut depth reaches or crosses the cylinder center')
        delta = 0
        if command == 'G1':
            last_feed = _number(words.get('F'), 'Source linear feed', .001, 100000)
            for key, destination in coordinates.items():
                if state[key] is None:
                    raise ValueError('Cutting move starts from an unknown position; establish rapid coordinates first')
                delta += (destination-state[key])**2
        transformed = []
        for key, destination in coordinates.items():
            if key == axis:
                # Source CAM formats XYZ to 0.001 mm. Recover exact whole
                # turns when the circumference was rounded at that boundary.
                circumference = math.pi*diameter
                turn = round(destination/circumference)
                angular_position = turn*circumference if abs(destination-turn*circumference) <= .00051 else destination
                angle = offset+angular_position*scale
                if abs(angle) > 360000:
                    raise ValueError('Rotary extent exceeds 1000 revolutions')
                angles.append(angle)
                transformed.append(f'A{angle:.6f}')
            else:
                transformed.append(f'{"X" if key in ("X", "Y") else key}{destination:.6f}')
            state[key] = destination
        if command == 'G1':
            length = math.sqrt(delta)
            if length <= 1e-12:
                # A zero-length move has no finite inverse-time duration.
                continue
            inverse = last_feed/length
            if not 1e-9 <= inverse <= 1e9:
                raise ValueError('Inverse-time feed is outside supported numeric limits')
            # Nine decimal places preserve short and very long move timing.
            output.append('G1 '+' '.join(transformed)+f' F{inverse:.9f}')
            moves += 1
        else:
            output.append('G0 '+' '.join(transformed))
    if not angles or not moves or last_feed is None:
        raise ValueError('Rotary job needs wrapped-axis motion and at least one nonzero cutting move')
    if source[-1].strip() != 'M5':
        raise ValueError('Rotary source must end with spindle off (M5)')
    output.append(f'G94 F{last_feed:.6f}')
    warnings = list(job.get('warnings', [])) + [
        'LinuxCNC rotary export only; ordinary three-axis GRBL and the current machine sender do not support this A-axis job.',
        'Configure an unwrapped continuous A axis, its direction, travel limits and seam zero. The remaining source planar coordinate is emitted as X, the axial direction for A rotation around X.',
        'Source coordinates within 0.00051 mm of an integer circumference are snapped to exact whole turns, matching source CAM decimal precision.',
        'Z remains relative to the outer stock surface. Set the rotary center and radial tool zero physically; fixture, chuck and holder collision checks are not included.',
        'Inverse-time feeds preserve nominal unwrapped surface timing including Z changes. Actual surface speed at reduced radius is lower; controller acceleration and axis limits still apply.',
        'Preview segments remain planar design coordinates; this export does not provide a validated cylindrical collision simulation.',
    ]
    return {**job, 'lines': output, 'warnings': warnings,
            'rotary': {'postprocessor': 'linuxcnc', 'export_only': True, 'diameter': diameter,
                       'circumference': math.pi*diameter, 'wrap_axis': axis, 'output_axis': 'A',
                       'angle_offset': offset, 'direction': direction, 'angle_bounds': [min(angles), max(angles)],
                       'feed_mode': 'G93', 'axial_output_axis': 'X', 'seam_snap_tolerance_mm': .00051, 'preview_coordinates': 'planar',
                       'reference': 'https://www.linuxcnc.org/docs/stable/html/gcode/g-code.html#gcode:g93-g94-g95'}}
