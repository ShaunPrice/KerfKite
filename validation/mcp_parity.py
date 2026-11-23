"""Assistant server parity: the C++ kerfkite-mcp against the Python reference (studio.mcp_server, FastMCP).

Both servers get the same requests: the tool and resource lists, and generated calls of the tools that need no
browser: the template collection, artwork and template-library files, job previews and G-code exports, Studio's
design calculations and the workshop's artifacts. The answers must agree: the declarations member for member (the
FastMCP-only "_meta" aside), each call's error flag and text or its structured result value for value, with
identifiers and paths compared by their form, and the files a call writes (projects, drawings, libraries, G-code)
byte for byte or as JSON. Arguments that do not fit a tool's schema are refused by both, in their own words.

Usage:
    PYTHONPATH=$PWD python validation/mcp_parity.py --server /tmp/kkb/kerfkite-mcp [--seed 1] [--cases 400]

It prints one line per difference and a summary, and exits 1 when any case differs.
"""

import argparse
import asyncio
import base64
import copy
import json
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from fastmcp import Client  # noqa: E402

from studio.mcp_server import create_server  # noqa: E402

PROJECT = {
    'mode': 'router',
    'width': 100,
    'height': 100,
    'feed': 600,
    'power': 1000,
    'safe': 5,
    'depth': 1.2,
    'step': 0.5,
    'plunge': 100,
    'passes': 1,
    'paths': [[[10, 10], [20, 10], [20, 20]]],
}


class Native:
    """kerfkite-mcp over its standard input and output."""

    def __init__(self, program, output):
        self.process = subprocess.Popen(
            [program, '--output-dir', str(output), '--static', str(ROOT / 'studio' / 'static')],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            encoding='utf-8',
        )
        self.next = 0
        self.ask(
            'initialize',
            {'protocolVersion': '2025-06-18', 'capabilities': {}, 'clientInfo': {'name': 'parity', 'version': '1'}},
        )
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})

    def send(self, message):
        self.process.stdin.write(json.dumps(message) + '\n')
        self.process.stdin.flush()

    def ask(self, method, params):
        self.next += 1
        self.send({'jsonrpc': '2.0', 'id': self.next, 'method': method, 'params': params})
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise SystemExit('kerfkite-mcp stopped')
            answer = json.loads(line)
            if answer.get('id') == self.next:
                if 'error' in answer:
                    return {'error': answer['error']}
                return answer['result']

    def close(self):
        self.process.stdin.close()
        self.process.wait(20)


def without_meta(value):
    if isinstance(value, dict):
        return {k: without_meta(v) for k, v in value.items() if k not in ('_meta', 'meta')}
    if isinstance(value, list):
        return [without_meta(v) for v in value]
    return value


PATHLIKE = re.compile(r'[0-9a-f]{32}(\.[a-z0-9._-]+)$')


def form(value, files):
    """A result with artifact names, written paths and random identifiers replaced by their form, and each
    written file's content read in their place."""
    if isinstance(value, dict):
        out = {}
        for key, item in value.items():
            if key.endswith('_path') and isinstance(item, str):
                text = Path(item).read_bytes()
                if item.endswith('.json'):
                    data = json.loads(text)
                    out[key] = ('json', strip_ids(data))
                else:
                    out[key] = ('bytes', text)
            elif key in ('artifact',) or key.endswith('_artifact'):
                match = PATHLIKE.search(str(item))
                out[key] = '<artifact' + (match.group(1) if match else '?') + '>'
                if key == 'artifact' and isinstance(item, str) and (files / item).is_file():
                    out['<content>'] = (files / item).read_bytes()
            elif key == 'id' and isinstance(item, str) and item.startswith('custom-'):
                out[key] = '<custom id>'
            else:
                out[key] = form(item, files)
        return out
    if isinstance(value, list):
        return [form(v, files) for v in value]
    return value


def strip_ids(value):
    if isinstance(value, dict):
        return {
            k: ('<custom id>' if k == 'id' and isinstance(v, str) and v.startswith('custom-') else strip_ids(v))
            for k, v in value.items()
        }
    if isinstance(value, list):
        return [strip_ids(v) for v in value]
    return value


def label_action(label):
    match = re.search(r'design_call \{"action": "([^"]+)"', label)
    return match.group(1) if match else None


class Tally:
    def __init__(self):
        self.cases = self.same = self.refused_alike = self.described = 0
        self.differences = []

    def record(self, label, problem):
        self.cases += 1
        if problem:
            self.differences.append(f'{label}: {problem}')
            print('DIFFERENT', label, problem[:600], flush=True)
        else:
            self.same += 1


