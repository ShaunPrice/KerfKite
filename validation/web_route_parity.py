"""Route parity: the C++ hosted editor (kerfkite-web) against the Flask reference (studio.web_server.create_app).

Both start from the same account store, made by studio.web_store (an scrypt password, an upload token and stored
files): Flask keeps it as web.sqlite3, and kerfkite-web moves a copy to its own store when it starts. The same
requests then go to both, as the same browser session would send them: the login, the session, saving, listing,
downloading and deleting files, the upload inbox, receivers, methods each route does not have, and generated
upload bodies. Their answers must agree: the status, a JSON answer value for value (identifiers, tokens and times
only in their form), the redirects, the cookies' names and attributes, and the security headers.

Where Flask answers with its HTML error page (abort(400), abort(403), abort(404), abort(405)) kerfkite-web answers
{"error": ...} with the same status; those are counted, not differences.

kerfkite-web runs with --behind-proxy, so this harness sends the PROXY protocol line itself; with --tls it runs
behind KerfKite's TLS relay instead (kerfkite-pendant-tls beside it) and the requests go over HTTPS.

Usage:
    PYTHONPATH=$PWD python validation/web_route_parity.py --server /tmp/kkb/kerfkite-web [--seed 1] [--cases 300] [--tls]

It prints one line per difference and a summary, and exits 1 when any case differs.
"""

import argparse
import hashlib
import http.client
import json
import random
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from werkzeug.security import generate_password_hash  # noqa: E402

from studio.web_server import create_app  # noqa: E402
from studio.web_store import WebStore  # noqa: E402

PASSWORD = 'parity password of some length'
SECURITY = (
    'Cache-Control',
    'X-Content-Type-Options',
    'Referrer-Policy',
    'Strict-Transport-Security',
    'Content-Security-Policy',
)
CORS = ('Access-Control-Allow-Origin', 'Access-Control-Allow-Methods', 'Access-Control-Allow-Headers', 'Vary')


class Answer:
    def __init__(self, status, headers, body):
        self.status = status
        self.headers = headers  # [(name, value)]
        self.body = body

    def header(self, name):
        for key, value in self.headers:
            if key.lower() == name.lower():
                return value
        return None

    def cookies(self):
        return [v for k, v in self.headers if k.lower() == 'set-cookie']

    def kind(self):
        return (self.header('Content-Type') or '').split(';')[0].strip()


class Reference:
    """The Flask app in this process."""

    def __init__(self, store, origin, destinations, allow_file_upload):
        self.app = create_app(store, origin, destinations, allow_file_upload)
        self.client = self.app.test_client(use_cookies=False)
        self.origin = origin

    def request(self, method, path, headers, body, peer):
        response = self.client.open(
            path, method=method, headers=headers, data=body, base_url=self.origin, environ_base={'REMOTE_ADDR': peer}
        )
        return Answer(response.status_code, list(response.headers.items()), response.get_data())


