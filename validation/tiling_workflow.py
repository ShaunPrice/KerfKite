"""Divide a job larger than the machine into tiles, export one and run one on the simulator."""

import re

from playwright.sync_api import expect


def run(page):
    loc = page.locator
    tab = lambda title: page.get_by_role('tab', name=title, exact=True).click()
    page.evaluate('() => window.studioReady')
    tab('Design')
    for key, value in {'width': 600, 'height': 300}.items():
        loc('#' + key).fill(str(value))
        loc('#' + key).dispatch_event('change')
    for key, value in {'shapeX': 20, 'shapeY': 20, 'shapeW': 500, 'shapeH': 260}.items():
        loc('#' + key).fill(str(value))
    loc('#rect').click()
    loc('#line').click()
    tab('CAM')
    loc('#mode').select_option('router')
    for key, value in {'depth': 1.5, 'step': 1.5, 'feed': 900, 'plunge': 200}.items():
        loc('#' + key).fill(str(value))
    page.evaluate("() => { document.getElementById('tilingPanel').open = true; }")
    loc('#tilePlan').click()
    expect(loc('#message')).to_have_text('Generate a current preview first')
    loc('#preview').click()
    expect(loc('#gcode')).to_contain_text('G1 Z-1.500 F200.0')
    loc('#tileWidth').fill('200')
    loc('#tileHeight').fill('150')
    loc('#tilePlan').click()
    expect(loc('#tileState')).to_contain_text('6 tiles with cutting in a grid of 3 × 2, each 200 × 150 mm')
    expect(loc('#tileList [data-tile]')).to_have_count(6)
    print('PASS a 500 × 260 mm job is divided into six tiles of 200 × 150 mm')

    with page.expect_download() as saved:
        loc('#tileList [data-tile="2,1"] button').first.click()
    assert saved.value.suggested_filename == 'tile-column3-row2.nc'
    program = open(saved.value.path(), encoding='ascii').read().splitlines()
    cuts = [line for line in program if line.startswith('G1 X')]
    assert cuts and program[-1] == 'M5', program[-3:]
    for line in program:
        found = re.fullmatch(r'G[01] X([-\d.]+) Y([-\d.]+).*', line)
        if found:
            assert -1e-6 <= float(found.group(1)) <= 200 and -1e-6 <= float(found.group(2)) <= 150, line
    print(f'PASS the far tile exports as its own program of {len(program)} lines, all inside 200 × 150 mm')

    # Run a tile: it needs the usual confirmation and goes through the run route with the tile named.
    tab('Control')
    loc('#port').select_option('simulator')
    loc('#connect').click()
    expect(loc('#state')).to_contain_text('Idle')
    tab('CAM')
    loc('#tileList [data-tile="0,0"] button').nth(1).click()
    expect(loc('#message')).to_contain_text('tick the check that work zero')
    page.evaluate("() => { document.getElementById('confirmed').checked = true; }")
    with page.expect_response(lambda response: response.url.endswith('/api/run')) as submitted:
        loc('#tileList [data-tile="0,0"] button').nth(1).click()
    assert submitted.value.request.post_data_json['tile'] == {'width': 200, 'height': 150, 'column': 0, 'row': 0}
    expect(loc('#message')).to_contain_text('Tile column 1, row 1 submitted')
    for _ in range(300):
        final = page.evaluate("() => api('status')")
        if not final['active'] and final['progress'] == 100:
            break
        page.wait_for_timeout(100)
    position = final['position']
    assert position, (final['state'], final['error'], final['progress'], final['log'][-5:])
    assert 0 <= position['x'] <= 200 and 0 <= position['y'] <= 150, position
    print(f'PASS the first tile ran on the simulator and the tool stayed inside it, ending at {position}')

    # An edit makes the tiles stale.
    tab('Design')
    loc('#rect').click()
    tab('CAM')
    loc('#tileList [data-tile="1,0"] button').first.click()
    expect(loc('#message')).to_contain_text('Divide the job into tiles again')
    print('PASS tiles are refused after the design changes')
    tab('Control')
    loc('#disconnect').click()


if __name__ == '__main__':
    import argparse

    from playwright.sync_api import sync_playwright

    from native_servers import studio

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', help='Path to a Chromium executable')
    parser.add_argument('--server', help='kerfkite-studio (default: KERFKITE_STUDIO or build/native)')
    arguments = parser.parse_args()
    with studio(arguments.server) as server, sync_playwright() as playwright:
        browser = playwright.chromium.launch(executable_path=arguments.browser)
        workshop = browser.new_page(viewport={'width': 1440, 'height': 980}, accept_downloads=True)
        workshop.on('dialog', lambda dialog: dialog.accept())
        workshop.goto(server.url)
        run(workshop)
        browser.close()
