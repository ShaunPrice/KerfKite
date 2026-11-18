"""Cylindrical 4-axis CAM. Input to the postprocessor is generated planar CAM."""
import math
import re

_WORD = re.compile(r'([A-Z])([+-]?(?:\d+(?:\.\d*)?|\.\d+))')
# Conventional machine axis about which the stock spins, then radial tool axis.
_AXES = {'A': ('X', 'Z'), 'B': ('Y', 'Z'), 'C': ('Z', 'X')}
STREAM_POSTS = ('grblhal', 'xpro-v5')


def _number(value, label, low, high):
    if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value) or not low <= value <= high:
        raise ValueError(f'{label} must be a finite number in {low}..{high}')
    return float(value)


def _cut_length(arc, axial, radial, radius, start_z, reference):
    if reference == 'surface' or not arc:
        return math.hypot(arc, axial, radial)
    angle = abs(arc) / radius
    first, last = radius + start_z, radius + start_z + radial
    linear = math.hypot(axial, radial)
    if abs(radial) < 1e-7 * max(first, last):
        return math.hypot(linear, angle * (first + last) / 2)
    # Integrate ds along a linear coordinated block with changing cutting radius.
    def primitive(r):
        return (r * math.hypot(linear, angle * r) + linear**2 / angle * math.asinh(angle * r / linear)) / 2
    return (primitive(last) - primitive(first)) / radial


def _fixture(options, radius):
    if 'fixture' not in options:
        return None
    f = options['fixture']
    if not isinstance(f, dict):
        raise ValueError('Supply an axial window and protected core radius for the fixture')
    low = _number(f.get('axial_min'), 'Fixture axial minimum', -2000, 2000)
    high = _number(f.get('axial_max'), 'Fixture axial maximum', -2000, 2000)
    core = _number(f.get('min_radius'), 'Fixture protected core radius', 0, radius)
    if high <= low:
        raise ValueError('Fixture axial maximum must exceed its minimum')
    return {'axial_min': low, 'axial_max': high, 'min_radius': core}


def generate_rotary(project, options):
    from .cam import generate
    if not isinstance(project, dict) or not isinstance(options, dict):
        raise ValueError('Supply a project and rotary setup')
    protocol = project.get('protocol', 'grbl')
    if protocol not in ('grbl', 'grblhal', 'xpro-v5', 'fluidnc', 'linuxcnc'):
        raise ValueError('Select a GRBL-family or LinuxCNC source for rotary CAM')
    # Generate fresh design toolpaths server-side. Explicitly carry mode/axis
    # changes in the rotary post; incoming files or raw lines are never executed.
    if protocol == 'linuxcnc' and options.get('postprocessor') in STREAM_POSTS:
        raise ValueError('Choose a GRBL-family source for this rotary postprocessor')
    return wrap_job(generate(project), options)


