"""Operate the native Pocket pendant against Studio's simulated GRBL controller.

Run with --serial emulator-5554 --apk path/to/KerfKite-Pocket-Android.apk.
Requires a disposable, booted emulator, Android platform-tools and Studio's Python dependencies.
"""
import argparse
import logging
from pathlib import Path
import re
import secrets
import subprocess
import sys
import threading
import time
from xml.etree import ElementTree

from flask import request
from werkzeug.serving import make_server

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from studio.device import DeviceController
from studio.monitor import create_monitor_app
from studio.phone_jobs import PhoneJobs


class Phone:
    hierarchy = '/data/local/tmp/kerfkite-pendant-ui.xml'

    def __init__(self, executable, serial):
        if not re.fullmatch(r'emulator-\d+', serial):
            raise ValueError('Select a disposable Android emulator.')
        self.adb = [executable, '-s', serial]

    def run(self, *args):
        result = subprocess.run([*self.adb, *args], capture_output=True, timeout=40)
        if result.returncode:
            # The command arguments may contain the temporary pairing token.
            raise RuntimeError(f'ADB {args[0]} failed with exit status {result.returncode}')
        return result.stdout.decode('utf-8', errors='replace').strip()

    def find(self, label, button=False):
        self.run('shell', 'uiautomator', 'dump', self.hierarchy)
        tree = ElementTree.fromstring(self.run('shell', 'cat', self.hierarchy))
        for node in tree.iter('node'):
            if label not in (node.get('text'), node.get('content-desc'), node.get('resource-id')):
                continue
            if node.get('enabled') != 'true' or (button and node.get('class') != 'android.widget.Button'):
                continue
            bounds = [int(value) for value in re.findall(r'\d+', node.get('bounds', ''))]
            if len(bounds) == 4:
                left, top, right, bottom = bounds
                if right > left and bottom > top:
                    return (left + right) // 2, (top + bottom) // 2
        return None

    def tap(self, label, button=False):
        for attempt in range(7):
            point = self.find(label, button=button)
            if point:
                self.run('shell', 'input', 'tap', *(str(v) for v in point))
                return
            # Inspect both ends of the native scrolling layout on small displays.
            width, height = map(int, re.findall(r'(\d+)x(\d+)', self.run('shell', 'wm', 'size'))[-1])
            low, high = int(height * .78), int(height * .30)
            a, b = (low, high) if attempt < 3 else (high, low)
            self.run('shell', 'input', 'swipe', str(width // 2), str(a), str(width // 2), str(b), '280')
        raise AssertionError('Missing enabled UI control: ' + label)

    def confirm(self):
        point = wait_until(lambda: self.find('android:id/button1', button=True), 'Confirmation dialog missing.')
        self.run('shell', 'input', 'tap', *(str(v) for v in point))


def wait_until(condition, description, timeout=18):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = condition()
        if value:
            return value
        time.sleep(.15)
    raise AssertionError(description)


def exercise(phone, apk, route):
    assert phone.run('shell', 'getprop', 'sys.boot_completed') == '1', 'Boot the emulator before running.'
    assert phone.run('shell', 'getprop', 'ro.kernel.qemu') == '1', 'Select a disposable emulator.'
    controller = DeviceController()
    controller.connect('simulator')
    jobs = PhoneJobs(controller)
    token = secrets.token_urlsafe(32)
    app = create_monitor_app(controller, token, jobs)
    accepted = []

    @app.after_request
    def record(response):
        if request.method == 'POST' and response.status_code == 200:
            accepted.append(request.get_json())
        return response

    server = make_server('127.0.0.1', 0, app, threaded=True)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    remote = None
    try:
        phone.run('install', '-r', str(apk))
        if route == 'reverse':
            remote = phone.run('reverse', '--no-rebind', 'tcp:0', f'tcp:{server.server_port}')
            if not remote.isdigit():
                raise RuntimeError('ADB did not allocate an emulator port.')
            address = f'127.0.0.1:{remote}'
        else:
            # Android Emulator routes 10.0.2.2 to the host's loopback interface.
            address = f'10.0.2.2:{server.server_port}'
        phone.run('shell', 'am', 'force-stop', 'au.cncstudio.pocket')
        phone.run('shell', 'am', 'start', '-W', '-n', 'au.cncstudio.pocket/.WorkshopActivity')
        phone.tap('CNC pendant', button=True)
        phone.tap('Monitor pairing link')
        phone.run('shell', 'input', 'text', f'http://{address}/#token={token}')
        phone.run('shell', 'input', 'keyevent', 'KEYCODE_BACK')  # Dismiss keyboard.
        phone.tap('Pair', button=True)
        phone.confirm()
        wait_until(lambda: phone.find('Simulator · Idle · grbl'), 'Native screen did not show simulator status.')

        phone.tap('Jog X positive')
        wait_until(lambda: len(accepted) == 1 and not controller.status()['active'], 'Jog failed.')
        assert (accepted[0]['axis'], accepted[0]['distance'], accepted[0]['feed']) == ('X', .1, 100)
        assert any('$J=G91 G21 X0.100' in entry for entry in controller.status()['log'])
        for action, label in [('home', 'Home'), ('zero', 'Zero XYZ')]:
            phone.tap(label, button=True)
            phone.confirm()
            wait_until(lambda: accepted[-1]['action'] == action and not controller.status()['active'], action + ' failed.')

        # Leave enough simulated motion queued to exercise feed hold and resume through the UI.
        jobs.arm(['G1 X0 Y0 F100'] * 20_000)
        phone.tap('Start', button=True)
        phone.confirm()
        wait_until(lambda: controller.status()['active'], 'Armed job did not start.')
        phone.tap('Pause', button=True)
        wait_until(lambda: controller.status()['state'] == 'Hold', 'Feed hold failed.')
        phone.tap('Resume', button=True)
        wait_until(lambda: controller.status()['state'] == 'Run', 'Resume failed.')
        phone.tap('Stop', button=True)
        phone.confirm()
        wait_until(lambda: not controller.status()['active'], 'Abort failed.')
        assert not jobs.status()['armed']
        assert [action['action'] for action in accepted] == ['jog', 'home', 'zero', 'start', 'pause', 'resume', 'stop']
        assert len({action['request_id'] for action in accepted}) == len(accepted)
        phone.tap('Forget pairing', button=True)
        wait_until(lambda: phone.find('Monitor pairing link'), 'Forget did not restore pairing.')
    finally:
        server.shutdown()
        worker.join()
        server.server_close()
        controller.disconnect()
        if remote and remote.isdigit():
            phone.run('reverse', '--remove', 'tcp:' + remote)
    print(f'PASS native pair, X jog, home, zero, start, pause, resume, stop and forget over {route}.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='adb', help='ADB executable')
    parser.add_argument('--serial', required=True, help='Disposable emulator serial')
    parser.add_argument('--apk', required=True, type=Path, help='KerfKite Pocket APK to install')
    parser.add_argument('--route', choices=('reverse', 'lan'), default='reverse', help='ADB reverse or emulator virtual LAN')
    args = parser.parse_args()
    logging.getLogger('werkzeug').setLevel(logging.ERROR)
    exercise(Phone(args.adb, args.serial), args.apk.resolve(), args.route)
