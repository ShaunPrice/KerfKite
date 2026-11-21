"""Connect a synthetic Grbl controller and watch the workspace follow what it reports."""

import logging
import threading
from collections import deque

from playwright.sync_api import expect
from werkzeug.serving import make_server

from studio.device import DeviceController
from studio.server import create_app

SETTINGS = {'13': 0, '20': 1, '22': 1, '30': 12000, '32': 0, '110': 5000, '111': 4000, '112': 600, '130': 420, '131': 297, '132': 60}  # fmt: skip
ANSWERS = {
    b'$$': [f'${key}={value}' for key, value in SETTINGS.items()] + ['ok'],
    b'$I': ['[VER:1.1h.20190825:]', '[OPT:V,15,128]', 'ok'],
    b'?': ['<Idle|MPos:0.000,0.000,0.000|FS:0,0>'],
}


class SyntheticGrbl:
    """A serial port that answers the controller's start-up questions and nothing else."""

    def __init__(self, *args, **kwargs):
        self.incoming = deque([b"Grbl 1.1h ['$' for help]\n"])
        self.writes = []

    def write(self, data):
        self.writes.append(data)
        for prefix, lines in ANSWERS.items():
            if prefix in data:
                self.incoming.extend(line.encode('ascii') + b'\n' for line in lines)
        return len(data)

    def readline(self, size=257):
        return self.incoming.popleft() if self.incoming else b''

    def close(self):
        pass


def run(page):
    logging.getLogger('werkzeug').setLevel(logging.ERROR)
    ports, written = [], []

    def open_port(*args, **kwargs):
        port = SyntheticGrbl()
        ports.append(port)
        return port

    device = DeviceController()
    device.driver.factory = open_port
    device.driver.ports = lambda: [{'device': 'KK-SYNTHETIC', 'description': 'Synthetic Grbl controller'}]
    server = make_server('127.0.0.1', 0, create_app(device, allow_hardware=True), threaded=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with page.context.browser.new_context(viewport={'width': 1440, 'height': 980}) as context:
            ui = context.new_page()
            ui.on('dialog', lambda dialog: dialog.accept())
            ui.goto(f'http://127.0.0.1:{server.server_port}/')
            ui.evaluate('() => window.studioReady')
            loc = ui.locator
            tab = lambda title: ui.get_by_role('tab', name=title, exact=True).click()
            tab('CAM')
            loc('#mode').select_option('router')
            loc('#feed').fill('6000')
            loc('#feed').dispatch_event('change')
            before = (loc('#width').input_value(), loc('#height').input_value())
            assert before != ('420', '297'), before
            tab('Control')
            loc('#port').select_option('KK-SYNTHETIC')
            loc('#connect').click()
            expect(loc('#message')).to_contain_text(
                'New machine: Grbl 1.1h.20190825 · 420 × 297 mm, Z travel 60 mm', timeout=20000
            )
            expect(loc('#message')).to_contain_text('Feed lowered to the controller limit of 4000')
            expect(loc('#message')).to_contain_text('Reported: X travel 420 mm, Y travel 297 mm, Z travel 60 mm')
            assert (loc('#width').input_value(), loc('#height').input_value()) == ('420', '297')
            assert loc('#feed').input_value() == '4000' and loc('#power').get_attribute('max') == '12000'
            assert loc('#profile option:checked').text_content() == 'Grbl 1.1h.20190825 · 420 × 297 mm'
            print('PASS a newly connected controller sets the work area, limits and a saved machine profile')

            loc('#width').fill('250')
            loc('#width').dispatch_event('change')
            loc('#disconnect').click()
            expect(loc('#state')).to_contain_text('Disconnected')
            loc('#connect').click()
            expect(loc('#message')).to_contain_text('Recognised Grbl 1.1h.20190825 · 420 × 297 mm', timeout=20000)
            assert loc('#width').input_value() == '250'  # a later change to the workspace is kept
            loc('#disconnect').click()
            expect(loc('#state')).to_contain_text('Disconnected')
            loc('#profile').select_option(index=0)
            assert loc('#width').input_value() != '420'
            loc('#connect').click()
            # A project set up for another machine follows this one.
            expect(loc('#width')).to_have_value('420', timeout=20000)
            assert loc('#profile option').evaluate_all('options => options.filter(o => o.dataset.custom).length') == 1
            print(
                'PASS the same controller is recognised on reconnection; edits are kept and its profile is not duplicated'
            )

            for port in ports:
                written += port.writes
            assert len(ports) == 3 and written and all(data in (b'$$\n', b'$I\n', b'?', b'\x18') for data in written), (
                set(written)
            )
            assert b'\x18' not in written
            print('PASS only $$, $I and status requests were sent: nothing was written to the controller settings')
            loc('#disconnect').click()
    finally:
        device.disconnect()
        server.shutdown()


if __name__ == '__main__':
    import argparse

    from playwright.sync_api import sync_playwright

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', help='Path to a Chromium executable')
    arguments = parser.parse_args()
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(executable_path=arguments.browser)
        run(browser.new_page())
        browser.close()
