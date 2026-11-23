"""HTTP-transport parity: the C++ kerfkite-mcp --transport http against the Python reference's
(studio.mcp_server --transport http, over FastMCP, Starlette and uvicorn), both with one operator's OAuth settings.

Everything runs in network and mount namespaces of its own (unshare), with no proxy: the loopback interface
carries public addresses (1.0.0.1 and up) for key-set stand-ins, HTTPS servers with a certificate authority made
for the run (SSL_CERT_FILE), and /etc/hosts names for them and for addresses that must be refused. Each side has
stand-ins of its own that answer alike, so that what each server fetched can be compared.

The parts:

- http: requests to both servers in step: the Host and Origin guard, the routes and their methods, the metadata
  and its CORS, the bearer header and generated tokens (claims, headers, signatures, encodings), and messages
  (media types, protocol versions, JSON that is not JSON-RPC, every method with good and bad parameters, and
  calls of the tools that need no browser).
- keys: a pair of fresh servers for each key set (several keys, none, kid-less keys, unusable and unsupported
  keys, EC keys, sets that are not JSON or not objects, statuses, oversized answers), then tokens that hit and
  miss it, refreshes and their 30-second limit, and the addresses a key set may not be fetched from.
- config: both servers started with each of a few hundred environments: both must refuse alike (the message word
  for word) or start and serve the same metadata.
- stdio: lines sent to both servers over standard input: what is answered, and what is reported instead.

Statuses, the headers that carry meaning (types, challenges, Allow as a set, Location, caching, CORS), bodies and
event data must agree; the text after "Parse error:" and "Validation error:" is kerfkite-mcp's own (docs/NATIVE.md).
Run it on Linux with the reference's Python (fastmcp installed) beside a build:

    PYTHONPATH=$PWD python validation/mcp_http_parity.py --server /tmp/kkb/kerfkite-mcp [--part all] [--seed 1]

kerfkite-pendant-tls must be beside kerfkite-mcp. It prints one line per difference and a summary, and exits 1
when any case differs.
"""

import argparse
import base64
import fcntl
import http.client
import json
import os
import random
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'validation'))

PROXIES = ('HTTPS_PROXY', 'HTTP_PROXY', 'https_proxy', 'http_proxy', 'ALL_PROXY', 'all_proxy', 'NO_PROXY', 'no_proxy')
NAMES = ('KERFKITE_MCP_PUBLIC_ORIGIN', 'KERFKITE_MCP_ISSUER', 'KERFKITE_MCP_JWKS_URL', 'KERFKITE_MCP_SUBJECT')
ORIGIN, ISSUER, SUBJECT = 'https://workshop.example.com', 'https://issuer.example.com', 'operator-1'
AUDIENCE, SCOPE = ORIGIN + '/mcp', 'kerfkite:workshop'
STANDINS = {'a': '1.0.0.1', 'b': '1.0.0.2', 'c': '1.0.0.3', 'd': '1.0.0.4'}
HOSTS = {
    'keys-a.example': '1.0.0.1',
    'keys-b.example': '1.0.0.2',
    'keys-c.example': '1.0.0.3',
    'keys-d.example': '1.0.0.4',
    'private.example': '10.0.0.1',
    'loop.example': '127.0.0.1',
    'shared.example': '100.64.0.1',
}


def b64(data):
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode()


