"""Probe a simulated board from the Control tab, then run and export the job following that surface."""

import re

from playwright.sync_api import expect


def run(page):
    loc = page.locator
    tab = lambda title: page.get_by_role('tab', name=title, exact=True).click()
    page.evaluate('() => window.studioReady')
    tab('Design')
    loc('#shapeW').fill('40')
    loc('#shapeH').fill('24')
    loc('#rect').click()
    tab('CAM')
    loc('#mode').select_option('router')
    for key, value in {'depth': 0.1, 'step': 0.1, 'feed': 300, 'plunge': 60}.items():
        loc('#' + key).fill(str(value))
    loc('#preview').click()
    expect(loc('#gcode')).to_contain_text('G1 Z-0.100 F60.0')
    plain = loc('#gcode').text_content().count('\n')
    tab('Control')
    loc('#port').select_option('simulator')
    loc('#connect').click()
    expect(loc('#state')).to_contain_text('Idle')
    page.get_by_text('Probe levelling', exact=True).click()
    expect(loc('#levelUse')).to_be_disabled()
    loc('#levelProbe').click()
    expect(loc('#message')).to_have_text('Confirm the probe setup first')
    loc('#levelSpacing').fill('15')
    loc('#levelConfirmed').check()
    loc('#levelProbe').click()
    expect(loc('#levelState')).to_contain_text('Surface probed at', timeout=30000)
    state = loc('#levelState').text_content()
    points, low, high = re.search(r'at (\d+) points .*: ([-+\d.]+) to ([-+\d.]+) mm', state).groups()
    assert int(points) >= 9 and float(low) <= 0 <= float(high) and float(high) - float(low) > 0.01, state
    expect(loc('#levelUse')).to_be_checked()
    expect(loc('#levelConfirmed')).not_to_be_checked()
    print(f'PASS simulated surface probed at {points} points ({low} to {high} mm) and offered for the next run')

    with page.expect_download() as saved:
        loc('#levelExport').click()
    levelled = open(saved.value.path(), encoding='ascii').read().splitlines()
    cuts = [line for line in levelled if re.fullmatch(r'G1 X\S+ Y\S+ F\S+ Z\S+', line)]
    depths = {float(line.rsplit('Z', 1)[1]) for line in cuts}
    assert len(levelled) > plain and len(cuts) > 20 and len(depths) > 5, (len(levelled), plain, len(cuts))
    shallowest, deepest = -0.1 + float(high) + 0.001, -0.1 + float(low) - 0.001
    assert all(deepest < depth < shallowest for depth in depths), sorted(depths)
    print(f'PASS levelled program exported: {len(levelled)} lines, {len(cuts)} cutting pieces at {len(depths)} depths')

    loc('#confirmed').check()
    with page.expect_response(lambda response: response.url.endswith('/api/run')) as submitted:
        loc('#run').click()
    assert submitted.value.request.post_data_json['leveled'] is True
    assert submitted.value.json()['lines'] == len(levelled)
    expect(loc('#message')).to_contain_text('Job submitted')
    loc('#confirmed').check()
    loc('#armPhone').click()
    expect(loc('#message')).to_contain_text('A levelled job starts from this workstation')
    print('PASS the run request follows the probed surface, and a phone start is refused while levelling is on')

    expect(loc('#progress')).to_have_js_property('value', 100, timeout=30000)
    page.get_by_role('button', name='Set XYZ zero', exact=True).click()
    expect(loc('#levelState')).to_have_text('No surface probed', timeout=10000)
    expect(loc('#levelUse')).to_be_disabled()
    expect(loc('#levelUse')).not_to_be_checked()
    loc('#disconnect').click()
    print('PASS setting work zero forgets the probed surface')


if __name__ == '__main__':
    import argparse
    import logging
    import threading

    from playwright.sync_api import sync_playwright
    from werkzeug.serving import make_server

    from studio.device import DeviceController
    from studio.server import create_app

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', help='Path to a Chromium executable')
    arguments = parser.parse_args()
    logging.getLogger('werkzeug').setLevel(logging.ERROR)
    controller = DeviceController()
    server = make_server('127.0.0.1', 0, create_app(controller), threaded=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(executable_path=arguments.browser)
            workshop = browser.new_page(viewport={'width': 1440, 'height': 980}, accept_downloads=True)
            workshop.on('dialog', lambda dialog: dialog.accept())
            workshop.goto(f'http://127.0.0.1:{server.server_port}/')
            run(workshop)
            browser.close()
    finally:
        controller.disconnect()
        server.shutdown()
