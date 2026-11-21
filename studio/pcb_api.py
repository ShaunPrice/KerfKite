"""Circuit board import for the editor: a copper layer, an outline and a drill file in, reviewable geometry out.

One design route, registered for the local and the hosted service alike. It reads the files it is sent and
answers with what they hold: it has no controller, writes nothing to a machine and starts no job. The geometry
comes from the native core through studio.pcb_import; nothing is computed here but where the board is placed.
See docs/PCB.md.
"""

import math

from flask import jsonify, request

from . import pcb_import

FILE_LIMIT = 4 << 20  # characters in each file: far beyond a board a desktop machine engraves
FILES = {'copper': 'copper layer', 'outline': 'board outline', 'drill': 'drill file'}
FIELDS = {
    *FILES,
    'side',
    'offset',
    'margin',
    'mirror_x',
    'width',
    'height',
    'tool_width',
    'v_bit',
    'passes',
    'overlap',
    'direction',
    'order',
    'arc_tolerance',
    'clip',
    'drill_format',
}
DRILL_FORMAT = ('units', 'integer', 'decimal', 'omitted_zeros')


def _number(value, name, low, high):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f'{name} must be a number')
    if not low <= value <= high:
        raise ValueError(f'{name} must be between {low} and {high}')
    return float(value)


def _pair(value, name):
    if not isinstance(value, list) or len(value) != 2:
        raise ValueError(f'Give the {name} as [X, Y]')
    return [
        _number(value[0], f'{name.capitalize()} X', -10000, 10000),
        _number(value[1], f'{name.capitalize()} Y', -10000, 10000),
    ]


def _cover(boxes):
    boxes = [box for box in boxes if box]
    if not boxes:
        return None
    return [
        min(box[0] for box in boxes),
        min(box[1] for box in boxes),
        max(box[2] for box in boxes),
        max(box[3] for box in boxes),
    ]


def _span(box):
    return f'X{box[0]:.3f} to X{box[2]:.3f} and Y{box[1]:.3f} to Y{box[3]:.3f}'