def number_bytes(n):
    return n.to_bytes(max(1, (n.bit_length() + 7) // 8), 'big')


# ---- the namespaces ------------------------------------------------------------------------------------------


def enter_namespaces():
    """Run this script again in network and mount namespaces of its own, without proxy variables."""
    if os.environ.get('KERFKITE_PARITY_NETNS') == '1':
        return
    env = {k: v for k, v in os.environ.items() if k not in PROXIES}
    env['KERFKITE_PARITY_NETNS'] = '1'
    for flags in (['-rnm'], ['-nm']):
        probe = subprocess.run(['unshare', *flags, 'true'], capture_output=True)  # noqa: S603, S607 - a fixed tool
        if probe.returncode == 0:
            os.execvpe('unshare', ['unshare', *flags, sys.executable, *sys.argv], env)  # noqa: S606, S607
    raise SystemExit('This harness needs unshare(1) and network and mount namespaces (Linux).')


def set_up_network(work):
    SIOCGIFFLAGS, SIOCSIFFLAGS, SIOCSIFADDR, SIOCSIFNETMASK = 0x8913, 0x8914, 0x8916, 0x891C
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    flags = struct.unpack('16sh14s', fcntl.ioctl(s, SIOCGIFFLAGS, struct.pack('16sh14s', b'lo', 0, b'\0' * 14)))[1]
    fcntl.ioctl(s, SIOCSIFFLAGS, struct.pack('16sh14s', b'lo', flags | 1, b'\0' * 14))
    for k, address in enumerate(sorted(STANDINS.values())):
        name = f'lo:{k + 1}'.encode()

        def pack(a, name=name):
            return struct.pack('16sH2s4s8s', name, socket.AF_INET, b'\0\0', socket.inet_aton(a), b'\0' * 8)

        fcntl.ioctl(s, SIOCSIFADDR, pack(address))
        fcntl.ioctl(s, SIOCSIFNETMASK, pack('255.255.255.255'))
    hosts = work / 'hosts'
    hosts.write_text('127.0.0.1 localhost\n::1 localhost\n' + ''.join(f'{a} {n}\n' for n, a in HOSTS.items()))
    subprocess.run(['mount', '--bind', str(hosts), '/etc/hosts'], check=True)  # noqa: S603, S607 - in our namespace


# ---- key-set stand-ins ---------------------------------------------------------------------------------------


class StandIn:
    """An HTTPS server for a key set: a scripted answer, and a log of the requests."""

    def __init__(self, address, port, cert, key):
        self.answer = (200, [('Content-Type', 'application/json')], b'{"keys": []}')
        self.log = []
        standin = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def do_GET(self):  # noqa: N802 - the handler's name
                standin.log.append((self.command, self.path, self.headers.get('Host')))
                status, headers, body = standin.answer
                self.send_response_only(status)
                for name, value in headers:
                    self.send_header(name, value)
                if not any(n.lower() == 'content-length' for n, _ in headers):
                    self.send_header('Content-Length', str(len(body)))
                self.send_header('Connection', 'close')
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer((address, port), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def take(self):
        out, self.log = self.log, []
        return out


# ---- the servers ---------------------------------------------------------------------------------------------


def own_host(host, side):
    """A Host header a stand-in received, with the side's own stand-in name or address made the same."""
    if host is None:
        return None
    for name in (
        ('keys-a.example', '1.0.0.1', 'keys-c.example', '1.0.0.3'),
        ('keys-b.example', '1.0.0.2', 'keys-d.example', '1.0.0.4'),
    )[side]:
        host = host.replace(
            name,
            '<'
            + ('keys' if 'keys' in name else 'address')
            + ('-x' if name[5:6] in 'cd' or name.endswith(('.3', '.4')) else '')
            + '>',
        )
    return host


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Servers:
    """The reference and kerfkite-mcp over HTTP, each with its own key-set address."""

    def __init__(
        self, program, work, cert, env_extra=None, jwks=('https://keys-a.example/jwks', 'https://keys-b.example/jwks')
    ):
        self.ports = (free_port(), free_port())
        self.processes, self.logs = [], []
        base = {k: v for k, v in os.environ.items() if k not in NAMES and k not in PROXIES}
        base.update(
            {
                'KERFKITE_MCP_PUBLIC_ORIGIN': ORIGIN,
                'KERFKITE_MCP_ISSUER': ISSUER,
                'KERFKITE_MCP_SUBJECT': SUBJECT,
                'SSL_CERT_FILE': str(cert),
                'PYTHONPATH': str(ROOT),
                **(env_extra or {}),
            }
        )
        commands = (
            [
                sys.executable,
                '-m',
                'studio.mcp_server',
                '--transport',
                'http',
                '--port',
                str(self.ports[0]),
                '--output-dir',
                str(work / 'ref-out'),
            ],
            [program, '--transport', 'http', '--port', str(self.ports[1]), '--output-dir', str(work / 'cpp-out')],
        )
        for side, (command, url) in enumerate(zip(commands, jwks)):
            log = (work / f'server-{side}.log').open('w')
            env = dict(base, KERFKITE_MCP_JWKS_URL=url)
            self.processes.append(subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT))  # noqa: S603
            self.logs.append(log)
        for port, process in zip(self.ports, self.processes):
            for _ in range(400):
                if process.poll() is not None:
                    raise SystemExit(f'a server stopped: {process.args}')
                try:
                    socket.create_connection(('127.0.0.1', port), timeout=0.2).close()
                    break
                except OSError:
                    time.sleep(0.05)
        self.outputs = (work / 'ref-out', work / 'cpp-out')

    def close(self):
        for process in self.processes:
            process.terminate()
        for process in self.processes:
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
        for log in self.logs:
            log.close()


def ask(port, method, path, headers=(), body=None, timeout=60):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=timeout)
    try:
        connection.putrequest(method, path, skip_host=True, skip_accept_encoding=True)
        for name, value in headers:
            connection.putheader(name, value)
        if body is not None:
            connection.putheader('Content-Length', str(len(body)))
        connection.endheaders(body)
        response = connection.getresponse()
        data = response.read() if method != 'HEAD' else b''
        return response.status, [(k.lower(), v) for k, v in response.getheaders()], data
    except (ConnectionError, http.client.HTTPException) as error:
        return 'connection failed: ' + type(error).__name__, [], b''
    finally:
        connection.close()


# ---- comparison ----------------------------------------------------------------------------------------------

COMPARED = (
    'content-type',
    'www-authenticate',
    'allow',
    'location',
    'cache-control',
    'vary',
    'access-control-allow-origin',
    'access-control-allow-methods',
    'access-control-max-age',
    'access-control-allow-headers',
    'x-accel-buffering',
)


def own_message(value):
    """The texts of kerfkite-mcp's own: what follows "Parse error:" and "Validation error:"."""
    if isinstance(value, dict):
        out = {k: own_message(v) for k, v in value.items()}
        message = out.get('message')
        if isinstance(message, str):
            for prefix in ('Parse error:', 'Validation error:'):
                if message.startswith(prefix):
                    out['message'] = prefix + ' <own>'
        return out
    if isinstance(value, list):
        return [own_message(v) for v in value]
    return value


def schema_refusal(value):
    """A tool's arguments refused by both schemas, each in its own words (as validation/mcp_parity.py counts them)."""
    if isinstance(value, dict) and value.get('isError') and isinstance(value.get('content'), list) and value['content']:
        text = value['content'][0].get('text', '')
        if (
            'validation error' in text.split('\n')[0]
            and ' for call[' in text.split('\n')[0]
            or text.startswith('Invalid arguments for tool ')
        ):
            return {**value, 'content': [{'type': 'text', 'text': '<refused by the schema>'}]}
    if isinstance(value, dict):
        return {k: schema_refusal(v) for k, v in value.items()}
    if isinstance(value, list):
        return [schema_refusal(v) for v in value]
    return value


def with_artifacts(formed, value, files):
    """The files a result names (*_artifact) in its place, as validation/mcp_parity.py reads written files."""
    contents = []

    def walk(v):
        if isinstance(v, dict):
            for k, item in v.items():
                if k.endswith('_artifact') and isinstance(item, str) and (files / item).is_file():
                    data = (files / item).read_bytes()
                    if item.endswith('.json'):
                        from mcp_parity import strip_ids

                        contents.append((k, 'json', strip_ids(json.loads(data))))
                    else:
                        contents.append((k, 'bytes', data))
                walk(item)
        elif isinstance(v, (list, tuple)):
            for item in v:
                walk(item)

    walk(value)
    return {'result': formed, 'files': sorted(contents, key=repr)}


def events(data):
    out = []
    for block in data.decode('utf-8', 'replace').replace('\r\n', '\n').split('\n\n'):
        lines = [line for line in block.split('\n') if line and not line.startswith(':')]
        if not lines:
            continue
        event = {'event': None, 'data': []}
        for line in lines:
            name, _, value = line.partition(':')
            value = value[1:] if value.startswith(' ') else value
            if name == 'event':
                event['event'] = value
            elif name == 'data':
                event['data'].append(value)
        out.append({'event': event['event'], 'data': schema_refusal(json.loads('\n'.join(event['data'])))})
    return out


def parsed_texts(value):
    """A tool result's text content read as the JSON it holds (kerfkite-mcp writes members in byte order)."""
    if isinstance(value, dict):
        out = {}
        for k, v in value.items():
            if k == 'text' and isinstance(v, str) and value.get('type') == 'text':
                try:
                    out[k] = {'<json>': json.loads(v)}
                    continue
                except ValueError:
                    pass
            out[k] = parsed_texts(v)
        return out
    if isinstance(value, list):
        return [parsed_texts(v) for v in value]
    return value


def view(answer, files=None, port=None):
    status, headers, body = answer
    shown = {}
    for name in COMPARED:
        values = [v for k, v in headers if k == name]
        if values:
            shown[name] = sorted(x.strip() for x in values[0].split(',')) if name == 'allow' else values[0]
    if port is not None and 'location' in shown:
        shown['location'] = shown['location'].replace(f':{port}/', ':<port>/')
    kind = shown.get('content-type', '')
    if kind.startswith('text/event-stream'):
        content = [{'event': e['event'], 'data': parsed_texts(e['data'])} for e in events(body)]
        if files is not None:
            content = [{'event': e['event'], 'data': files(e['data'])} for e in content]
    elif kind.startswith('application/json') and body:
        try:
            content = own_message(json.loads(body))
        except ValueError:
            content = body
    else:
        content = body
    return {'status': status, 'headers': shown, 'body': content}


class Tally:
    def __init__(self):
        self.cases = self.same = self.own = self.described = 0
        self.differences = []
        self.rules = {}

    def record(self, label, ref, got, own=None, agree=None):
        self.cases += 1
        if ref == got:
            self.same += 1
        elif agree and agree(ref, got):
            self.same += 1
            self.described += 1
        elif own:
            self.own += 1
            self.rules[own] = self.rules.get(own, 0) + 1
            print('OWN', own + ':', label[:200])
        else:
            self.differences.append(label)
            print('DIFFERENT', label[:300])
            print('  Python:', json.dumps(ref, default=repr)[:700])
            print('  C++:   ', json.dumps(got, default=repr)[:700])


def described(action):
    """design_call's geometry and extrusions: Shapely's vertices against the polygon and solid kernels', compared by
    what they describe as validation/mcp_parity.py compares them, the rest of the answer alike."""
    from validation.server_route_parity import extrusion_agrees, geometry_agrees

    agree = geometry_agrees if action == 'geometry' else extrusion_agrees

    def check(ref, got):
        results, rest = [], []
        for shown in (ref, got):
            shown = json.loads(json.dumps(shown, default=repr))
            try:
                answer = shown['body'][0]['data']['result']['result']
                results.append(answer.pop('structuredContent')['result'])
                answer.pop('content')
            except (KeyError, IndexError, TypeError, AttributeError):
                return False
            rest.append(shown)
        return rest[0] == rest[1] and agree(results[0], results[1]) is None

    return check


# ---- tokens --------------------------------------------------------------------------------------------------


class Keys:
    def __init__(self, rng):
        from cryptography.hazmat.primitives.asymmetric import ec, rsa

        self.rsa = [rsa.generate_private_key(public_exponent=65537, key_size=2048) for _ in range(3)]
        self.small = rsa.generate_private_key(public_exponent=3, key_size=1024)
        self.ec = ec.generate_private_key(ec.SECP256R1())
        self.rng = rng

    @staticmethod
    def jwk(key, **extra):
        numbers = key.public_key().public_numbers()
        return {'kty': 'RSA', 'n': b64(number_bytes(numbers.n)), 'e': b64(number_bytes(numbers.e)), **extra}

    @staticmethod
    def private_jwk(key, **extra):
        numbers = key.private_numbers()
        members = {
            'd': numbers.d,
            'p': numbers.p,
            'q': numbers.q,
            'dp': numbers.dmp1,
            'dq': numbers.dmq1,
            'qi': numbers.iqmp,
        }
        return {**Keys.jwk(key), **{k: b64(number_bytes(v)) for k, v in members.items()}, **extra}

    def ec_jwk(self, **extra):
        numbers = self.ec.public_key().public_numbers()
        return {
            'kty': 'EC',
            'crv': 'P-256',
            'x': b64(numbers.x.to_bytes(32, 'big')),
            'y': b64(numbers.y.to_bytes(32, 'big')),
            **extra,
        }

    @staticmethod
    def sign(key, header, claims, header_text=None, claims_text=None, raw_payload=False):
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import padding

        h = b64((header_text or json.dumps(header)).encode('utf-8', 'surrogatepass'))
        p_text = (claims_text or json.dumps(claims)).encode('utf-8', 'surrogatepass')
        p = p_text.decode('latin-1') if raw_payload else b64(p_text)
        signing = (h + '.' + p).encode('latin-1')
        return h + '.' + p + '.' + b64(key.sign(signing, padding.PKCS1v15(), hashes.SHA256()))


def claims(now, **changes):
    out = {'sub': SUBJECT, 'iss': ISSUER, 'aud': AUDIENCE, 'scope': SCOPE, 'exp': now + 600, 'iat': now}
    for k, v in changes.items():
        if v is ...:
            out.pop(k, None)
        else:
            out[k] = v
    return out


def token_cases(rng, keys, now):
    """(label, token) pairs for a key set holding keys.rsa[0] as k1, keys.rsa[1] without a kid... as set_a()."""
    k = keys.rsa[0]
    header = {'alg': 'RS256', 'kid': 'k1', 'typ': 'JWT'}
    good = claims(now)
    yield 'good', keys.sign(k, header, good)
    yield 'good, header without typ', keys.sign(k, {'alg': 'RS256', 'kid': 'k1'}, good)
    for name, value in (
        ('exp past', now - 10),
        ('exp float', now + 600.5),
        ('exp text', str(now + 600)),
        ('exp true', True),
        ('exp null', None),
        ('exp absent', ...),
        ('exp huge', 10**12),
        ('exp list', [now + 600]),
    ):
        yield name, keys.sign(k, header, claims(now, exp=value))
    for name, value in (
        ('nbf future', now + 300),
        ('nbf past', now - 300),
        ('nbf null', None),
        ('nbf text', 'x'),
        ('nbf float', now - 0.5),
    ):
        yield name, keys.sign(k, header, claims(now, nbf=value))
    for name, change in (
        ('iss other', {'iss': ISSUER + '/'}),
        ('iss absent', {'iss': ...}),
        ('iss list', {'iss': [ISSUER]}),
        ('aud list', {'aud': ['x', AUDIENCE]}),
        ('aud other', {'aud': ORIGIN}),
        ('aud list other', {'aud': [ORIGIN]}),
        ('aud absent', {'aud': ...}),
        ('sub other', {'sub': 'operator-2'}),
        ('sub absent', {'sub': ...}),
        ('sub number', {'sub': 1}),
        ('scope words', {'scope': 'a ' + SCOPE + '\u3000b'}),
        ('scope list', {'scope': ['x', SCOPE]}),
        ('scope list with number', {'scope': [SCOPE, 1]}),
        ('scope list with list', {'scope': [SCOPE, ['x']]}),
        ('scope other', {'scope': 'kerfkite:other'}),
        ('scope absent, scp', {'scope': ..., 'scp': SCOPE}),
        ('scope number, scp list', {'scope': 5, 'scp': [SCOPE]}),
        ('scope absent', {'scope': ...}),
        ('client_id', {'client_id': 'c', 'azp': 'a'}),
    ):
        yield name, keys.sign(k, header, claims(now, **change))
    for name, h in (
        ('alg RS384', {'alg': 'RS384', 'kid': 'k1'}),
        ('alg none', {'alg': 'none', 'kid': 'k1'}),
        ('alg absent', {'kid': 'k1'}),
        ('alg number', {'alg': 5, 'kid': 'k1'}),
        ('crit b64 false', {'alg': 'RS256', 'kid': 'k1', 'b64': False, 'crit': ['b64']}),
        ('crit b64 true', {'alg': 'RS256', 'kid': 'k1', 'b64': True, 'crit': ['b64']}),
        ('b64 false without crit', {'alg': 'RS256', 'kid': 'k1', 'b64': False}),
        ('b64 text', {'alg': 'RS256', 'kid': 'k1', 'b64': 'no', 'crit': ['b64']}),
        ('crit unknown', {'alg': 'RS256', 'kid': 'k1', 'x': 1, 'crit': ['x']}),
        ('crit missing', {'alg': 'RS256', 'kid': 'k1', 'crit': ['typ']}),
        ('crit typ', {'alg': 'RS256', 'kid': 'k1', 'typ': 'JWT', 'crit': ['typ']}),
        ('crit text', {'alg': 'RS256', 'kid': 'k1', 'crit': 'typ'}),
        ('crit null', {'alg': 'RS256', 'kid': 'k1', 'crit': None}),
        ('typ number', {'alg': 'RS256', 'kid': 'k1', 'typ': 1}),
        ('jku', {'alg': 'RS256', 'kid': 'k1', 'jku': 'https://x.example/jwks'}),
        ('jku not a URL', {'alg': 'RS256', 'kid': 'k1', 'jku': 'x'}),
        ('x5c', {'alg': 'RS256', 'kid': 'k1', 'x5c': ['a', 1]}),
        ('jwk', {'alg': 'RS256', 'kid': 'k1', 'jwk': []}),
        ('extra member', {'alg': 'RS256', 'kid': 'k1', 'zzz': {'a': 1}}),
    ):
        yield 'header ' + name, keys.sign(k, h, good, raw_payload=h.get('b64') is False and h.get('crit') == ['b64'])
    t = keys.sign(k, header, good)
    h, p, s = t.split('.')
    yield (
        'signature flipped',
        h
        + '.'
        + p
        + '.'
        + b64(bytes([base64.urlsafe_b64decode(s + '==')[0] ^ 1]) + base64.urlsafe_b64decode(s + '==')[1:]),
    )
    yield 'signature of another key', keys.sign(keys.rsa[1], header, good)
    yield 'signature empty', h + '.' + p + '.'
    yield 'payload changed', h + '.' + b64(json.dumps(claims(now, sub='x')).encode()) + '.' + s
    yield 'padded header', h + '=' * (-len(h) % 4) + '.' + p + '.' + s
    yield 'padded signature', h + '.' + p + '.' + s + '=='
    yield 'two parts', h + '.' + p
    yield 'four parts', t + '.x'
    yield 'empty', ''
    yield 'spaces', ' ' + t
    yield 'standard alphabet', t.replace('-', '+').replace('_', '/')
    yield 'non-ASCII', t[:-1] + '\u00e9'
    yield 'header with junk', '!' + h + '.' + p + '.' + s
    yield 'header JSON list', b64(b'[1]') + '.' + p + '.' + s
    yield 'header not JSON', b64(b'{x') + '.' + p + '.' + s
    yield (
        'header UTF-16',
        keys.sign(k, header, good, header_text=None).replace(h, b64(json.dumps(header).encode('utf-16')), 1),
    )
    yield 'header UTF-8 signature', keys.sign(k, header, good, header_text='\ufeff' + json.dumps(header))
    yield 'claims list', keys.sign(k, header, None, claims_text='[1]')
    yield 'claims not JSON', keys.sign(k, header, None, claims_text='{')
    yield 'claims UTF-16', keys.sign(k, header, None, claims_text=None) if False else keys.sign(k, header, good)
    yield 'claims with duplicate sub', keys.sign(k, header, None, claims_text=json.dumps(good)[:-1] + ', "sub": "x"}')
    yield 'big header', keys.sign(k, {**header, 'pad': 'x' * 600}, good)
    yield 'big payload', keys.sign(k, header, claims(now, pad='y' * 130000))
    yield 'kid absent (one key without)', keys.sign(keys.rsa[1], {'alg': 'RS256'}, good)
    yield 'kid empty', keys.sign(k, {'alg': 'RS256', 'kid': ''}, good)
    yield 'kid _default', keys.sign(keys.rsa[1], {'alg': 'RS256', 'kid': '_default'}, good)
    yield 'kid number', keys.sign(k, {'alg': 'RS256', 'kid': 1}, good)
    yield 'kid zero', keys.sign(k, {'alg': 'RS256', 'kid': 0}, good)
    yield 'kid list', keys.sign(k, {'alg': 'RS256', 'kid': ['k1']}, good)
    yield 'kid empty list', keys.sign(k, {'alg': 'RS256', 'kid': []}, good)
    for _ in range(12):
        c = claims(
            now,
            **{
                rng.choice(['exp', 'nbf', 'sub', 'aud', 'iss', 'scope', 'x']): rng.choice(
                    [None, 1, now + 100, 'x', [], {}, True, ISSUER, AUDIENCE, SCOPE, SUBJECT]
                )
            },
        )
        yield 'random claims ' + json.dumps(c)[:60], keys.sign(k, header, c)


def key_set_a(keys):
    """k1, a key without kid, k3 with use and alg, and keys FastMCP passes over."""
    return {
        'keys': [
            keys.jwk(keys.rsa[0], kid='k1', use='sig', alg='RS256'),
            keys.jwk(keys.rsa[1]),
            keys.jwk(keys.rsa[2], kid='k3', use='enc', key_ops=['encrypt']),
            {'kty': 'oct', 'kid': 'k4', 'k': 'AAAA'},
            keys.ec_jwk(kid='k5'),
            'not an object',
        ]
    }


# ---- the http part -------------------------------------------------------------------------------------------


def http_part(program, work, cert, standins, rng, tally):
    from mcp_parity import cases as tool_cases
    from mcp_parity import form

    keys = Keys(rng)
    for standin in standins.values():
        standin.answer = (200, [('Content-Type', 'application/json')], json.dumps(key_set_a(keys)).encode())
    servers = Servers(program, work, cert)
    try:
        now = int(time.time())
        good = keys.sign(keys.rsa[0], {'alg': 'RS256', 'kid': 'k1', 'typ': 'JWT'}, claims(now))
        host = [('Host', 'workshop.example.com')]
        bearer = [('Authorization', 'Bearer ' + good)]
        types = [('Accept', 'application/json, text/event-stream'), ('Content-Type', 'application/json')]

        def both(label, method, path, headers=(), body=None, own=None, files=False, agree=None):
            answers = [ask(port, method, path, headers, body) for port in servers.ports]
            fetched = [standins['a'].take(), standins['b'].take()]
            shown = []
            for side, answer in enumerate(answers):
                out = servers.outputs[side]
                shown.append(
                    view(
                        answer,
                        (lambda v, out=out: with_artifacts(form(v, out), v, out)) if files else None,
                        servers.ports[side],
                    )
                )
            for side, log in enumerate(fetched):
                shown[side]['fetched'] = [(m, p, own_host(h, side)) for m, p, h in log]
            tally.record(label, shown[0], shown[1], own, agree)
            return answers

        # The first token fetches the key set.
        both('first token', 'POST', '/mcp', host + bearer + types, b'{"jsonrpc":"2.0","id":1,"method":"ping"}')
        # Routes, methods, the guard and the metadata.
        origins = [
            None,
            '',
            'https://workshop.example.com',
            'https://workshop.example.com/',
            'https://WORKSHOP.example.com:443',
            'https://workshop.example.com:8443',
            'http://workshop.example.com',
            'https://evil.example',
            'http://127.0.0.1:8775',
            'http://localhost',
            'http://[::1]:5',
            'null',
            'https://workshop.example.com/path',
            'https://[::1',
            'https://a@workshop.example.com',
            ' https://workshop.example.com ',
            'ws://workshop.example.com',
        ]
        hosts = [
            None,
            '',
            'workshop.example.com',
            'WORKSHOP.EXAMPLE.COM',
            'workshop.example.com:443',
            'workshop.example.com.',
            '127.0.0.1:9',
            'localhost',
            '[::1]:80',
            '::1',
            'evil.example',
            ' workshop.example.com',
            '\xa0workshop.example.com',
            'workshop.example.com:1:2',
        ]
        forwarded = [None, 'https', 'http', 'wss', 'HTTPS', ' https ', 'ftp']
        paths = [
            '/mcp',
            '/mcp/',
            '/mcp//',
            '/.well-known/oauth-protected-resource/mcp',
            '/.well-known/oauth-protected-resource/mcp/',
            '/.well-known/oauth-protected-resource',
            '/',
            '/nothing',
            '/MCP',
            '/mcp?x=1',
            '/mcp/?x=1',
            '/m%63p',
            '/mcp/extra',
            '//mcp',
        ]
        methods = ['GET', 'POST', 'DELETE', 'HEAD', 'OPTIONS', 'PUT', 'PATCH']
        auths = [
            None,
            'Bearer ' + good,
            'bearer ' + good,
            'BEARER ' + good,
            'Basic abc',
            '',
            'Bearer',
            'Bearer ',
            'Bearer x.y.z',
            'Bearer  ' + good,
        ]
        for _ in range(400):
            headers = []
            h = rng.choice(hosts)
            if h is not None:
                headers.append(('Host', h))
            o = rng.choice(origins)
            if o is not None:
                headers.append(('Origin', o))
            f = rng.choice(forwarded)
            if f is not None:
                headers.append(('X-Forwarded-Proto', f))
            a = rng.choice(auths)
            if a is not None:
                headers.append(('Authorization', a))
            if rng.random() < 0.3:
                headers.append(('Access-Control-Request-Method', rng.choice(['GET', 'POST', 'OPTIONS', ''])))
            if rng.random() < 0.2:
                headers.append(
                    (
                        'Access-Control-Request-Headers',
                        rng.choice(
                            [
                                'mcp-protocol-version',
                                'Content-Type, MCP-Protocol-Version',
                                'x-other',
                                '',
                                'accept , content-language',
                            ]
                        ),
                    )
                )
            if rng.random() < 0.05:
                headers.append(('Access-Control-Request-Private-Network', 'true'))
            method, path = rng.choice(methods), rng.choice(paths)
            body = None
            if method in ('POST', 'PUT', 'PATCH'):
                headers += types
                body = b'{"jsonrpc":"2.0","id":1,"method":"ping"}'
            both(f'{method} {path} {headers}', method, path, headers, body)
        # Bearer tokens.
        for label, token in token_cases(rng, keys, now):
            try:
                value = 'Bearer ' + token
                value.encode('latin-1')
            except UnicodeEncodeError:
                continue
            own = None
            if label in ('header UTF-16',):
                own = 'JSON by the server\'s own reader'
            if len(value) > 60000:
                own = 'request heads of up to 64 KiB'
            both(
                'token ' + label,
                'POST',
                '/mcp',
                host + [('Authorization', value)] + types,
                b'{"jsonrpc":"2.0","id":1,"method":"ping"}',
                own=own,
            )
        # Messages.
        bodies = [
            b'',
            b'{',
            b'[]',
            b'null',
            b'"x"',
            b'{"jsonrpc":"2.0","id":1,"method":"ping"}',
            '{"jsonrpc":"2.0","id":1,"method":"ping"}'.encode('utf-16'),
            b'\xef\xbb\xbf{"jsonrpc":"2.0","id":1,"method":"ping"}',
            b'\xff\xfe\xfd',
            b'{"jsonrpc":"2.0","id":1,"method":"ping"} x',
            b'[{"jsonrpc":"2.0","id":1,"method":"ping"}]',
            b'{"jsonrpc":"1.0","id":1,"method":"ping"}',
            b'{"jsonrpc":"2.0","method":"notifications/initialized"}',
            b'{"jsonrpc":"2.0","id":1.5,"method":"tools/list"}',
            b'{"jsonrpc":"2.0","id":null,"method":"tools/list"}',
            b'{"jsonrpc":"2.0","id":true,"method":"tools/list"}',
            b'{"jsonrpc":"2.0","id":1,"result":{}}',
            b'{"jsonrpc":"2.0","id":1,"error":{"code":1,"message":"m"}}',
            b'{"jsonrpc":"2.0","id":"1","error":{"code":"5","message":"m"}}',
            b'{"jsonrpc":"2.0","id":1,"method":"x","error":{"code":1,"message":"m"}}',
            b'{"jsonrpc":"2.0","id":1,"method":"ping","params":[]}',
            b'{"jsonrpc":"2.0","id":9}',
            b'{"jsonrpc":"2.0","id":-5,"method":"ping"}',
            b'{"jsonrpc":"2.0","id":"s","method":"ping","params":null}',
        ]
        own_reader = [
            b'{"jsonrpc":"2.0","id":1,"method":"ping","params":{"x":NaN}}',
            b'{"jsonrpc":"2.0","id":1,"method":"ping","params":{"x":-Infinity}}',
            b'{"jsonrpc":"2.0","id":1,"method":"ping","params":{"x":"\\ud800"}}',
            b'{"jsonrpc":"2.0","id":1,"method":"ping","params":{"x":' + b'[' * 250 + b']' * 250 + b'}}',
        ]
        for body in own_reader:
            both(
                f'message {body[:60]!r}',
                'POST',
                '/mcp',
                host + bearer + types,
                body,
                own='JSON by the server\'s own reader',
            )
        for body in bodies:
            for accept in (
                'application/json, text/event-stream',
                'application/json',
                'text/event-stream',
                'application/json;q=1, text/event-stream;q=0.5',
                '*/*',
                None,
            ):
                for content in (
                    'application/json',
                    'application/json; charset=utf-8',
                    'text/plain',
                    'APPLICATION/JSON',
                    'text/plain, application/json',
                    None,
                ):
                    if rng.random() > 0.2 and (accept, content) != (
                        'application/json, text/event-stream',
                        'application/json',
                    ):
                        continue
                    headers = host + bearer
                    if accept is not None:
                        headers.append(('Accept', accept))
                    if content is not None:
                        headers.append(('Content-Type', content))
                    own = 'JSON by the server\'s own reader' if body.startswith((b'\xff\xfe', b'\xfe\xff')) else None
                    both(f'message {body[:60]!r} {accept} {content}', 'POST', '/mcp', headers, body, own=own)
        for version in ('2025-06-18', '2025-11-25', '2024-11-05', '2025-03-26', '1999-01-01', '', '2025-06-18 ', 'x'):
            for body in (
                b'{"jsonrpc":"2.0","id":1,"method":"ping"}',
                b'{"jsonrpc":"2.0","method":"notifications/initialized"}',
                b'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"p","version":"1"}}}',
            ):
                both(
                    f'protocol version {version!r} {body[:50]!r}',
                    'POST',
                    '/mcp',
                    host + bearer + types + [('MCP-Protocol-Version', version)],
                    body,
                )
        requests = [
            (
                'initialize',
                {'protocolVersion': '2025-06-18', 'capabilities': {}, 'clientInfo': {'name': 'p', 'version': '1'}},
            ),
            (
                'initialize',
                {
                    'protocolVersion': '1999-01-01',
                    'capabilities': {'elicitation': {}},
                    'clientInfo': {'name': 'p', 'version': '1'},
                },
            ),
            ('initialize', {'protocolVersion': 5, 'capabilities': {}, 'clientInfo': {'name': 'p', 'version': '1'}}),
            (
                'initialize',
                {
                    'protocolVersion': '2025-06-18',
                    'capabilities': {'roots': {'listChanged': 'yes'}},
                    'clientInfo': {'name': 'p', 'version': '1'},
                },
            ),
            (
                'initialize',
                {
                    'protocolVersion': '2025-06-18',
                    'capabilities': {'experimental': {'x': None}},
                    'clientInfo': {'name': 'p', 'version': '1'},
                },
            ),
            ('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}, 'clientInfo': {'name': 'p'}}),
            ('initialize', None),
            ('ping', None),
            ('ping', {'_meta': {'progressToken': 1.5}}),
            ('ping', {'task': {'ttl': 1}}),
            ('tools/list', None),
            ('tools/list', {'cursor': 5}),
            ('resources/list', None),
            ('resources/templates/list', None),
            ('resources/read', {'uri': 'cnc-studio://artwork-guide'}),
            ('resources/read', {'uri': 'CNC-STUDIO://artwork-guide'}),
            ('resources/read', {'uri': ' cnc-studio://artwork-guide '}),
            ('resources/read', {'uri': 'cnc-studio://ARTWORK-GUIDE'}),
            ('resources/read', {'uri': 'kerfkite://nope'}),
            ('resources/read', {'uri': 'nouri'}),
            ('resources/read', {'uri': 5}),
            ('resources/read', {'uri': 'x://a b'}),
            ('resources/subscribe', {'uri': 'x:y'}),
            ('resources/unsubscribe', {'uri': 'x:y'}),
            ('prompts/list', None),
            ('prompts/get', {'name': 'x'}),
            ('prompts/get', {'name': 'x', 'arguments': {'a': 1}}),
            (
                'completion/complete',
                {'ref': {'type': 'ref/prompt', 'name': 'x'}, 'argument': {'name': 'a', 'value': 'b'}},
            ),
            ('completion/complete', {'ref': {'type': 'ref/x'}, 'argument': {'name': 'a', 'value': 'b'}}),
            ('logging/setLevel', {'level': 'debug'}),
            ('logging/setLevel', {'level': 'loud'}),
            ('tasks/list', None),
            ('tasks/get', {'taskId': 'x'}),
            ('tasks/get', {}),
            ('tasks/result', {'taskId': 'x'}),
            ('tasks/cancel', {'taskId': 'x'}),
            ('nope', None),
            ('notifications/initialized', None),
            ('tools/call', {}),
            ('tools/call', {'name': 'nope'}),
            ('tools/call', {'name': 'list_templates', 'arguments': []}),
            ('tools/call', {'name': 'list_templates', 'arguments': None}),
            ('tools/call', {'name': 'list_templates', 'arguments': {'limit': 1}, 'task': {'ttl': 1000}}),
            ('tools/call', {'name': 'list_templates', 'arguments': {'limit': 'x'}, 'task': {}}),
            ('tools/call', {'name': 'nope', 'task': {}}),
            ('tools/call', {'name': 'list_templates', 'arguments': {'limit': 1}, 'task': None}),
        ]
        for n, (method, params) in enumerate(requests):
            message = {'jsonrpc': '2.0', 'id': n, 'method': method}
            if params is not None:
                message['params'] = params
            both(
                f'request {json.dumps(message)[:120]}',
                'POST',
                '/mcp',
                host + bearer + types,
                json.dumps(message).encode(),
            )
        from studio.template_catalog import TemplateCatalog

        templates = TemplateCatalog().search(limit=100)
        for n, (tool, arguments) in enumerate(tool_cases(rng, 80, templates)):
            message = {
                'jsonrpc': '2.0',
                'id': 1000 + n,
                'method': 'tools/call',
                'params': {'name': tool, 'arguments': arguments},
            }
            action = arguments.get('action') if tool == 'design_call' and isinstance(arguments, dict) else None
            agree = described(action) if action in ('geometry', 'extrude') else None
            both(
                f'tool {tool} {json.dumps(arguments)[:100]}',
                'POST',
                '/mcp',
                host + bearer + types,
                json.dumps(message).encode(),
                files=True,
                agree=agree,
            )
        # A key the cache lacks: one refresh, then none for 30 seconds.
        missing = keys.sign(keys.rsa[0], {'alg': 'RS256', 'kid': 'k9'}, claims(now))
        for k in range(3):
            both(
                f'unknown kid, try {k}',
                'POST',
                '/mcp',
                host + [('Authorization', 'Bearer ' + missing)] + types,
                b'{"jsonrpc":"2.0","id":1,"method":"ping"}',
            )
        both(
            'known kid after the misses',
            'POST',
            '/mcp',
            host + bearer + types,
            b'{"jsonrpc":"2.0","id":1,"method":"ping"}',
        )
    finally:
        servers.close()


# ---- the keys part -------------------------------------------------------------------------------------------


def keys_part(program, work, cert, standins, rng, tally):
    keys = Keys(rng)
    now = int(time.time())
    json_type = [('Content-Type', 'application/json')]

    def answer(document=None, status=200, headers=None, body=None):
        data = body if body is not None else json.dumps(document).encode()
        return (status, json_type if headers is None else headers, data)

    k1, k2, k3 = (keys.jwk(keys.rsa[0], kid='k1'), keys.jwk(keys.rsa[1]), keys.jwk(keys.rsa[2], kid='k3'))
    small = keys.jwk(keys.small, kid='small')
    leading = dict(k1, n='AAAA' + k1['n'])
    sets = [
        ('k1, k3 and one without kid', answer({'keys': [k1, k2, k3]})),
        ('one key without kid', answer({'keys': [k2]})),
        ('two keys without kid', answer({'keys': [k2, dict(k1, kid='')]})),
        ('no keys', answer({'keys': []})),
        ('no keys member', answer({'other': 1})),
        ('keys an object', answer({'keys': {'a': k1}})),
        ('keys text', answer({'keys': 'k1'})),
        ('keys null', answer({'keys': None})),
        ('keys a number', answer({'keys': 5})),
        ('a list', answer([k1])),
        ('not JSON', answer(body=b'{"keys": [')),
        ('empty body', answer(body=b'')),
        ('UTF-16', answer(body=json.dumps({'keys': [k1]}).encode('utf-16'))),
        ('UTF-8 signature', answer(body=b'\xef\xbb\xbf' + json.dumps({'keys': [k1]}).encode())),
        ('not UTF-8', answer(body=b'{"keys": [], "x": "\xff"}')),
        ('status 404', answer({'keys': [k1]}, status=404)),
        ('status 500', answer({'keys': [k1]}, status=500)),
        ('a redirect', answer({'keys': [k1]}, status=302, headers=[*json_type, ('Location', '/jwks')])),
        (
            'a long declared length',
            (200, [*json_type, ('Content-Length', '70000')], json.dumps({'keys': [k1]}).encode()),
        ),
        ('a body over 64 KiB', answer({'keys': [k1], 'pad': 'x' * 66000})),
        ('a body of 64 KiB', answer(body=(lambda d: d + b' ' * (65536 - len(d)))(json.dumps({'keys': [k1]}).encode()))),
        ('an unhashable kid on a key passed over', answer({'keys': [k1, {'kty': 'oct', 'kid': ['x'], 'k': 'AA'}]})),
        ('a kid list on an RSA key', answer({'keys': [k1, dict(k3, kid=['k3'])]})),
        ('a number kid', answer({'keys': [k1, dict(k3, kid=3)]})),
        ('an EC key', answer({'keys': [k1, keys.ec_jwk(kid='k5')]})),
        ('an EC key without kid', answer({'keys': [keys.ec_jwk()]})),
        ('an EC key off its curve', answer({'keys': [k2, dict(keys.ec_jwk(), y=keys.ec_jwk()['x'])]})),
        ('duplicate kids', answer({'keys': [dict(k3, kid='k1'), k1]})),
        ('the _default kid', answer({'keys': [dict(k3, kid='_default'), k2]})),
        ('use and key_ops that disagree', answer({'keys': [dict(k1, use='enc', key_ops=['verify']), k3]})),
        ('use enc', answer({'keys': [dict(k1, use='enc')]})),
        ('alg RS384 on the key', answer({'keys': [dict(k1, alg='RS384')]})),
        ('a small exponent', answer({'keys': [small, k1]})),
        ('a modulus with leading zeros', answer({'keys': [leading]})),
        ('an even exponent', answer({'keys': [dict(k1, e='Ag')]})),
        (
            'unsupported key types',
            answer({'keys': [{'kty': 'OKP', 'kid': 'k1', 'crv': 'Ed25519', 'x': 'AA'}, {'kid': 'k1'}, k3]}),
        ),
        ('a private key', answer({'keys': [dict(k1, d='AQAB')]})),
        ('a whole private key', answer({'keys': [keys.private_jwk(keys.rsa[0], kid='k1')]})),
    ]
    tokens = [
        ('kid k1', lambda: keys.sign(keys.rsa[0], {'alg': 'RS256', 'kid': 'k1'}, claims(now))),
        ('no kid, the kid-less key', lambda: keys.sign(keys.rsa[1], {'alg': 'RS256'}, claims(now))),
        ('no kid, k1', lambda: keys.sign(keys.rsa[0], {'alg': 'RS256'}, claims(now))),
        ('kid k3', lambda: keys.sign(keys.rsa[2], {'alg': 'RS256', 'kid': 'k3'}, claims(now))),
        ('kid k5', lambda: keys.sign(keys.rsa[0], {'alg': 'RS256', 'kid': 'k5'}, claims(now))),
        ('kid _default', lambda: keys.sign(keys.rsa[1], {'alg': 'RS256', 'kid': '_default'}, claims(now))),
        ('kid small', lambda: keys.sign(keys.small, {'alg': 'RS256', 'kid': 'small'}, claims(now))),
        ('kid 3', lambda: keys.sign(keys.rsa[2], {'alg': 'RS256', 'kid': 3}, claims(now))),
        ('kid unknown', lambda: keys.sign(keys.rsa[0], {'alg': 'RS256', 'kid': 'k9'}, claims(now))),
        ('kid unknown again', lambda: keys.sign(keys.rsa[0], {'alg': 'RS256', 'kid': 'k8'}, claims(now))),
        ('kid list', lambda: keys.sign(keys.rsa[0], {'alg': 'RS256', 'kid': ['k1']}, claims(now))),
        ('kid k1 at the end', lambda: keys.sign(keys.rsa[0], {'alg': 'RS256', 'kid': 'k1'}, claims(now))),
    ]
    body = b'{"jsonrpc":"2.0","id":1,"method":"ping"}'
    base = [
        ('Host', 'workshop.example.com'),
        ('Accept', 'application/json, text/event-stream'),
        ('Content-Type', 'application/json'),
    ]

    def run(label, servers, order, own=None):
        for name, make in order:
            headers = base + [('Authorization', 'Bearer ' + make())]
            answers = [ask(port, 'POST', '/mcp', headers, body) for port in servers.ports]
            shown = [view(a, port=p) for a, p in zip(answers, servers.ports)]
            for side, standin in enumerate((standins['a'], standins['b'])):
                shown[side]['fetched'] = [(m, p, own_host(h, side)) for m, p, h in standin.take()]
            tally.record(f'keys {label}: {name}', shown[0], shown[1], own)
            stats['accepted'] += shown[0]['status'] == 200
            stats['fetches'] += len(shown[0]['fetched'])

    stats = {'accepted': 0, 'fetches': 0}
    for label, served in sets:
        for standin in standins.values():
            standin.answer = served
        servers = Servers(program, work, cert)
        try:
            own = None
            if label in ('UTF-16',):
                own = 'JSON by the server\'s own reader'
            if label in ('a private key', 'a whole private key'):
                own = 'private keys passed over'
            run(label, servers, tokens, own)
        finally:
            servers.close()
    # Where the key set may be fetched from: each side its own stand-in, named or by address, and addresses that are
    # refused. A first token makes the fetch.
    for standin in standins.values():
        standin.answer = answer({'keys': [k1]})
    first = [('kid k1', tokens[0][1]), ('kid k1 again', tokens[0][1])]
    for template in (
        'https://{name}/jwks',
        'https://{address}/jwks',
        'https://{name}:443/jwks',
        'https://{upper}/jwks',
        'https://{name}',
        'https://{name}/',
        'https://{name}/a/b?',
        'https://{name}:0443/jwks',
        'https://{name}:8443/jwks',
        'https://private.example/jwks',
        'https://loop.example/jwks',
        'https://shared.example/jwks',
        'https://nowhere.example/jwks',
        'https://{name}./jwks',
    ):
        urls = []
        for side in (0, 1):
            name, address = ('keys-a.example', '1.0.0.1') if side == 0 else ('keys-b.example', '1.0.0.2')
            urls.append(template.format(name=name, address=address, upper=name.upper()))
        if '?' in template:
            continue
        servers = Servers(program, work, cert, jwks=tuple(urls))
        try:
            run('fetched from ' + template, servers, first)
        finally:
            servers.close()
    # The refresh limit: a missing key fetches at most once in 30 seconds.
    for standin in standins.values():
        standin.answer = answer({'keys': [k1]})
    servers = Servers(program, work, cert)
    try:
        run('refresh limit', servers, [tokens[8], tokens[9]])
        time.sleep(31)
        run('refresh limit, 31 seconds later', servers, [tokens[9], tokens[8], tokens[0]])
    finally:
        servers.close()
    print(f"keys: {stats['accepted']} tokens accepted, {stats['fetches']} key-set fetches by each server")


# ---- the config part -----------------------------------------------------------------------------------------

ORIGINS = [
    'https://workshop.example.com',
    'https://workshop.example.com/',
    'https://WORKSHOP.Example.COM',
    'HTTPS://workshop.example.com',
    'https://workshop.example.com:8443',
    'https://workshop.example.com:443',
    'https://workshop.example.com:0443',
    'https://workshop.example.com:',
    'https://workshop.example.com:65536',
    'https://workshop.example.com/path',
    'https://workshop.example.com/.',
    'https://workshop.example.com.',
    'http://workshop.example.com',
    'https://localhost',
    'https://workshop.local',
    'https://a.home.arpa',
    'https://example',
    'https://8.8.8.8',
    'https://8.8.8.8:9',
    'https://10.0.0.1',
    'https://100.64.0.1',
    'https://224.0.0.1',
    'https://[2606:4700::1111]',
    'https://[2606:4700:0:0::1111]:8443',
    'https://[::ffff:8.8.8.8]',
    'https://[::1]',
    'https://[fe80::1]',
    'https://[2606:4700::1111%25eth0]',
    'https://user@workshop.example.com',
    'https://@workshop.example.com',
    'https://:@workshop.example.com',
    'https://u:p@workshop.example.com',
    'https://workshop.example.com?x',
    'https://workshop.example.com#x',
    ' https://workshop.example.com',
    'https://workshop.example.com ',
    'https://work shop.example.com',
    'https://workshop.example.com\\x',
    '',
    'x' * 2049,
    'https://' + 'a' * 2030 + '.example',
    'https://xn--bcher-kva.example',
    'https://b\u00fccher.example',
    'https://1.2.3',
    'https://foo.123',
    'https://a..b.example',
    'https://-a.example',
    'https://a_b.example',
    'https://*.example.com',
    'https:workshop.example.com',
    'https://work\tshop.example.com',
    'https://workshop.example.com/\u00e9',
    None,
]
PATHS = ['', '/', '/realms/kerfkite', '/a/b/', '/a/../b', '/a%20b', '/a b', '/x?y', '/caf\u00e9', '/|^', '/{x}', "/a'b"]
SUBJECTS = ['operator-1', '', ' x', 'x ', 'a\tb', 'x' * 2048, 'x' * 2049, '\u00fcber', '\u200b', 'a b', None]


def started(program_args, env, port):
    """('refused', message) or ('started', metadata, challenge) for one server and environment."""
    process = subprocess.Popen(program_args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)  # noqa: S603
    for _ in range(400):
        if process.poll() is not None:
            break
        try:
            socket.create_connection(('127.0.0.1', port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.05)
    if process.poll() is not None:
        errors = process.stderr.read().decode('utf-8', 'replace').strip().splitlines()
        last = errors[-1] if errors else ''
        for prefix in ('ValueError: ', 'kerfkite-mcp: error: '):
            last = last.removeprefix(prefix)
        return ['refused', last]
    try:
        host = None
        try:
            from pydantic import AnyHttpUrl

            host = AnyHttpUrl(env['KERFKITE_MCP_PUBLIC_ORIGIN']).host
        except Exception:  # noqa: BLE001 - a host for the request only
            host = 'workshop.example.com'
        meta = ask(port, 'GET', '/.well-known/oauth-protected-resource/mcp', [('Host', host)])
        challenge = ask(port, 'POST', '/mcp', [('Host', host), ('Content-Type', 'application/json')], b'{}')
        return ['started', view(meta), view(challenge)]
    finally:
        process.terminate()
        try:
            process.wait(10)
        except subprocess.TimeoutExpired:
            process.kill()


def config_part(program, work, cert, rng, tally, count=160):
    base = {k: v for k, v in os.environ.items() if k not in NAMES and k not in PROXIES}
    base.update({'SSL_CERT_FILE': str(cert), 'PYTHONPATH': str(ROOT)})
    good = {
        'KERFKITE_MCP_PUBLIC_ORIGIN': ORIGIN,
        'KERFKITE_MCP_ISSUER': ISSUER,
        'KERFKITE_MCP_JWKS_URL': 'https://keys-a.example/jwks',
        'KERFKITE_MCP_SUBJECT': SUBJECT,
    }

    def url(path_too=True):
        origin = rng.choice(ORIGINS)
        if origin is None or not path_too or rng.random() < 0.4:
            return origin
        return origin.rstrip('/') + rng.choice(PATHS)

    environments = []
    for name in NAMES:
        for value in ORIGINS if name != 'KERFKITE_MCP_SUBJECT' else SUBJECTS:
            environments.append({**good, name: value})
    for _ in range(count):
        environments.append(
            {
                'KERFKITE_MCP_PUBLIC_ORIGIN': url(False) if rng.random() < 0.5 else ORIGIN,
                'KERFKITE_MCP_ISSUER': url() if rng.random() < 0.6 else ISSUER,
                'KERFKITE_MCP_JWKS_URL': url() if rng.random() < 0.6 else 'https://keys-a.example/jwks',
                'KERFKITE_MCP_SUBJECT': rng.choice(SUBJECTS) if rng.random() < 0.3 else SUBJECT,
            }
        )
    for settings in environments:
        env = dict(base)
        for name, value in settings.items():
            if value is not None:
                env[name] = value
        try:
            for value in env.values():
                value.encode('utf-8')
        except UnicodeEncodeError:
            continue
        port = free_port()
        ref = started(
            [
                sys.executable,
                '-m',
                'studio.mcp_server',
                '--transport',
                'http',
                '--port',
                str(port),
                '--output-dir',
                str(work / 'config-ref'),
            ],
            env,
            port,
        )
        port = free_port()
        got = started(
            [program, '--transport', 'http', '--port', str(port), '--output-dir', str(work / 'config-cpp')], env, port
        )
        # The reference checks the origin, the issuer, the subject and the key set's address in that order: a URL
        # kerfkite-mcp refuses ahead of the one the reference stopped at (or where it started) was one the
        # reference took, refused by kerfkite-mcp's own rule.
        order = ['PUBLIC_ORIGIN', 'ISSUER', 'SUBJECT', 'JWKS_URL']

        def place(answer):
            if answer[0] != 'refused':
                return len(order)
            for k, name in enumerate(order):
                if 'KERFKITE_MCP_' + name + ' ' in answer[1] + ' ':
                    return k
            return -1

        own = None
        if got[0] == 'refused' and 'must be a public HTTPS' in got[1] and place(got) < place(ref):
            own = 'URLs as plainly as AnyHttpUrl writes them'
        tally.record(f'config {json.dumps(settings)[:240]}', ref, got, own)


# ---- the stdio part ------------------------------------------------------------------------------------------


class Lines:
    """A server over standard input and output, line by line: what each line brought, up to a ping's answer."""

    def __init__(self, command, env):
        self.process = subprocess.Popen(
            command, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL
        )  # noqa: S603
        self.sync = 0

    def send(self, line, expect=True):
        """What the line brought: its answer when it is a request (awaited first: a tool runs on a thread of its
        own) unless expect is false, then what else came before a ping sent after it was answered."""
        expected = None
        try:
            message = json.loads(line)
            if (
                isinstance(message, dict)
                and message.get('jsonrpc') == '2.0'
                and isinstance(message.get('method'), str)
                and (
                    isinstance(message.get('id'), str)
                    or (isinstance(message.get('id'), int) and not isinstance(message.get('id'), bool))
                )
                and (message.get('params') is None or isinstance(message.get('params'), dict))
            ) and expect:
                expected = message['id']
        except ValueError:
            pass
        self.process.stdin.write(line + b'\n')
        self.process.stdin.flush()
        out = []

        def read():
            text = self.process.stdout.readline()
            if not text:
                out.append('<closed>')
                return None
            return json.loads(text)

        while expected is not None:
            message = read()
            if message is None:
                return out
            out.append(message)
            if message.get('id') == expected and ('result' in message or 'error' in message):
                break
        self.sync += 1
        self.process.stdin.write(
            json.dumps({'jsonrpc': '2.0', 'id': f'sync-{self.sync}', 'method': 'ping'}).encode() + b'\n'
        )
        self.process.stdin.flush()
        while True:
            message = read()
            if message is None or message.get('id') == f'sync-{self.sync}':
                return out
            out.append(message)

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(20)
        except subprocess.TimeoutExpired:
            self.process.kill()


def stdio_part(program, work, rng, tally, sessions=12):
    env = {k: v for k, v in os.environ.items() if k not in NAMES and k not in PROXIES}
    env['PYTHONPATH'] = str(ROOT)
    initialize = {
        'jsonrpc': '2.0',
        'id': 'init',
        'method': 'initialize',
        'params': {'protocolVersion': '2025-06-18', 'capabilities': {}, 'clientInfo': {'name': 'p', 'version': '1'}},
    }
    pool = [
        b'',
        b'   ',
        b'{',
        b'nonsense',
        b'[]',
        b'[{"jsonrpc":"2.0","id":1,"method":"ping"}]',
        b'{}',
        b'null',
        b'{"jsonrpc":"2.0","id":1,"method":"ping"}',
        b'{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
        b'{"jsonrpc":"2.0","id":3,"method":"resources/list"}',
        b'{"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":"cnc-studio://artwork-guide"}}',
        b'{"jsonrpc":"2.0","id":5,"method":"prompts/list"}',
        b'{"jsonrpc":"2.0","id":6,"method":"nope"}',
        b'{"jsonrpc":"2.0","id":7,"method":"logging/setLevel","params":{"level":"info"}}',
        b'{"jsonrpc":"2.0","id":8,"method":"completion/complete","params":{"ref":{"type":"ref/prompt","name":"x"},"argument":{"name":"a","value":"b"}}}',
        b'{"jsonrpc":"2.0","id":9,"method":"ping","params":{"_meta":[]}}',
        b'{"jsonrpc":"2.0","method":"notifications/initialized"}',
        b'{"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":1}}',
        b'{"jsonrpc":"2.0","method":"notifications/nope"}',
        b'{"jsonrpc":"2.0","id":1.5,"method":"tools/list"}',
        b'{"jsonrpc":"2.0","id":"x","result":{}}',
        b'{"jsonrpc":"2.0","id":"x","error":{"code":1,"message":"m"}}',
        b'{"jsonrpc":"1.0","id":10,"method":"ping"}',
        b'{"jsonrpc":"2.0","id":11,"method":"initialize","params":{"protocolVersion":"1999-01-01","capabilities":{},"clientInfo":{"name":"p","version":"1"}}}',
        b'{"jsonrpc":"2.0","id":12,"method":"initialize","params":{}}',
        b'{"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"nope"}}',
        b'{"jsonrpc":"2.0","id":14,"method":"tools/call","params":{"name":"list_templates","arguments":{"limit":1}}}',
        b'{"jsonrpc":"2.0","id":15,"method":"prompts/get","params":{"name":"x"}}',
        b'{"jsonrpc":"2.0","id":16,"method":"tasks/list"}',
        b' {"jsonrpc":"2.0","id":17,"method":"ping"} ',
        b'{"jsonrpc":"2.0","id":18,"method":"ping"}\r',
        json.dumps(initialize).encode(),
    ]
    # Lines kerfkite-mcp's JSON reader refuses where pydantic read them: answered by the reference only.
    own_reader = [
        b'{"jsonrpc":"2.0","id":19,"method":"ping","params":{"x":NaN}}',
        b'{"jsonrpc":"2.0","id":20,"method":"ping","params":{"x":Infinity}}',
        b'{"jsonrpc":"2.0","id":21,"method":"ping","params":{"x":' + b'[' * 199 + b']' * 199 + b'}}',
    ]
    pool += own_reader
    for session in range(sessions):
        lines = [rng.choice(pool) for _ in range(rng.randint(8, 24))]
        if session % 3 == 0:
            lines.insert(rng.randint(0, 3), json.dumps(initialize).encode())
        sides = [
            Lines([sys.executable, '-m', 'studio.mcp_server', '--output-dir', str(work / 'stdio-ref')], env),
            Lines([program, '--output-dir', str(work / 'stdio-cpp')], env),
        ]
        try:
            for k, line in enumerate(lines):
                own = 'JSON by the server\'s own reader' if line in own_reader else None
                answers = [parsed_texts(side.send(line, expect=n == 0 or not own)) for n, side in enumerate(sides)]
                tally.record(f'stdio session {session} line {k}: {line[:100]!r}', answers[0], answers[1], own)
        finally:
            for side in sides:
                side.close()


# ---- main ----------------------------------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--server', required=True, help='kerfkite-mcp, with kerfkite-pendant-tls beside it')
    parser.add_argument('--part', default='all', choices=('all', 'http', 'keys', 'config', 'stdio'))
    parser.add_argument('--seed', type=int, default=1)
    args = parser.parse_args()
    enter_namespaces()
    program = str(Path(args.server).resolve())
    relay = Path(program).parent / 'kerfkite-pendant-tls'
    work = Path(tempfile.mkdtemp(prefix='kerfkite-http-parity-'))
    try:
        set_up_network(work)
        cert, key = work / 'standin.pem', work / 'standin-key.pem'
        subprocess.run(  # noqa: S603 - the program under test
            [
                str(relay),
                '-make-cert',
                '-hosts',
                ','.join([*STANDINS.values(), *[n for n in HOSTS if n.startswith('keys')]]),
                '-cert',
                str(cert),
                '-key',
                str(key),
            ],
            check=True,
            capture_output=True,
        )
        standins = {name: StandIn(address, 443, str(cert), str(key)) for name, address in STANDINS.items()}
        rng = random.Random(args.seed)
        tally = Tally()
        if args.part in ('all', 'http'):
            http_part(program, work, cert, standins, rng, tally)
        if args.part in ('all', 'keys'):
            keys_part(program, work, cert, standins, rng, tally)
        if args.part in ('all', 'config'):
            config_part(program, work, cert, rng, tally)
        if args.part in ('all', 'stdio'):
            stdio_part(program, work, rng, tally)
        if tally.described:
            print(f'  {tally.described} geometries the same by what they describe')
        for rule, count in sorted(tally.rules.items()):
            print(f'  {count} by the own rule: {rule}')
        print(
            f'{tally.cases} cases: {tally.same} the same, {tally.own} different by kerfkite-mcp\'s own rules, {len(tally.differences)} different'
        )
        sys.exit(1 if tally.differences else 0)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
