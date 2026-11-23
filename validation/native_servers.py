"""Start KerfKite's C++ servers for the browser workflows, demos and checks.

The program is the path given, or KERFKITE_STUDIO, or build/native/kerfkite-studio (build/native/Release on
Windows) as native/README.md builds it. It runs on the loopback interface in simulation unless the caller passes
--enable-hardware, which no check in this folder does. With --monitor it also serves the paired phone monitor,
whose pairing address it prints.

    with studio() as server:         # server.url == 'http://127.0.0.1:PORT/'
        ...

web() starts the HTTPS editor, kerfkite-web (KERFKITE_WEB), the same way, through its TLS relay.
"""

import contextlib
import json
import os
import re
import socket
import subprocess
import tempfile
import threading
import urllib.parse
import urllib.request
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
        request = urllib.request.Request(self.monitor + 'api/status', headers={'Authorization': 'Bearer ' + self.token})
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
        request = urllib.request.Request(
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
