"""Remote-tool parity: the C++ kerfkite-mcp's pendant_* and hosted_* tools against the Python reference's
(studio.mcp_pendant and studio.mcp_files).

The configuration part starts both servers with each of a few hundred environments (base addresses, tokens and
token files, accounts and password files, certificate authorities) and both must start or refuse alike, with the
same message.

The monitor part points both at a monitor stand-in, a scripted HTTP server on the loopback interface (and over
HTTPS with a certificate authority of its own), and the editor part at an editor stand-in over HTTPS. Each case
calls the same tool with the same arguments on both servers, the operator giving the same answer to a
confirmation, with the stand-in answering alike; the results must agree (the error texts word for word), and so
must the requests the stand-in received (method, path, credentials, body) and the confirmations asked. The answers
include what a well-behaved server sends and what a faulty one might: redirects, statuses, oversized and cut-short
bodies, too many headers, bodies that are not JSON or not UTF-8, and generated Set-Cookie headers for the
cookie parser.

The web part runs a whole session against KerfKite's own hosted editor (two kerfkite-web servers over HTTPS from
the same account store, one for each side, with a receiver stand-in): list, upload, download, send, delete.

Run it with the reference's Python (fastmcp installed) beside the build:

    PYTHONPATH=$PWD python validation/mcp_remote_parity.py --server /tmp/kkb/kerfkite-mcp [--web /tmp/kkb/kerfkite-web] [--part all] [--seed 1]

kerfkite-pendant-tls must be beside kerfkite-mcp. It prints one line per difference and a summary, and exits 1
when any case differs.
"""

import argparse
import asyncio
import contextlib
import hashlib
import json
import logging
import os
import random
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

TOKEN = 'pairing-token-0123456789abcdefABCDEF_-xyz'  # noqa: S105 - a test account
PASSWORD = 'parity password of some length'  # noqa: S105 - a test account
LOGIN_COOKIE, SESSION_COOKIE = '__Host-cnc-login', '__Host-cnc-session'
L, S, T = 'login' + 'L' * 35, 'session' + 'S' * 33, 'csrf' + 'T' * 36
NAMES = (
    'KERFKITE_PENDANT_URL',
    'KERFKITE_PENDANT_TOKEN',
    'KERFKITE_PENDANT_TOKEN_FILE',
    'KERFKITE_PENDANT_CA_FILE',
    'KERFKITE_FILES_URL',
    'KERFKITE_FILES_USERNAME',
    'KERFKITE_FILES_PASSWORD',
    'KERFKITE_FILES_PASSWORD_FILE',
    'KERFKITE_FILES_CA_FILE',
)
UUID = re.compile(r'[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}')


@contextlib.contextmanager
def environment(values):
    saved = {name: os.environ.pop(name) for name in NAMES if name in os.environ}
    os.environ.update(values)
    try:
        yield
    finally:
        for name in NAMES:
            os.environ.pop(name, None)
        os.environ.update(saved)


# ---- the two servers -----------------------------------------------------------------------------------------


