"""Circuit board import: Gerber layers, Excellon drill files and isolation toolpaths.

The work is done by the native core (native/core/src/gerber.cpp, excellon.cpp and isolation.cpp), which is the
only implementation: there is no Python version to fall back to. Without the compiled extension these functions
raise ValueError. Everything returned is in millimetres. See docs/PCB.md.

Importing only reads the files. Nothing here writes to a controller or starts a job.

Typical use::

    outline = outline_paths(read('board-Edge_Cuts.gbr'))
    offset = placement([outline['bounds']], margin=5)          # bring the board to the work origin
    outline = outline_paths(read('board-Edge_Cuts.gbr'), offset=offset)
    copper = isolation(read('board-F_Cu.gbr'), tool_width=v_bit_width(0.1, 30, 0.1), offset=offset)
    for operation in copper['operations']:                     # engrave: one tool, so one ordered job
        job = generate({**settings, 'depth': 0.1, **operation})
    drills = drill_operations(read('board.drl'), offset=offset)
    for operation in drills['operations']:                     # one job for each drill diameter
        job = generate({**settings, **operation})
"""

import json
import math
import os

try:
    from ._relief_native import core_call as _core_call
except ImportError:  # the extension is absent, or was built before the core was added
    _core_call = None

GERBER_LIMIT = 16 << 20  # bytes, as the core enforces
EXCELLON_LIMIT = 8 << 20


def available():
    """Whether the compiled extension carries the board importers."""
    if _core_call is None:
        return False
    try:
        _core_call('gerber', '{}')
    except ValueError as error:
        return 'Unknown command' not in str(error)  # an extension built before the importers were added
    return True


def read(path, limit=GERBER_LIMIT):
    """The text of a board file. Bytes that are not UTF-8 can only be in comments, and are replaced."""
    if os.path.getsize(path) > limit:
        raise ValueError(f'The file is larger than {limit >> 20} MiB')
    with open(path, 'rb') as file:
        return file.read(limit + 1).decode('utf-8', errors='replace')


def _call(command, text, options):
    if _core_call is None:
        raise ValueError('Circuit board import needs the compiled KerfKite extension, which is not installed')
    if isinstance(text, (bytes, bytearray)):
        text = bytes(text).decode('utf-8', errors='replace')
    if not isinstance(text, str):
        raise ValueError('Give the board file as text')
    request = {'text': text, **{key: value for key, value in options.items() if value is not None}}
    try:
        body = json.dumps(request, allow_nan=False)
    except (TypeError, ValueError):
        raise ValueError('The import request holds a value that cannot be sent to the native core') from None
    return json.loads(_core_call(command, body))


def gerber(text, arc_tolerance=None):
    """A Gerber RS-274X or X2 layer: apertures, the objects drawn in order, bounds and warnings."""
    return _call('gerber', text, {'arc_tolerance': arc_tolerance})


def excellon(text, units=None, integer=None, decimal=None, omitted_zeros=None):
    """An Excellon drill file: tools with their holes, slots, bounds, and how the numbers were read.

    units ('mm' or 'inch'), integer and decimal (digits either side of the implied point) and omitted_zeros
    ('leading' or 'trailing') state the number format outright when the file does not, or states it wrongly.
    """
    return _call(
        'excellon', text, {'units': units, 'integer': integer, 'decimal': decimal, 'omitted_zeros': omitted_zeros}
    )


_isolation_asked = None  # (the extension's entry point, its answer)


def isolation_available():
    """Whether the compiled extension carries isolation toolpaths (an older build may have only the importers)."""
    global _isolation_asked
    # The session asks this on every request, and an extension does not change while it is loaded: the core
    # is asked once for each entry point, not once a request.
    if _isolation_asked is None or _isolation_asked[0] is not _core_call:
        _isolation_asked = (_core_call, _asks_isolation())
    return _isolation_asked[1]


