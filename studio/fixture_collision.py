"""Continuous clearance check for Studio's posted three-axis linear moves.

The cutter and holder are upright cylinders. A translated cylinder touches
an axis-aligned box when its Z interval overlaps the box and its centre is
within one radius of the XY rectangle, at the same instant along the move.
"""

import math
import re

from flask import jsonify, request


WORD = re.compile(r'([A-Z])([-+]?(?:\d+(?:\.\d*)?|\.\d+))')
MAX_EVENTS = 100
MAX_CHECKS = 2_000_000


def _number(value, field, low=-1_000_000, high=1_000_000):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not low <= value <= high:
        raise ValueError(f'{field} must be a finite number from {low} to {high}')
    return float(value)


def _vector(value, field):
    if not isinstance(value, list) or len(value) != 3:
        raise ValueError(f'{field} needs three coordinates in millimetres')
    return tuple(_number(v, field) for v in value)


def _slab(interval, origin, delta, low, high):
    """Intersect a time interval with low <= origin + delta*t <= high."""
    if delta == 0:
        return interval if low <= origin <= high else None
    a, b = (low - origin) / delta, (high - origin) / delta
    entry, leave = max(interval[0], min(a, b)), min(interval[1], max(a, b))
    return (entry, leave) if entry <= leave else None


def _contact(start, delta, box_min, box_max, radius, bottom, top):
    interval = _slab((0.0, 1.0), start[2], delta[2], box_min[2] - top, box_max[2] - bottom)
    if interval is None:
        return None
    # Reject XY travel outside the bounding rectangle of the rounded footprint.
    for axis in (0, 1):
        interval = _slab(interval, start[axis], delta[axis], box_min[axis] - radius, box_max[axis] + radius)
        if interval is None:
            return None
    contacts = []
    # The exact XY footprint is two rectangular strips plus four corner discs.
    for axis in (0, 1):
        strip = _slab(interval, start[axis], delta[axis], box_min[axis], box_max[axis])
        if strip is not None:
            contacts.append(strip[0])
    dx, dy = delta[:2]
    speed = math.hypot(dx, dy)
    for x in (box_min[0], box_max[0]):
        for y in (box_min[1], box_max[1]):
            ox, oy = start[0] - x, start[1] - y
            if speed == 0:
                if math.hypot(ox, oy) <= radius:
                    contacts.append(interval[0])
                continue
            # Project onto a unit vector to avoid subtracting large squared lengths.
            ux, uy = dx / speed, dy / speed
            lateral = abs(oy * ux - ox * uy)
            if lateral > radius:
                continue
            half = math.sqrt((radius - lateral) * (radius + lateral))
            centre = -(ox * ux + oy * uy)
            entry = max(interval[0], (centre - half) / speed)
            if entry <= min(interval[1], (centre + half) / speed):
                contacts.append(entry)
    return min(contacts, default=None)


