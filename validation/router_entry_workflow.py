"""Check ramp and helix entry selection, preview, persistence and unsupported offline export."""

import json
from pathlib import Path
from playwright.sync_api import expect


def run(page):
    root = Path(__file__).resolve().parents[1]
    outputs = []
    for offline in (False, True):
        with page.context.browser.new_context(viewport={'width': 1440, 'height': 980}, accept_downloads=True) as ctx:
            if offline:
                ctx.set_offline(True)
            ui = ctx.new_page()
            ui.on('dialog', lambda dialog: dialog.accept())
            ui.goto((root / 'studio/static/index.html').as_uri() if offline else page.url)
            ui.evaluate('() => window.studioReady')
            loc = ui.locator
            ui.get_by_role('tab', name='Design', exact=True).click()
            loc('#shapeW').fill('24')
            loc('#shapeH').fill('24')
            loc('#rect').click()
            ui.get_by_role('tab', name='CAM', exact=True).click()
            loc('#mode').select_option('router')
            for key, value in {
                'depth': 2,
                'step': 1,
                'tool_diameter': 4,
                'stepover': 2,
                'feed': 400,
                'plunge': 90,
                'entry_angle': 3,
            }.items():
                if key == 'entry_angle':
                    loc('#router_entry').select_option('ramp')
                loc('#' + key).fill(str(value))
            loc('#preview').click()
            expect(loc('#gcode')).to_contain_text('Z-0.250 F90.0')
            outputs.append(loc('#gcode').text_content())
            loc('#operation').select_option('pocket')
            loc('#router_entry').select_option('helix')
            loc('#entry_radius').fill('1.5')
            loc('#preview').click()
            if offline:
                expect(loc('#message')).to_contain_text('Studio server')
                expect(loc('#gcode')).to_contain_text('new preview')
                continue
            expect(loc('#gcode')).to_contain_text('Z-2.000')
            helix = loc('#gcode').text_content()
            assert sum(l.startswith('G1 X') and l.endswith('F90.0') for l in helix.splitlines()) >= 72
            ui.get_by_text('Rotatable 3D toolpath preview', exact=True).click()
            loc('#simulateStock').click()
            ui.wait_for_function('window.stockPreview && Math.min(...window.stockPreview.heights.flat()) < -1.9')
            ui.get_by_role('tab', name='Projects', exact=True).click()
            with ui.expect_download() as saved:
                loc('#projectDownload').click()
            raw = Path(saved.value.path()).read_bytes()
            settings = json.loads(raw)['settings']
            assert settings['router_entry'] == 'helix' and settings['entry_radius'] == 1.5
            loc('#loadProject').click()
            loc('#file').set_input_files(
                {'name': 'pocket.cncstudio.json', 'mimeType': 'application/json', 'buffer': raw}
            )
            expect(loc('#message')).to_contain_text('Project opened')
            loc('#preview').click()
            expect(loc('#gcode')).to_have_text(helix)
    assert outputs[0] == outputs[1]
    print('PASS profile ramp on both editors, pocket helix stock simulation and save/reopen, explicit offline boundary')