def compare_call(tally, label, ref, got, ref_files, got_files):
    ref, got = without_meta(ref), without_meta(got)
    if 'error' in got and 'error' not in ref:
        return tally.record(
            label, f'JSON-RPC error {got["error"]} where the reference answered {json.dumps(ref)[:300]}'
        )
    ref_error, got_error = ref.get('isError'), got.get('isError')
    if ref_error != got_error:
        return tally.record(
            label, f'isError {ref_error} != {got_error}: {json.dumps(ref)[:300]} | {json.dumps(got)[:300]}'
        )
    if ref_error:
        a, b = ref['content'][0]['text'], got['content'][0]['text']
        if 'validation error' in a and b.startswith('Invalid arguments for tool'):
            tally.refused_alike += 1
            return tally.record(label, None)
        return tally.record(label, None if a == b else f'error {a!r} != {b!r}')
    if 'structuredContent' in ref or 'structuredContent' in got:
        a, b = form(ref.get('structuredContent'), ref_files), form(got.get('structuredContent'), got_files)
        if label_action(label) in ('geometry', 'extrude') and a != b:
            # Shapely's vertices against the polygon and solid kernels': compared by what they describe, as
            # validation/server_route_parity.py compares the routes.
            from validation.server_route_parity import extrusion_agrees, geometry_agrees

            agree = geometry_agrees if label_action(label) == 'geometry' else extrusion_agrees
            problem = agree(ref['structuredContent']['result'], got['structuredContent']['result'])
            tally.described += problem is None
            return tally.record(label, problem)
        if a != b:
            return tally.record(label, f'result {str(a)[:400]} != {str(b)[:400]}')
        # The text content is the same value as JSON (the order of members aside), or none for both.
        texts = [[json.loads(c['text']) for c in answer.get('content', [])] for answer in (ref, got)]
        if form(texts[0], ref_files) != form(texts[1], got_files):
            return tally.record(label, f'text content {str(texts[0])[:300]} != {str(texts[1])[:300]}')
        return tally.record(label, None)
    return tally.record(label, None if ref == got else f'{json.dumps(ref)[:300]} != {json.dumps(got)[:300]}')


def cases(rng, n, templates):
    names = ['Rose', '  spaced  ', '', ' ', 'x' * 80, 'x' * 81, 'tab\tname', 'café <b>&"\'', 5, None]
    categories = ['Flowers', 'Botanical vines', '', 'Nope'] + [t['category'] for t in templates[:3]]
    queries = ['', 'rose', 'ROSE', 'vine', 'zzzz', 'leaf', 'Flowers', 'o']

    def path(k=3):
        return [[round(rng.uniform(0, 300), 3), round(rng.uniform(0, 300), 3)] for _ in range(k)]

    for _ in range(n):
        tool = rng.choice(
            [
                'list_templates',
                'get_template',
                'create_artwork',
                'save_template',
                'preview_job',
                'export_gcode',
                'design_actions',
                'design_call',
                'workshop_upload',
                'workshop_read_file',
            ]
        )
        if tool == 'list_templates':
            args = {
                'query': rng.choice(queries),
                'category': rng.choice(categories),
                'limit': rng.choice([1, 2, 5, 30, 0, -3, 150]),
            }
        elif tool == 'get_template':
            args = {'template_id': rng.choice([t['id'] for t in templates[:40]] + ['nope', 5])}
            for key in ('x', 'y', 'width', 'height'):
                if rng.random() < 0.6:
                    args[key] = rng.choice([0, 5, 10.5, 100, 500, 1999, 2000, 2001, -1, 0.05, 'ten', True])
        elif tool == 'create_artwork':
            paths = [path(rng.randint(1, 5)) for _ in range(rng.randint(0, 4))]
            if rng.random() < 0.2:
                paths.append([[1, 2, 3]])
            if rng.random() < 0.1:
                paths = 'not paths'
            args = {'name': rng.choice(names), 'paths': paths}
            if rng.random() < 0.5:
                args['width'] = rng.choice([750, 300, 100, 0, 5000, 400.5])
            if rng.random() < 0.5:
                args['height'] = rng.choice([400, 300, 100, 0, 5000, 200.25])
        elif tool == 'save_template':
            paths = [path(rng.randint(1, 5)) for _ in range(rng.randint(0, 3))]
            if rng.random() < 0.15:
                paths = [[[5, 5], [5, 5]]]
            args = {'name': rng.choice(names), 'category': rng.choice(names + ['Mine']), 'paths': paths}
        elif tool in ('preview_job', 'export_gcode'):
            project = copy.deepcopy(PROJECT)
            for _ in range(rng.randint(0, 2)):
                key = rng.choice(list(project))
                project[key] = rng.choice([None, -1, 0, 'x', [1], 1e9, 50, 'laser', 'knife'])
            if rng.random() < 0.3:
                project['mode'] = rng.choice(['laser', 'knife', 'router'])
            args = {'project': project if rng.random() < 0.95 else [1, 2]}
        elif tool == 'design_actions':
            args = {'query': rng.choice(['', 'solid', 'STL', 'import', 'nothing', 'model'])}
        elif tool == 'design_call':
            action = rng.choice(
                [
                    'protocols',
                    'assembly/example',
                    'geometry',
                    'extrude',
                    'nope',
                    'solid-cad',
                    'program-import',
                    'tool-changes',
                ]
            )
            payload = rng.choice(
                [
                    None,
                    {},
                    {'action': 'union', 'objects': [{'paths': [[[0, 0], [10, 0], [10, 10], [0, 10], [0, 0]]]}]},
                    {'paths': [[[0, 0], [10, 0], [10, 10], [0, 10], [0, 0]]], 'height': 5},
                    {'model': {'version': 1, 'features': []}},
                    {'name': 'a.nc', 'text': 'G0 X1 Y2\nG1 X3 Y4 F100\n'},
                    {'project': PROJECT},
                    {'model': 5},
                ]
            )
            args = {'action': action, 'payload': payload}
        elif tool == 'workshop_upload':
            data = rng.choice([b'G0 X1\n', b'', b'\x00\x01\x02', 'é'.encode()])
            text = base64.b64encode(data).decode()
            text = rng.choice([text, text + '=', text[:-1] if text else '!', 'not base64!', text])
            args = {
                'name': rng.choice(['a.nc', 'part.tar.gz', 'noext', 'dir/b.SVG', 'x.y.z.w', '.hidden']),
                'content_base64': text,
            }
        else:
            args = {
                'artifact': rng.choice(['nope', '0' * 32 + '.nc', 'x' * 32 + '.nc']),
                'offset': rng.choice([0, 1, -1]),
                'length': rng.choice([1, 24576, 0, 200000]),
            }
        yield tool, args


