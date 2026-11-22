"""pytest plugin: record each top-level studio.cam.generate call as a case for the native core.

Loaded by validation/native_core_parity.py with ``-p native_core_recorder``. Each recorded line holds the
project exactly as it was passed and what the Python reference did with it. Calls whose project cannot be
written as JSON without changing its meaning (tuples, non-finite numbers, non-text keys) are left out, as
are the rotary generator's internal calls. Top-level calls of studio.rotary's generate_rotary and wrap_job
are recorded the same way, with their rotary setup.
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
    if value is None or kind in (bool, str, int):
        return True
    if kind is float:
        return math.isfinite(value)
    if kind is list:
        return all(plain(item) for item in value)
    if kind is dict:
        return all(type(key) is str and plain(item) for key, item in value.items())
    return False


def outcome(result):
    """The parts of a job the native core is compared on."""
    segments = result['segments']
    return {
        'lines': result['lines'],
        'segments': segments.to_list() if hasattr(segments, 'to_list') else list(segments),
        'cut_length': result['cut_length'],
        'cut_minutes': result['cut_minutes'],
        'mode': result['mode'],
        'bounds': result['bounds'],
        'warnings': result['warnings'],
    }


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


def pytest_sessionfinish(session):
    target = os.environ.get('KERFKITE_CORE_CASES')
    if target:
        with open(target, 'w', encoding='utf-8') as handle:
            handle.writelines(line + '\n' for line in CASES)
