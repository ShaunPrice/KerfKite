"""Exercise rotary posting, physical-axis mapping and controller simulation via the Studio UI."""

import json
import math
import re
import time
from pathlib import Path

from playwright.sync_api import expect


def run(page):
    page.evaluate('() => studioReady')
    loc = page.locator
    tab = lambda name: page.get_by_role('tab', name=name, exact=True).click()
    errors = []
    page.on('pageerror', lambda e: errors.append(str(e)))
    requests = []
    page.on(
        'request',
        lambda req: (
            requests.append((req.url.split('/api/')[-1], req.post_data_json))
            if req.method == 'POST' and req.url.split('/api/')[-1] in ('arm', 'run')
            else None
        ),
    )

    def load(mode):
        tab('Projects')
        loc('#loadProject').click()
        end = math.pi * 10
        settings = {
            'mode': mode,
            'protocol': 'grblhal',
            'width': 40,
            'height': 40,
            'operation': 'line',
            'feed': 600,
            'power': 750,
            'plunge': 110,
            'safe': 3,
            'depth': 1,
            'step': 1,
            'passes': 1,
        }
        obj = {
            'name': 'Wrapped ' + mode,
            'paths': [[[3, 0], [6, end / 2], [3, end]]],
            **({'depths': [[-0.25, -0.8, -0.25]]} if mode == 'router' else {'powers': [[0.2, 0.8, 0.2]]}),
        }
        content = json.dumps({'format': 'pi-cnc-studio/1', 'objects': [obj], 'settings': settings}).encode()
        loc('#file').set_input_files(
            {'name': mode + '.cncstudio.json', 'mimeType': 'application/json', 'buffer': content}
        )
        page.wait_for_function('mode=>CNCProjects.document().objects[0]?.name === "Wrapped "+mode', arg=mode)
        tab('Rotary')
        loc('#rotaryDiameter').fill('10')
        loc('#rotaryPost').select_option('grblhal')

    def settled():
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            state = page.request.get(page.url.rstrip('/') + '/api/status').json()
            if state['state'] == 'Idle' and state['progress'] == 100 and not state['active']:
                return
            page.wait_for_timeout(100)
        raise AssertionError('Rotary simulator job failed to finish')

    load('router')
    loc('#rotaryFeed').select_option('cut')
    loc('#rotarySeam').click()
    expect(loc('#rotaryStatus')).to_contain_text('1 seam endpoint pair')
    loc('#rotaryOutput').select_option('B')
    loc('#rotaryPreview').click()
    expect(loc('#rotaryMetrics')).to_contain_text('B 0.000 to 360.000°')
    expect(loc('#rotaryCode')).to_contain_text('B360.000000')
    expect(loc('#rotaryCode')).to_contain_text('Z-0.800000')
    loc('#rotaryProgress').fill('10')
    loc('#rotaryPlay').click()
    page.wait_for_function("Number(document.getElementById('rotaryProgress').value)>16")
    loc('#rotaryPlay').click()
    with page.expect_download() as saved:
        loc('#rotaryExport').click()
    program = Path(saved.value.path()).read_text()
    assert 'G93' in program and 'Y3.000000 B360.000000' in program and 'G94 F' in program
    expect(loc('#rotaryRun')).to_be_disabled()
    loc('#rotaryAngle').fill('15')
    expect(loc('#rotaryExport')).to_be_disabled()
    expect(loc('#rotaryResult')).not_to_be_visible()
    loc('#rotaryFixture').check()
    loc('#rotaryMax').fill('5')
    loc('#rotaryPreview').click()
    expect(loc('#rotaryStatus')).to_contain_text(re.compile('fixture', re.I))
    expect(loc('#rotaryExport')).to_be_disabled()

    load('laser')
    loc('#rotarySeam').click()
    expect(loc('#rotaryStatus')).to_contain_text('power')
    loc('#rotaryOutput').select_option('A')
    loc('#rotaryPreview').click()
    expect(loc('#rotaryMetrics')).to_contain_text('A 0.000 to 360.000°')
    text = loc('#rotaryCode').text_content()
    assert 'M4 S150' in text and re.search(r'^G1 .* S600$', text, re.M) and re.search(r'^G1 .* S150$', text, re.M)
    tab('Control')
    loc('#port').select_option('simulator')
    loc('#connect').click()
    page.wait_for_function(
        "latest.simulated && latest.connected && latest.protocol==='grblhal' && latest.rotary_axes?.includes('A')"
    )
    tab('Rotary')
    loc('#rotaryConfirmed').check()
    expect(loc('#rotaryRun')).to_be_enabled()
    with page.expect_response(lambda r: r.url.endswith('/api/run')) as submitted:
        loc('#rotaryRun').click()
    assert submitted.value.status == 200, submitted.value.text()
    assert requests[-1][1]['rotary']['output_axis'] == 'A' and requests[-1][1]['rotary_confirmed']
    settled()

    # The standard run and phone-arm paths must carry the exact wrapped setup too.
    def reveal(control):
        panel = loc(control).evaluate("e => e.closest('[role=tabpanel]')?.getAttribute('aria-labelledby')")
        if panel:
            loc('#' + panel).click()

    for control, endpoint in (('#run', 'run'), ('#armPhone', 'arm')):
        tab('Rotary')
        loc('#rotaryConfirmed').check()
        reveal('#confirmed')
        loc('#confirmed').check()
        reveal(control)
        with page.expect_response(lambda r: r.url.endswith('/api/' + endpoint)) as sent:
            loc(control).click()
        assert sent.value.status == 200, sent.value.text()
        route, payload = requests[-1]
        assert route == endpoint and payload['rotary_confirmed'] and payload['rotary']['output_axis'] == 'A'
        if endpoint == 'run':
            settled()
    tab('Rotary')
    with page.expect_response(lambda r: r.url.endswith('/api/disarm')) as disarmed:
        loc('#rotaryAngle').fill('12')
    assert disarmed.value.status == 200
    expect(loc('#rotaryExport')).to_be_disabled()
    tab('Control')
    loc('#disconnect').click()
    page.wait_for_function('!latest.connected')
    # A standalone file-loaded editor can post the xPRO program without the server.
    disk = page.context.browser.new_page(accept_downloads=True)
    try:
        disk.on('pageerror', lambda e: errors.append(str(e)))
        disk.goto((Path(__file__).resolve().parents[1] / 'studio' / 'static' / 'index.html').as_uri())
        disk.evaluate('() => studioReady')
        disk.get_by_role('tab', name='CAM', exact=True).click()
        disk.locator('#mode').select_option('laser')
        disk.locator('#cad-command').fill('line 3 0 3 31.416')
        disk.locator('#cad-command').press('Enter')
        disk.get_by_role('tab', name='Rotary', exact=True).click()
        disk.locator('#rotaryPost').select_option('xpro-v5')
        disk.locator('#rotaryDiameter').fill('10')
        disk.locator('#rotaryPreview').click()
        expect(disk.locator('#rotaryMetrics')).to_contain_text('A 0.000 to 360.000°')
        expect(disk.locator('#rotaryRun')).to_be_disabled()
        with disk.expect_download() as saved:
            disk.locator('#rotaryExport').click()
        st = Path(saved.value.path()).read_text()
        assert 'M4 S' in st and re.search(r'G93\nG1 X3\.000000 A360\.000000', st)
    finally:
        disk.close()
    assert not errors, errors
    print(
        'PASS rotary continuous seam, variable router depth/laser power, cylinder playback, fixture refusal, stale preview and exact rotary job submission to simulator'
    )
