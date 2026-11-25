"""Zero on a simulated touch plate, write and calibrate a setting, add a surfacing job and run a macro."""

import json

from playwright.sync_api import expect


def run(page):
    loc = page.locator
    tab = lambda title: page.get_by_role('tab', name=title, exact=True).click()
    opened = lambda name: page.evaluate('id => { document.getElementById(id).open = true; }', name)
    status = lambda: page.evaluate("() => api('status')")
    page.evaluate('() => window.studioReady')
    tab('Control')
    expect(loc('#setupToolsPanel')).to_be_visible()
    loc('#port').select_option('simulator')
    loc('#connect').click()
    expect(loc('#state')).to_contain_text('Idle')
    opened('setupToolsPanel')

    # The corner plate: the simulator's stock corner is 12.5 mm left of and 9.5 mm in front of the cutter.
    opened('setupProbe')
    loc('#setupProbeRun').click()
    expect(loc('#message')).to_have_text('Confirm the plate setup first')
    loc('#setupProbeAxes').select_option('xyz')
    loc('#setupProbeThickness').fill('12')
    loc('#setupProbeWall').fill('8')
    expect(loc('#setupProbeTool')).to_have_value('3.175')
    loc('#setupProbeConfirmed').check()
    loc('#setupProbeRun').click()
    expect(loc('#setupProbeState')).to_contain_text('Work zero is the front-left corner of the stock', timeout=30000)
    expect(loc('#setupProbeConfirmed')).not_to_be_checked()
    position = status()['position']
    assert abs(position['x'] - 10) < 1e-6 and abs(position['y'] - 10) < 1e-6 and abs(position['z'] - 17) < 1e-6, (
        position
    )
    # Back at the corner of the stock by a macro: 12.5 and 9.5 mm from where the cutter began.
    opened('setupMacros')
    expect(loc('#setupMacroList option')).to_have_count(4)
    loc('#setupMacroName').fill('To the corner')
    loc('#setupMacroText').fill('G90\nG0 X0 Y0 ; the corner of the stock')
    loc('#setupMacroSave').click()
    expect(loc('#setupMacroList option')).to_have_count(5)
    loc('#setupMacroRun').click()
    expect(loc('#message')).to_have_text('Macro finished: 2 lines sent', timeout=15000)
    log = status()['log']
    assert log[-2:] == ['G90', 'G0 X0 Y0'], log[-4:]
    probes = [row for row in log if row.startswith('[PRB:')][-6:]
    assert len(probes) == 6
    print(f'PASS the corner plate set work zero ({len(probes)} contacts) and a saved macro ran: {log[-1]}')

    loc('#setupMacroText').fill('$X')
    loc('#setupMacroRun').click()
    expect(loc('#message')).to_contain_text('plain G-code words only')
    print('PASS a macro with a controller command in it is refused before it is sent')

    # Settings: write the X steps, then calibrate from a measured move.
    opened('setupSettings')
    loc('#setupSettingsRefresh').click()
    expect(loc('#setupSettingsList [data-setting]')).to_have_count(2)  # the simulator reports $30 and $32
    page.evaluate("() => api('setup/setting', {confirmed: true, key: '100', value: 80})")
    page.wait_for_function("async () => (await api('status')).settings['100'] === 80")
    page.wait_for_function("() => latest.settings && latest.settings['100'] === 80")
    loc('#setupSettingsRefresh').click()
    expect(loc('#setupSettingsList [data-setting="100"] label')).to_contain_text('X steps per mm')
    loc('#setupSettingsList [data-setting="30"] input').fill('24000')
    loc('#setupSettingsList [data-setting="30"] button').click()
    expect(loc('#message')).to_have_text('$30 is now 24000', timeout=15000)
    with page.expect_download() as saved:
        loc('#setupSettingsExport').click()
    exported = json.load(open(saved.value.path(), encoding='utf-8'))
    assert (
        exported['format'] == 'kerfkite-controller-settings/1'
        and exported['settings']['30'] == 24000
        and exported['settings']['100'] == 80
    )
    opened('setupCalibrate')
    loc('#setupCalAxis').select_option('Y')
    loc('#setupCalAxis').select_option('X')
    expect(loc('#setupCalSteps')).to_have_value('80')
    loc('#setupCalMeasured').fill('99.2')
    expect(loc('#setupCalResult')).to_have_text('Corrected: 80.645 steps per mm (+0.806%)')
    loc('#setupCalWrite').click()
    expect(loc('#message')).to_contain_text('X now has 80.645 steps per mm', timeout=15000)
    assert status()['settings']['100'] == 80.645
    loc('#setupCalMeasured').fill('40')
    expect(loc('#setupCalResult')).to_contain_text('Check the measurement')
    print('PASS a setting is written and exported, and a measured move corrects the steps per mm to 80.645')

    # The surfacing job lands in the design with its own operation and previews as a router job.
    opened('setupSurface')
    expect(loc('#setupSurfaceWidth')).to_have_value('300')
    loc('#setupSurfaceStep').fill('40')
    loc('#setupSurfaceAdd').click()
    expect(loc('#message')).to_contain_text('must not be more than the cutter diameter')
    loc('#setupSurfaceStep').fill('15')
    loc('#setupSurfaceAdd').click()
    expect(loc('#message')).to_contain_text('Surfacing job added')
    job = page.evaluate(
        '() => ({name: objects.at(-1).name, settings: objects.at(-1).operationSettings, box: CNCLayout.extent(objects.at(-1))})'
    )
    assert (
        job['name'] == 'Spoilboard surfacing'
        and job['settings']['operation'] == 'line'
        and job['settings']['mode'] == 'router'
    )
    assert job['settings']['tool_diameter'] == 25.4 and job['settings']['depth'] == 0.5
    assert job['box'] == [12.7, 12.7, 287.3, 167.3], job['box']
    tab('CAM')
    loc('#mode').select_option('router')
    loc('#preview').click()
    expect(loc('#gcode')).to_contain_text('Z-0.500', timeout=20000)
    print(f'PASS the surfacing job covers {job["box"]} with a 25.4 mm cutter and previews at 0.5 mm deep')
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