def board(payload):
    """What the files of one side of a board hold, placed on the work area: see docs/PCB.md.

    payload: 'copper', 'outline' and 'drill' are the texts of the files, any of them absent. 'side' is 'top'
    or 'bottom'; the underside is mirrored about 'mirror_x', by default the middle of the placed outline.
    'offset' places the board, by default so that the outline's lowest corner is 'margin' (5 mm) from the
    work origin. 'tool_width', or 'v_bit' {tip_width, angle, depth}, with 'passes', 'overlap', 'direction',
    'order' and 'arc_tolerance' set the isolation; 'clip' cuts it to the outline. 'drill_format' states the
    drill file's units, integer and decimal digits and omitted_zeros where the file does not.
    """
    if not isinstance(payload, dict):
        raise ValueError('Send the board files as a JSON object')
    unknown = sorted(set(payload) - FIELDS)
    if unknown:
        raise ValueError('The board import has a field that is not known: ' + str(unknown[0])[:60])
    if not pcb_import.isolation_available():
        raise ValueError('Circuit board import needs the compiled KerfKite extension, which is not installed')
    texts = {}
    for key, name in FILES.items():
        text = payload.get(key)
        if text is None or text == '':
            continue
        if not isinstance(text, str):
            raise ValueError(f'Send the {name} as text')
        if len(text) > FILE_LIMIT:
            raise ValueError(f'The {name} is larger than {FILE_LIMIT >> 20} MiB')
        texts[key] = text
    if not texts:
        raise ValueError('Choose a copper layer, a board outline or a drill file')
    side = payload.get('side', 'top')
    if side not in ('top', 'bottom'):
        raise ValueError('The side must be "top" or "bottom"')
    drill_format = payload.get('drill_format') or {}
    if not isinstance(drill_format, dict) or set(drill_format) - set(DRILL_FORMAT):
        raise ValueError('The drill format may state units, integer, decimal and omitted_zeros')
    drill_format = {key: value for key, value in drill_format.items() if value not in (None, '')}
    work = {}
    if payload.get('width') is not None or payload.get('height') is not None:
        work = {'width': payload.get('width'), 'height': payload.get('height')}  # the core checks the pair
    arc_tolerance = payload.get('arc_tolerance')

    # The tool: its width of cut at the engraving depth, stated or worked out for a V-bit.
    tool_width = payload.get('tool_width')
    v_bit = payload.get('v_bit')
    if 'copper' in texts:
        if v_bit is not None:
            if tool_width is not None:
                raise ValueError('Give the tool width or the V-bit, not both')
            if not isinstance(v_bit, dict) or set(v_bit) != {'tip_width', 'angle', 'depth'}:
                raise ValueError('Give the V-bit as its tip_width, angle and depth')
            tool_width = pcb_import.v_bit_width(v_bit['tip_width'], v_bit['angle'], v_bit['depth'])
        if tool_width is None:
            raise ValueError('Give the tool width, or the V-bit, for the isolation of the copper layer')

    # Where each file's content lies before it is placed. This reads each file once more than the conversions
    # below need, which costs little and keeps the placement here, in one place.
    found = {}
    first = {}
    if 'outline' in texts:
        first['outline'] = pcb_import.outline_paths(texts['outline'], arc_tolerance=arc_tolerance)
        found['outline'] = first['outline']['bounds']
    if 'copper' in texts:
        first['copper'] = pcb_import.gerber(texts['copper'], arc_tolerance=arc_tolerance)
        found['copper'] = first['copper']['bounds']
    if 'drill' in texts:
        first['drill'] = pcb_import.excellon(texts['drill'], **drill_format)
        found['drill'] = first['drill']['bounds']
    # The outline places the board when there is one: it is the same for both sides.
    reference = found.get('outline') or _cover(found.values())
    if reference is None:
        raise ValueError('The files draw nothing to place')

    if payload.get('offset') is not None:
        offset = _pair(payload['offset'], 'offset')
    else:
        margin = _number(payload.get('margin', 5), 'Margin', 0, 1000)
        offset = [round(margin - reference[0], 6), round(margin - reference[1], 6)]
    mirror_x = None
    if side == 'bottom':
        if payload.get('mirror_x') is not None:
            mirror_x = _number(payload['mirror_x'], 'Mirror X', -10000, 10000)
        elif 'outline' in texts:
            mirror_x = round((reference[0] + reference[2]) / 2 + offset[0], 6)
        else:
            raise ValueError(
                'The underside needs the board outline, or the mirror line used for the rest of the board, '
                'so that both sides register'
            )
    elif payload.get('mirror_x') is not None:
        raise ValueError('Only the underside is mirrored: choose the bottom side to give a mirror line')
    placed = {'offset': offset, 'mirror_x': mirror_x, **work}

    answer = {'side': side, 'offset': offset, 'mirror_x': mirror_x, 'copper': None, 'outline': None, 'drill': None}
    warnings = []
    if 'outline' in texts:
        answer['outline'] = pcb_import.outline_paths(texts['outline'], arc_tolerance=arc_tolerance, **placed)
    if 'copper' in texts:
        clip = None
        if payload.get('clip'):
            if answer['outline'] is None:
                raise ValueError('Cutting the isolation to the outline needs the board outline')
            clip = answer['outline']['paths']
        answer['copper'] = pcb_import.isolation(
            texts['copper'],
            tool_width,
            passes=payload.get('passes'),
            overlap=payload.get('overlap'),
            direction=payload.get('direction'),
            order=payload.get('order'),
            arc_tolerance=arc_tolerance,
            clip=clip,
            **placed,
        )
    if 'drill' in texts:
        drill = pcb_import.drill_operations(texts['drill'], **placed, **drill_format)
        drill['plated'] = first['drill']['plated']
        drill['file_function'] = first['drill']['file_function']
        drill['counts'] = first['drill']['counts']
        answer['drill'] = drill

    # Layers that do not lie on one board are the commonest sign of a misread file.
    outline_box = answer['outline'] and answer['outline']['bounds']
    if outline_box and answer['drill']:
        holes = answer['drill']['bounds']
        if (
            holes[0] < outline_box[0]
            or holes[1] < outline_box[1]
            or holes[2] > outline_box[2]
            or holes[3] > outline_box[3]
        ):
            warnings.append(
                f'Holes lie outside the board outline: they span {_span(holes)} and the board spans '
                f'{_span(outline_box)}. Check the drill file\'s units and number format'
            )
    if outline_box and answer['copper']:
        copper = answer['copper']['copper_bounds']
        reach = 2.0  # a pour may run a little past the edge; a layer of another board does not stop there
        if (
            copper[0] < outline_box[0] - reach
            or copper[1] < outline_box[1] - reach
            or copper[2] > outline_box[2] + reach
            or copper[3] > outline_box[3] + reach
        ):
            warnings.append(
                f'The copper reaches beyond the board outline: it spans {_span(copper)} and the board spans '
                f'{_span(outline_box)}. Check that the layers belong to one board'
            )
    if side == 'bottom':
        warnings.append(
            f'The underside is mirrored about X{mirror_x:.3f}: turn the board over left to right about that line, '
            'and use the same outline and placement for every layer of the board'
        )
    answer['bounds'] = _cover(
        [outline_box, answer['copper'] and answer['copper']['bounds'], answer['drill'] and answer['drill']['bounds']]
    )
    answer['warnings'] = warnings
    return answer


def register(app):
    @app.post('/api/pcb-import')
    def pcb_import_route():
        return jsonify(board(request.get_json(silent=True)))