def _asks_isolation():
    if _core_call is None:
        return False
    try:
        _core_call('gerber-isolation', '{}')
    except ValueError as error:
        return 'Unknown command' not in str(error)
    return True


def v_bit_width(tip_width, angle, depth):
    """The width a V-bit cuts at a depth: its flat tip plus what the cone adds on both sides.

    tip_width and depth are in millimetres and angle is the included angle of the cone in degrees.
    """
    values = (tip_width, angle, depth)
    if not all(isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) for v in values):
        raise ValueError('The tip width, angle and depth of a V-bit must be numbers')
    if not (0 <= tip_width <= 10 and 1 <= angle <= 179 and 0 < depth <= 10):
        raise ValueError('A V-bit needs a tip width of 0 to 10 mm, an angle of 1 to 179 degrees and a depth above 0')
    return round(tip_width + 2 * depth * math.tan(math.radians(angle) / 2), 6)


def isolation(
    text,
    tool_width,
    passes=None,
    overlap=None,
    direction=None,
    order=None,
    offset=None,
    mirror_x=None,
    clip=None,
    width=None,
    height=None,
    arc_tolerance=None,
):
    """Isolation toolpaths round the copper of a Gerber layer, as line operations for studio.cam.generate.

    tool_width is the width of cut at the engraving depth (v_bit_width gives it for a V-bit). Contour k lies
    tool_width / 2 + k * tool_width * (1 - overlap) outside the copper. mirror_x turns the layer over about
    that X for the underside of a board; clip is a list of closed paths (the board outline) outside which
    nothing is cut. 'unreachable' lists the places where copper features are nearer than the tool is wide and
    stay joined; a warning names the smallest gap. See docs/PCB.md.
    """
    options = {
        'tool_width': tool_width,
        'passes': passes,
        'overlap': overlap,
        'direction': direction,
        'order': order,
        'offset': offset,
        'mirror_x': mirror_x,
        'clip': clip,
        'width': width,
        'height': height,
        'arc_tolerance': arc_tolerance,
    }
    return _call('gerber-isolation', text, options)


def isolation_paths(answer):
    """Every contour of an isolation answer, in cutting order."""
    return [path for operation in answer['operations'] for path in operation['paths']]


def drill_operations(
    text,
    offset=None,
    width=None,
    height=None,
    units=None,
    integer=None,
    decimal=None,
    omitted_zeros=None,
    mirror_x=None,
):
    """The holes of a drill file as drilling operations for studio.cam.generate, one group for each diameter.

    Each entry of 'operations' is a project fragment: merge it over the job settings and generate it.
    Operations of one diameter may share an ordered job; each diameter is a separate job. Slots are listed
    under 'slots' and are not drilled.
    """
    options = {'offset': offset, 'width': width, 'height': height, 'mirror_x': mirror_x}
    options.update(units=units, integer=integer, decimal=decimal, omitted_zeros=omitted_zeros)
    return _call('excellon-drills', text, options)


def outline_paths(text, offset=None, tolerance=None, arc_tolerance=None, width=None, height=None, mirror_x=None):
    """The board outline of a profile (Edge.Cuts) layer as closed 'paths' for a cut-out line job.

    Lines and arcs are joined end to end where their ends lie within tolerance (0.01 mm). An outline that
    is open, or where more than two lines meet, is refused with the place.
    """
    options = {
        'offset': offset,
        'tolerance': tolerance,
        'arc_tolerance': arc_tolerance,
        'width': width,
        'height': height,
        'mirror_x': mirror_x,
    }
    return _call('gerber-outline', text, options)


def placement(bounds, margin=0.0):
    """The offset that brings the lowest corner of several bounds to (margin, margin).

    Use one offset for every layer of a board, so that the holes stay registered with the outline.
    """
    boxes = [box for box in bounds if box]
    if not boxes:
        raise ValueError('There is nothing to place')
    return [margin - min(box[0] for box in boxes), margin - min(box[1] for box in boxes)]
