"""pytest plugin: record each top-level studio.cam.generate call as a case for the native core.

Loaded by validation/native_core_parity.py with ``-p native_core_recorder``. Each recorded line holds the
project exactly as it was passed and what the Python reference did with it. Calls whose project cannot be
written as JSON without changing its meaning (tuples, non-finite numbers, non-text keys) are left out, as
are the rotary generator's internal calls.
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


def pytest_configure(config):
    import studio.cam as cam

    original = cam.generate

    @functools.wraps(original)
    def recorded(project, *args, **kwargs):
        # Only calls that ask the question the native core answers: a project, in any segment form. The
        # backend keyword selects between Python's own implementations and is tested on its own.
        outer = _DEPTH[0] == 0 and not args and set(kwargs) <= {'packed_segments'}
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


def pytest_sessionfinish(session):
    target = os.environ.get('KERFKITE_CORE_CASES')
    if target:
        with open(target, 'w', encoding='utf-8') as handle:
            handle.writelines(line + '\n' for line in CASES)
