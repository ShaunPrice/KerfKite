"""Workshop parity: the C++ kerfkite-mcp's workshop (the editor in Chromium over the DevTools protocol) against the
Python reference's (studio.mcp_workshop, Playwright).

Both drive the same Chromium and the same editor, each from a workstation server of its own in simulator mode. A
scripted session runs on both, then a random walk: each step lists the visible controls (the same on both while
they agree) and operates one of them by its kind, a fill, a selection, a check, a point within it or a click.
After each step the answers must agree: the workshop state, the controls, a command's message, the exported
files' names and sizes, the dialogs and downloads. Errors that the workshop's own checks make must have the same
text; errors of the browser driver (Playwright's words and the protocol's) need only be errors on both. Where
answers differ only while the editor is still working (a solid rebuilding, a preview generating), the state is
read again a moment later. Last, each server opens the storage the other saved, and their macro libraries must
agree.

Run it as a user that can start Chromium with its sandbox (not root):

    PYTHONPATH=$PWD python validation/mcp_workshop_parity.py --server /tmp/kkb/kerfkite-mcp --chromium /path/to/chrome [--seed 1] [--steps 60]

It prints one line per difference and a summary, and exits 1 when any case differs.
"""

import argparse
import asyncio
import base64
import json
import logging
import os
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

# The workshop's own refusals, which both must word alike; anything else is the browser driver's.
OWN = (
    'Control changed. Search the controls again.',
    'Supply local pixel coordinates as indicated in the tool description.',
    'Position lies outside this control.',
    'Use a page index from workshop_state.',
    'Set value to true or false.',
    'Supply a keyboard key or chord.',
    'Unknown workshop action.',
    'Open the control’s panel before pointing at it.',
    'Use offset ≥ 0 and limit 1–100.',
    'Enter a nonnegative offset and a length of 1–12000 characters.',
    'Enter up to 64 KiB of CAD source or commands.',
    'Action value exceeds the input limit.',
    'Supply the generated artifact filename from this workshop.',
    'Unknown workshop artifact.',
    'File exceeds 8 MiB.',
)
BINARY = ('.pdf', '.png')
READ_ONLY = {'workshop_state', 'workshop_controls', 'workshop_text', 'workshop_macros'}


class Native:
    def __init__(self, program, output, chromium):
        self.process = subprocess.Popen(
            [program, '--output-dir', str(output), '--static', str(ROOT / 'studio' / 'static'), '--chromium', chromium],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            encoding='utf-8',
        )
        self.next = 0
        self.ask_raw(
            'initialize',
            {'protocolVersion': '2025-06-18', 'capabilities': {}, 'clientInfo': {'name': 'parity', 'version': '1'}},
        )

    def ask_raw(self, method, params):
        self.next += 1
        self.process.stdin.write(
            json.dumps({'jsonrpc': '2.0', 'id': self.next, 'method': method, 'params': params}) + '\n'
        )
        self.process.stdin.flush()
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise SystemExit('kerfkite-mcp stopped')
            answer = json.loads(line)
            if answer.get('id') == self.next:
                return answer.get('result', answer)

    async def call(self, name, args):
        return await asyncio.to_thread(self.ask_raw, 'tools/call', {'name': name, 'arguments': args})

    def close(self):
        self.process.stdin.close()
        self.process.wait(60)


class Reference:
    def __init__(self, client):
        self.client = client

    async def call(self, name, args):
        result = await self.client.call_tool_mcp(name, args)
        return result.model_dump(mode='json', exclude_none=True, by_alias=True)


def value_of(answer):
    if answer.get('isError'):
        return ('error', answer['content'][0]['text'] if answer.get('content') else '')
    if 'structuredContent' in answer:
        return ('value', answer['structuredContent'])
    content = answer.get('content') or [{}]
    return ('content', content[0].get('type'))


def normal(value):
    """Artifact names compare by their form, binary files by having content, and the editor's numbered refs for
    controls without an id by their form too: the page numbers such a control when a listing first meets it, so
    the numbers follow the moment each side's editor drew it."""
    if isinstance(value, str) and value.startswith('control~') and value[8:].isdigit():
        return 'control~'
    if isinstance(value, dict):
        out = {}
        for key, item in value.items():
            if key == 'artifact' and isinstance(item, str):
                out[key] = '<artifact>' + re.sub(r'^[0-9a-f]{32}', '', item)
            elif key == 'size' and isinstance(value.get('artifact'), str) and value['artifact'].endswith(BINARY):
                out[key] = '<size>' if item > 0 else 0
            else:
                out[key] = normal(item)
        return out
    if isinstance(value, list):
        return [normal(v) for v in value]
    return value


