"""pytest plugin: record each top-level studio.cam.generate call as a case for the native core.

Loaded by validation/native_core_parity.py with ``-p native_core_recorder``. Each recorded line holds the
project exactly as it was passed and what the Python reference did with it. Calls whose project cannot be
written as JSON without changing its meaning (tuples, non-finite numbers, non-text keys) are left out, as
are the rotary generator's internal calls. Top-level calls of studio.rotary's generate_rotary and wrap_job
are recorded the same way, with their rotary setup, and so are those of studio.relief.prepare_relief,
studio.simulation.simulate, studio.tool_changes.plan_tool_changes, studio.fixture_collision.check_fixtures and
studio.mesh_import.stl_heightmap, whose expected answers come from their Python, as the native core is compared
with; so are Studio's solid routes, its CAD script preview (studio.scad_geometry.preview) and its reader of
foreign drawings (studio.program_import.import_program).
"""

import functools
import json
import math
import os

CASES = {}
_DEPTH = [0]


def plain(value):
    """Whether JSON text carries this value to another program unchanged."""
    kind = type(value)
    if kind is str:
        # A lone surrogate is Python text but not UTF-8, which the core's JSON is.
        return value.isascii() or not any('\ud800' <= c <= '\udfff' for c in value)
    if value is None or kind in (bool, int):
        return True
    if kind is float:
        return math.isfinite(value)
    if kind is list:
        return all(plain(item) for item in value)
    if kind is dict:
        return all(type(key) is str and plain(item) for key, item in value.items())
    return False


# Members a job has beside its program, when its generator gives them: the paths and depths it cut, the holes
# and the drill of a drilling job, and the number of operations of an ordered one.
EXTRA = ('prepared_paths', 'prepared_depths', 'drill_holes', 'simulation_tool', 'operations')


def outcome(result):
    """The parts of a job the native core is compared on: all of them."""
    segments = result['segments']
    shown = {
        'lines': result['lines'],
        'segments': segments.to_list() if hasattr(segments, 'to_list') else list(segments),
        'cut_length': result['cut_length'],
        'cut_minutes': result['cut_minutes'],
        'mode': result['mode'],
        'bounds': result['bounds'],
        'warnings': result['warnings'],
    }
    shown.update((key, result[key]) for key in EXTRA if key in result)
    return shown


def reference(generate, project):
    """One case line for a project, or None when the project cannot be carried as JSON."""
    if not plain(project):
        return None, None
    text = json.dumps(project, allow_nan=False)
    try:
        result = generate(project)
    except ValueError as error:
        expect, raised = {'error': str(error)}, error
    except Exception as error:  # noqa: BLE001 - anything else is recorded by name and re-raised by the caller
        expect, raised = {'raises': type(error).__name__}, error
    else:
        expect, raised = {'ok': outcome(result)}, None
        return '{"project": %s, "expect": %s}' % (text, json.dumps(expect, allow_nan=False)), result
    return '{"project": %s, "expect": %s}' % (text, json.dumps(expect)), raised


def rotary_reference(call, subject, options, key):
    """One case line for a rotary call, or None when it cannot be carried as JSON.

    key is "project" for generate_rotary, compared on the parts outcome() names and the rotary setup it
    reports, or "job" for wrap_job, whose whole result is compared: it returns the given job's other keys.
    """
    if not plain(subject) or not plain(options):
        return None, None
    given = '"%s": %s, "rotary": %s' % (key, json.dumps(subject, allow_nan=False), json.dumps(options, allow_nan=False))
    try:
        result = call(subject, options)
    except ValueError as error:
        expect, raised = {'error': str(error)}, error
    except Exception as error:  # noqa: BLE001 - recorded by name and re-raised by the caller
        expect, raised = {'raises': type(error).__name__}, error
    else:
        shown = dict(outcome(result), rotary=result['rotary']) if key == 'project' else result
        if not plain(shown):
            return None, result
        return '{%s, "expect": %s}' % (given, json.dumps({'ok': shown}, allow_nan=False)), result
    return '{%s, "expect": %s}' % (given, json.dumps(expect)), raised


def where_computed_left_out(result):
    """A relief without what says where its contact ran: the same design gives the same surface anywhere."""
    result = dict(result, surface=dict(result['surface']))
    result.pop('device', None)
    result['surface'].pop('backend', None)
    return result