def _moves(lines, start):
    """Accept the explicit commands Studio posts; reject unknown modal state."""
    if not isinstance(lines, list) or not 1 <= len(lines) <= 250_000:
        raise ValueError('Check a generated program with 1–250000 lines')
    position = start
    metric = absolute = ended = False
    for line_no, raw in enumerate(lines, 1):

        def reject(reason):
            raise ValueError(f'Cannot check line {line_no}: {reason}')

        if not isinstance(raw, str) or len(raw) > 256:
            reject('invalid line')
        words = raw.strip().upper().split()
        if not words:
            continue
        if ended:
            reject('instructions follow program end')
        op = words[0]
        if op == 'G21' and len(words) == 1:
            metric = True
            continue
        if op == 'G90' and len(words) == 1:
            absolute = True
            continue
        if op in ('G17', 'G94', 'G54', 'G40', 'G49', 'G80', 'G61', 'M5', 'M30') and len(words) == 1:
            ended = op == 'M30'
            continue
        allowed = {'G0': 'XYZ', 'G1': 'XYZFS', 'G4': 'P', 'M3': 'S', 'M4': 'S'}.get(op)
        if allowed is None:
            reject(f'unsupported instruction {op}')
        values = {}
        for word in words[1:]:
            match = WORD.fullmatch(word)
            if match is None or match[1] not in allowed or match[1] in values:
                reject(f'unsupported or duplicate word {word}')
            value = float(match[2])
            if not math.isfinite(value) or abs(value) > 1_000_000:
                reject('value outside supported range')
            values[match[1]] = value
        if op not in ('G0', 'G1'):
            if op == 'G4' and 'P' not in values:
                reject('missing dwell interval')
            continue
        if not metric or not absolute:
            reject('expected G21 millimetres and G90 absolute positioning before motion')
        if not any(axis in values for axis in 'XYZ'):
            reject('missing XYZ coordinates')
        destination = tuple(values.get(axis, current) for axis, current in zip('XYZ', position))
        yield line_no, op, position, destination
        position = destination


def check_fixtures(payload):
    if not isinstance(payload, dict):
        raise ValueError('Supply a posted program and fixture setup')
    start = _vector(payload.get('start'), 'Initial tool-tip XYZ')
    tool = payload.get('tool')
    if not isinstance(tool, dict):
        raise ValueError('Supply the mounted tool and holder dimensions')
    diameter, stickout, holder_diameter, length = (
        _number(tool.get(key), key, 0.001, 2000) for key in ('diameter', 'stickout', 'holder_diameter', 'holder_length')
    )
    components = [('tool', diameter / 2, 0, stickout), ('holder', holder_diameter / 2, stickout, stickout + length)]
    fixtures = payload.get('fixtures')
    if not isinstance(fixtures, list) or not 1 <= len(fixtures) <= 64:
        raise ValueError('Supply between 1 and 64 fixture boxes')
    boxes = []
    for fixture in fixtures:
        if not isinstance(fixture, dict):
            raise ValueError('Invalid fixture box')
        name = fixture.get('name')
        if not isinstance(name, str) or not name.strip() or len(name) > 80:
            raise ValueError('Give each fixture a name of 1–80 characters')
        low = _vector(fixture.get('min'), f'{name} minimum corner')
        high = _vector(fixture.get('max'), f'{name} maximum corner')
        if any(a >= b for a, b in zip(low, high)):
            raise ValueError(f'{name} requires a positive size on every axis')
        boxes.append((name, low, high))

    events = []
    count = moves = 0
    for line, motion, origin, destination in _moves(payload.get('lines'), start):
        moves += 1
        if moves * len(boxes) * len(components) > MAX_CHECKS:
            raise ValueError('Program/fixture combination exceeds two million sweep checks')
        delta = tuple(b - a for a, b in zip(origin, destination))
        hits = []
        for index, (name, low, high) in enumerate(boxes):
            for component, radius, bottom, top in components:
                t = _contact(origin, delta, low, high, radius, bottom, top)
                if t is not None:
                    count += 1
                    if len(events) < MAX_EVENTS:
                        point = [round(p + t * d, 5) for p, d in zip(origin, delta)]
                        hits.append(
                            (
                                t,
                                index,
                                component,
                                {
                                    'line': line,
                                    'motion': motion,
                                    'component': component,
                                    'fixture': name,
                                    'position': point,
                                },
                            )
                        )
        events.extend(hit[3] for hit in sorted(hits)[: MAX_EVENTS - len(events)])
    if not moves:
        raise ValueError('Program contains no supported XYZ motion')
    return {
        'collision': bool(count),
        'events': events,
        'contact_count': count,
        'moves_checked': moves,
        'fixtures_checked': len(boxes),
    }


def register(app):
    @app.post('/api/fixture-collision')
    def fixture_collision_check():
        return jsonify(check_fixtures(request.get_json()))