class Native:
    """A kerfkite-web process, behind the PROXY protocol or the TLS relay."""

    def __init__(self, program, data_dir, origin, allow_file_upload, tls):
        self.tls = tls
        arguments = [
            program,
            '--data-dir',
            str(data_dir),
            '--static',
            str(ROOT / 'studio' / 'static'),
            'serve',
            '--public-url',
            origin,
        ]
        port = int(urllib.parse.urlsplit(origin).port)
        if tls:
            cert, key = Path(data_dir).parent / 'web.pem', Path(data_dir).parent / 'web-key.pem'
            made = subprocess.run(
                [program, 'certificate', '--cert', str(cert), '--key', str(key), '--hosts', 'localhost', '127.0.0.1']
            )
            if made.returncode:
                raise SystemExit('kerfkite-web could not make a certificate')
            arguments += ['--cert', str(cert), '--key', str(key), '--host', '127.0.0.1', '--port', str(port)]
            self.context = ssl.create_default_context(cafile=str(cert))
        else:
            arguments += ['--behind-proxy', '--port', '0']
        if allow_file_upload:
            arguments.append('--allow-file-upload')
        self.log = Path(data_dir).parent / 'kerfkite-web.log'
        with self.log.open('w') as out:
            self.process = subprocess.Popen(arguments, stdout=out, stderr=subprocess.STDOUT)
        lines = []
        for _ in range(300):
            lines = self.log.read_text().splitlines()
            if len(lines) >= 2 or self.process.poll() is not None:
                break
            time.sleep(0.05)
        if len(lines) < 2 or 'HTTPS editor and upload inbox' not in lines[0]:
            raise SystemExit(f'kerfkite-web did not start: {lines!r}')
        self.port = port if tls else int(lines[1].rsplit(':', 1)[1])
        self.host = urllib.parse.urlsplit(origin).netloc

    def said(self, text):
        return text in self.log.read_text()

    def close(self):
        self.process.terminate()
        self.process.wait(10)

    def request(self, method, path, headers, body, peer):
        headers = {'Host': self.host, **headers}
        if self.tls:
            connection = http.client.HTTPSConnection('localhost', self.port, context=self.context, timeout=60)
            connection.putrequest(method, path, skip_host=True, skip_accept_encoding=True)
            for key, value in headers.items():
                connection.putheader(key, value)
            if body is not None:
                connection.putheader('Content-Length', str(len(body)))
            connection.endheaders(body)
            response = connection.getresponse()
            answer = Answer(response.status, response.getheaders(), response.read())
            connection.close()
            return answer
        head = f'PROXY TCP4 {peer} 127.0.0.1 40000 443\r\n{method} {path} HTTP/1.1\r\n'
        for key, value in headers.items():
            head += f'{key}: {value}\r\n'
        if body is not None:
            head += f'Content-Length: {len(body)}\r\n'
        raw = head.encode('latin-1') + b'\r\n' + (body or b'')
        with socket.create_connection(('127.0.0.1', self.port), timeout=60) as s:
            s.sendall(raw)
            data = b''
            while True:
                chunk = s.recv(1 << 16)
                if not chunk:
                    break
                data += chunk
        top, _, rest = data.partition(b'\r\n\r\n')
        lines = top.decode('latin-1').split('\r\n')
        status = int(lines[0].split(' ')[1])
        fields = [tuple(part.strip() for part in line.split(':', 1)) for line in lines[1:]]
        length = next((int(v) for k, v in fields if k.lower() == 'content-length'), len(rest))
        return Answer(status, fields, rest[:length] if method != 'HEAD' else b'')


RANDOM_KEYS = {'id', 'token', 'created'}


def normal(value):
    """A JSON answer with its random identifiers, tokens and times replaced by their form."""
    if isinstance(value, dict):
        out = {}
        for key, item in value.items():
            if key in RANDOM_KEYS and isinstance(item, str):
                out[key] = f'<{key}:{len(item)}>'
            elif key == 'created' and isinstance(item, (int, float)):
                out[key] = '<time>'
            else:
                out[key] = normal(item)
        return out
    if isinstance(value, list):
        return [normal(item) for item in value]
    return value


def cookie_form(text):
    parts = [p.strip() for p in text.split(';')]
    name = parts[0].split('=', 1)[0]
    empty = parts[0].endswith('=')
    attributes = [p for p in parts[1:] if not p.startswith('Expires=')]
    return name, empty, attributes