def preview_reference(kind, data):
    """One case line for a relief ("relief"), a stock preview ("stock"), a tool-change plan ("tool_changes"), a
    fixture check ("fixtures") or an STL height field ("stl") from the Python reference, with the native core's
    limits for a relief; or None when the request cannot be carried as JSON."""
    from studio.relief import prepare_relief
    from studio.simulation import simulate
    from studio.fixture_collision import check_fixtures
    from studio.mesh_import import stl_heightmap
    from studio.tool_changes import plan_tool_changes

    if not plain(data):
        return None
    given = '"%s": %s' % (kind, json.dumps(data, allow_nan=False))
    try:
        if kind == 'solid_cad':
            from studio.solid_cad import rebuild

            result = _SOLID.get('rebuild', rebuild)(data['model'], backend='python')
        elif kind == 'cad_script':
            from studio.scad_geometry import preview

            payload = {key: value for key, value in data.items() if key != 'order'}
            result = _SOLID.get('preview', preview)(payload, backend='python')
        elif kind == 'program_import':
            from studio.program_import import import_program

            name = data.get('name') if isinstance(data, dict) else None
            if isinstance(name, str) and name.lower().endswith('.dxf'):
                # DXF is read by the core's own reader, compared to the tolerance by validation/dxf_import_check.py.
                return None

            result = _SOLID.get('import', import_program)(data, backend='python')
        elif kind == 'solid_cad_section':
            from studio.solid_cad import horizontal_section

            result = _SOLID.get('section', horizontal_section)(data['model'], data.get('z'), backend='python')
        elif kind == 'relief':
            result = where_computed_left_out(prepare_relief(data, backend='python', _native_limits=True))
        elif kind == 'tool_changes':
            result = plan_tool_changes(data, backend='python')
        elif kind == 'fixtures':
            result = check_fixtures(data, backend='python')
        elif kind == 'stl':
            result = stl_heightmap(data, backend='python')
        else:
            result = simulate(data, backend='python')
    except ValueError as error:
        expect = {'error': str(error)}
    except Exception as error:  # noqa: BLE001 - recorded by name
        expect = {'raises': type(error).__name__}
    else:
        if not plain(result):
            return None
        return '{%s, "expect": %s}' % (given, json.dumps({'ok': result}, allow_nan=False))
    return '{%s, "expect": %s}' % (given, json.dumps(expect))


def _record_preview(module, name, kind):
    original = getattr(module, name)

    @functools.wraps(original)
    def recorded(data, *args, **kwargs):
        if _DEPTH[0] == 0 and not args and not kwargs and _studio_as_left_alone():
            _DEPTH[0] += 1
            try:
                line = preview_reference(kind, data)
            finally:
                _DEPTH[0] -= 1
            if line is not None:
                CASES.setdefault(line, None)
        return original(data, *args, **kwargs)

    setattr(module, name, recorded)


# The reference functions as they were before any test replaced them, so that a test watching for calls of the
# Python history does not see the recorder's own.
_SOLID = {}


def _record_solid_cad():
    """Studio's solid routes: a rebuild asked of the core (model_json), and a horizontal section."""
    import studio.solid_cad as solid_cad

    original_json, original_section = solid_cad.model_json, solid_cad.horizontal_section
    _SOLID.update(rebuild=solid_cad.rebuild, section=original_section)

    def record(kind, request):
        if _DEPTH[0] == 0 and _studio_as_left_alone():
            _DEPTH[0] += 1
            try:
                line = preview_reference(kind, request)
            finally:
                _DEPTH[0] -= 1
            if line is not None:
                CASES.setdefault(line, None)

    @functools.wraps(original_json)
    def model_json(model):
        record('solid_cad', solid_cad._request(model))
        return original_json(model)

    @functools.wraps(original_section)
    def horizontal_section(model, z, **kwargs):
        if not kwargs:
            record('solid_cad_section', solid_cad._request(model, z=z))
        return original_section(model, z, **kwargs)

    solid_cad.model_json, solid_cad.horizontal_section = model_json, horizontal_section

    import studio.scad_geometry as scad_geometry

    original_preview_json, original_preview = scad_geometry.preview_json, scad_geometry.preview
    _SOLID.update(preview=original_preview)

    @functools.wraps(original_preview_json)
    def preview_json(payload):
        if isinstance(payload, dict):
            record('cad_script', scad_geometry._request(payload))
        return original_preview_json(payload)

    @functools.wraps(original_preview)
    def preview(payload, **kwargs):
        if not kwargs and isinstance(payload, dict):
            record('cad_script', scad_geometry._request(payload))
        return original_preview(payload, **kwargs)

    scad_geometry.preview_json, scad_geometry.preview = preview_json, preview

    import studio.program_import as program_import

    original_import_json, original_import = program_import.import_json, program_import.import_program
    _SOLID.update({'import': original_import})

    @functools.wraps(original_import_json)
    def import_json(payload):
        record('program_import', payload)
        return original_import_json(payload)

    @functools.wraps(original_import)
    def import_program(payload, **kwargs):
        if not kwargs:
            record('program_import', payload)
        return original_import(payload, **kwargs)

    program_import.import_json, program_import.import_program = import_json, import_program