def short(value):
    text = json.dumps(value, ensure_ascii=False)
    return text if len(text) <= 160 else text[:157] + '...'


def first_difference(a, b, path='$'):
    numbers = (int, float)
    if isinstance(a, bool) or isinstance(b, bool) or not (isinstance(a, numbers) and isinstance(b, numbers)):
        if type(a) is not type(b):
            return f'{path}: {short(a)} != {short(b)}'
    if isinstance(a, dict):
        for key in list(a) + [k for k in b if k not in a]:
            if key not in a:
                return f'{path}.{key}: only in the C++ answer, {short(b[key])}'
            if key not in b:
                return f'{path}.{key}: only in the reference answer, {short(a[key])}'
            found = first_difference(a[key], b[key], f'{path}.{key}')
            if found:
                return found
        return None
    if isinstance(a, list):
        for i, (x, y) in enumerate(zip(a, b)):
            found = first_difference(x, y, f'{path}[{i}]')
            if found:
                return found
        if len(a) != len(b):
            return f'{path}: {len(a)} items != {len(b)}'
        return None
    return None if a == b else f'{path}: {short(a)} != {short(b)}'


def error_text(text):
    return text.split(': ', 1)[1] if text.startswith('Error calling tool ') and ': ' in text else text


class Tally:
    def __init__(self):
        self.cases = self.same = self.driver_errors = self.settled = 0
        self.differences = []
        self.refusals = {}  # the drivers' words for the errors both made: reference, C++ -> count
        self.trace = False
        self.dump = False

    def record(self, label, problem):
        self.cases += 1
        if problem:
            self.differences.append(f'{label}: {problem}')
            print('DIFFERENT', label, '--', problem, flush=True)
        else:
            self.same += 1
            if self.trace:
                print('same', label, flush=True)


def compare(tally, va, vb, count=True):
    if va[0] == 'error' and vb[0] == 'error':
        ta, tb = error_text(va[1]), error_text(vb[1])
        if ta in OWN or tb in OWN:
            return None if ta == tb else f'error {ta!r} != {tb!r}'
        if count:
            tally.driver_errors += 1
            pair = (ta.split('\n')[0][:140], tb.split('\n')[0][:140])
            tally.refusals[pair] = tally.refusals.get(pair, 0) + 1
        return None
    if va[0] != vb[0]:
        return f'{va[0]} {short(va[1])} != {vb[0]} {short(vb[1])}'
    return first_difference(normal(va[1]), normal(vb[1]))


def without_state(name, value):
    """An answer's part apart from the editor state it reports, and whether it reports one."""
    if name == 'workshop_action' and isinstance(value, dict) and 'pages' in value:
        return None, True
    if name in ('workshop_command', 'workshop_macro') and isinstance(value, dict) and 'state' in value:
        return {k: v for k, v in value.items() if k != 'state'}, True
    return value, False


async def settles(tally, ref, got, name, args):
    """Whether a read the two answered differently agrees within ten seconds: each editor finishes its own work
    (a solid rebuilt, a script applied) in its own time."""
    for _ in range(20):
        await asyncio.sleep(0.5)
        again = await asyncio.gather(ref.call(name, args), got.call(name, args))
        if not compare(tally, value_of(again[0]), value_of(again[1]), count=False):
            tally.settled += 1
            return True
    return False


async def both(tally, label, ref, got, name, args, args_got=None):
    a, b = await asyncio.gather(ref.call(name, args), got.call(name, args if args_got is None else args_got))
    va, vb = value_of(a), value_of(b)
    problem = compare(tally, va, vb)
    if problem and name in READ_ONLY:
        if await settles(tally, ref, got, name, args):
            problem = None
    elif problem and va[0] == vb[0] == 'value':
        pa, stated = without_state(name, va[1])
        pb, _ = without_state(name, vb[1])
        if (
            stated
            and not first_difference(normal(pa), normal(pb))
            and await settles(tally, ref, got, 'workshop_state', {})
        ):
            problem = None
    if problem and tally.dump and not tally.differences:
        sa, sb = await asyncio.gather(ref.call('workshop_state', {}), got.call('workshop_state', {}))
        print('STATE reference', json.dumps(value_of(sa), ensure_ascii=False)[:3000])
        print('STATE C++      ', json.dumps(value_of(sb), ensure_ascii=False)[:3000])
    if problem and name == 'workshop_controls' and va[0] == vb[0] == 'value':
        listed = lambda v: {(c.get('ref'), c.get('label'), c.get('visible')) for c in v.get('controls') or []}  # noqa: E731
        only_a, only_b = listed(va[1]) - listed(vb[1]), listed(vb[1]) - listed(va[1])
        problem += f'; only the reference lists {sorted(only_a)[:4]}, only the C++ {sorted(only_b)[:4]}'
    tally.record(label, problem)
    return a, b