def wrap_job(job, options):
    if not isinstance(job, dict) or not isinstance(options, dict):
        raise ValueError('Rotary job and options must be objects')
    post, mode = options.get('postprocessor'), job.get('mode')
    if post not in ('linuxcnc', *STREAM_POSTS):
        raise ValueError('Select LinuxCNC, grblHAL or xPRO V5 independent-A rotary output')
    if mode not in ('router', 'laser') or (post == 'linuxcnc' and mode != 'router'):
        raise ValueError('LinuxCNC supports router jobs only; rotary laser engraving requires grblHAL or xPRO V5')
    wrapped = options.get('wrap_axis')
    if wrapped not in ('X', 'Y'):
        raise ValueError('wrap_axis must explicitly be X or Y')
    axial = 'Y' if wrapped == 'X' else 'X'
    drive = options.get('output_axis', 'A')
    if not isinstance(drive, str) or drive not in _AXES or (post == 'xpro-v5' and drive != 'A'):
        raise ValueError('Use A on xPRO V5, or A/B/C on grblHAL and LinuxCNC')
    along, radial = _AXES[drive]
    diameter = _number(options.get('diameter'), 'Diameter', 1, 2000)
    radius, circumference = diameter / 2, math.pi * diameter
    fixture = _fixture(options, radius)
    offset = _number(options.get('angle_offset', 0), 'Angle offset', -360000, 360000)
    direction = _number(options.get('direction', 1), 'Direction', -1, 1)
    if direction not in (-1, 1):
        raise ValueError('Direction must be -1 or 1')
    reference = options.get('feed_reference', 'surface')
    if reference not in ('surface', 'cut'):
        raise ValueError('Choose surface or cut radius as the rotary feed reference')
    source = job.get('lines')
    if not isinstance(source, list) or not 1 <= len(source) <= 200000:
        raise ValueError('Rotary source must contain 1..200000 G-code lines')
    if any(not isinstance(line, str) or len(line) > 256 for line in source):
        raise ValueError('Invalid or oversized source G-code line')
    if source[-1] == 'M30':
        source = source[:-1]
    if not source or source[-1].strip() != 'M5':
        raise ValueError('Rotary source must end with spindle off (M5)')
    state = dict.fromkeys('XYZ')
    if mode == 'laser':
        state['Z'] = 0.0  # Laser focus at the stock surface.
    output, angles, axial_positions, z_positions = [], [], [], []
    units = absolute = feed_mode = inverse_active = emitting = False
    last_feed, minutes = None, 0.0
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
            words[match[1]] = _number(value, 'Source word', -1e9, 1e9)
        command = tokens[0]
        if command in ('G21', 'G90', 'G17', 'G54', 'G94', 'M5', 'G40', 'G49', 'G80', 'G61') and len(tokens) == 1:
            if post in STREAM_POSTS and command in ('G40', 'G49', 'G80', 'G61'):
                raise ValueError('Source contains a LinuxCNC-only setup command')
            if command == 'G21': units = True
            if command == 'G90': absolute = True
            if command == 'M5': emitting = False
            if command == 'G94':
                feed_mode = True
                inverse_active = post != 'xpro-v5'
                command = 'G93' if inverse_active else 'G94'
            output.append(command)
            continue
        spindle = 'M4' if mode == 'laser' else 'M3'
        if command == spindle and set(words) == {'M', 'S'}:
            _number(words['S'], 'Spindle power', 0 if mode == 'laser' else .001, 100000)
            emitting = True
            output.append(raw.strip())
            continue
        if mode == 'router' and command == 'G4' and set(words) == {'G', 'P'} and 0 <= words['P'] <= 60:
            output.append(raw.strip())
            continue
        if command not in ('G0', 'G1'):
            raise ValueError('Unsupported rotary source command')
        if not (units and absolute and feed_mode):
            raise ValueError('Rotary source must establish G21 G90 G94 before motion')
        linear = command == 'G1'
        allowed = set('GXYZ') | ({'F'} if linear else set())
        if mode == 'laser':
            allowed.discard('Z')
            if linear:
                allowed.add('S')
                if not emitting: raise ValueError('Laser cutting requires M4')
                _number(words.get('S'), 'Laser segment power', 0, 100000)
            elif emitting:
                raise ValueError('Laser rapids require M5')
        if set(words) - allowed or not set(words).intersection('XYZ'):
            raise ValueError('Unsupported words in rotary motion')
        coords = {k: words[k] for k in 'XYZ' if k in words}
        z = coords.get('Z', state['Z'])
        if z is not None:
            if radius + z <= 0:
                raise ValueError('Cut depth reaches or crosses the cylinder center')
            if fixture and radius + z < fixture['min_radius'] - 1e-9:
                raise ValueError('Rotary toolpath enters the protected fixture core')
            z_positions.append(z)
        if axial in coords:
            x = coords[axial]
            if fixture and not fixture['axial_min'] - 1e-9 <= x <= fixture['axial_max'] + 1e-9:
                raise ValueError('Rotary toolpath exceeds the fixture axial window')
            axial_positions.append(x)
        if linear:
            last_feed = _number(words.get('F'), 'Source linear feed', .001, 100000)
            if any(state[k] is None for k in coords):
                raise ValueError('Cutting move starts from an unknown position; establish rapid coordinates first')
            delta = {k: coords[k] - state[k] if k in coords else 0 for k in 'XYZ'}
            length = _cut_length(delta[wrapped], delta[axial], delta['Z'], radius, state['Z'] or 0, reference)
        transformed = []
        for k, destination in coords.items():
            if k == wrapped:
                turn = round(destination / circumference)
                arc = turn * circumference if abs(destination-turn*circumference) <= .00051 else destination
                angle = offset + direction * 360 * arc / circumference
                if abs(angle) > 360000:
                    raise ValueError('Rotary extent exceeds 1000 revolutions')
                angles.append(angle)
                transformed.append(f'{drive}{angle:.6f}')
            else:
                transformed.append(f'{along if k == axial else radial}{destination:.6f}')
            state[k] = destination
        if linear:
            if length <= 1e-12:
                continue
            inverse = _number(last_feed / length, 'Inverse-time feed', 1e-9, 1e9)
            if not inverse_active:
                output.append('G93')
                inverse_active = True
            transformed.append(f'F{inverse:.9f}')
            if mode == 'laser':
                transformed.append(f'S{words["S"]:.0f}')
            minutes += length / last_feed
        elif post == 'xpro-v5' and inverse_active:
            # Vendor GCode.cpp requires an F word for G0 under G93. Issue the
            # rapid in normal mode and restore inverse time on the next cut.
            output.append('G94')
            inverse_active = False
        output.append(command + ' ' + ' '.join(transformed))
    if not angles or not axial_positions or not z_positions or not minutes or last_feed is None:
        raise ValueError('Rotary job requires initialized axial and angular coordinates and a nonzero cutting move')
    output.append(f'G94 F{last_feed:.6f}')
    meta = dict(postprocessor=post, export_only=post == 'linuxcnc', diameter=diameter, circumference=circumference,
                wrap_axis=wrapped, output_axis=drive, axial_output_axis=along, radial_output_axis=radial,
                angle_offset=offset, direction=direction, angle_bounds=[min(angles), max(angles)],
                axial_bounds=[min(axial_positions), max(axial_positions)], radial_bounds=[min(z_positions),max(z_positions)],
                feed_mode='G93', feed_reference=reference, feed_minutes=minutes, fixture=fixture,
                seam_snap_tolerance_mm=.00051, preview_coordinates='planar')
    warnings = list(job.get('warnings', [])) + [
        f'{drive} rotates about {along}. Set the axial work zero, angular seam and surface work zero on {radial} for this fixture.',
        'Continuous angles retain full turns. Check angular travel and clearance of the tool, chuck and holder.',
    ]
    if post == 'linuxcnc':
        warnings.append('LinuxCNC rotary output is file export only.')
    return {**job, 'lines': output, 'warnings': warnings, 'rotary': meta}


