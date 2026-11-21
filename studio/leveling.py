"""Probe levelling: follow a probed stock surface while cutting.

The work is done by the native core (native/core/src/leveling.cpp), which is the only implementation:
there is no Python version to fall back to. Without the compiled extension these functions raise ValueError.

Typical use, for an engraving such as a circuit board::

    plan = probe_plan({'bounds': job['bounds'], 'width': 300, 'height': 200, 'spacing': 10})
    # run plan['lines'] on a Grbl-family controller and collect one [PRB:...] report per point
    surface = height_map(plan, reports=reports)
    program = level(job['lines'], surface)
"""

import json

try:
    from ._relief_native import core_call as _core_call
except ImportError:  # the extension is absent, or was built before the core was added
    _core_call = None


def available():
    return _core_call is not None


def _call(command, request):
    if _core_call is None:
        raise ValueError('Probe levelling needs the compiled KerfKite extension, which is not installed')
    try:
        text = json.dumps(request, allow_nan=False)
    except (TypeError, ValueError):
        raise ValueError('The levelling request holds a value that cannot be sent to the native core') from None
    return json.loads(_core_call(command, text))


def probe_plan(request):
    """A Grbl probing program (G38.2) over the cutting bounds: lines, points, order and grid."""
    return _call('probe-plan', request)


def height_map(plan, heights=None, reports=None, reference=None, limit=None):
    """The surface as offsets from the reference point, from the Z heights or [PRB:...] reports of a plan."""
    if not isinstance(plan, dict):
        raise ValueError('The probing plan must be an object')
    request = {'grid': plan.get('grid'), 'order': plan.get('order')}
    for key, value in (('heights', heights), ('reports', reports), ('reference', reference), ('limit', limit)):
        if value is not None:
            request[key] = value
    return _call('height-map', request)


def level(lines, surface, max_segment=None):
    """The program with each cutting move split and its depth corrected to follow the surface."""
    request = {'lines': lines, 'height_map': surface}
    if max_segment is not None:
        request['max_segment'] = max_segment
    return _call('level', request)
