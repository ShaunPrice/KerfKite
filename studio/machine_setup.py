"""Describe a connected machine from what its controller reports, so the workspace can be set to match.

Nothing here writes to a controller. The description is built from the settings and identification the
drivers read at connection: `$$` and `$I` on Grbl-family firmware, M115, M211 and M503 on Marlin.
"""

import math
import re

from .protocols import get_protocol

# Studio's own limits for a workspace and its feeds.
TRAVEL_LIMIT = 2000
FEED_LIMIT = 10000


def _positive(value):
    try:
        value = float(value)
    except (TypeError, ValueError):
        return None
    return value if math.isfinite(value) and value > 0 else None


def _text(value):
    return f'{value:g}'


def _grbl(status, out):
    settings = status.get('settings') or {}
    setting = lambda key: _positive(settings.get(key))
    flag = lambda key: None if key not in settings else float(settings[key]) != 0
    lines = [str(line) for line in status.get('identification') or []]
    banner = str(status.get('banner') or '')
    version = next(
        (re.match(r'\[VER:([^:\]]*)', line).group(1).strip() for line in lines if line.startswith('[VER:')), ''
    )
    if '[FIRMWARE:grblHAL]' in lines:
        out['firmware'], detected = ('grblHAL ' + version).strip(), 'grblhal'
    elif banner.startswith('FluidNC') or 'FluidNC' in version:
        out['firmware'], detected = (version if 'FluidNC' in version else banner.split('[')[0].strip()), 'fluidnc'
    elif any(line.startswith('[MSG:Using machine:CNC_xPRO_V5') for line in lines):
        out['firmware'], detected = ('Grbl_Esp32 ' + version).strip() + ' · xPRO V5', 'xpro-v5'
    elif version or banner.startswith('Grbl'):
        out['firmware'], detected = 'Grbl ' + (version or banner.split()[1]), 'grbl'
    else:
        detected = None
    out['detected_protocol'] = detected
    out['travel'] = {axis: setting(key) for axis, key in zip('xyz', ('130', '131', '132'), strict=True)}
    out['max_feed'] = {axis: setting(key) for axis, key in zip('xyz', ('110', '111', '112'), strict=True)}
    out['max_power'] = setting('30')
    if status.get('protocol') == 'xpro-v5' and flag('32') and setting('Laser/FullPower'):
        out['max_power'] = setting('Laser/FullPower')
    out['laser_mode'] = flag('32')
    out['soft_limits'], out['hard_limits'], out['homing'] = flag('20'), flag('21'), flag('22')
    out['axes'] = status.get('position_count') or None
    if flag('13'):
        out['warnings'].append('The controller reports positions in inches ($13=1); Studio works in millimetres')


def _marlin(status, out):
    readback = status.get('readback') or {}
    if readback.get('firmware'):
        out['firmware'] = readback['firmware'] + (
            ' · ' + readback['machine_type'] if readback.get('machine_type') else ''
        )
    out['detected_protocol'] = 'marlin' if 'marlin' in str(readback.get('firmware', '')).lower() else None
    for key in ('travel', 'max_feed'):
        values = readback.get(key) or [None] * 3
        out[key] = {axis: _positive(value) for axis, value in zip('xyz', values, strict=True)}
    out['soft_limits'] = readback.get('soft_endstops')
    if out['soft_limits'] is False and readback.get('travel'):
        out['warnings'].append(
            'Soft endstops are off; the travel reported is the configured bed size, not an enforced limit'
        )


def describe(status):
    """What the connected controller says about its machine, and the workspace settings that follow."""
    out = dict(
        available=False, reason=None, waiting=False, identity=None, name=None, firmware=None, protocol=None,
        detected_protocol=None, travel=dict(x=None, y=None, z=None), max_feed=dict(x=None, y=None, z=None),
        max_power=None, laser_mode=None, soft_limits=None, hard_limits=None, homing=None, axes=None,
        environment={}, reported=[], missing=[], warnings=[],
    )  # fmt: skip
    if not status.get('connected'):
        out['reason'] = 'Connect a controller to read its machine settings'
        return out
    if status.get('simulated'):
        out['reason'] = 'The simulator has no machine settings; the workspace is left as it is'
        return out
    if status.get('error'):
        out['reason'] = 'The controller reported a fault; resolve it and reconnect'
        return out
    if not status.get('ready'):
        out['reason'], out['waiting'] = 'Waiting for the controller to report its settings', True
        return out
    protocol = status.get('protocol', 'grbl')
    profile = get_protocol(protocol)
    out['protocol'] = protocol
    (_grbl if profile.family == 'grbl' else _marlin)(status, out)
    if out['detected_protocol'] and out['detected_protocol'] != protocol:
        out['warnings'].append(
            f'The controller identifies as {get_protocol(out["detected_protocol"]).name}, but the connection uses '
            f'{profile.name}; reconnect with the matching dialect'
        )

    environment, reported, missing = {'protocol': protocol}, out['reported'], out['missing']
    travel, feed = out['travel'], out['max_feed']
    for axis, field in (('x', 'width'), ('y', 'height')):
        if travel[axis] is None:
            missing.append(f'{axis.upper()} travel')
        elif travel[axis] > TRAVEL_LIMIT:
            out['warnings'].append(
                f'{axis.upper()} travel of {_text(travel[axis])} mm is beyond the {TRAVEL_LIMIT} mm a workspace allows'
            )
        else:
            environment[field] = travel[axis]
            reported.append(f'{axis.upper()} travel {_text(travel[axis])} mm')
    if travel['z'] is None:
        missing.append('Z travel')
    elif travel['z'] > TRAVEL_LIMIT:
        out['warnings'].append(
            f'Z travel of {_text(travel["z"])} mm is beyond the {TRAVEL_LIMIT} mm a machine profile allows'
        )
    else:
        environment['z_travel'] = travel['z']
        reported.append(f'Z travel {_text(travel["z"])} mm')
    planar = [feed[axis] for axis in 'xy' if feed[axis] is not None]
    if len(planar) == 2:
        environment['feed_max'] = min(*planar, FEED_LIMIT)
        reported.append(f'feed up to {_text(environment["feed_max"])} mm/min')
    else:
        missing.append('maximum feed')
    if feed['z'] is not None:
        environment['plunge_max'] = min(feed['z'], FEED_LIMIT)
        reported.append(f'plunge up to {_text(environment["plunge_max"])} mm/min')
    if out['max_power'] is not None:
        environment['power_max'] = out['max_power']
        reported.append(f'spindle speed or laser power up to {_text(out["max_power"])}')
    elif profile.family == 'grbl':
        missing.append('maximum spindle speed or laser power')
    if out['laser_mode'] is not None:
        environment['laser_mode'] = out['laser_mode']
        reported.append('laser mode on' if out['laser_mode'] else 'laser mode off')
    if out['homing'] is not None:
        reported.append('homing enabled' if out['homing'] else 'homing disabled')
    if out['soft_limits'] is not None:
        reported.append('soft limits on' if out['soft_limits'] else 'soft limits off')
    if 'width' in environment and 'height' in environment:
        size = f'{_text(environment["width"])} × {_text(environment["height"])} mm'
        out['name'] = ((out['firmware'] or profile.name) + ' · ' + size)[:80]
        out['identity'] = '|'.join(
            [protocol, out['firmware'] or '', *(_text(travel[axis]) if travel[axis] else '' for axis in 'xyz')]
        )[:120]
    out['environment'] = environment
    out['available'] = True
    return out