class Tally:
    def __init__(self, verbose=False):
        self.cases = self.same = self.html_errors = self.scrypt_unavailable = 0
        self.differences = []
        self.verbose = verbose

    def compare(self, label, ref, got):
        self.cases += 1
        if self.verbose:
            print(f'{ref.status} {got.status} {label}: {ref.body[:90]!r} | {got.body[:90]!r}')
        problems = []
        if ref.status != got.status:
            problems.append(f'status {ref.status} != {got.status}')
        for name in SECURITY + CORS:
            if (ref.header(name) or None) != (got.header(name) or None):
                problems.append(f'{name}: {ref.header(name)!r} != {got.header(name)!r}')
        if ref.header('Location') != got.header('Location'):
            problems.append(f'Location {ref.header("Location")!r} != {got.header("Location")!r}')
        if sorted(cookie_form(c) for c in ref.cookies()) != sorted(cookie_form(c) for c in got.cookies()):
            problems.append(f'cookies {ref.cookies()} != {got.cookies()}')
        html_error = False
        if ref.kind() == 'application/json' or got.kind() == 'application/json':
            if (
                ref.kind() == 'text/html'
                and got.kind() == 'application/json'
                and ref.status >= 400
                and ref.status == got.status
            ):
                html_error = True  # Flask's error page; kerfkite-web's {"error": ...}
            elif ref.kind() != got.kind():
                problems.append(f'type {ref.kind()!r} != {got.kind()!r}')
            else:
                a, b = normal(json.loads(ref.body)), normal(json.loads(got.body))
                if a != b:
                    problems.append(f'json {json.dumps(a)[:300]} != {json.dumps(b)[:300]}')
        elif ref.kind() == 'text/html' and ref.status < 400 and ref.status not in (204, 302):
            pattern = rb'name="csrf" value="[^"]*"'
            if re.sub(pattern, b'', ref.body) != re.sub(pattern, b'', got.body):
                problems.append(f'page {ref.body[:200]!r} != {got.body[:200]!r}')
        elif ref.kind() == 'text/html' and ref.status in (401, 429):
            if ref.body != got.body:
                problems.append(f'text {ref.body[:200]!r} != {got.body[:200]!r}')
        elif ref.status == 200 and ref.body != got.body:
            problems.append(f'body {len(ref.body)} bytes != {len(got.body)} bytes')
        if problems:
            self.differences.append(label + ': ' + '; '.join(problems))
            print('DIFFERENT', label, '; '.join(problems), flush=True)
        else:
            self.same += 1
            self.html_errors += html_error


class Session:
    """One browser's cookies and token on one server."""

    def __init__(self, server, origin, peer):
        self.server, self.origin, self.peer = server, origin, peer
        self.cookies, self.token = {}, ''

    def send(self, method, path, body=None, content_type=None, headers=None, csrf=True):
        fields = {}
        if self.cookies:
            fields['Cookie'] = '; '.join(f'{k}={v}' for k, v in self.cookies.items())
        if content_type:
            fields['Content-Type'] = content_type
        if method not in ('GET', 'HEAD', 'OPTIONS') and csrf:
            fields['Origin'] = self.origin
            if self.token:
                fields['X-Studio-Token'] = self.token
        fields.update(headers or {})
        if isinstance(body, str):
            body = body.encode('utf-8')
        answer = self.server.request(method, path, fields, body, self.peer)
        for text in answer.cookies():
            name, value = text.split(';')[0].split('=', 1)
            if value:
                self.cookies[name] = value
            else:
                self.cookies.pop(name, None)
        return answer

    def login(self, user, password):
        form = self.send('GET', '/login')
        match = re.search(rb'name="csrf" value="([^"]+)"', form.body)
        body = urllib.parse.urlencode(
            {'csrf': match.group(1).decode() if match else '', 'username': user, 'password': password}
        )
        return form, self.send('POST', '/login', body, 'application/x-www-form-urlencoded')


NAMES = [
    'part.nc',
    'Part_2.GCODE',
    'drawing.svg',
    'job.cncstudio.json',
    'set.templates.json',
    'tool.paths3d.json',
    'a.txt',
    '',
    '../x.nc',
    'a/b.nc',
    'a\\b.nc',
    ' lead.nc',
    'trail.nc ',
    '.hidden.nc',
    '_under.nc',
    'end_.nc_',
    'café.nc',
    'x' * 126 + '.nc',
    'x' * 130 + '.nc',
    'a b.nc',
    'con.nc',
    'LPT1.svg',
    'tab\t.nc',
    'semi;colon.nc',
    'NC',
    '.nc',
    'x.NC',
    'x.svg.svg',
    5,
    None,
    ['a.nc'],
]
CONTENTS = ['G0 X0\n', 'M5', '', 'x\x00y', 'é✓ text', 7, None, ['G0'], 'x' * 5000, '<svg/>']