async def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--server', required=True)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--cases', type=int, default=400)
    a = parser.parse_args()
    rng = random.Random(a.seed)  # noqa: S311 - generated test cases, not secrets
    tally = Tally()
    with tempfile.TemporaryDirectory() as work:
        ref_files, got_files = Path(work) / 'reference', Path(work) / 'native'
        reference = create_server(ref_files)
        native = Native(a.server, got_files)
        try:
            async with Client(reference) as client:
                listed = [
                    without_meta(t.model_dump(mode='json', exclude_none=True, by_alias=True))
                    for t in (await client.list_tools_mcp()).tools
                ]
                theirs = without_meta(native.ask('tools/list', {}))['tools']
                # Without a browser here, the C++ server's list is the reference's tools it has built so far.
                names = {t['name'] for t in theirs}
                for tool in theirs:
                    mine = next((t for t in listed if t['name'] == tool['name']), None)
                    tally.record(
                        'declaration of ' + tool['name'],
                        None if mine == tool else f'{json.dumps(mine)[:400]} != {json.dumps(tool)[:400]}',
                    )
                resources = without_meta(
                    (await client.list_resources_mcp()).model_dump(mode='json', exclude_none=True, by_alias=True)
                )
                tally.record(
                    'resources',
                    None if resources == without_meta(native.ask('resources/list', {})) else 'resource lists differ',
                )
                text = (await client.read_resource_mcp('cnc-studio://artwork-guide')).model_dump(
                    mode='json', exclude_none=True, by_alias=True
                )
                tally.record(
                    'artwork guide',
                    None
                    if without_meta(text)
                    == without_meta(native.ask('resources/read', {'uri': 'cnc-studio://artwork-guide'}))
                    else 'guide differs',
                )
                templates = (await client.call_tool_mcp('list_templates', {'limit': 100})).structuredContent['result']
                made = []
                for index, (tool, args) in enumerate(cases(rng, a.cases, templates)):
                    if tool not in names:
                        continue
                    if tool == 'workshop_read_file' and made and rng.random() < 0.6:
                        args = {**args, 'artifact': None}
                    if tool == 'workshop_read_file' and args.get('artifact') is None:
                        ref_artifact, got_artifact = made[rng.randrange(len(made))]
                        ref_args, got_args = {**args, 'artifact': ref_artifact}, {**args, 'artifact': got_artifact}
                    else:
                        ref_args = got_args = args
                    ref = (await client.call_tool_mcp(tool, ref_args)).model_dump(
                        mode='json', exclude_none=True, by_alias=True
                    )
                    got = native.ask('tools/call', {'name': tool, 'arguments': got_args})
                    compare_call(tally, f'{index} {tool} {json.dumps(args)[:160]}', ref, got, ref_files, got_files)
                    if tool == 'workshop_upload' and not ref.get('isError') and not got.get('isError'):
                        made.append((ref['structuredContent']['artifact'], got['structuredContent']['artifact']))
        finally:
            native.close()
    print(
        f'{tally.cases} cases: {tally.same} the same ({tally.refused_alike} arguments refused by both in their own words, '
        f'{tally.described} regions and solids the same by what they describe), {len(tally.differences)} different'
    )
    return 1 if tally.differences else 0


if __name__ == '__main__':
    sys.exit(asyncio.run(main()))
