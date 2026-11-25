"""The native core for the Python tests, through `kerfkite-cam`.

The board and levelling tests were written against the Python reference's thin modules `studio.leveling` and
`studio.pcb_import`, which handed each request to the core through the compiled extension. `leveling` and
`pcb_import` here give the same calls the same names, and hand each request to `kerfkite-cam`, so the tests
check the program a package carries. `generate` is the planar CAM (`kerfkite-cam generate --json`).

A request the core refuses (exit status 2) raises ValueError with the core's message, as the extension did.

The tool is taken from KERFKITE_CAM, or from build/native as the build instructions leave it; `CLI` is None
without it, and the tests are skipped.
"""

import json
import os
import subprocess
import tempfile
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]

GERBER_LIMIT = 16 << 20  # bytes, as the core enforces
EXCELLON_LIMIT = 8 << 20


def find_cli():
    named = os.environ.get('KERFKITE_CAM')
    places = [Path(named)] if named else []
    places += [ROOT / 'build/native/kerfkite-cam', ROOT / 'build/native/Release/kerfkite-cam.exe']
    return next((str(place) for place in places if place.is_file()), None)


CLI = find_cli()


def _answer(done):
    if done.returncode == 2:
        raise ValueError(done.stderr.strip())
    if done.returncode != 0:
        raise RuntimeError(f'kerfkite-cam ended with status {done.returncode}: {done.stderr.strip()}')
    return json.loads(done.stdout)


def _body(request, what):
    try:
        return json.dumps(request, allow_nan=False)
    except (TypeError, ValueError):
        raise ValueError(f'The {what} request holds a value that cannot be sent to the native core') from None


def _run(arguments, text):
    return _answer(subprocess.run([CLI, *arguments], input=text, capture_output=True, text=True, encoding='utf-8'))


def call(command, request, what='levelling'):
    """A command that takes a JSON request on standard input."""
    return _run([command, '-'], _body(request, what))


def generate(project):
    """The whole job of a project, as the reference's studio.cam.generate returned it."""
    return _run(['generate', '-', '--json'], _body(project, 'CAM'))


# --- studio.leveling -------------------------------------------------------------------------------------


def probe_plan(request):
    """A Grbl probing program (G38.2) over the cutting bounds: lines, points, order and grid."""
    return call('probe-plan', request)


def height_map(plan, heights=None, reports=None, reference=None, limit=None):
    """The surface as offsets from the reference point, from the Z heights or [PRB:...] reports of a plan."""
    if not isinstance(plan, dict):
        raise ValueError('The probing plan must be an object')
    request = {'grid': plan.get('grid'), 'order': plan.get('order')}
    for key, value in (('heights', heights), ('reports', reports), ('reference', reference), ('limit', limit)):
        if value is not None:
            request[key] = value
    return call('height-map', request)


def level(lines, surface, max_segment=None):
    """The program with each cutting move split and its depth corrected to follow the surface."""
    request = {'lines': lines, 'height_map': surface}
    if max_segment is not None:
        request['max_segment'] = max_segment
    return call('level', request)


leveling = SimpleNamespace(probe_plan=probe_plan, height_map=height_map, level=level)

# --- studio.pcb_import -----------------------------------------------------------------------------------


def read(path, limit=GERBER_LIMIT):
    """The text of a board file. Bytes that are not UTF-8 can only be in comments, and are replaced."""
    if os.path.getsize(path) > limit:
        raise ValueError(f'The file is larger than {limit >> 20} MiB')
    with open(path, 'rb') as file:
        return file.read(limit + 1).decode('utf-8', errors='replace')


def board(command, text, options):
    """A board command: the file is the input and each option that is not None a --option VALUE pair."""
    if isinstance(text, (bytes, bytearray)):
        text = bytes(text).decode('utf-8', errors='replace')
    if not isinstance(text, str):
        raise ValueError('Give the board file as text')
    arguments = []
    for key, value in options.items():
        if value is not None:
            arguments += ['--' + key.replace('_', '-'), _body(value, 'import')]
    with tempfile.TemporaryDirectory() as folder:
        source = Path(folder) / 'board'
        source.write_bytes(text.encode('utf-8'))
        return _answer(
            subprocess.run([CLI, command, str(source), *arguments], capture_output=True, text=True, encoding='utf-8')
        )


def gerber(text, arc_tolerance=None):
    """A Gerber RS-274X or X2 layer: apertures, the objects drawn in order, bounds and warnings."""
    return board('gerber', text, {'arc_tolerance': arc_tolerance})


def excellon(text, units=None, integer=None, decimal=None, omitted_zeros=None):
    """An Excellon drill file: tools with their holes, slots, bounds, and how the numbers were read."""
    return board(
        'excellon', text, {'units': units, 'integer': integer, 'decimal': decimal, 'omitted_zeros': omitted_zeros}
    )


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
    """Isolation toolpaths round the copper of a Gerber layer, as line operations for generate."""
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
    return board('gerber-isolation', text, options)


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
    """The holes of a drill file as drilling operations for generate, one group for each diameter."""
    options = {'offset': offset, 'width': width, 'height': height, 'mirror_x': mirror_x}
    options.update(units=units, integer=integer, decimal=decimal, omitted_zeros=omitted_zeros)
    return board('excellon-drills', text, options)


def outline_paths(text, offset=None, tolerance=None, arc_tolerance=None, width=None, height=None, mirror_x=None):
    """The board outline of a profile (Edge.Cuts) layer as closed 'paths' for a cut-out line job."""
    options = {
        'offset': offset,
        'tolerance': tolerance,
        'arc_tolerance': arc_tolerance,
        'width': width,
        'height': height,
        'mirror_x': mirror_x,
    }
    return board('gerber-outline', text, options)


def placement(bounds, margin=0.0):
    """The offset that brings the lowest corner of several bounds to (margin, margin)."""
    boxes = [box for box in bounds if box]
    if not boxes:
        raise ValueError('There is nothing to place')
    return [margin - min(box[0] for box in boxes), margin - min(box[1] for box in boxes)]


pcb_import = SimpleNamespace(
    read=read,
    board=board,
    gerber=gerber,
    excellon=excellon,
    isolation=isolation,
    isolation_paths=isolation_paths,
    drill_operations=drill_operations,
    outline_paths=outline_paths,
    placement=placement,
)