def controller_rotary_axes(protocol, state):
    """Return rotary axes supported by current readback for this connection."""
    if not state.get('connected') or protocol not in STREAM_POSTS:
        return []
    if state.get('simulated'):
        return ['A'] if protocol == 'xpro-v5' else list('ABC')
    info, settings = state.get('identification', []), state.get('settings', {})
    count = state.get('position_count', 0)
    if not isinstance(count, int) or not 4 <= count <= 9:
        return []
    if protocol == 'xpro-v5':
        if count != 4 or not any(re.fullmatch(r'\[MSG:Using machine:CNC_xPRO_V5_XYAZ_(?:NC|NO)\]', row) for row in info):
            return []
        mask = 1
    else:
        if '[FIRMWARE:grblHAL]' not in info or f'[AXS:{count}:{"XYZABCUVW"[:count]}]' not in info:
            return []
        value = settings.get('376')
        if not isinstance(value, (float, int)) or not math.isfinite(value) or value != int(value) or not 0 <= value <= 63:
            return []
        mask = int(value)
    axes = []
    for i, axis in enumerate('ABC'[:count-3]):
        # Confirm resolution, maximum rate and acceleration exist for the drive.
        calibrated = all(isinstance(settings.get(str(n+i)), (float,int)) and
                         math.isfinite(settings[str(n+i)]) and settings[str(n+i)] > 0 for n in (103,113,123))
        if calibrated and mask & (1 << i):
            axes.append(axis)
    return axes


def validate_rotary_machine(rotary, status):
    post, drive = rotary['postprocessor'], rotary['output_axis']
    if post not in STREAM_POSTS or post != status.get('protocol'):
        raise ValueError('Rotary stream requires the selected grblHAL or independent-A xPRO V5 protocol')
    if drive not in controller_rotary_axes(post, status):
        raise ValueError('Rotary axis unavailable. Connect the matching simulator or verify independent rotary firmware, position and calibration readback.')
    if status.get('simulated'):
        return
    settings = status.get('settings', {})
    axes = [(rotary['axial_output_axis'],rotary['axial_bounds']), (rotary['radial_output_axis'],rotary['radial_bounds'])]
    if settings.get('20', 0):
        axes.append((drive, rotary['angle_bounds']))
    for axis, (lo, hi) in axes:
        travel = settings.get(str(130 + 'XYZABC'.index(axis)))
        if not isinstance(travel,(float,int)) or not math.isfinite(travel) or travel <= 0:
            raise ValueError('Missing configured travel readback for ' + axis)
        if max(hi,0) - min(lo,0) > travel + 1e-6:
            raise ValueError('Rotary job exceeds configured ' + axis + ' travel')