def upload_body(rng):
    data = {}
    if rng.random() < 0.95:
        name = rng.choice(NAMES)
        if name is not None:
            data['name'] = name
    if rng.random() < 0.95:
        content = rng.choice(CONTENTS)
        if content is not None:
            data['content'] = content
    roll = rng.random()
    if roll < 0.2 and isinstance(data.get('content'), str):
        data['sha256'] = hashlib.sha256(data['content'].encode('utf-8', 'surrogatepass')).hexdigest()
    elif roll < 0.3:
        data['sha256'] = rng.choice(['', '0' * 64, 1, 0, [], ['x'], 'nope'])
    if rng.random() < 0.05:
        return json.dumps(rng.choice([[1, 2], 'text', 5, None]))
    return json.dumps(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--server', required=True, help='the kerfkite-web program')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--cases', type=int, default=300, help='generated upload bodies')
    parser.add_argument('--tls', action='store_true', help='through the TLS relay, over HTTPS')
    parser.add_argument('--verbose', action='store_true', help='print every case')
    a = parser.parse_args()
    rng = random.Random(a.seed)
    origin = 'https://localhost:18543'
    work = Path(tempfile.mkdtemp(prefix='kerfkite-web-parity-'))
    tally = Tally(a.verbose)
    native = None
    try:
        reference_dir, native_dir = work / 'reference', work / 'native'
        store = WebStore(reference_dir)
        # PBKDF2 hashes in werkzeug's format, which every system's kerfkite-web checks, and one account with the
        # scrypt hash studio.web_store writes, which only a system with libcrypto's scrypt can check.
        with store.db() as db:
            for name, password in (('parity', PASSWORD), ('other', 'the other password, long')):
                db.execute(
                    'INSERT INTO users VALUES(?,?)',
                    (name, generate_password_hash(password, method='pbkdf2:sha256:600000', salt_length=32)),
                )
        store.add_user('legacy', 'the legacy password, scrypt')
        token = store.issue_token('parity')
        for index in range(5):
            store.put_file('parity', f'stored-{index}.nc', f'G0 X{index}\n'.encode(), 'web' if index % 2 else 'upload')
            time.sleep(0.01)
        store.put_file('other', 'theirs.nc', b'G0 Y9\n', 'web')
        destinations = {
            'nobody': {'url': 'https://localhost:1', 'ca_file': None, 'allow_http': False, 'token': 'r' * 40},
            'plain': {'url': 'http://localhost:1', 'ca_file': None, 'allow_http': True, 'token': 'p' * 40},
        }
        (reference_dir / 'destinations.json').write_text(json.dumps(destinations))
        shutil.copytree(reference_dir, native_dir)
        reference = Reference(store, origin, destinations, allow_file_upload=True)
        native = Native(a.server, native_dir, origin, allow_file_upload=True, tls=a.tls)
        ref, got = Session(reference, origin, '192.0.2.10'), Session(native, origin, '192.0.2.10')

        def both(label, method, path, body=None, content_type=None, headers=None, csrf=True):
            tally.compare(
                label,
                ref.send(method, path, body, content_type, headers, csrf),
                got.send(method, path, body, content_type, headers, csrf),
            )

        # Before logging in.
        for path in (
            '/',
            '/editor.html',
            '/app.js',
            '/api/session',
            '/api/files',
            '/api/destinations',
            '/favicon.ico',
            '/login/',
        ):
            both('anonymous GET ' + path, 'GET', path)
        both('anonymous POST /api/files', 'POST', '/api/files', '{}', 'application/json')
        both('receive without a token', 'POST', '/api/receive', '{}', 'application/json')
        both(
            'receive with a wrong token',
            'POST',
            '/api/receive',
            '{}',
            'application/json',
            {'Authorization': 'Bearer nope'},
        )
        both('receive preflight from a file page', 'OPTIONS', '/api/receive', headers={'Origin': 'null'})
        both(
            'receive preflight from a site', 'OPTIONS', '/api/receive', headers={'Origin': 'https://elsewhere.example'}
        )
        both('another host', 'GET', '/login', headers={'Host': 'elsewhere.example'})

        # The login form and the login.
        both(
            'login without the form',
            'POST',
            '/login',
            'username=parity&password=x',
            'application/x-www-form-urlencoded',
        )
        for label, user, password in (
            ('wrong password', 'parity', 'wrong'),
            ('unknown user', 'nobody', PASSWORD),
            ('empty', '', ''),
        ):
            a_form, a_post = ref.login(user, password)
            b_form, b_post = got.login(user, password)
            tally.compare('login form', a_form, b_form)
            tally.compare('login: ' + label, a_post, b_post)
        a_form, a_post = ref.login('PARITY', PASSWORD)
        b_form, b_post = got.login('PARITY', PASSWORD)
        tally.compare('login form', a_form, b_form)
        tally.compare('login', a_post, b_post)
        # The account the Python made with scrypt: moved as it is.
        legacy_ref, legacy_got = Session(reference, origin, '192.0.2.11'), Session(native, origin, '192.0.2.11')
        a_form, a_post = legacy_ref.login('legacy', 'the legacy password, scrypt')
        b_form, b_post = legacy_got.login('legacy', 'the legacy password, scrypt')
        if a_post.status == 302 and b_post.status == 401 and native.said('cannot be checked here (scrypt)'):
            tally.scrypt_unavailable += 1  # Windows and macOS: reset-password sets a hash this system checks
        else:
            tally.compare('login with the Python\'s scrypt hash', a_post, b_post)
        a_session, b_session = ref.send('GET', '/api/session'), got.send('GET', '/api/session')
        tally.compare('session', a_session, b_session)
        ref.token = json.loads(a_session.body).get('token', '')
        got.token = json.loads(b_session.body).get('token', '')

        # Pages and the editor's files.
        for path in (
            '/',
            '/app.js',
            '/styles.css',
            '/static/app.js',
            '/no-such-file.js',
            '/api/profiles',
            '/api/files',
            '/api/destinations',
        ):
            both('GET ' + path, 'GET', path)
        both('HEAD /app.js', 'HEAD', '/app.js')
        # Writes without the token or from another origin.
        both(
            'save without the token',
            'POST',
            '/api/files',
            '{"name": "a.nc", "content": "x"}',
            'application/json',
            {'X-Studio-Token': 'nope'},
        )
        both(
            'save from another origin',
            'POST',
            '/api/files',
            '{"name": "a.nc", "content": "x"}',
            'application/json',
            {'Origin': 'https://elsewhere.example'},
        )
        both('save as text', 'POST', '/api/files', '{"name": "a.nc", "content": "x"}', 'text/plain')
        both('save broken JSON', 'POST', '/api/files', '{"name": ', 'application/json')
        # Saving, listing, downloading and deleting.
        saved = ref.send('POST', '/api/files', '{"name": "new.nc", "content": "G1 X5\\n"}', 'application/json')
        copied = got.send('POST', '/api/files', '{"name": "new.nc", "content": "G1 X5\\n"}', 'application/json')
        tally.compare('save', saved, copied)
        both('list', 'GET', '/api/files')
        stored_ref = [f for f in json.loads(ref.send('GET', '/api/files').body) if f['name'] == 'stored-3.nc'][0]['id']
        stored_got = [f for f in json.loads(got.send('GET', '/api/files').body) if f['name'] == 'stored-3.nc'][0]['id']
        tally.compare(
            'the moved files keep their identifiers',
            Answer(200, [], b''),
            Answer(200 if stored_ref == stored_got else 500, [], b''),
        )
        tally.compare(
            'download a moved file',
            ref.send('GET', '/api/files/' + stored_ref),
            got.send('GET', '/api/files/' + stored_got),
        )
        new_ref, new_got = json.loads(saved.body)['id'], json.loads(copied.body)['id']
        tally.compare('download', ref.send('GET', '/api/files/' + new_ref), got.send('GET', '/api/files/' + new_got))
        both(
            'download another account\'s file',
            'GET',
            '/api/files/' + [f for f in json.loads(got.send('GET', '/api/files').body)][0]['id'][::-1],
        )
        both('download nothing', 'GET', '/api/files/')
        tally.compare(
            'delete', ref.send('POST', f'/api/files/{new_ref}/delete'), got.send('POST', f'/api/files/{new_got}/delete')
        )
        both('delete again', 'POST', '/api/files/0123/delete')
        both('list after delete', 'GET', '/api/files')
        both('GET a delete route', 'GET', f'/api/files/{new_ref}/delete')

        # Methods each route does not have.
        for method, path in (
            ('PUT', '/api/session'),
            ('DELETE', '/api/files'),
            ('PATCH', '/login'),
            ('POST', '/api/session'),
            ('POST', '/api/profiles'),
            ('POST', '/app.js'),
            ('PUT', '/api/receive'),
            ('OPTIONS', '/api/files'),
            ('OPTIONS', '/'),
            ('POST', '/'),
        ):
            both(f'{method} {path}', method, path, '{}', 'application/json')
            if method not in ('GET', 'HEAD', 'OPTIONS'):
                both(
                    f'{method} {path} without the token', method, path, '{}', 'application/json', {'X-Studio-Token': ''}
                )

        # The routes every server has.
        project = {
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
        both('generate', 'POST', '/api/generate', json.dumps(project), 'application/json')
        both('generate as text', 'POST', '/api/generate', json.dumps(project), 'text/plain')
        both('generate a list', 'POST', '/api/generate', '[1, 2]', 'application/json')
        both('a design route', 'POST', '/api/solid-cad', '{"model": {"features": []}}', 'application/json')

        # The upload inbox, with the token made by the Python store.
        inbox = {'Authorization': 'Bearer ' + token}
        both(
            'receive',
            'POST',
            '/api/receive',
            '{"name": "in.gcode", "content": "G1 Y1\\n"}',
            'application/json',
            inbox,
            csrf=False,
        )
        both(
            'receive from a file page',
            'POST',
            '/api/receive',
            '{"name": "in2.gcode", "content": "G1 Y2\\n"}',
            'application/json',
            {**inbox, 'Origin': 'null'},
            csrf=False,
        )
        both('receive GET', 'GET', '/api/receive', headers=inbox)

        # Generated upload bodies, to the account, the inbox and a receiver.
        for case in range(a.cases):
            body = upload_body(rng)
            route = rng.choice(['/api/files', '/api/receive', '/api/send'])
            headers = inbox if route == '/api/receive' else None
            if route == '/api/send':
                data = json.loads(body)
                if isinstance(data, dict):
                    data['destination'] = rng.choice(['nobody', 'plain', 'missing', 5, None, ['nobody']])
                    if data['destination'] is None:
                        del data['destination']
                    if isinstance(data['destination'] if 'destination' in data else '', list):
                        continue  # the reference fails with a TypeError (a 500): not a case to compare
                    body = json.dumps(data)
            both(
                f'upload {case} to {route}: {body[:80]}',
                'POST',
                route,
                body,
                'application/json',
                headers,
                csrf=route != '/api/receive',
            )
        both('list at the end', 'GET', '/api/files')

        # Logging out, then ten failed logins from one address.
        both('logout', 'POST', '/api/logout')
        both('session after logout', 'GET', '/api/session')
        ref.cookies, got.cookies, ref.token, got.token = {}, {}, '', ''
        for attempt in range(11):
            a_form, a_post = ref.login('other', 'wrong password number ' + str(attempt))
            b_form, b_post = got.login('other', 'wrong password number ' + str(attempt))
            tally.compare(f'failed login {attempt}', a_post, b_post)
        a_form, a_post = ref.login('other', 'the other password, long')
        b_form, b_post = got.login('other', 'the other password, long')
        tally.compare('login after ten failures', a_post, b_post)
    finally:
        if native:
            native.close()
        shutil.rmtree(work, ignore_errors=True)
    print(
        f'{tally.cases} cases: {tally.same} the same ({tally.html_errors} of them Flask\'s HTML error page as JSON), '
        f'{len(tally.differences)} different'
    )
    if tally.scrypt_unavailable:
        print(
            'The account with the Python\'s scrypt hash was refused, as it is where the system has no scrypt: set its password again'
        )
    return 1 if tally.differences else 0


if __name__ == '__main__':
    sys.exit(main())
