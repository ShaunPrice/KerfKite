"""Check Pocket's certificate pinning against a real Studio phone listener, without a phone.

Compiles the plain-Java pinning class Pocket uses (PinnedTls) with a JDK, then makes pinned requests to the phone
listeners of workstation servers (kerfkite-studio) started here on the simulator, with their TLS relay
(kerfkite-pendant-tls, beside the server unless --relay names it), on the loopback interface only. Requires `javac`
and `java` on PATH.

    python validation/pendant_tls_pin_check.py [--server build/native/kerfkite-studio] [--relay PROGRAM]
"""

import shutil
import subprocess
import tempfile
from pathlib import Path

from native_servers import program

ROOT = Path(__file__).resolve().parents[1]

SOURCES = ROOT / 'android/app/src'
PACKAGE = 'au/cncstudio/pocket'


class NativeListener:
    """A kerfkite-studio process whose phone listener is enabled through its own route, as the editor does."""

    def __init__(self, program, relay, plaintext):
        import json
        import urllib.request

        self.json, self.request = json, urllib.request
        command = [program, '--port', '0', '--monitor-host', '127.0.0.1', '--monitor-port', '0']
        command += ['--pendant-tls', relay] + (['--monitor-plaintext'] if plaintext else [])
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE, text=True)  # noqa: S603
        line = self.process.stdout.readline()
        self.url = line.split(' at ', 1)[1].strip()
        self.token = self.call('GET', 'api/session')['token']

    def call(self, method, path, body=None):
        data = None if body is None else self.json.dumps(body).encode()
        headers = {'Content-Type': 'application/json', 'X-Studio-Token': getattr(self, 'token', '')}
        with self.request.urlopen(
            self.request.Request(self.url + path, data, headers, method=method), timeout=30
        ) as reply:  # noqa: S310
            return self.json.loads(reply.read())

    def enable(self):
        return self.call('POST', 'api/phone-monitor', {'action': 'enable'})

    def disable(self):
        try:
            self.call('POST', 'api/phone-monitor', {'action': 'disable'})
        finally:
            self.process.terminate()
            self.process.wait(10)


def main():
    import argparse

    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--server', help='kerfkite-studio (default: KERFKITE_STUDIO or build/native)')
    parser.add_argument('--relay', help='its TLS relay, kerfkite-pendant-tls')
    arguments = parser.parse_args()
    if not shutil.which('javac') or not shutil.which('java'):
        raise SystemExit('A JDK (javac and java) is required.')
    server = Path(program('kerfkite-studio', arguments.server, 'KERFKITE_STUDIO'))
    relay = arguments.relay or str(server.with_name('kerfkite-pendant-tls' + server.suffix))
    genuine, impostor, plain = (NativeListener(str(server), relay, flag) for flag in (False, False, True))
    with tempfile.TemporaryDirectory() as classes:
        files = [SOURCES / 'main/java' / PACKAGE / 'PinnedTls.java']
        files += [SOURCES / 'test/java' / PACKAGE / name for name in ('PinnedTlsCheck.java', 'PinnedTlsProbe.java')]
        subprocess.run(['javac', '-d', classes, *map(str, files)], check=True)

        def java(name, *args):
            command = ['java', '-cp', classes, 'au.cncstudio.pocket.' + name, *args]
            done = subprocess.run(command, capture_output=True, text=True, timeout=60)
            if done.returncode:
                # A JVM that could not run the class says nothing useful on stdout: stop with what it reported.
                raise SystemExit(
                    f'{name} exited with status {done.returncode}:\n{done.stderr.strip() or done.stdout.strip()}'
                )
            return done.stdout.strip()

        print(java('PinnedTlsCheck'))
        try:
            a, b, c = genuine.enable(), impostor.enable(), plain.enable()
            assert a['encryption'] == b['encryption'] == 'pinned', (
                'The listener is not encrypted: is the TLS relay beside the server?'
            )
            assert a['fingerprint'] != b['fingerprint'], 'Each pairing must have its own certificate.'
            assert c['encryption'] == 'none' and c['fingerprint'] is None

            def probe(listener, fingerprint, token):
                return java('PinnedTlsProbe', f"https://127.0.0.1:{listener['port']}", fingerprint, token)

            cases = [
                ('the pinned certificate is accepted', probe(a, a['fingerprint'], a['token']), 'STATUS 200'),
                ('a wrong bearer token is refused by Studio', probe(a, a['fingerprint'], 'x' * 43), 'STATUS 401'),
                ('another fingerprint is rejected', probe(a, b['fingerprint'], a['token']), 'REJECTED'),
                ('a different receiver at the address is rejected', probe(b, a['fingerprint'], a['token']), 'REJECTED'),
                ('a plaintext listener is rejected', probe(c, a['fingerprint'], c['token']), 'REJECTED'),
            ]
            for name, result, expected in cases:
                assert result.startswith(expected), f'{name}: expected {expected}, got {result!r}'
                print(f'PASS {name} ({result})')
        finally:
            for monitor in (genuine, impostor, plain):
                monitor.disable()
    print('PASS Pocket certificate pinning against the Studio phone listener')


if __name__ == '__main__':
    main()