async def scripted(tally, ref, got):
    await both(tally, 'state', ref, got, 'workshop_state', {})
    for query, offset, limit, visible in (
        ('rect', 0, 5, False),
        ('', 0, 35, True),
        ('', 30, 20, False),
        ('zzzz', 0, 5, False),
        ('', -1, 5, False),
        ('', 0, 101, False),
    ):
        await both(
            tally,
            f'controls {query!r} {offset} {limit}',
            ref,
            got,
            'workshop_controls',
            {'query': query, 'offset': offset, 'limit': limit, 'visible_only': visible},
        )
    await both(tally, 'click rect', ref, got, 'workshop_action', {'action': 'click', 'ref': 'rect'})
    await both(tally, 'inspect width', ref, got, 'workshop_action', {'action': 'inspect', 'ref': 'width'})
    await both(tally, 'fill width', ref, got, 'workshop_action', {'action': 'fill', 'ref': 'width', 'value': '321'})
    await both(tally, 'fill width empty', ref, got, 'workshop_action', {'action': 'fill', 'ref': 'width', 'value': ''})
    await both(
        tally, 'fill width text', ref, got, 'workshop_action', {'action': 'fill', 'ref': 'width', 'value': 'abc'}
    )
    await both(tally, 'inspect width again', ref, got, 'workshop_action', {'action': 'inspect', 'ref': 'width'})
    # A chord with an unknown key: Playwright stops at it with the keys before it still held (the C++ looks every
    # key up first and holds none), so each held key is pressed once more on both sides to let it go.
    for key in (
        'Control+a',
        'Shift+KeyA',
        'Delete',
        'Control++',
        'ArrowLeft',
        'Escape',
        'Shift+',
        'Control+Nope',
        'Shift',
        'Control',
    ):
        await both(tally, f'key {key}', ref, got, 'workshop_action', {'action': 'key', 'ref': 'width', 'value': key})
    await both(tally, 'inspect width after keys', ref, got, 'workshop_action', {'action': 'inspect', 'ref': 'width'})
    await both(tally, 'bad key', ref, got, 'workshop_action', {'action': 'key', 'ref': 'width', 'value': 'Ctrl@'})
    await both(tally, 'bad check', ref, got, 'workshop_action', {'action': 'check', 'ref': 'width', 'value': 'maybe'})
    await both(
        tally, 'check a number', ref, got, 'workshop_action', {'action': 'check', 'ref': 'width', 'value': 'true'}
    )
    await both(
        tally, 'select a number', ref, got, 'workshop_action', {'action': 'select', 'ref': 'width', 'value': '1'}
    )
    await both(tally, 'bad ref', ref, got, 'workshop_action', {'action': 'click', 'ref': 'no-such-control'})
    await both(tally, 'bad page', ref, got, 'workshop_action', {'action': 'page', 'value': '7'})
    await both(tally, 'page 0', ref, got, 'workshop_action', {'action': 'page', 'value': '0'})
    await both(tally, 'bad points', ref, got, 'workshop_action', {'action': 'point', 'ref': 'width', 'points': [1]})
    await both(tally, 'outside', ref, got, 'workshop_action', {'action': 'point', 'ref': 'width', 'points': [5000, 5]})
    await both(tally, 'point', ref, got, 'workshop_action', {'action': 'point', 'ref': 'width', 'points': [5, 5]})
    await both(
        tally,
        'unknown upload',
        ref,
        got,
        'workshop_action',
        {'action': 'upload', 'ref': 'file', 'value': '0' * 32 + '.svg'},
    )
    await both(
        tally, 'bad upload name', ref, got, 'workshop_action', {'action': 'upload', 'ref': 'file', 'value': '../x'}
    )
    for line in ('rect 60 30', 'circle 20', 'view front', 'bogus words here', 'cube([40,25,5]);', '   ', '　'):
        await both(tally, f'command {line!r}', ref, got, 'workshop_command', {'line': line})
    await both(tally, 'macros', ref, got, 'workshop_macros', {})
    await both(
        tally,
        'save macro',
        ref,
        got,
        'workshop_save_macro',
        {'name': 'Box', 'kind': 'commands', 'source': 'rect {width} 10', 'parameters': {'width': 30}},
    )
    await both(
        tally,
        'save macro again',
        ref,
        got,
        'workshop_save_macro',
        {'name': 'Box', 'kind': 'commands', 'source': 'rect {width} 12'},
    )
    await both(
        tally,
        'replace macro',
        ref,
        got,
        'workshop_save_macro',
        {'name': 'Box', 'kind': 'commands', 'source': 'rect {width} 12', 'parameters': {'width': 25}, 'replace': True},
    )
    await both(tally, 'run macro', ref, got, 'workshop_macro', {'name': 'Box', 'parameters': {'width': 44}})
    await both(tally, 'run missing macro', ref, got, 'workshop_macro', {'name': 'Nope'})
    # Dialogs: dismissed without an answer, accepted with one (a prompt's text, or an empty confirmation).
    await both(tally, 'prompt dismissed', ref, got, 'workshop_action', {'action': 'click', 'ref': 'saveMaterial'})
    await both(
        tally,
        'prompt answered',
        ref,
        got,
        'workshop_action',
        {'action': 'click', 'ref': 'saveMaterial', 'confirm': 'Parity preset'},
    )
    await both(tally, 'layers tab', ref, got, 'workshop_action', {'action': 'click', 'ref': 'design-tab-layers'})
    await both(
        tally, 'confirmation dismissed', ref, got, 'workshop_action', {'action': 'click', 'ref': 'designLayerDelete'}
    )
    await both(
        tally,
        'confirmation accepted',
        ref,
        got,
        'workshop_action',
        {'action': 'click', 'ref': 'designLayerDelete', 'confirm': ''},
    )
    # A page the editor opens in a new tab, selected, read and left.
    await both(
        tally, 'open the materials guide', ref, got, 'workshop_action', {'action': 'click', 'ref': 'openMaterialsGuide'}
    )
    await both(tally, 'state with the guide open', ref, got, 'workshop_state', {})
    await both(tally, 'select the guide', ref, got, 'workshop_action', {'action': 'page', 'value': '1'})
    await both(tally, "the guide's text", ref, got, 'workshop_text', {'length': 400})
    await both(tally, "the guide's controls", ref, got, 'workshop_controls', {'limit': 20})
    await both(tally, 'select the editor again', ref, got, 'workshop_action', {'action': 'page', 'value': '0'})
    for kind in ('project', 'svg', 'gcode', 'stl'):
        await both(tally, f'export {kind}', ref, got, 'workshop_export', {'kind': kind})
    for offset, length in ((0, 300), (100, 50), (-1, 5), (0, 20000)):
        await both(tally, f'text {offset} {length}', ref, got, 'workshop_text', {'offset': offset, 'length': length})
    await both(tally, 'print', ref, got, 'workshop_print', {})
    await both(tally, 'snapshot', ref, got, 'workshop_snapshot', {})
    svg = base64.b64encode(
        b'<svg xmlns="http://www.w3.org/2000/svg" width="10mm" height="10mm" viewBox="0 0 10 10"><path d="M1 1L9 9"/></svg>'
    ).decode()
    a, b = await asyncio.gather(
        ref.call('workshop_upload', {'name': 'shape.svg', 'content_base64': svg}),
        got.call('workshop_upload', {'name': 'shape.svg', 'content_base64': svg}),
    )
    if not a.get('isError') and not b.get('isError'):
        ra, rb = a['structuredContent']['artifact'], b['structuredContent']['artifact']
        x, y = await asyncio.gather(
            ref.call('workshop_action', {'action': 'upload', 'ref': 'file', 'value': ra}),
            got.call('workshop_action', {'action': 'upload', 'ref': 'file', 'value': rb}),
        )
        tally.record('upload into the editor', compare(tally, value_of(x), value_of(y)))
        x, y = await asyncio.gather(
            ref.call('workshop_action', {'action': 'upload', 'ref': 'width', 'value': ra}),
            got.call('workshop_action', {'action': 'upload', 'ref': 'width', 'value': rb}),
        )
        tally.record('upload into a number', compare(tally, value_of(x), value_of(y)))


