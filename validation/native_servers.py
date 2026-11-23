"""Start KerfKite's C++ servers for the browser workflows, demos and checks.

The program is the path given, or KERFKITE_STUDIO, or build/native/kerfkite-studio (build/native/Release on
Windows) as native/README.md builds it. It runs on the loopback interface in simulation unless the caller passes
--enable-hardware, which no check in this folder does. With --monitor it also serves the paired phone monitor,
whose pairing address it prints.

    with studio() as server:         # server.url == 'http://127.0.0.1:PORT/'
        ...

web() starts the HTTPS editor, kerfkite-web (KERFKITE_WEB), the same way, through its TLS relay. Proxy stands
between a client and one of these servers, where a check sees, holds or changes what passes; owner() plays the
editor's part in a phone Start.
"""

import contextlib
import http.client
import http.server
import json
import os
import re
import socket
import subprocess
import tempfile
import threading
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STATIC = ROOT / 'studio' / 'static'


def program(name, given=None, variable=None):
    """The path of one of the native programs: given, from its environment variable, or from build/native."""
    if given:
        return str(Path(given).resolve())
    chosen = os.environ.get(variable) if variable else None
    if chosen:
        return chosen
    suffix = '.exe' if os.name == 'nt' else ''
    for folder in (ROOT / 'build' / 'native', ROOT / 'build' / 'native' / 'Release'):
        candidate = folder / (name + suffix)
        if candidate.is_file():
            return str(candidate)
    raise SystemExit(f'{name} was not found: build the native programs (native/README.md) or pass its path')


def beside(server, name):
    """Whether a program of that name is installed beside the server (the STEP add-on, the TLS relay)."""
    return (Path(server).parent / (name + ('.exe' if os.name == 'nt' else ''))).is_file()


class Running:
    """A started server: its address, and with --monitor the phone monitor's loopback address and pairing token."""

    def __init__(self, url, monitor=None, token=None):
        self.url, self.monitor, self.token = url, monitor, token

    def monitor_status(self):
        """The phone monitor's /api/status, as the paired phone reads it."""
        address, headers = self.monitor + 'api/status', {'Authorization': 'Bearer ' + self.token}
        request = urllib.request.Request(address, headers=headers)  # noqa: S310 - the loopback server under test
        with urllib.request.urlopen(request, timeout=10) as response:  # noqa: S310 - the loopback server under test
            return json.loads(response.read())

    def status(self):
        """The workstation's /api/status, as the editor reads it."""
        with urllib.request.urlopen(self.url + 'api/status', timeout=10) as response:  # noqa: S310 - as above
            return json.loads(response.read())

    def post(self, route, payload):
        """A POST to the workstation, with the session token the editor sends."""
        with urllib.request.urlopen(self.url + 'api/session', timeout=10) as response:  # noqa: S310 - as above
            token = json.loads(response.read())['token']
        request = urllib.request.Request(  # noqa: S310 - as above
            self.url + route,
            data=json.dumps(payload).encode(),
            headers={'Content-Type': 'application/json', 'X-Studio-Token': token},
        )
        with urllib.request.urlopen(request, timeout=30) as response:  # noqa: S310 - as above
            return json.loads(response.read())

    def enable_monitor(self, plaintext=True):
        """Phone pairing switched on as the Control tab switches it on; the monitor's address and token kept."""
        info = self.post('api/phone-monitor', {'action': 'enable', 'plaintext': plaintext})
        self.monitor = f'{info["scheme"]}://127.0.0.1:{info["port"]}/'
        self.token = urllib.parse.parse_qs(info['fragment'])['token'][0]
        return info

    def disable_monitor(self):
        self.post('api/phone-monitor', {'action': 'disable'})
        self.monitor = self.token = None

    def arm(self, project):
        """A job armed for a phone Start, as the editor arms a confirmed preview; its arm_id."""
        return self.post('api/arm', {'confirmed': True, 'phone_arm_challenge': True, 'project': project})['arm_id']


def moves(count):
    """A router project of about count straight cuts, for a job that runs long enough to pause, resume and stop
    from a phone: the simulator takes a line about every 10 ms."""
    zigzag = [[10 + 80 * (i % 2), 10 + 80 * i / count] for i in range(count)]
    return {'mode': 'router', 'width': 100, 'height': 100, 'paths': [zigzag], 'operation': 'line', 'depth': 1,
            'step': 1, 'feed': 600, 'plunge': 100, 'safe': 4, 'power': 1000, 'tool_diameter': 2}  # fmt: skip


@contextlib.contextmanager
def owner(server, arm_id, interval=0.25):
    """The editor's part in an armed preview while the block runs: it checks in as an open editor does, and confirms
    each phone Start that waits for it. Yields the challenges it confirmed."""
    done, confirmed = threading.Event(), []

    def answer():
        while not done.wait(interval):
            try:
                state = server.post('api/arm/check', {'arm_id': arm_id})
                if state.get('challenge'):
                    server.post('api/arm/confirm', {'arm_id': arm_id, 'challenge': state['challenge']})
                    confirmed.append(state['challenge'])
            except urllib.error.HTTPError:
                pass  # A Start that was cancelled or overtaken; the check reports it.

    thread = threading.Thread(target=answer, daemon=True)
    thread.start()
    try:
        yield confirmed
    finally:
        done.set()
        thread.join()


@dataclass
class Seen:
    """A request as Proxy passed it on: method, path, body and the address it came from."""

    method: str
    path: str
    body: bytes
    peer: str

    def json(self):
        return json.loads(self.body) if self.body else None


