"""Exercise laser cut patterns in the simulator workstation and disconnected file editor."""

import json
from pathlib import Path
from playwright.sync_api import expect


def check(page):
    loc = page.locator
    tab = lambda name: page.get_by_role('tab', name=name, exact=True).click()
    page.evaluate('() => window.studioReady')
    tab('Design')
    loc('#rect').click()
    tab('CAM')
    loc('#mode').select_option('laser')
    loc('#power').fill('700')
    loc('#laser_pattern').select_option('perforate')
    loc('#laser_dash').fill('3')
    loc('#laser_gap').fill('1')
    loc('#lead_shape').select_option('arc-left')
    loc('#lead_in').fill('3')
    tab('Layers')
    loc('#designLayerCapture').click()
    tab('CAM')
    loc('#laser_pattern').select_option('continuous')
    loc('#preview').click()
    expect(loc('#gcode')).to_contain_text(' S0')
    perforation = loc('#gcode').text_content()
    zero_moves = [l for l in perforation.splitlines() if l.startswith('G1 ') and l.endswith(' S0')]
    assert len(zero_moves) >= 4, (perforation[:500], len(zero_moves))
    # A different selected-object operation takes precedence over its saved layer.
    loc('#laser_pattern').select_option('bridges')
    loc('#laser_bridge_count').fill('2')
    loc('#laser_gap').fill('2')
    loc('#assignOperation').click()
    loc('#preview').click()
    expect(loc('#gcode')).to_contain_text(' S0')
    bridges = loc('#gcode').text_content()
    assert 2 <= sum(l.startswith('G1 ') and l.endswith(' S0') for l in bridges.splitlines()) <= 4
    assert bridges != perforation
    tab('Projects')
    with page.expect_download() as download:
        loc('#projectDownload').click()
    raw = Path(download.value.path()).read_bytes()
    saved = json.loads(raw)
    assert saved['extras']['designLayers'][0]['settings']['laser_pattern'] == 'perforate'
    assert saved['objects'][0]['operationSettings']['laser_pattern'] == 'bridges'
    loc('#loadProject').click()
    loc('#file').set_input_files(dict(name='pattern.cncstudio.json', mimeType='application/json', buffer=raw))
    expect(loc('#message')).to_contain_text('Project opened')
    expect(loc('#laser_bridge_count')).to_have_value('2')
    loc('#preview').click()
    expect(loc('#gcode')).to_have_text(bridges)
    return bridges


def run(page):
    root = Path(__file__).resolve().parents[1]
    results = []
    for offline in (False, True):
        with page.context.browser.new_context(viewport={'width': 1440, 'height': 980}, accept_downloads=True) as ctx:
            if offline:
                ctx.set_offline(True)
            editor = ctx.new_page()
            editor.on('dialog', lambda dialog: dialog.accept())
            editor.goto((root / 'studio/static/index.html').as_uri() if offline else page.url)
            results.append(check(editor))
    assert results[0] == results[1]
    print('PASS laser pattern layer and object overrides, portable project and identical server/offline G-code')