class Native:
    """kerfkite-mcp over standard input and output, answering its confirmation requests as the operator would."""

    def __init__(self, program, output, env, confirms=True):
        variables = {k: v for k, v in os.environ.items() if k not in NAMES}
        variables.update(env)
        self.errors = tempfile.TemporaryFile()
        self.process = subprocess.Popen(  # noqa: S603 - the programs under test
            [program, '--output-dir', str(output)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.errors,
            text=True,
            encoding='utf-8',
            env=variables,
        )
        self.next, self.answer, self.prompts = 0, 'accept', []
        capabilities = {'elicitation': {}} if confirms else {}
        self.ask(
            'initialize',
            {
                'protocolVersion': '2025-06-18',
                'capabilities': capabilities,
                'clientInfo': {'name': 'parity', 'version': '1'},
            },
        )

    def send(self, message):
        self.process.stdin.write(json.dumps(message) + '\n')
        self.process.stdin.flush()

    def ask(self, method, params):
        self.next += 1
        self.send({'jsonrpc': '2.0', 'id': self.next, 'method': method, 'params': params})
        while True:
            line = self.process.stdout.readline()
            if not line:
                self.errors.seek(0)
                raise SystemExit('kerfkite-mcp stopped: ' + self.errors.read().decode('utf-8', 'replace'))
            message = json.loads(line)
            if message.get('method') == 'elicitation/create':
                self.prompts.append(message['params']['message'])
                result = {'action': self.answer, **({'content': {}} if self.answer == 'accept' else {})}
                self.send({'jsonrpc': '2.0', 'id': message['id'], 'result': result})
            elif message.get('id') == self.next:
                return message.get('result', message)

    async def call(self, name, args):
        return await asyncio.to_thread(self.ask, 'tools/call', {'name': name, 'arguments': args})

    def close(self):
        self.process.stdin.close()
        self.process.wait(60)


class Reference:
    """studio.mcp_server's FastMCP server in this process, through FastMCP's client."""

    def __init__(self, server, confirms=True):
        from fastmcp import Client
        from fastmcp.client.elicitation import ElicitResult
        from mcp.client.session import ClientSession

        async def unchecked(session, name, result):
            return None

        # The servers are compared, not a client's checks: the SDK's client would refuse a result its output schema
        # does not describe (hosted_destinations' list where an editor answered otherwise) before it is seen.
        ClientSession._validate_tool_result = unchecked

        self.answer, self.prompts = 'accept', []

        async def confirm(message, response_type, params, context):
            self.prompts.append(message)
            return ElicitResult(action=self.answer, content={} if self.answer == 'accept' else None)

        self.client = Client(server, elicitation_handler=confirm if confirms else None)

    async def __aenter__(self):
        await self.client.__aenter__()
        return self

    async def __aexit__(self, *exc):
        await self.client.__aexit__(*exc)

    async def call(self, name, args):
        result = await self.client.call_tool_mcp(name, args)
        return result.model_dump(mode='json', exclude_none=True, by_alias=True)


def reference_server(output, env):
    from studio.mcp_server import create_server

    with environment(env):
        return create_server(output)


def value_of(answer):
    if answer.get('isError'):
        text = answer['content'][0]['text'] if answer.get('content') else ''
        # Arguments that the schema refuses: pydantic's words against kerfkite-mcp's, as validation/mcp_parity.py
        # counts them.
        if 'validation error for call[' in text or text.startswith('Invalid arguments for tool'):
            return ('error', '<arguments refused>')
        return ('error', text)
    if 'structuredContent' in answer:
        return ('value', answer['structuredContent'])
    content = answer.get('content') or [{}]
    return ('content', content[0].get('type'))


def normal(value):
    """A status's new request_id and a new artifact's random name compare by their form; a file record's
    identifier and time from kerfkite-web too."""
    if isinstance(value, dict):
        out = {}
        for key, item in value.items():
            if key == 'request_id' and isinstance(item, str) and UUID.fullmatch(item):
                out[key] = '<uuid4>'
            elif key == 'artifact' and isinstance(item, str):
                out[key] = '<artifact>' + re.sub(r'^[0-9a-f]{32}', '', item)
            elif key in ('id', 'file_id') and isinstance(item, str) and re.fullmatch(r'[0-9a-f]{32}', item):
                out[key] = '<id>'
            elif key == 'created' and isinstance(item, (int, float)):
                out[key] = '<time>'
            else:
                out[key] = normal(item)
        return out
    if isinstance(value, (list, tuple)):
        return [normal(v) for v in value]
    return value


def first_difference(a, b, path='$'):
    """Where two outcomes first differ, and what each has there."""
    if isinstance(a, dict) and isinstance(b, dict):
        for key in list(a) + [k for k in b if k not in a]:
            if key not in a or key not in b or a[key] != b[key]:
                return first_difference(a.get(key, '<absent>'), b.get(key, '<absent>'), f'{path}.{key}')
    if isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        for index, (x, y) in enumerate(zip(a, b)):
            if x != y:
                return first_difference(x, y, f'{path}[{index}]')
    return path, a, b


def short(value):
    text = json.dumps(value, ensure_ascii=False, default=str)
    return text if len(text) <= 300 else text[:297] + '...'


class Tally:
    def __init__(self, trace=False):
        self.cases = self.same = self.own = 0
        self.differences = []
        self.trace = trace

    def compare(self, label, ref, got, own=None):
        """own names the rule of kerfkite-mcp's own (docs/NATIVE.md) by which a case may differ."""
        self.cases += 1
        if self.trace:
            print(f'{label}: {short(ref)}' + (f'\n  C++ ({own}): {short(got)}' if own and ref != got else ''))
        if ref == got:
            self.same += 1
            return True
        if own:
            self.own += 1
            return True
        self.differences.append(label)
        where, a, b = first_difference(ref, got)
        print(f'DIFFERENT {label}\n  at {where}\n  Python: {short(a)}\n  C++:    {short(b)}')
        return False


# ---- the stand-ins ----------------------------------------------------------------------------------------------


def http_answer(status, body=b'', headers=(), reason='Status', length=None):
    if not isinstance(body, bytes):
        body = json.dumps(body).encode()
    head = f'HTTP/1.1 {status} {reason}\r\n'
    for name, value in headers:
        head += f'{name}: {value}\r\n'
    head += f'Content-Length: {len(body) if length is None else length}\r\nConnection: close\r\n\r\n'
    return head.encode('latin-1') + body


class StandIn:
    """A scripted HTTP/1.1 server on the loopback interface, over TLS when given a certificate. answer(request) gives
    the bytes to send, 'close' to close without an answer, or ('stall', seconds)."""

    def __init__(self, answer, cert=None, key=None):
        self.listener = socket.create_server(('127.0.0.1', 0))
        self.port = self.listener.getsockname()[1]
        self.context = None
        if cert:
            self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            self.context.load_cert_chain(cert, key)
        self.answer, self.requests, self.lock = answer, [], threading.Lock()
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                connection, _ = self.listener.accept()
            except OSError:
                return
            threading.Thread(target=self.one, args=(connection,), daemon=True).start()

    def one(self, connection):
        try:
            connection.settimeout(30)
            if self.context:
                connection = self.context.wrap_socket(connection, server_side=True)
            stream = connection.makefile('rb')
            line = stream.readline(65537)
            if not line:
                return
            method, path, _ = line.decode('latin-1').split(' ', 2)
            headers = {}
            while True:
                header = stream.readline(65537)
                if header in (b'\r\n', b'\n', b''):
                    break
                name, _, value = header.decode('latin-1').partition(':')
                headers[name.strip().lower()] = value.strip()
            body = stream.read(int(headers.get('content-length') or 0))
            request = {'method': method, 'path': path, 'headers': headers, 'body': body}
            with self.lock:
                self.requests.append(request)
            reply = self.answer(request)
            if isinstance(reply, tuple):
                time.sleep(reply[1])
            elif reply != 'close':
                connection.sendall(reply)
        except (OSError, ValueError):
            pass
        finally:
            with contextlib.suppress(OSError):
                connection.close()


def request_form(request, keys=('authorization', 'content-type', 'cookie', 'origin', 'x-studio-token', 'host')):
    body = request['body']
    try:
        body = (
            json.loads(body) if request['headers'].get('content-type') == 'application/json' else body.decode('latin-1')
        )
    except ValueError:
        body = body.decode('latin-1')
    return [request['method'], request['path'], {k: request['headers'].get(k) for k in keys}, body]


def certificate(work, relay, name):
    cert, key = work / f'{name}.pem', work / f'{name}-key.pem'
    subprocess.run(  # noqa: S603 - the programs under test
        [str(relay), '-make-cert', '-hosts', '127.0.0.1,localhost', '-cert', str(cert), '-key', str(key)],
        check=True,
        capture_output=True,
    )
    return cert, key


# ---- configuration ----------------------------------------------------------------------------------------------


def reference_start(env):
    from studio.mcp_files import HostedFiles
    from studio.mcp_pendant import PendantClient

    with environment(env):
        try:
            pendant = PendantClient.from_environment()
            files = HostedFiles.from_environment()
        except Exception as error:  # what create_server raises, and the reference stops with
            return ['refused', str(error)]
    return ['started', pendant is not None, files is not None]


def native_start(program, output, env):
    variables = {k: v for k, v in os.environ.items() if k not in NAMES}
    variables.update(env)
    lines = [
        {
            'jsonrpc': '2.0',
            'id': 1,
            'method': 'initialize',
            'params': {
                'protocolVersion': '2025-06-18',
                'capabilities': {},
                'clientInfo': {'name': 'parity', 'version': '1'},
            },
        },
        {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'},
    ]
    done = subprocess.run(  # noqa: S603 - the programs under test
        [program, '--output-dir', str(output)],
        input=''.join(json.dumps(m) + '\n' for m in lines),
        capture_output=True,
        text=True,
        encoding='utf-8',
        env=variables,
        timeout=60,
    )
    if done.returncode:
        last = done.stderr.strip().splitlines()[-1] if done.stderr.strip() else ''
        return ['refused', last.removeprefix('kerfkite-mcp: error: ')]
    names = [t['name'] for t in json.loads(done.stdout.splitlines()[1])['result']['tools']]
    return ['started', 'pendant_status' in names, 'hosted_list_files' in names]


def addresses(rng, https_only):
    schemes = ['https://'] if https_only else ['http://', 'https://']
    hosts = [
        '127.0.0.1',
        '127.255.255.254',
        '127.0.0.01',
        '127.1',
        '0177.0.0.1',
        'localhost',
        'LocalHost',
        'localhost.',
        '[::1]',
        '[::ffff:127.0.0.1]',
        '[::ffff:7f00:1]',
        '[0:0:0:0:0:0:0:1]',
        '[::1%25lo]',
        '[::1%lo]',
        '[fe80::1%eth0]',
        '[::]',
        '[::ffff:10.0.0.1]',
        '[1::]',
        '[v1.x]',
        '[v1.a:b]',
        '[127.0.0.1]',
        '[::1',
        '::1]',
        '10.0.0.5',
        '192.168.1.20',
        'example.com',
        'monitor.local',
        '',
        'bücher.example',
        'ÄBC.example',
        '[::1]x',
        'host:8080:9090',
        '1.2.3.4.5',
        '[::1%25%25x]',
        '[::1%]',
    ]
    ports = [
        '',
        ':',
        ':0',
        ':80',
        ':443',
        ':8765',
        ':65535',
        ':65536',
        ':00080',
        ':-1',
        ':+1',
        ':８０',
        ':1_0',
        ':99999999999',
        ':0x50',
    ]
    tails = ['', '/', '//', '/api', '?', '?x=1', '#', '#x', '/ ', ' ', '\t', '/ ', ' ', '\\', '\x7f', '\x1f']
    out = set()
    for scheme in schemes:
        for host in hosts:
            out.add(scheme + host + rng.choice(['', ':8765', '']))
        for port in ports:
            out.add(scheme + '127.0.0.1' + port)
            out.add(scheme + '[::1]' + port)
        for tail in tails:
            out.add(scheme + 'localhost:8765' + tail)
    for _ in range(60):
        out.add(
            rng.choice(['http://', 'https://', 'HTTP://', 'Https://', 'ftp://', '//', 'http:/', '', ' https://'])
            + rng.choice(['', 'user@', ':@', 'user:pw@', '@'])
            + rng.choice(hosts)
            + rng.choice(ports)
            + rng.choice(tails)
        )
    return sorted(out)


def configuration(tally, program, work, rng):
    ca, _ = certificate(work, Path(program).with_name('kerfkite-pendant-tls'), 'config-ca')
    files = work / 'config'
    files.mkdir()
    (files / 'empty.pem').write_text('')
    (files / 'garbage.pem').write_text('-----BEGIN CERTIFICATE-----\nnot base64\n-----END CERTIFICATE-----\n')
    tokens = {
        'plain': TOKEN,
        'spaced': f'  {TOKEN}\n',
        'separators': f'\x1c{TOKEN}\x1f',
        'nul': f'\x00{TOKEN}',
        'short': 'a' * 31,
        'longest': 'b' * 256,
        'long': 'c' * 257,
        'too-long': 'd' * 258,
        'plus': TOKEN + '+',
        'latin': TOKEN + 'é',
        'empty': '',
        'newlines': '\n\n',
    }
    for name, text in tokens.items():
        (files / f'token-{name}').write_bytes(text.encode('utf-8'))
    passwords = {
        'plain': PASSWORD,
        'newline': PASSWORD + '\n',
        'crlf': PASSWORD + '\r\n',
        'two': PASSWORD + '\n\n',
        'cr-only': PASSWORD + '\r',
        'lfcr': PASSWORD + '\n\r',
        'longest': 'p' * 256,
        'long': 'q' * 257,
        'file-limit': 'r' * 1026,
        'file-over': 's' * 1027,
        'latin1': None,
        'empty': '',
        'just-newline': '\n',
        'bom': '﻿' + PASSWORD,
    }
    for name, text in passwords.items():
        (files / f'password-{name}').write_bytes(
            b'\xe9t\xe9 password long enough' if text is None else text.encode('utf-8')
        )
    cases = []
    # The pendant's settings, alone and in their combinations.
    url = 'http://127.0.0.1:8765'
    for env in (
        {},
        {'KERFKITE_PENDANT_URL': url},
        {'KERFKITE_PENDANT_TOKEN': TOKEN},
        {'KERFKITE_PENDANT_CA_FILE': str(ca)},
        {
            'KERFKITE_PENDANT_URL': url,
            'KERFKITE_PENDANT_TOKEN': TOKEN,
            'KERFKITE_PENDANT_TOKEN_FILE': str(files / 'token-plain'),
        },
        {'KERFKITE_PENDANT_TOKEN': TOKEN, 'KERFKITE_PENDANT_TOKEN_FILE': str(files / 'token-plain')},
        {'KERFKITE_PENDANT_URL': url, 'KERFKITE_PENDANT_TOKEN': TOKEN},
    ):
        cases.append(('pendant settings', env))
    for name in tokens:
        cases.append((f'token {name}', {'KERFKITE_PENDANT_URL': url, 'KERFKITE_PENDANT_TOKEN': tokens[name]}))
        cases.append(
            (
                f'token file {name}',
                {'KERFKITE_PENDANT_URL': url, 'KERFKITE_PENDANT_TOKEN_FILE': str(files / f'token-{name}')},
            )
        )
    for path in (files / 'missing', files):
        cases.append(
            (f'token file {path.name}', {'KERFKITE_PENDANT_URL': url, 'KERFKITE_PENDANT_TOKEN_FILE': str(path)})
        )
    for address in addresses(rng, False):
        cases.append((f'pendant url {address!r}', {'KERFKITE_PENDANT_URL': address, 'KERFKITE_PENDANT_TOKEN': TOKEN}))
    for scheme in ('http', 'https'):
        for name, path in (
            ('ca', ca),
            ('empty', files / 'empty.pem'),
            ('garbage', files / 'garbage.pem'),
            ('missing', files / 'missing'),
            ('directory', files),
        ):
            cases.append(
                (
                    f'pendant {scheme} CA {name}',
                    {
                        'KERFKITE_PENDANT_URL': f'{scheme}://127.0.0.1:8765',
                        'KERFKITE_PENDANT_TOKEN': TOKEN,
                        'KERFKITE_PENDANT_CA_FILE': str(path),
                    },
                )
            )
        cases.append(
            (
                f'pendant {scheme} CA and bad token',
                {
                    'KERFKITE_PENDANT_URL': f'{scheme}://127.0.0.1:8765',
                    'KERFKITE_PENDANT_TOKEN': 'x',
                    'KERFKITE_PENDANT_CA_FILE': str(files / 'missing'),
                },
            )
        )
    # The hosted account's.
    origin = 'https://127.0.0.1:18543'
    account = {'KERFKITE_FILES_URL': origin, 'KERFKITE_FILES_USERNAME': 'parity', 'KERFKITE_FILES_PASSWORD': PASSWORD}
    for drop in (None, 'KERFKITE_FILES_URL', 'KERFKITE_FILES_USERNAME', 'KERFKITE_FILES_PASSWORD'):
        cases.append((f'files settings without {drop}', {k: v for k, v in account.items() if k != drop}))
    cases.append(
        ('files password and file', {**account, 'KERFKITE_FILES_PASSWORD_FILE': str(files / 'password-plain')})
    )
    cases.append(('files CA only', {'KERFKITE_FILES_CA_FILE': str(ca)}))
    cases.append(
        (
            'pendant wrong and files wrong',
            {
                'KERFKITE_PENDANT_URL': 'http://example.com',
                'KERFKITE_PENDANT_TOKEN': TOKEN,
                'KERFKITE_FILES_URL': 'http://x',
            },
        )
    )
    cases.append(('pendant and files', {'KERFKITE_PENDANT_URL': url, 'KERFKITE_PENDANT_TOKEN': TOKEN, **account}))
    for name in passwords:
        without = {k: v for k, v in account.items() if k != 'KERFKITE_FILES_PASSWORD'}
        cases.append(
            (f'password file {name}', {**without, 'KERFKITE_FILES_PASSWORD_FILE': str(files / f'password-{name}')})
        )
        if passwords[name] is not None and '\x00' not in passwords[name] and passwords[name]:
            cases.append((f'password {name}', {**account, 'KERFKITE_FILES_PASSWORD': passwords[name]}))
    for user in ('Parity', 'a', 'a' * 64, 'a' * 65, 'with space', 'mail@example.com', 'déjà', 'a/b', '.', '-', 'x\t'):
        cases.append((f'username {user!r}', {**account, 'KERFKITE_FILES_USERNAME': user}))
    for address in addresses(rng, True):
        cases.append((f'files url {address!r}', {**account, 'KERFKITE_FILES_URL': address}))
    for name, path in (
        ('ca', ca),
        ('empty', files / 'empty.pem'),
        ('garbage', files / 'garbage.pem'),
        ('missing', files / 'missing'),
    ):
        cases.append((f'files CA {name}', {**account, 'KERFKITE_FILES_CA_FILE': str(path)}))
    output = work / 'config-out'
    for label, env in cases:
        if any('\x00' in v for v in env.values()):
            continue  # no environment holds a NUL
        tally.compare(label, reference_start(env), native_start(program, output, env))


# ---- the paired monitor ----------------------------------------------------------------------------------------

STATUS = {
    'connected': True,
    'simulated': True,
    'state': 'Idle',
    'active': False,
    'progress': 0.25,
    'position': {'x': 1.5, 'y': -2, 'z': 0.125},
    'error': None,
    'protocol': 'grbl',
    'pendant': {'jog': True},
    'controls': ['start', 'pause'],
    'armed': False,
    'control_epoch': 'e1',
    'extra': 'not passed on',
}


def monitor_default(request):
    if request['path'] == '/api/status':
        return http_answer(200, STATUS)
    return http_answer(200, {'ok': True})


def status_cases():
    big = b'{"state": "x"}' + b' ' * (131072 - 14)
    many = [(f'X-{i}', 'v') for i in range(98)]  # with Content-Length and Connection: 100 header lines
    return [
        ('normal', None),
        ('empty object', http_answer(200, {})),
        ('token echoed', http_answer(200, {'state': TOKEN, 'error': f'bad {TOKEN} token', 'controls': [TOKEN]})),
        ('401', http_answer(401, {'error': 'no'})),
        ('302', http_answer(302, b'', [('Location', '/elsewhere')])),
        ('304', http_answer(304, b'')),
        ('399', http_answer(399, {})),
        ('500 error', http_answer(500, {'error': 'Controller busy'})),
        ('500 null error', http_answer(500, {'error': None})),
        ('500 no error', http_answer(500, {})),
        ('500 long error', http_answer(500, {'error': 'é' * 400})),
        ('500 object error', http_answer(500, {'error': {'code': 7}})),
        ('500 list error', http_answer(500, {'error': [1, 'two', None, True, False, 2.5]})),
        ('500 number error', http_answer(500, {'error': 1.5})),
        ('404 text', http_answer(404, b'Not Found')),
        ('999', http_answer(999, {'error': 'odd'})),
        ('list', http_answer(200, [1, 2])),
        ('string', http_answer(200, '"text"'.encode())),
        ('not JSON', http_answer(200, b'<html>')),
        ('not UTF-8', http_answer(200, b'{"state": "\xff"}')),
        ('byte order mark', http_answer(200, b'\xef\xbb\xbf{}')),
        ('largest', http_answer(200, big)),
        ('too large', http_answer(200, big + b' ')),
        ('closed', 'close'),
        ('cut short', http_answer(200, b'{"state": "Id', length=40)),
        ('cut short whole', http_answer(200, b'{}', length=10)),
        ('chunk cut short', b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\na\r\n{"a"'),
        ('100 headers', http_answer(200, {}, many)),
        ('101 headers', http_answer(200, {}, many + [('X-last', 'v')])),
        ('long header', http_answer(200, {}, [('X-Long', 'v' * 65520)])),
        ('longer header', http_answer(200, {}, [('X-Long', 'v' * 65540)])),
        ('nested', http_answer(200, {'position': {'x': [1, {'y': None}]}, 'state': 'é中\U0001f600'})),
    ]


def uuid_forms(rng):
    base = '12345678-9abc-4def-8123-456789abcdef'
    hexed = base.replace('-', '')
    return [
        base,
        base.upper(),
        '{' + base + '}',
        '{{' + base + '}}',
        'urn:uuid:' + base,
        'uuid:' + base,
        hexed,
        '--' + hexed,
        'uurn:rn:' + hexed,
        ' ' + hexed[1:],
        hexed[:-1] + ' ',
        '+' + hexed[1:],
        '-' + '0' * 31,
        '-' + hexed[1:],
        '0x' + hexed[2:],
        '0X' + hexed[2:],
        hexed[:4] + '_' + hexed[5:],
        hexed[:4] + '__' + hexed[6:],
        '_' + hexed[1:],
        hexed[:-1] + '_',
        '0x_' + hexed[3:],
        hexed[:31],
        hexed + '0',
        hexed[:-1] + 'g',
        '',
        'not a uuid',
        '{}' + hexed + '{}',
        ' ' + hexed[1:],
        '00000000' * 4,
        'ffffffff' * 4,
        '1' + 'urn:' + hexed[1:],
        '\u0661' * 32,
        '\uff11' + hexed[1:],
        '\u0660x' + hexed[2:],
        '\x1c' + hexed[1:],
        '\u3000' + hexed[1:],
        '\u0660X_' + hexed[3:],
    ]


def action_cases(rng):
    rid = '12345678-9abc-4def-8123-456789abcdef'
    out = []
    for action in ('start', 'pause', 'resume', 'stop'):
        for answer in ('accept', 'decline', 'cancel'):
            out.append(('pendant_job', {'action': action, 'request_id': rid, 'control_epoch': 'e1'}, answer, None))
        out.append(('pendant_job', {'action': action, 'request_id': rid}, 'accept', None))
        out.append(('pendant_job', {'action': action, 'request_id': rid, 'control_epoch': 'x' * 129}, 'accept', None))
    for answer in ('accept', 'decline', 'cancel'):
        for action in ('home', 'zero'):
            out.append(
                ('pendant_reference', {'action': action, 'request_id': rid, 'control_epoch': 'e2'}, answer, None)
            )
    for form in uuid_forms(rng):
        out.append(('pendant_job', {'action': 'pause', 'request_id': form, 'control_epoch': 'e'}, 'accept', None))
    for epoch in ('', 'x', 'x' * 128, 'x' * 129, 'é' * 128, 'é' * 129, '\U0001f600' * 128):
        out.append(('pendant_job', {'action': 'resume', 'request_id': rid, 'control_epoch': epoch}, 'accept', None))
    for reply in ({'ok': False}, {'ok': 'true'}, {'ok': 1}, {}, {'ok': True, 'extra': 1}):
        out.append(
            (
                'pendant_job',
                {'action': 'start', 'request_id': rid, 'control_epoch': 'e1'},
                'accept',
                http_answer(200, reply),
            )
        )
    for status, body in ((409, {'error': 'Not armed'}), (401, {}), (307, b''), (200, b'nope'), (200, [True])):
        out.append(
            (
                'pendant_reference',
                {'action': 'home', 'request_id': rid, 'control_epoch': 'e1'},
                'accept',
                http_answer(status, body),
            )
        )
    distances = [
        1,
        -1,
        0.001,
        0.0009999,
        10,
        10.0001,
        -10,
        1e-3,
        0.00123456789,
        3.14159265,
        1234.5,
        0,
        -0.0,
        5e-4,
        9.99999999,
        2.5,
        7,
    ]
    feeds = [0.1, 0.0999, 1000, 1000.5, 250.75, 100, 1e3, 999.99999]
    for _ in range(40):
        args = {
            'axis': rng.choice('XYZ'),
            'distance': rng.choice(distances),
            'feed': rng.choice(feeds),
            'request_id': rng.choice([rid] * 4 + uuid_forms(rng)[:8]),
            'control_epoch': rng.choice(['e1'] * 6 + ['', 'x' * 129]),
        }
        out.append(('pendant_jog', args, rng.choice(['accept'] * 3 + ['decline', 'cancel']), None))
    for distance in distances:
        for feed in (0.1, 250.75, 1000, 99.999999):
            args = {'axis': 'Y', 'distance': distance, 'feed': feed, 'request_id': rid, 'control_epoch': 'e1'}
            out.append(('pendant_jog', args, 'accept', None))
    # Arguments as a client might send them: numbers as text, a truth value, a small axis.
    for distance, feed, axis in (
        ('1.5', 100, 'X'),
        (2, '100', 'Y'),
        (True, 100, 'Z'),
        (1, 100, 'x'),
        ('1e-3', '0.1', 'Z'),
        ('nan', 100, 'X'),
    ):
        args = {'axis': axis, 'distance': distance, 'feed': feed, 'request_id': rid, 'control_epoch': 'e1'}
        out.append(('pendant_jog', args, 'accept', None))
    for distance, feed in ((1, 100), (0.1, 50)):
        out.append(
            (
                'pendant_jog',
                {'axis': 'X', 'distance': distance, 'feed': feed, 'request_id': rid, 'control_epoch': 'e1'},
                'accept',
                http_answer(500, {'error': 'Limit switch'}),
            )
        )
    return out


async def listed(tally, label, ref, got):
    """The tools, in the order clients list them."""
    mine = [t.name for t in await ref.client.list_tools()]
    theirs = [t['name'] for t in (await asyncio.to_thread(got.ask, 'tools/list', {}))['tools']]
    tally.compare(label, mine, theirs)


async def monitor_case(tally, label, ref, got, monitor, tool, args, answer='accept', reply=None):
    outcomes = []
    for side in (ref, got):
        monitor.requests.clear()
        monitor.answer = (lambda request, r=reply: r) if reply is not None else monitor_default
        side.answer, side.prompts[:] = answer, []
        result = value_of(await side.call(tool, args))
        outcomes.append([normal(result), [request_form(r) for r in monitor.requests], list(side.prompts)])
    tally.compare(label, *outcomes)


async def pendant(tally, program, work, rng, quick=False):
    relay = Path(program).with_name('kerfkite-pendant-tls')
    cert, key = certificate(work, relay, 'monitor')
    other, _ = certificate(work, relay, 'other')
    plain, secure = StandIn(monitor_default), StandIn(monitor_default, cert, key)
    runs = [
        ('http', plain, {'KERFKITE_PENDANT_URL': f'http://127.0.0.1:{plain.port}', 'KERFKITE_PENDANT_TOKEN': TOKEN}),
        (
            'localhost',
            plain,
            {'KERFKITE_PENDANT_URL': f'http://localhost:{plain.port}/', 'KERFKITE_PENDANT_TOKEN': TOKEN},
        ),
        (
            'https',
            secure,
            {
                'KERFKITE_PENDANT_URL': f'https://127.0.0.1:{secure.port}',
                'KERFKITE_PENDANT_TOKEN': TOKEN,
                'KERFKITE_PENDANT_CA_FILE': str(cert),
            },
        ),
        (
            'https other CA',
            secure,
            {
                'KERFKITE_PENDANT_URL': f'https://127.0.0.1:{secure.port}',
                'KERFKITE_PENDANT_TOKEN': TOKEN,
                'KERFKITE_PENDANT_CA_FILE': str(other),
            },
        ),
        (
            'https system CA',
            secure,
            {'KERFKITE_PENDANT_URL': f'https://localhost:{secure.port}', 'KERFKITE_PENDANT_TOKEN': TOKEN},
        ),
    ]
    for name, monitor, env in runs:
        output = work / f'pendant-{name}'
        reference = reference_server(output / 'reference', env)
        native = Native(program, output / 'native', env)
        try:
            async with Reference(reference) as ref:
                await listed(tally, f'{name} tools listed', ref, native)
                full = name in ('http', 'https') and not quick
                for label, reply in status_cases() if full else status_cases()[:4]:
                    await monitor_case(
                        tally, f'{name} status {label}', ref, native, monitor, 'pendant_status', {}, reply=reply
                    )
                if not full:
                    continue
                for index, (tool, args, answer, reply) in enumerate(action_cases(rng)):
                    await monitor_case(
                        tally,
                        f'{name} {tool} {index} {short(args)} {answer}',
                        ref,
                        native,
                        monitor,
                        tool,
                        args,
                        answer,
                        reply,
                    )
                if name == 'http':
                    await monitor_case(
                        tally, f'{name} status stalled', ref, native, monitor, 'pendant_status', {}, reply=('stall', 8)
                    )
        finally:
            native.close()
    # A client that cannot ask its operator.
    env = runs[0][2]
    output = work / 'pendant-unasked'
    reference = reference_server(output / 'reference', env)
    native = Native(program, output / 'native', env, confirms=False)
    try:
        async with Reference(reference, confirms=False) as ref:
            rid = '12345678-9abc-4def-8123-456789abcdef'
            for tool, args in (
                ('pendant_job', {'action': 'start', 'request_id': rid, 'control_epoch': 'e'}),
                ('pendant_job', {'action': 'stop', 'request_id': rid}),
                ('pendant_job', {'action': 'pause', 'request_id': rid, 'control_epoch': 'e'}),
                ('pendant_reference', {'action': 'zero', 'request_id': rid, 'control_epoch': 'e'}),
                ('pendant_jog', {'axis': 'Z', 'distance': -0.5, 'feed': 60, 'request_id': rid, 'control_epoch': 'e'}),
            ):
                await monitor_case(tally, f'unasked {tool} {args.get("action", "")}', ref, native, plain, tool, args)
    finally:
        native.close()
    # Base addresses that http.client reads its own way: an IPv6 address without a port.
    for address in (
        'http://[::1]',
        'http://[::ffff:127.0.0.1]',
        'http://[::1%25lo]',
        'https://[fe80::abcd]',
        'https://[::1]/',
        'https://[1::]',
    ):
        env = {'KERFKITE_PENDANT_URL': address, 'KERFKITE_PENDANT_TOKEN': TOKEN}
        output = work / f'pendant-quirk-{len(address)}-{hashlib.sha256(address.encode()).hexdigest()[:6]}'
        reference = reference_server(output / 'reference', env)
        native = Native(program, output / 'native', env)
        try:
            async with Reference(reference) as ref:
                await monitor_case(tally, f'address {address}', ref, native, plain, 'pendant_status', {})
                await monitor_case(
                    tally,
                    f'address {address} pause',
                    ref,
                    native,
                    plain,
                    'pendant_job',
                    {'action': 'pause', 'request_id': '12345678-9abc-4def-8123-456789abcdef', 'control_epoch': 'e'},
                )
        finally:
            native.close()


# ---- the hosted editor stand-in --------------------------------------------------------------------------------

FILES = [
    {'name': 'first.nc', 'content': b'G0 X1\n', 'created': 1700000001.5, 'source': 'web'},
    {
        'name': 'second.svg',
        'content': b'<svg xmlns="http://www.w3.org/2000/svg"/>',
        'created': 1700000000.25,
        'source': 'upload',
    },
]


class Editor:
    """The editor stand-in's account: its files and receivers, and the answers a case puts in place of its own for
    (method, path), each a list taken in order."""

    def __init__(self, origin):
        self.origin = origin
        self.reset()

    def reset(self, overrides=None):
        self.files = []
        for f in FILES:
            identifier = hashlib.sha256(f['name'].encode()).hexdigest()[:32]
            self.files.append(
                {
                    'id': identifier,
                    'name': f['name'],
                    'content': f['content'],
                    'sha256': hashlib.sha256(f['content']).hexdigest(),
                    'bytes': len(f['content']),
                    'created': f['created'],
                    'source': f['source'],
                }
            )
        self.overrides = {k: list(v) for k, v in (overrides or {}).items()}
        self.counter = 0

    def record(self, f):
        return {k: f[k] for k in ('id', 'name', 'bytes', 'sha256', 'created', 'source')}

    def answer(self, request):
        method, path = request['method'], request['path']
        waiting = self.overrides.get((method, path))
        if waiting:
            return waiting.pop(0)
        cookie = request['headers'].get('cookie', '')
        if (method, path) == ('GET', '/login'):
            return http_answer(
                200, b'<form>', [('Set-Cookie', f'{LOGIN_COOKIE}={L}; Path=/; Secure; HttpOnly; SameSite=Strict')]
            )
        if (method, path) == ('POST', '/login'):
            form = urllib.parse.parse_qs(request['body'].decode())
            if (
                form.get('csrf') != [L]
                or form.get('username') != ['parity']
                or form.get('password') != [PASSWORD]
                or cookie != f'{LOGIN_COOKIE}={L}'
            ):
                return http_answer(200, b'<form>wrong</form>')
            return http_answer(
                302,
                b'',
                [('Location', '/'), ('Set-Cookie', f'{SESSION_COOKIE}={S}; Path=/; Secure; HttpOnly; SameSite=Strict')],
            )
        if cookie != f'{SESSION_COOKIE}={S}':
            return http_answer(401, {'error': 'Sign in'})
        if method == 'POST' and (
            request['headers'].get('x-studio-token') != T or request['headers'].get('origin') != self.origin
        ):
            return http_answer(403, {'error': 'Forbidden'})
        if path == '/api/session':
            return http_answer(200, {'token': T, 'username': 'parity', 'web_mode': True})
        if path == '/api/destinations':
            return http_answer(200, ['bench', 'laser'])
        if (method, path) == ('GET', '/api/files'):
            return http_answer(200, [self.record(f) for f in self.files])
        if method == 'GET' and path.startswith('/api/files/'):
            found = [f for f in self.files if f['id'] == path[11:]]
            return (
                http_answer(200, found[0]['content'], [('Content-Type', 'application/octet-stream')])
                if found
                else http_answer(404, b'gone')
            )
        if method == 'POST' and path.endswith('/delete'):
            self.files = [f for f in self.files if f['id'] != path[11:-7]]
            return http_answer(200, {'ok': True})
        if method == 'POST' and path in ('/api/files', '/api/send'):
            data = json.loads(request['body'])
            content = data['content'].encode()
            self.counter += 1
            identifier = hashlib.sha256(content + bytes([self.counter])).hexdigest()[:32]
            if path == '/api/files':
                self.files.insert(
                    0,
                    {
                        'id': identifier,
                        'name': data['name'],
                        'content': content,
                        'sha256': hashlib.sha256(content).hexdigest(),
                        'bytes': len(content),
                        'created': 1700000100.0,
                        'source': 'web',
                    },
                )
            return http_answer(
                200,
                {
                    'id': identifier,
                    'name': data['name'],
                    'sha256': hashlib.sha256(content).hexdigest(),
                    'bytes': len(content),
                    'status': 'stored_for_review',
                },
            )
        return http_answer(404, {'error': 'Not found'})


def cookie_header(rng):
    tokens = [
        L,
        L[:31],
        'z' * 129,
        f'"{L}"',
        '"' + ''.join(f'\\{ord(c):03o}' for c in L) + '"',
        '"\\001' + L[4:] + '"',
        '',
        'ab cd',
        'a,b',
        '[x]',
        'a"b',
        'a\\b',
        'Wed, 21 Oct 2015 07:28:00 GMT',
        'Wednesday, 21-Oct-2015 07:28:00 GMT',
        'tab\tx',
        '"x"y"',
        '"\\"' + L + '"',
        'é' + L,
        L + ';',
    ]
    names = [
        LOGIN_COOKIE,
        LOGIN_COOKIE,
        LOGIN_COOKIE.lower(),
        'other',
        '$Version',
        'a/b',
        'a=b',
        'Secure',
        'Path',
        'expires',
        'x(y',
        '$',
    ]
    attributes = [
        'Secure',
        'Secure',
        'secure',
        'SECURE',
        'Secure=',
        'Secure=1',
        'Secure=""',
        '$Secure',
        '$secure=x',
        'HttpOnly',
        'Path=/',
        'Path',
        'Max-Age=5',
        'SameSite=Strict',
        'Expires=Wed, 21 Oct 2015 07:28:00 GMT',
        'Foo',
        'Foo=bar',
        '$Foo',
        'Comment="x"',
        'Version=1',
        'Domain=x',
        'Partitioned',
    ]
    separators = ['; ', ';', ' ', ';;', ' ; ', '\t;', ', ']
    parts = []
    for _ in range(rng.randint(1, 3)):
        parts.append(f'{rng.choice(names)}={rng.choice(tokens)}' if rng.random() < 0.9 else rng.choice(names))
    for _ in range(rng.randint(0, 4)):
        parts.insert(rng.randint(0, len(parts)) if rng.random() < 0.15 else len(parts), rng.choice(attributes))
    text = ''
    for part in parts:
        text += (rng.choice(separators) if text else rng.choice(['', '', ' '])) + part
    return text


async def editor_case(tally, label, ref, got, stand_in, editor, tool, args, answer='accept', overrides=None, own=None):
    outcomes = []
    for side in (ref, got):
        editor.reset(overrides)
        stand_in.requests.clear()
        side.answer, side.prompts[:] = answer, []
        result = value_of(await side.call(tool, args))
        outcomes.append([normal(result), [request_form(r) for r in stand_in.requests], list(side.prompts)])
    tally.compare(label, *outcomes, own=own)


def artifacts(outputs):
    """The same workshop artifacts in both servers' folders."""
    contents = {
        'a' * 32 + '.nc': b'G0 X5 Y5\nG1 Z-1 F100\n',
        'b' * 32 + '.svg': b'<svg xmlns="http://www.w3.org/2000/svg"><path d="M0 0L1 1"/></svg>',
        'c' * 32 + '.nc': b'\xff\xfe not text',
        'd' * 32 + '.nc': b'',
        'e' * 32 + '.nc': b'G0\x00X1\n',
        'f' * 32 + '.nc': b'G0 X1\n' * 1398102,
        '1' * 32 + '.cncstudio.json': '{"format": "pi-cnc-studio/1", "name": "é"}'.encode(),
    }
    for output in outputs:
        output.mkdir(parents=True, exist_ok=True)
        for name, data in contents.items():
            (output / name).write_bytes(data)
    return list(contents)


async def hosted(tally, program, work, rng, quick=False):
    relay = Path(program).with_name('kerfkite-pendant-tls')
    cert, key = certificate(work, relay, 'editor')
    holder = {}
    stand_in = StandIn(lambda request: holder['editor'].answer(request), cert, key)
    origin = f'https://127.0.0.1:{stand_in.port}'
    editor = holder['editor'] = Editor(origin)
    env = {
        'KERFKITE_FILES_URL': origin + '/',
        'KERFKITE_FILES_USERNAME': 'Parity',
        'KERFKITE_FILES_PASSWORD': PASSWORD,
        'KERFKITE_FILES_CA_FILE': str(cert),
    }
    output = work / 'hosted'
    names = artifacts([output / 'reference', output / 'native'])
    reference = reference_server(output / 'reference', env)
    native = Native(program, output / 'native', env)
    first, second = editor.files[0]['id'], editor.files[1]['id']

    async def both(label, tool, args, answer='accept', overrides=None, own=None):
        await editor_case(tally, label, ref, native, stand_in, editor, tool, args, answer, overrides, own)

    try:
        async with Reference(reference) as ref:
            await listed(tally, 'tools listed with an editor account', ref, native)
            await both('list', 'hosted_list_files', {})
            await both('destinations', 'hosted_destinations', {})
            for label, reply in (
                ('object', {'a': 1}),
                ('text', 'bench'),
                ('number', 5),
                ('null', None),
                ('list of others', [1, ['bench'], None]),
                ('nothing', []),
            ):
                await both(
                    f'destinations {label}',
                    'hosted_destinations',
                    {},
                    overrides={('GET', '/api/destinations'): [http_answer(200, reply)]},
                )
            for label, reply in (
                ('object', {'x': {}}),
                ('items not objects', [1]),
                ('text items', ['ab']),
                ('list items', [[1]]),
                ('null items', [None]),
                ('missing key', [{'id': 'x', 'name': 'n', 'bytes': 1, 'sha256': 's', 'created': 1}]),
                ('number', 3),
                ('text', 'abc'),
                ('null', None),
            ):
                await both(
                    f'list {label}',
                    'hosted_list_files',
                    {},
                    overrides={('GET', '/api/files'): [http_answer(200, reply)]},
                )
            for label, replies in (
                ('expired then fine', [http_answer(401, {})]),
                ('expired twice', [http_answer(401, {}), http_answer(401, {})]),
                ('500', [http_answer(500, {'error': 'x'})]),
                ('403', [http_answer(403, b'')]),
                ('302', [http_answer(302, b'', [('Location', '/')])]),
                ('not JSON', [http_answer(200, b'nope')]),
                ('byte order mark', [http_answer(200, b'\xef\xbb\xbf[]')]),
                ('not UTF-8', [http_answer(200, b'["\xff"]')]),
                ('largest', [http_answer(200, b'[]' + b' ' * 262142)]),
                ('too large', [http_answer(200, b'[]' + b' ' * 262143)]),
                ('closed', ['close']),
                ('cut short', [http_answer(200, b'[]', length=9)]),
                ('101 headers', [http_answer(200, [], [(f'X-{i}', 'v') for i in range(99)])]),
            ):
                await both(f'list {label}', 'hosted_list_files', {}, overrides={('GET', '/api/files'): replies})
            # Logging in: the form, the session, the verification.
            login = {
                'form 404': {('GET', '/login'): [http_answer(404, b'')]},
                'form redirect': {('GET', '/login'): [http_answer(302, b'', [('Location', '/x')])]},
                'no login cookie': {('GET', '/login'): [http_answer(200, b'')]},
                'login cookie not secure': {
                    ('GET', '/login'): [http_answer(200, b'', [('Set-Cookie', f'{LOGIN_COOKIE}={L}; Path=/')])]
                },
                'wrong password': {('POST', '/login'): [http_answer(200, b'<form>')]},
                'login elsewhere': {
                    ('POST', '/login'): [
                        http_answer(302, b'', [('Location', '/x'), ('Set-Cookie', f'{SESSION_COOKIE}={S}; Secure')])
                    ]
                },
                'two locations': {
                    ('POST', '/login'): [
                        http_answer(
                            302,
                            b'',
                            [('Location', '/'), ('Location', '/x'), ('Set-Cookie', f'{SESSION_COOKIE}={S}; Secure')],
                        )
                    ]
                },
                'no session cookie': {('POST', '/login'): [http_answer(302, b'', [('Location', '/')])]},
                'session 401': {('GET', '/api/session'): [http_answer(401, {})]},
                'session not JSON': {('GET', '/api/session'): [http_answer(200, b'x')]},
                'session list': {('GET', '/api/session'): [http_answer(200, [])]},
                'session not web': {('GET', '/api/session'): [http_answer(200, {'token': T, 'web_mode': 1})]},
                'session short token': {
                    ('GET', '/api/session'): [http_answer(200, {'token': T[:31], 'web_mode': True})]
                },
                'session no token': {('GET', '/api/session'): [http_answer(200, {'web_mode': True})]},
                # Header lines as http.client's email parser reads them.
                'location with a space after': {
                    ('POST', '/login'): [
                        http_answer(302, b'', [('Location', '/ '), ('Set-Cookie', f'{SESSION_COOKIE}={S}; Secure')])
                    ]
                },
                'location with a tab after': {
                    ('POST', '/login'): [
                        http_answer(302, b'', [('Location', '/\t'), ('Set-Cookie', f'{SESSION_COOKIE}={S}; Secure')])
                    ]
                },
                'location twice': {
                    ('POST', '/login'): [
                        http_answer(
                            302,
                            b'',
                            [('Location', '/'), ('location', '/x'), ('Set-Cookie', f'{SESSION_COOKIE}={S}; Secure')],
                        )
                    ]
                },
                'location folded': {
                    ('POST', '/login'): [
                        http_answer(
                            302, b'', [('Location', '/\r\n x'), ('Set-Cookie', f'{SESSION_COOKIE}={S}; Secure')]
                        )
                    ]
                },
                'cookie folded': {
                    ('POST', '/login'): [
                        http_answer(302, b'', [('Location', '/'), ('Set-Cookie', f'{SESSION_COOKIE}={S};\r\n Secure')])
                    ]
                },
                'cookie folded in quotes': {
                    ('POST', '/login'): [
                        http_answer(
                            302, b'', [('Location', '/'), ('Set-Cookie', f'{SESSION_COOKIE}="{S}\r\n "; Secure')]
                        )
                    ]
                },
                'cookie with spaces after': {
                    ('POST', '/login'): [
                        http_answer(302, b'', [('Location', '/'), ('Set-Cookie', f'{SESSION_COOKIE}={S}; Secure   ')])
                    ]
                },
                'cookie field named with a space': {
                    ('POST', '/login'): [
                        http_answer(302, b'', [('Location', '/'), ('Set-Cookie ', f'{SESSION_COOKIE}={S}; Secure')])
                    ]
                },
                'cookie after a line that is not a field': {
                    ('POST', '/login'): [
                        http_answer(
                            302,
                            b'',
                            [('Location', '/'), ('Not a field', 'x'), ('Set-Cookie', f'{SESSION_COOKIE}={S}; Secure')],
                        )
                    ]
                },
                'cookie after an envelope line': {
                    ('POST', '/login'): [
                        http_answer(
                            302,
                            b'',
                            [('Location', '/'), ('From x', ''), ('Set-Cookie', f'{SESSION_COOKIE}={S}; Secure')],
                        )
                    ]
                },
                'cookie with latin-1': {
                    ('POST', '/login'): [
                        http_answer(
                            302, b'', [('Location', '/'), ('Set-Cookie', f'{SESSION_COOKIE}={S}\u00e9; Secure')]
                        )
                    ]
                },
            }
            for label, overrides in login.items():
                await both(
                    f'login {label}',
                    'hosted_list_files',
                    {},
                    overrides={('GET', '/api/files'): [http_answer(401, {})], **overrides},
                )
            # Logins that succeed only for the reference, each followed by one that logs both out again: a token of 34
            # digits as a number (Python's int keeps them, kerfkite-mcp reads the nearest double), and a session cookie
            # after a lone CR in another field, which Go's HTTP client refuses as a control character.
            for label, overrides, rule in (
                (
                    'session number token',
                    {('GET', '/api/session'): [http_answer(200, {'token': 10**33, 'web_mode': True})]},
                    'integers past 64 bits',
                ),
                (
                    'cookie after a lone CR',
                    {
                        ('POST', '/login'): [
                            http_answer(
                                302, b'', [('Location', '/'), ('X-A', f'1\rSet-Cookie: {SESSION_COOKIE}={S}; Secure')]
                            )
                        ]
                    },
                    'control characters in a header',
                ),
            ):
                await both(
                    f'login {label}',
                    'hosted_list_files',
                    {},
                    overrides={('GET', '/api/files'): [http_answer(401, {})], **overrides},
                    own=rule,
                )
                await both(
                    f'login after {label}',
                    'hosted_list_files',
                    {},
                    overrides={
                        ('GET', '/api/files'): [http_answer(401, {})],
                        ('GET', '/login'): [http_answer(404, b'')],
                    },
                    own=rule,
                )
            await both(
                'login cookie with a control character',
                'hosted_list_files',
                {},
                overrides={
                    ('GET', '/login'): [http_answer(200, b'', [('Set-Cookie', f'{LOGIN_COOKIE}={L}; Secure=\x01')])],
                    ('GET', '/api/session'): [http_answer(200, {'web_mode': False})],
                },
                own='control characters in a header',
            )
            # The cookie parser, on generated Set-Cookie headers: the login cookie, then the session's.
            failing = [http_answer(200, {'web_mode': False})] * 3
            for index in range(40 if quick else 400):
                headers = [('Set-Cookie', cookie_header(rng)) for _ in range(rng.randint(1, 3))]
                which = rng.random() < 0.5
                overrides = {('GET', '/api/session'): failing, ('GET', '/api/files'): [http_answer(401, {})]}
                if which:
                    overrides[('GET', '/login')] = [http_answer(200, b'', headers)] * 2
                else:
                    overrides[('POST', '/login')] = [http_answer(302, b'', [('Location', '/')] + headers)] * 2
                await both(
                    f'cookie {index} {"login" if which else "session"} {short(headers)}',
                    'hosted_list_files',
                    {},
                    overrides=overrides,
                )
            await both('list after', 'hosted_list_files', {})
            # Uploads.
            for name in names:
                for filename in (
                    'part.nc',
                    'Part.NC',
                    'bad name.nc',
                    '../x.nc',
                    'x.txt',
                    'a' * 129 + '.nc',
                    'é.svg',
                    'x.paths3d.json',
                ):
                    await both(
                        f'upload {name[:34]} {filename}', 'hosted_upload_file', {'artifact': name, 'name': filename}
                    )
            for artifact in ('0' * 32 + '.nc', 'not-an-artifact', 'A' * 32 + '.nc', ''):
                await both(
                    f'upload artifact {artifact!r}', 'hosted_upload_file', {'artifact': artifact, 'name': 'part.nc'}
                )
            receipt = {
                'id': 'f' * 32,
                'name': 'part.nc',
                'bytes': 21,
                'sha256': hashlib.sha256(b'G0 X5 Y5\nG1 Z-1 F100\n').hexdigest(),
                'status': 'stored_for_review',
            }
            for label, reply in (
                ('right', receipt),
                ('other checksum', {**receipt, 'sha256': '0' * 64}),
                ('other status', {**receipt, 'status': 'ok'}),
                ('bytes text', {**receipt, 'bytes': '21'}),
                ('bytes float', {**receipt, 'bytes': 21.0}),
                ('other name', {**receipt, 'name': 'x.nc'}),
                ('number id', {**receipt, 'id': 12345678901234567890123456789012}),
                ('short id', {**receipt, 'id': 'f' * 31}),
                ('upper id', {**receipt, 'id': 'F' * 32}),
                ('no id', {k: v for k, v in receipt.items() if k != 'id'}),
                ('extra', {**receipt, 'created': 5}),
                ('list', [receipt]),
                ('rejected', None),
            ):
                answer = (
                    http_answer(400, {'error': 'Upload must contain text'})
                    if reply is None
                    else http_answer(200, reply)
                )
                await both(
                    f'upload receipt {label}',
                    'hosted_upload_file',
                    {'artifact': names[0], 'name': 'part.nc'},
                    overrides={('POST', '/api/files'): [answer]},
                    own='integers past 64 bits' if label == 'number id' else None,
                )
            # Downloads.
            for file_id in (first, second, '0' * 32, first.upper(), first[:31], 'x' * 32, ''):
                await both(f'download {file_id!r}', 'hosted_download_file', {'file_id': file_id})
            record = editor.record(editor.files[0])
            for label, entry in (
                ('size', {**record, 'bytes': 99}),
                ('size float', {**record, 'bytes': 6.0}),
                ('size text', {**record, 'bytes': '6'}),
                ('checksum', {**record, 'sha256': '0' * 64}),
                ('no size', {k: v for k, v in record.items() if k != 'bytes'}),
                ('no checksum', {k: v for k, v in record.items() if k != 'sha256'}),
                ('no checksum, size', {k: v for k, v in record.items() if k != 'sha256'} | {'bytes': 3}),
                ('no name', {k: v for k, v in record.items() if k != 'name'}),
                ('bad name', {**record, 'name': '../x.nc'}),
                ('other kind', {**record, 'name': 'x.exe'}),
                ('number name', {**record, 'name': 5}),
            ):
                await both(
                    f'download {label}',
                    'hosted_download_file',
                    {'file_id': first},
                    overrides={('GET', '/api/files'): [http_answer(200, [entry])]},
                )
            for label, data in (
                ('invalid start', b'G0 \xff X1'),
                ('cut short', b'G0 \xe2\x82'),
                ('continuation', b'\xe2\x28\xa1'),
                ('second continuation', b'\xf0\x90\x28\xbc'),
                ('surrogate', b'\xed\xa0\x80'),
                ('overlong', b'\xc0\xaf'),
                ('four cut', b'\xf0\x90\x80'),
                ('past range', b'\xf4\x90\x80\x80'),
                ('late', b'G1' * 50 + b'\xf8'),
                ('nul', b'G0\x00'),
                ('empty', b''),
            ):
                entry = {**record, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                await both(
                    f'download content {label}',
                    'hosted_download_file',
                    {'file_id': first},
                    overrides={
                        ('GET', '/api/files'): [http_answer(200, [entry])],
                        ('GET', f'/api/files/{first}'): [http_answer(200, data)],
                    },
                )
            await both(
                'download too large',
                'hosted_download_file',
                {'file_id': first},
                overrides={('GET', f'/api/files/{first}'): [http_answer(200, b'x' * (8 * 1024 * 1024 + 1))]},
            )
            # Deleting.
            for answer in ('accept', 'decline', 'cancel'):
                await both(f'delete {answer}', 'hosted_delete_file', {'file_id': first}, answer)
            for label, reply in (
                ('not ok', {'ok': False}),
                ('ok text', {'ok': 'yes'}),
                ('list', []),
                ('refused', None),
            ):
                answer = http_answer(500, {}) if reply is None else http_answer(200, reply)
                await both(
                    f'delete reply {label}',
                    'hosted_delete_file',
                    {'file_id': first},
                    overrides={('POST', f'/api/files/{first}/delete'): [answer]},
                )
            await both(
                'delete no name',
                'hosted_delete_file',
                {'file_id': first},
                overrides={
                    ('GET', '/api/files'): [http_answer(200, [{k: v for k, v in record.items() if k != 'name'}])]
                },
            )
            await both(
                'delete size float',
                'hosted_delete_file',
                {'file_id': first},
                'decline',
                overrides={('GET', '/api/files'): [http_answer(200, [{**record, 'bytes': 6.5, 'name': None}])]},
            )
            await both('delete unknown', 'hosted_delete_file', {'file_id': '9' * 32})
            # Sending to a receiver.
            for args in (
                {'destination': 'bench', 'file_id': first},
                {'destination': 'bench', 'artifact': names[0], 'name': 'part.nc'},
                {'destination': 'bench'},
                {'destination': 'bench', 'file_id': first, 'name': 'x.nc'},
                {'destination': 'bench', 'file_id': first, 'artifact': names[0]},
                {'destination': 'bench', 'artifact': names[0]},
                {'destination': 'nowhere', 'file_id': first},
                {'destination': '', 'file_id': first},
                {'destination': 'lase', 'file_id': first},
                {'destination': 'laser', 'artifact': names[2], 'name': 'part.nc'},
                {'destination': 'laser', 'file_id': '0' * 32},
            ):
                for answer in ('accept', 'decline'):
                    await both(f'send {short(args)} {answer}', 'hosted_send_file', args, answer)
            for label, reply in (('text', 'bench laser'), ('object', {'bench': 1}), ('number', 5), ('null', None)):
                await both(
                    f'send destinations {label}',
                    'hosted_send_file',
                    {'destination': 'bench', 'file_id': first},
                    overrides={('GET', '/api/destinations'): [http_answer(200, reply)]},
                )
            await both(
                'send receipt not verified',
                'hosted_send_file',
                {'destination': 'bench', 'file_id': first},
                overrides={('POST', '/api/send'): [http_answer(200, {'status': 'stored_for_review'})]},
            )
            await both(
                'send rejected',
                'hosted_send_file',
                {'destination': 'bench', 'file_id': first},
                overrides={
                    ('POST', '/api/send'): [http_answer(400, {'error': 'Choose an operator-configured receiver'})]
                },
            )
    finally:
        native.close()
    # Without a client that can ask, and with an editor that cannot be reached.
    output = work / 'hosted-unasked'
    names = artifacts([output / 'reference', output / 'native'])
    reference = reference_server(output / 'reference', env)
    native = Native(program, output / 'native', env, confirms=False)
    try:
        async with Reference(reference, confirms=False) as ref:
            await both('unasked delete', 'hosted_delete_file', {'file_id': first})
            await both('unasked send', 'hosted_send_file', {'destination': 'bench', 'file_id': first})
            await both('unasked upload', 'hosted_upload_file', {'artifact': names[0], 'name': 'part.nc'})
    finally:
        native.close()
    wrong, _ = certificate(work, relay, 'wrong')
    for address, authority in (
        (f'https://127.0.0.1:{stand_in.port}', wrong),
        ('https://127.0.0.1:1', cert),
        ('https://[fe80::abcd]', cert),
        ('https://[::1]', cert),
        ('https://[1::]', cert),
    ):
        variables = {**env, 'KERFKITE_FILES_URL': address, 'KERFKITE_FILES_CA_FILE': str(authority)}
        output = work / f'hosted-{hashlib.sha256(address.encode()).hexdigest()[:8]}'
        reference = reference_server(output / 'reference', variables)
        native = Native(program, output / 'native', variables)
        try:
            async with Reference(reference) as ref:
                await both(f'unreachable {address}', 'hosted_list_files', {})
        finally:
            native.close()


# ---- KerfKite's own hosted editor -------------------------------------------------------------------------------


class Receiver:
    """A receiver's upload inbox: it stores nothing and confirms what it was sent."""

    def __init__(self):
        self.stand_in = StandIn(self.answer)

    def answer(self, request):
        data = json.loads(request['body'])
        content = data['content'].encode()
        digest = hashlib.sha256(content).hexdigest()
        return http_answer(
            200,
            {
                'id': digest[:32],
                'name': data['name'],
                'sha256': digest,
                'bytes': len(content),
                'status': 'stored_for_review',
            },
        )


def kerfkite_web(program, data_dir, port):
    cert, key = data_dir.parent / f'{data_dir.name}.pem', data_dir.parent / f'{data_dir.name}-key.pem'
    subprocess.run(  # noqa: S603 - the programs under test
        [program, 'certificate', '--cert', str(cert), '--key', str(key), '--hosts', '127.0.0.1', 'localhost'],
        check=True,
        capture_output=True,
    )
    log = data_dir.parent / f'{data_dir.name}.log'
    with log.open('w') as out:
        process = subprocess.Popen(  # noqa: S603 - the programs under test
            [
                program,
                '--data-dir',
                str(data_dir),
                '--static',
                str(ROOT / 'studio' / 'static'),
                'serve',
                '--public-url',
                f'https://127.0.0.1:{port}',
                '--cert',
                str(cert),
                '--key',
                str(key),
                '--host',
                '127.0.0.1',
                '--port',
                str(port),
            ],
            stdout=out,
            stderr=subprocess.STDOUT,
        )
    for _ in range(300):
        if 'HTTPS editor and upload inbox' in log.read_text() or process.poll() is not None:
            break
        time.sleep(0.05)
    if process.poll() is not None:
        raise SystemExit(f'kerfkite-web did not start: {log.read_text()!r}')
    for _ in range(100):
        with contextlib.suppress(OSError), socket.create_connection(('127.0.0.1', port), timeout=1):
            break
        time.sleep(0.05)
    return process, cert


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


async def web(tally, program, web_program, work):
    from werkzeug.security import generate_password_hash

    from studio.web_store import WebStore

    receiver = Receiver()
    base = work / 'web-store'
    store = WebStore(base)
    with store.db() as db:
        db.execute(
            'INSERT INTO users VALUES(?,?)',
            ('parity', generate_password_hash(PASSWORD, method='pbkdf2:sha256:600000', salt_length=32)),
        )
    for index in range(3):
        store.put_file('parity', f'stored-{index}.nc', f'G0 X{index}\n'.encode(), 'web' if index % 2 else 'upload')
        time.sleep(0.01)
    (base / 'destinations.json').write_text(
        json.dumps(
            {
                'bench': {
                    'url': f'http://127.0.0.1:{receiver.stand_in.port}',
                    'ca_file': None,
                    'allow_http': True,
                    'token': 'r' * 40,
                }
            }
        )
    )
    sides, processes = [], []
    try:
        for name in ('reference', 'native'):
            data_dir = work / f'web-{name}'
            shutil.copytree(base, data_dir)
            port = free_port()
            process, cert = kerfkite_web(web_program, data_dir, port)
            processes.append(process)
            sides.append(
                {
                    'KERFKITE_FILES_URL': f'https://127.0.0.1:{port}',
                    'KERFKITE_FILES_USERNAME': 'parity',
                    'KERFKITE_FILES_PASSWORD': PASSWORD,
                    'KERFKITE_FILES_CA_FILE': str(cert),
                }
            )
        output = work / 'web'
        names = artifacts([output / 'reference', output / 'native'])
        reference = reference_server(output / 'reference', sides[0])
        native = Native(program, output / 'native', sides[1])
        try:
            async with Reference(reference) as ref:
                kept = {}

                async def step(label, tool, args, answer='accept', keep=None):
                    results = []
                    for side, key in ((ref, 'ref'), (native, 'got')):
                        side.answer = answer
                        given = {k: (kept[key][v] if isinstance(v, tuple) else v) for k, v in args.items()}
                        result = value_of(await side.call(tool, given))
                        if keep:
                            kept.setdefault(key, {}).update(keep(result))
                        results.append(normal(result))
                    tally.compare(f'web {label}', *results)

                def ids(result):
                    value = result[1]
                    files = value.get('result', value) if isinstance(value, dict) else []
                    return {('file', i): f['id'] for i, f in enumerate(files)} if isinstance(files, list) else {}

                await step('destinations', 'hosted_destinations', {})
                await step('list', 'hosted_list_files', {}, keep=ids)
                await step(
                    'upload',
                    'hosted_upload_file',
                    {'artifact': names[0], 'name': 'mcp-part.nc'},
                    keep=lambda r: {'upload': r[1].get('id')} if r[0] == 'value' else {},
                )
                await step('list after upload', 'hosted_list_files', {}, keep=ids)
                await step('download', 'hosted_download_file', {'file_id': ('file', 1)})
                await step('send stored file', 'hosted_send_file', {'destination': 'bench', 'file_id': ('file', 2)})
                await step(
                    'send artifact declined',
                    'hosted_send_file',
                    {'destination': 'bench', 'artifact': names[1], 'name': 'mcp.svg'},
                    'decline',
                )
                await step(
                    'send artifact',
                    'hosted_send_file',
                    {'destination': 'bench', 'artifact': names[1], 'name': 'mcp.svg'},
                )
                await step(
                    'send to an unknown receiver', 'hosted_send_file', {'destination': 'nobody', 'file_id': ('file', 2)}
                )
                await step('delete cancelled', 'hosted_delete_file', {'file_id': ('file', 0)}, 'cancel')
                await step('delete', 'hosted_delete_file', {'file_id': ('file', 0)})
                await step('download deleted', 'hosted_download_file', {'file_id': ('file', 0)})
                await step('list at the end', 'hosted_list_files', {})
        finally:
            native.close()
    finally:
        for process in processes:
            process.terminate()
            process.wait(10)


async def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--server', required=True, help='kerfkite-mcp, with kerfkite-pendant-tls beside it')
    parser.add_argument('--web', help='kerfkite-web, for the session against KerfKite\'s own hosted editor')
    parser.add_argument('--part', choices=('all', 'config', 'pendant', 'hosted', 'web'), default='all')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--quick', action='store_true', help='fewer generated cases')
    parser.add_argument('--trace', action='store_true', help='print every case')
    a = parser.parse_args()
    logging.getLogger('fastmcp').setLevel(logging.CRITICAL)  # a refused call is part of the comparison, not news
    logging.getLogger('mcp').setLevel(logging.CRITICAL)
    rng = random.Random(a.seed)  # noqa: S311 - generated test cases, not secrets
    tally = Tally(a.trace)
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        if a.part in ('all', 'config'):
            configuration(tally, a.server, work, rng)
        if a.part in ('all', 'pendant'):
            await pendant(tally, a.server, work, rng, a.quick)
        if a.part in ('all', 'hosted'):
            await hosted(tally, a.server, work, rng, a.quick)
        if a.part in ('all', 'web') and a.web:
            await web(tally, a.server, a.web, work)
    print(
        f'{tally.cases} cases: {tally.same} the same, {tally.own} different by kerfkite-mcp\'s own rules, '
        f'{len(tally.differences)} different'
    )
    return 1 if tally.differences else 0


if __name__ == '__main__':
    sys.exit(asyncio.run(main()))