# Headers that belong to one connection, not to the request or answer carried on it.
HOP = {'connection', 'keep-alive', 'proxy-connection', 'transfer-encoding', 'te', 'trailer', 'upgrade'}


class Proxy:
    """A plain HTTP proxy in front of a server's address, for a check to see, hold or change what a client sends
    through it. before(seen) runs before a request is passed on and may wait; after(seen, status, body) runs before
    the answer is returned and may return a replacement body. It serves on host (the loopback unless a phone must
    reach it over the LAN) and port (any free one by default) until closed."""

    def __init__(self, upstream, before=None, after=None, host='127.0.0.1', port=0):
        address = urllib.parse.urlsplit(upstream)
        self.upstream, self.before, self.after = (address.hostname, address.port), before, after
        self.server = http.server.ThreadingHTTPServer((host, port), _Forward)
        self.server.daemon_threads = True
        self.server.proxy = self
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def __enter__(self):
        return self

    def __exit__(self, *exception):
        self.close()


class _Forward(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        proxy = self.server.proxy
        length = int(self.headers.get('Content-Length') or 0)
        seen = Seen(self.command, self.path, self.rfile.read(length) if length else b'', self.client_address[0])
        if proxy.before:
            proxy.before(seen)
        headers = {name: value for name, value in self.headers.items() if name.lower() not in HOP}
        headers['Host'] = '%s:%d' % proxy.upstream
        connection = http.client.HTTPConnection(*proxy.upstream, timeout=120)
        try:
            connection.request(self.command, self.path, body=seen.body or None, headers=headers)
            answer = connection.getresponse()
            body, status, reason, given = answer.read(), answer.status, answer.reason, answer.getheaders()
        finally:
            connection.close()
        if proxy.after:
            replaced = proxy.after(seen, status, body)
            body = body if replaced is None else replaced
        self.send_response(status, reason)
        for name, value in given:
            if name.lower() not in HOP | {'content-length', 'date', 'server'}:
                self.send_header(name, value)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    do_HEAD = do_POST = do_GET


@contextlib.contextmanager
def studio(given=None, args=(), port=0, env=None):
    """kerfkite-studio on the loopback, yielding a Running once it is ready; stopped afterwards."""
    command = [
        program('kerfkite-studio', given, 'KERFKITE_STUDIO'),
        '--port',
        str(port),
        '--static',
        str(STATIC),
        *args,
    ]
    process = subprocess.Popen(  # noqa: S603 - the program under test
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env
    )
    try:
        url = monitor = token = None
        lines = []
        for line in process.stdout:
            lines.append(line)
            found = re.match(r'KerfKite Studio at (http://127\.0\.0\.1:\d+/)', line)
            if found:
                url = found.group(1)
            paired = re.match(r'Phone pairing: (https?)://<PI-LAN-IP>:(\d+)/#(?:.*&)?token=([^&\s]+)', line)
            if paired:
                monitor, token = f'{paired.group(1)}://127.0.0.1:{paired.group(2)}/', paired.group(3)
            if line.strip() == 'KERFKITE_READY':
                break
        if url is None or process.poll() is not None:
            raise RuntimeError('kerfkite-studio did not start:\n' + ''.join(lines[-20:]))
        # Keep reading what it prints, so that a full pipe never stops it.
        threading.Thread(target=lambda: [None for _ in process.stdout], daemon=True).start()
        yield Running(url, monitor, token)
    finally:
        process.terminate()
        try:
            process.wait(10)
        except subprocess.TimeoutExpired:
            process.kill()


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


class Web:
    """A started kerfkite-web: its HTTPS origin, the certificate it serves, its data folder and program."""

    def __init__(self, origin, cert, data, program):
        self.origin, self.cert, self.data, self.program = origin, cert, data, program


@contextlib.contextmanager
def web(given=None, users=(), args=()):
    """kerfkite-web over HTTPS on the loopback with a certificate made for it and the accounts given as (name,
    password) pairs, yielding a Web once it serves; stopped and its data removed afterwards."""
    path = program('kerfkite-web', given, 'KERFKITE_WEB')
    with tempfile.TemporaryDirectory() as work:
        data, cert, key = str(Path(work) / 'web-data'), str(Path(work) / 'web.pem'), str(Path(work) / 'web-key.pem')
        for name, password in users:
            subprocess.run(  # noqa: S603 - the program under test
                [path, '--data-dir', data, 'create-user', name], input=f'{password}\n{password}\n', text=True,
                capture_output=True, check=True,
            )  # fmt: skip
        subprocess.run(  # noqa: S603
            [path, 'certificate', '--cert', cert, '--key', key, '--hosts', 'localhost', '127.0.0.1'],
            capture_output=True, check=True,
        )  # fmt: skip
        port = free_port()
        origin = f'https://127.0.0.1:{port}'
        command = [path, '--data-dir', data, '--static', str(STATIC), 'serve', '--public-url', origin, '--cert', cert,
                   '--key', key, '--host', '127.0.0.1', '--port', str(port), *args]  # fmt: skip
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)  # noqa: S603
        try:
            lines = [process.stdout.readline(), process.stdout.readline()]
            if 'HTTPS editor and upload inbox' not in lines[0] or process.poll() is not None:
                raise RuntimeError('kerfkite-web did not start: ' + ''.join(lines))
            threading.Thread(target=lambda: [None for _ in process.stdout], daemon=True).start()
            yield Web(origin, cert, data, path)
        finally:
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