def _studio_as_left_alone():
    """Whether the reference runs as Studio runs it, with neither fallback library forced."""
    # A test that forces the Shapely fallback for offsets, pockets and fills is not asking the question the
    # core answers: the core has the kernel's paths, and the fallback's vertices differ. Nor is one that forces
    # the fallback library for solids and drawings: outlines taken from it are those of another kernel.
    return (
        os.environ.get('KERFKITE_POLYGON_BACKEND') != 'shapely'
        and os.environ.get('KERFKITE_SOLID_BACKEND') != 'manifold'
    )


def _record_rotary(module, name, key):
    original = getattr(module, name)

    @functools.wraps(original)
    def recorded(subject, options, *args, **kwargs):
        # As for generate: the backend keyword chooses between Python's own implementations.
        outer = _DEPTH[0] == 0 and not args and not kwargs and _studio_as_left_alone()
        _DEPTH[0] += 1
        try:
            if not outer:
                return original(subject, options, *args, **kwargs)
            line, result = rotary_reference(original, subject, options, key)
            if line is not None:
                CASES.setdefault(line, None)
            elif result is None:  # not carried as JSON, so not run yet
                return original(subject, options)
            if isinstance(result, BaseException):
                raise result
            return result
        finally:
            _DEPTH[0] -= 1

    setattr(module, name, recorded)


def pytest_configure(config):
    import studio.cam as cam

    original = cam.generate

    @functools.wraps(original)
    def recorded(project, *args, **kwargs):
        # Only calls that ask the question the native core answers: a project, in any segment form. The
        # backend keyword selects between Python's own implementations and is tested on its own.
        outer = _DEPTH[0] == 0 and not args and set(kwargs) <= {'packed_segments'} and _studio_as_left_alone()
        _DEPTH[0] += 1
        try:
            if not outer:
                return original(project, *args, **kwargs)
            # The case is recorded from the list form of the segments, which keeps the reference's number
            # types; packed segments hold every value as a double.
            line, result = reference(original, project)
            if line is not None:
                CASES.setdefault(line, None)
            if line is None or kwargs:
                return original(project, *args, **kwargs)
            if isinstance(result, BaseException):
                raise result
            return result
        finally:
            _DEPTH[0] -= 1

    cam.generate = recorded

    import studio.rotary as rotary

    _record_rotary(rotary, 'generate_rotary', 'project')
    _record_rotary(rotary, 'wrap_job', 'job')

    import studio.relief as relief
    import studio.simulation as simulation

    _record_preview(relief, 'prepare_relief', 'relief')
    _record_preview(simulation, 'simulate', 'stock')

    import studio.tool_changes as tool_changes

    _record_preview(tool_changes, 'plan_tool_changes', 'tool_changes')

    import studio.fixture_collision as fixture_collision

    _record_preview(fixture_collision, 'check_fixtures', 'fixtures')

    import studio.mesh_import as mesh_import

    _record_preview(mesh_import, 'stl_heightmap', 'stl')
    _record_solid_cad()


def pytest_sessionfinish(session):
    target = os.environ.get('KERFKITE_CORE_CASES')
    if target:
        with open(target, 'w', encoding='utf-8') as handle:
            handle.writelines(line + '\n' for line in CASES)
