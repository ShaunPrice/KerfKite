"""Operate the native Pocket pendant on a physical handset against Studio's simulated GRBL controller.

The emulator workflow (android_pendant_workflow.py) refuses real devices. This one is for a
supervised handset on the same private network as the workstation:

    python validation/android_handset_pendant_workflow.py --serial 192.168.1.50:5555 --host 192.168.1.20

--serial is the ADB serial of the handset (USB or wireless debugging) and --host is the
workstation address the handset can reach. The workstation firewall must admit the handset on
--port for the duration of the run. Pocket must already be installed; the app is not reinstalled
and no project is modified. The controller is always the simulator: nothing here opens a serial port.

The run pairs over the LAN, sends a 0.1 mm X jog, Home and Zero XYZ, starts an armed job, pauses,
resumes, sends Pocket to the background and back with the job running, stops, and forgets the pairing.
It refuses to continue when the handset already holds a pairing unless --replace-pairing is given.

With --encrypted the run uses Studio's own phone listener and its pairing link, which carries the
fingerprint of the certificate created for that pairing (Pocket 0.5.0 or later). Without it the run
uses the plain HTTP listener that Pocket 0.4.1 understands.
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
from pocket_app import PACKAGE, component
from studio import phone_monitor_service
from studio.monitor import create_monitor_app
from studio.phone_jobs import PhoneJobs

# The launcher's own intent: returning with a different intent starts a second editor on top of the pendant.
LAUNCH = (
    'shell',
    'am',
    'start',
    '-W',
    '-a',
    'android.intent.action.MAIN',
    '-c',
    'android.intent.category.LAUNCHER',
    '-n',
    component(),
)


class Phone:
    hierarchy = '/data/local/tmp/kerfkite-handset-ui.xml'

    def __init__(self, executable, serial):
        self.adb = [executable, '-s', serial]

    def run(self, *args):
        result = subprocess.run([*self.adb, *args], capture_output=True, timeout=40)
        if result.returncode:
            # The command arguments may contain the temporary pairing token.
            raise RuntimeError(f'ADB {args[0]} failed with exit status {result.returncode}')
        return result.stdout.decode('utf-8', errors='replace').strip()

    def tree(self):
        # A dump can fail while a window is animating on a real device.
        for _ in range(4):
            try:
                self.run('shell', 'uiautomator', 'dump', self.hierarchy)
                return ElementTree.fromstring(self.run('shell', 'cat', self.hierarchy))
            except (RuntimeError, ElementTree.ParseError):
                time.sleep(0.5)
        raise RuntimeError('UI hierarchy unavailable')

    def find(self, label, button=False, prefix=False):
        for node in self.tree().iter('node'):
            values = (node.get('text'), node.get('content-desc'), node.get('resource-id'))
            matched = any(v and v.startswith(label) for v in values) if prefix else label in values
            if not matched or node.get('enabled') != 'true':
                continue
            if button and node.get('class') != 'android.widget.Button':
                continue
            bounds = [int(value) for value in re.findall(r'\d+', node.get('bounds', ''))]
            if len(bounds) == 4:
                left, top, right, bottom = bounds
                if right > left and bottom > top:
                    return (left + right) // 2, (top + bottom) // 2
        return None

    def tap(self, label, button=False):
        width, height = map(int, re.findall(r'(\d+)x(\d+)', self.run('shell', 'wm', 'size'))[-1])
        for attempt in range(7):
            point = self.find(label, button=button)
            if point:
                self.run('shell', 'input', 'tap', *(str(v) for v in point))
                return
            low, high = int(height * 0.78), int(height * 0.30)
            a, b = (low, high) if attempt < 3 else (high, low)
            self.run('shell', 'input', 'swipe', str(width // 2), str(a), str(width // 2), str(b), '280')
        raise AssertionError('Missing enabled UI control: ' + label)

    def confirm(self):
        point = wait_until(lambda: self.find('android:id/button1', button=True), 'Confirmation dialog missing.')
        self.run('shell', 'input', 'tap', *(str(v) for v in point))

    def cleanup(self):
        try:
            self.run('shell', 'rm', '-f', self.hierarchy)
        except RuntimeError:
            pass


def wait_until(condition, description, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = condition()
        if value:
            return value
        time.sleep(0.15)
    raise AssertionError(description)


def step(name, action):
    started = time.monotonic()
    action()
    print(f'PASS {name} ({time.monotonic() - started:.1f} s)', flush=True)


def exercise(phone, host, port, replace_pairing, encrypted=False):
    assert phone.run('shell', 'getprop', 'sys.boot_completed') == '1', 'Unlock and boot the handset before running.'
    assert phone.run('shell', 'pm', 'list', 'packages', PACKAGE), 'Install KerfKite Pocket on the handset first.'
    print(
        'Handset:',
        phone.run('shell', 'getprop', 'ro.product.model'),
        '· Android',
        phone.run('shell', 'getprop', 'ro.build.version.release'),
        '· API',
        phone.run('shell', 'getprop', 'ro.build.version.sdk'),
        flush=True,
    )
    controller = DeviceController()
    controller.connect('simulator')
    jobs = PhoneJobs(controller)
    token = secrets.token_urlsafe(32)
    accepted, peers = [], set()

    def record(response):
        peers.add(request.remote_addr)
        if request.method == 'POST' and response.status_code == 200:
            accepted.append(request.get_json())
        return response

    if encrypted:
        # Studio's own listener, as the workstation starts it. Its monitor app is given the same recorder.
        build = phone_monitor_service.create_monitor_app

        def recording(*args, **kwargs):
            app = build(*args, **kwargs)
            app.after_request(record)
            return app

        phone_monitor_service.create_monitor_app = recording
        monitor = phone_monitor_service.PhoneMonitor(controller, jobs, port=port)
        try:
            info = monitor.enable()
        finally:
            phone_monitor_service.create_monitor_app = build
        assert info['encryption'] == 'pinned', 'Install the web extra: the listener is not encrypted.'
        link = f'https://{host}:{port}/#{info["fragment"]}'
        print('Listener: HTTPS, certificate fingerprint', info['fingerprint'][:16] + '…', flush=True)

        def close():
            monitor.disable()
    else:
        app = create_monitor_app(controller, token, jobs)
        app.after_request(record)
        server = make_server('0.0.0.0', port, app, threaded=True)  # noqa: S104 - the handset connects over the LAN
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        link = f'http://{host}:{port}/#token={token}'

        def close():
            server.shutdown()
            worker.join()
            server.server_close()

    paired = False
    try:
        phone.run('shell', 'am', 'force-stop', PACKAGE)
        phone.run(*LAUNCH)
        phone.tap('CNC pendant', button=True)
        time.sleep(1.2)
        if phone.find('Forget pairing', button=True):
            if not replace_pairing:
                raise AssertionError(
                    'The handset already holds a pendant pairing. Rerun with --replace-pairing to forget it.'
                )
            phone.tap('Forget pairing', button=True)
            time.sleep(1)

        def pair():
            phone.tap('Monitor pairing link')
            # The handset's shell would otherwise treat the & before the fingerprint as a command separator.
            phone.run('shell', 'input', 'text', link.replace('&', '\\&'))
            # Back would leave the pendant when no keyboard is showing.
            if 'mInputShown=true' in phone.run('shell', 'dumpsys', 'input_method'):
                phone.run('shell', 'input', 'keyevent', 'KEYCODE_BACK')
            phone.tap('Pair', button=True)
            phone.confirm()
            wait_until(
                lambda: phone.find('Simulator', prefix=True),
                'The pendant did not show simulator status. Check that the workstation firewall admits the handset on the monitor port.',
            )

        step('pair over the LAN and show simulator status', pair)
        paired = True

        def jog():
            phone.tap('Jog X positive')
            wait_until(lambda: len(accepted) == 1 and not controller.status()['active'], 'Jog failed.')
            assert (accepted[0]['axis'], accepted[0]['distance'], accepted[0]['feed']) == ('X', 0.1, 100)
            assert any('$J=G91 G21 X0.100' in entry for entry in controller.status()['log'])

        step('jog X +0.1 mm', jog)

        for action, label in [('home', 'Home'), ('zero', 'Zero XYZ')]:

            def confirmed(action=action, label=label):
                phone.tap(label, button=True)
                phone.confirm()
                wait_until(
                    lambda: accepted[-1]['action'] == action and not controller.status()['active'], action + ' failed.'
                )

            step(action, confirmed)

        # Leave enough simulated motion queued to exercise feed hold, resume and a background return.
        jobs.arm(['G1 X0 Y0 F100'] * 20_000)

        def start():
            phone.tap('Start', button=True)
            phone.confirm()
            wait_until(lambda: controller.status()['active'], 'Armed job did not start.')

        step('start armed job', start)

        def pause():
            phone.tap('Pause', button=True)
            wait_until(lambda: controller.status()['state'] == 'Hold', 'Feed hold failed.')

        step('pause', pause)

        def resume():
            phone.tap('Resume', button=True)
            wait_until(lambda: controller.status()['state'] == 'Run', 'Resume failed.')

        step('resume', resume)

        def background():
            phone.run('shell', 'input', 'keyevent', 'KEYCODE_HOME')
            time.sleep(6)
            assert controller.status()['active'], 'The job stopped while Pocket was in the background.'
            phone.run(*LAUNCH)
            wait_until(lambda: phone.find('Stop', button=True), 'Pendant controls missing after returning to Pocket.')

        step('background for 6 s and return to the pendant with the job running', background)

        def stop():
            phone.tap('Stop', button=True)
            phone.confirm()
            wait_until(lambda: not controller.status()['active'], 'Abort failed.')
            assert not jobs.status()['armed']

        step('stop after the background return', stop)

        assert [action['action'] for action in accepted] == ['jog', 'home', 'zero', 'start', 'pause', 'resume', 'stop']
        assert len({action['request_id'] for action in accepted}) == len(accepted)

        def forget():
            phone.tap('Forget pairing', button=True)
            wait_until(lambda: phone.find('Monitor pairing link'), 'Forget did not restore pairing.')

        step('forget pairing', forget)
        paired = False
    finally:
        close()
        controller.disconnect()
        phone.cleanup()
        if paired:
            print('The handset still holds the test pairing. Open CNC pendant and choose Forget pairing.', flush=True)
    assert len(peers) == 1, 'Requests arrived from more than one address.'
    print(
        'PASS native pair, X jog, home, zero, start, pause, resume, background return, stop and forget on a physical handset'
        + (' over the pinned HTTPS listener.' if encrypted else '.')
    )


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--adb', default='adb', help='ADB executable')
    parser.add_argument('--serial', required=True, help='ADB serial of the supervised handset')
    parser.add_argument('--host', required=True, help='Workstation address the handset can reach')
    parser.add_argument('--port', type=int, default=8766, help='Monitor port to listen on')
    parser.add_argument('--replace-pairing', action='store_true', help='Forget a pairing already stored on the handset')
    parser.add_argument('--encrypted', action='store_true', help="Pair with Studio's pinned HTTPS listener")
    args = parser.parse_args()
    logging.getLogger('werkzeug').setLevel(logging.ERROR)
    exercise(Phone(args.adb, args.serial), args.host, args.port, args.replace_pairing, args.encrypted)
