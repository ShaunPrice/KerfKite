"""Check Pocket's certificate pinning against a real Studio phone listener, without a phone.

Compiles the plain-Java pinning class Pocket uses (PinnedTls) with a JDK, then makes pinned requests to
phone listeners started here on the simulator. Requires `javac` and `java` on PATH and Studio's web extra.

    python validation/pendant_tls_pin_check.py
"""

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from studio.device import DeviceController
from studio.phone_jobs import PhoneJobs
from studio.phone_monitor_service import PhoneMonitor

SOURCES = ROOT / 'android/app/src'
PACKAGE = 'au/cncstudio/pocket'


def main():
    if not shutil.which('javac') or not shutil.which('java'):
        raise SystemExit('A JDK (javac and java) is required.')
    controller = DeviceController()
    controller.connect('simulator')
    jobs = PhoneJobs(controller)
    genuine, impostor, plain = (PhoneMonitor(controller, jobs, port=0, plaintext=flag) for flag in (False, False, True))
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
                'Install the web extra: the listener is not encrypted.'
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
            controller.disconnect()
    print('PASS Pocket certificate pinning against the Studio phone listener')


if __name__ == '__main__':
    main()