async def walk(tally, ref, got, rng, steps):
    listing = {'query': '', 'offset': 0, 'limit': 100, 'visible_only': True}
    for step in range(steps):
        a, b = await both(tally, f'walk {step} controls', ref, got, 'workshop_controls', listing)
        # The control chosen from the reference's listing, and the ref the C++'s listing gives the same control.
        listed_a = (a.get('structuredContent') or {}).get('controls') or []
        listed_b = (b.get('structuredContent') or {}).get('controls') or []
        if first_difference(normal(listed_a), normal(listed_b)):
            a, b = await asyncio.gather(ref.call('workshop_controls', listing), got.call('workshop_controls', listing))
            listed_a = (a.get('structuredContent') or {}).get('controls') or []
            listed_b = (b.get('structuredContent') or {}).get('controls') or []
            if first_difference(normal(listed_a), normal(listed_b)):
                break  # the difference is counted; the walk cannot go on from the same place
        choices = [i for i, c in enumerate(listed_a) if not c['disabled']]
        if not choices:
            continue
        index = rng.choice(choices)
        control = listed_a[index]
        kind = control['type']
        if kind in ('number', 'text', 'textarea', 'search', 'range', 'color', 'email', 'url'):
            args = {
                'action': 'fill',
                'ref': control['ref'],
                'value': rng.choice(['0', '5', '12.5', '150', '', 'abc', ' 7 ']),
            }
        elif kind in ('select-one', 'select-multiple') and control.get('options'):
            args = {
                'action': 'select',
                'ref': control['ref'],
                'value': rng.choice(control['options'])[rng.randint(0, 1)],
            }
        elif kind in ('checkbox', 'radio'):
            args = {'action': 'check', 'ref': control['ref'], 'value': rng.choice(['true', 'false'])}
        elif kind in ('canvas', 'svg'):
            action = rng.choice(['point', 'drag', 'wheel', 'click', 'double_click'])
            x, y = rng.randint(1, 300), rng.randint(1, 200)
            points = {
                'point': [x, y],
                'drag': [x, y, x + rng.randint(-40, 40), y + rng.randint(-40, 40)],
                'wheel': [x, y, 0, rng.choice([-120, 120])],
            }
            args = {'action': action, 'ref': control['ref'], 'points': points.get(action)}
        elif kind == 'file':
            continue
        else:
            args = {'action': rng.choice(['click', 'click', 'click', 'inspect']), 'ref': control['ref']}
        mirrored = {**args, 'ref': listed_b[index]['ref']}
        await both(
            tally,
            f'walk {step} {args["action"]} {control["ref"]} ({kind})',
            ref,
            got,
            'workshop_action',
            args,
            mirrored,
        )


async def interchange(tally, work, server, chromium):
    """Each server opens the storage the other saved: the macro libraries must agree."""
    from fastmcp import Client

    from studio.mcp_server import create_server

    native = Native(server, Path(work) / 'reference', chromium)
    try:
        async with Client(create_server(Path(work) / 'native')) as client:
            ref = Reference(client)
            a, b = await asyncio.gather(ref.call('workshop_macros', {}), native.call('workshop_macros', {}))
            problem = compare(tally, value_of(a), value_of(b))
            listed = value_of(a)[1]
            listed = listed.get('result', []) if isinstance(listed, dict) else []
            if not problem and not any(isinstance(m, dict) and m.get('name') == 'Box' for m in listed):
                problem = 'the saved macro is missing from both'
            tally.record("macros read from the other server's storage", problem)
    finally:
        native.close()


async def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--server', required=True)
    parser.add_argument('--chromium', required=True)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--steps', type=int, default=60)
    parser.add_argument('--verbose', action='store_true', help="list the drivers' words for the errors both made")
    parser.add_argument('--trace', action='store_true', help='print every case')
    parser.add_argument('--dump', action='store_true', help='print both states at the first difference')
    a = parser.parse_args()
    os.environ['KERFKITE_MCP_CHROMIUM'] = a.chromium
    logging.getLogger('werkzeug').setLevel(logging.ERROR)
    logging.getLogger('fastmcp').setLevel(logging.CRITICAL)  # a refused call is part of the comparison, not news
    from fastmcp import Client

    from studio.mcp_server import create_server

    rng = random.Random(a.seed)  # noqa: S311 - generated test steps, not secrets
    tally = Tally()
    tally.trace = a.trace
    tally.dump = a.dump
    with tempfile.TemporaryDirectory() as work:
        reference = create_server(Path(work) / 'reference')
        native = Native(a.server, Path(work) / 'native', a.chromium)
        try:
            async with Client(reference) as client:
                ref, got = Reference(client), native
                await scripted(tally, ref, got)
                await walk(tally, ref, got, rng, a.steps)
        finally:
            native.close()
        await interchange(tally, work, a.server, a.chromium)
    if a.verbose:
        for (ta, tb), n in sorted(tally.refusals.items(), key=lambda item: -item[1]):
            print(f'{n:4} refused: {ta}\n       C++: {tb}')
    print(
        f'{tally.cases} cases: {tally.same} the same ({tally.driver_errors} refused by both browser drivers in their own words, '
        f'{tally.settled} the same once the editor had finished working), {len(tally.differences)} different'
    )
    return 1 if tally.differences else 0


if __name__ == '__main__':
    sys.exit(asyncio.run(main()))
