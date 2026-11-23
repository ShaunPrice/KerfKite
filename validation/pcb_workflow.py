"""Import a two-sided circuit board in the editor, read its warnings, and engrave it levelled on the simulator.

The board files are the hand-written fixtures in tests/pcb. Importing must send nothing to the machine; the
isolation is then previewed at the engraving depth, probed and run following the probed surface, each drill
diameter is its own stage, and the underside registers with the top.
"""

import re
from pathlib import Path

from playwright.sync_api import expect

FIXTURES = Path(__file__).resolve().parent.parent / 'tests' / 'pcb'


def run(page):
    loc = page.locator
    tab = lambda title: page.get_by_role('tab', name=title, exact=True).click()
    page.evaluate('() => window.studioReady')
    assert page.evaluate('window.studioSession.pcb_import') is True
    tab('Projects')
    expect(loc('#pcb-panel')).to_be_visible()

    # A server that does not advertise the import (an extension built without it) leaves the panel hidden.
    older = page.context.browser.new_context().new_page()

    def without_import(route):
        response = route.fetch()
        route.fulfill(response=response, json={**response.json(), 'pcb_import': False})

    older.route('**/api/session', without_import)
    older.goto(page.url)
    older.evaluate('() => window.studioReady')
    older.get_by_role('tab', name='Projects', exact=True).click()
    expect(older.locator('#exchange-source-file')).to_be_visible()
    expect(older.locator('#pcb-panel')).to_be_hidden()
    older.context.close()
    print('PASS the panel is offered only when the server advertises circuit board import')

    loc('#pcb-copper').set_input_files(str(FIXTURES / 'board_top.gbr'))
    loc('#pcb-outline').set_input_files(str(FIXTURES / 'board_outline.gbr'))
    loc('#pcb-drill').set_input_files(str(FIXTURES / 'board_drill.drl'))

    # A tool that cuts 0.3 mm wide cannot pass the 0.295 mm either side of the track between two header pads.
    loc('#pcb-kind').select_option('width')
    loc('#pcb-width').fill('0.3')
    loc('#pcb-review').click()
    expect(loc('#pcb-status')).to_contain_text('The cutter cannot reach 2 places')
    expect(loc('#pcb-warnings')).to_contain_text(
        'Copper: The cutter cannot pass between separate copper features in 2 places'
    )
    expect(loc('#pcb-warnings')).to_contain_text('the smallest gap is 0.295 mm')
    expect(loc('#pcb-warnings')).to_contain_text('no wider than 0.292 mm would separate everything')
    expect(loc('#pcbMarks circle.pcb-unreachable')).to_have_count(2)
    marks = page.evaluate(
        "[...document.querySelectorAll('#pcbMarks circle')].map(c => [c.cx.baseVal.value, c.cy.baseVal.value])"
    )
    height = float(loc('#height').input_value())
    # The pads are at X25 and X27.54, Y25 once placed; the track runs between them at X26.27.
    assert sorted(round(x, 2) for x, _ in marks) == [26.0, 26.54] and all(
        abs(height - y - 25) < 0.9 for _, y in marks
    ), marks
    assert page.evaluate("document.querySelector('#pcbCopper path').getAttribute('d').length") > 1000
    expect(loc('#pcb-summary')).to_contain_text('Copper (Copper,L1,Top): 9 flashes, 10 lines, 0 arcs, 1 regions')
    expect(loc('#pcb-summary')).to_contain_text('Outline: 2 closed loops, 40 × 30 mm')
    print('PASS board reviewed: copper, outline and holes understood; two unreachable places warned of and marked')

    # The drill file leaves its digits to a default: that is said, with the fields to correct it.
    expect(loc('#pcb-warnings')).to_contain_text('Drill file: The file does not state its number format')
    expect(loc('#pcb-format')).to_have_attribute('open', '')
    expect(loc('#pcb-format-read')).to_contain_text('3.3 digits (from the default)')
    expect(loc('#pcb-summary')).to_contain_text('Holes: 2 × 0.4 mm, 4 × 0.8 mm, 1 × 1 mm')
    loc('#pcb-integer').fill('2')
    loc('#pcb-decimal').fill('4')
    loc('#pcb-review').click()
    expect(loc('#message')).to_contain_text('The holes do not fit the work area')
    expect(loc('#pcb-result')).to_be_hidden()
    loc('#pcb-integer').fill('3')
    loc('#pcb-decimal').fill('3')
    loc('#pcb-review').click()
    expect(loc('#pcb-format-read')).to_contain_text('3.3 digits (from the request)')
    expect(loc('#pcb-warnings')).not_to_contain_text('does not state its number format')
    print('PASS drill number format shown, a wrong format refused with the reason, the right one accepted')

    # A 30 degree V-bit with a 0.1 mm tip, 0.1 mm deep, cuts 0.154 mm wide and passes everywhere.
    loc('#pcb-kind').select_option('v')
    loc('#pcb-tip').fill('0.1')
    loc('#pcb-angle').fill('30')
    loc('#pcb-depth').fill('0.1')
    expect(loc('#pcb-width')).to_have_value('0.15359')
    loc('#pcb-passes').fill('2')
    loc('#pcb-review').click()
    expect(loc('#pcb-status')).to_contain_text('Review ready: 5 objects for 5 tools.')
    expect(loc('#pcbMarks circle')).to_have_count(0)
    expect(loc('#pcb-warnings')).not_to_contain_text('cannot pass')
    loc('#pcb-commit').click()
    expect(loc('#pcb-status')).to_contain_text('Added 5 objects in 5 stages')
    assert page.evaluate('project().operations.length') == 0  # every stage starts with Output off
    state = page.evaluate("api('status')")
    assert not state['connected'] and not state.get('active'), state
    assert page.evaluate('objects.length') == 5
    assert page.evaluate('window.projectExtras.pcbBoards[0].offset') == [-95, 115]
    print('PASS board added as five reviewable objects in five stages, Output off, nothing sent to the machine')

    # The isolation stage: preview at the engraving depth.
    loc('#pcb-stage-list button').filter(has_text='PCB top · isolation').click()
    expect(loc('#message')).to_contain_text('PCB top · isolation is the job')
    assert page.evaluate('project().operations.length') == 1
    tab('CAM')
    expect(loc('#mode')).to_have_value('router')
    for key, value in {'feed': 200, 'plunge': 60, 'power': 12000, 'safe': 2}.items():
        loc('#' + key).fill(str(value))
    loc('#preview').click()
    expect(loc('#gcode')).to_contain_text('G1 Z-0.100 F60.0')
    program = loc('#gcode').text_content().splitlines()
    plunges = {line for line in program if line.startswith('G1 Z')}
    assert plunges == {'G1 Z-0.100 F60.0'}, plunges
    cuts = [line for line in program if line.startswith('G1 X')]
    assert len(cuts) > 300, len(cuts)
    print(f'PASS isolation previewed: {len(program)} lines, every plunge to the 0.1 mm engraving depth')

    # Probe the board and run the engraving following the probed surface.
    tab('Control')
    loc('#port').select_option('simulator')
    loc('#connect').click()
    expect(loc('#state')).to_contain_text('Idle')
    page.get_by_text('Probe levelling', exact=True).click()
    loc('#levelSpacing').fill('15')
    loc('#levelConfirmed').check()
    loc('#levelProbe').click()
    expect(loc('#levelState')).to_contain_text('Surface probed at', timeout=30000)
    points, low, high = re.search(
        r'at (\d+) points .*: ([-+\d.]+) to ([-+\d.]+) mm', loc('#levelState').text_content()
    ).groups()
    with page.expect_download() as saved:
        loc('#levelExport').click()
    levelled = open(saved.value.path(), encoding='ascii').read().splitlines()
    depths = {float(line.rsplit('Z', 1)[1]) for line in levelled if re.fullmatch(r'G1 X\S+ Y\S+ F\S+ Z\S+', line)}
    shallowest, deepest = -0.1 + float(high) + 0.001, -0.1 + float(low) - 0.001
    assert len(depths) > 5 and all(deepest < depth < shallowest for depth in depths), sorted(depths)[:5]
    loc('#confirmed').check()
    with page.expect_response(lambda response: response.url.endswith('/api/run')) as submitted:
        loc('#run').click()
    assert submitted.value.request.post_data_json['leveled'] is True
    assert submitted.value.json()['lines'] == len(levelled)
    expect(loc('#progress')).to_have_js_property('value', 100, timeout=60000)
    print(f'PASS engraving probed at {points} points and run levelled on the simulator: {len(levelled)} lines')

    # Each drill diameter is a stage of its own, with its own tool.
    tab('Projects')
    loc('#pcb-stage-list button').filter(has_text='PCB top · drill 0.8 mm').click()
    tab('CAM')
    loc('#preview').click()
    expect(loc('#gcode')).to_contain_text('G1 Z-1.800 F60.0')
    drilled = loc('#gcode').text_content().splitlines()
    assert sum(line == 'G1 Z-1.800 F60.0' for line in drilled) == 4, drilled
    assert 'G0 X25.000 Y25.000' in drilled and 'G0 X30.080 Y25.000' in drilled, drilled
    tab('Projects')
    loc('#pcb-all-stages').click()
    tab('CAM')
    loc('#preview').click()
    expect(loc('#message')).to_contain_text('Tool identity or geometry differs between operations')
    loc('#generateToolPlan').click()
    expect(loc('#message')).to_contain_text('Prepared 5 independent stage files and 4 manual tool changes')
    stages = loc('#manualToolStages p').all_text_contents()
    assert [re.search(r'tool \d+ · (.*?) · ', stage).group(1) for stage in stages] == [
        'V-bit 30° cutting 0.15359 mm',
        'Drill 0.4 mm',
        'Drill 0.8 mm',
        'Drill 1 mm',
        'End mill 1.5 mm',
    ], stages
    print('PASS four holes drilled as one stage; all stages refuse to run as one job and export as five tool stages')

    # The underside: its copper alone, registered with the board already in the design and mirrored about its middle.
    tab('Projects')
    loc('#pcb-copper').set_input_files(str(FIXTURES / 'board_bottom.gbr'))
    loc('#pcb-outline').set_input_files([])
    loc('#pcb-drill').set_input_files([])
    loc('#pcb-side').select_option('bottom')
    expect(loc('#pcb-register')).to_be_checked()
    expect(loc('#pcb-corner-x')).to_be_disabled()
    loc('#pcb-review').click()
    expect(loc('#pcb-summary')).to_contain_text('Placed mirrored about X25, with offset X-95 Y115')
    expect(loc('#pcb-warnings')).to_contain_text('The underside is mirrored about X25.000')
    loc('#pcb-commit').click()
    expect(loc('#pcb-status')).to_contain_text('Added 1 objects in 1 stage')
    boards = page.evaluate('window.projectExtras.pcbBoards')
    assert [board['side'] for board in boards] == ['top', 'bottom'] and boards[1]['mirror_x'] == 25, boards
    # A top-side hole at X10 Y30 is at X40 from below, and a contour of the underside goes round the pad there.
    surrounds = page.evaluate(
        """() => objects.at(-1).paths.some(path => {
            const xs = path.map(p => p[0]), ys = path.map(p => p[1]);
            return Math.min(...xs) < 40 && Math.max(...xs) > 40 && Math.min(...ys) < 30 && Math.max(...ys) > 30
                && Math.max(...xs) - Math.min(...xs) < 12;
        })"""
    )
    assert surrounds
    saved = page.evaluate('CNCProjects.document()')
    assert len(saved['extras']['pcbBoards']) == 2 and saved['extras']['pcbBoards'][0]['outline_bounds'] == [
        5,
        5,
        45,
        35,
    ]
    names = page.evaluate('CNCDesignLayers.layers().map(row => row.name)')
    assert names[-1] == 'PCB underside · isolation' and names.count('PCB top · isolation') == 1, names
    # Leave the simulator as it was found: no probed surface, disconnected.
    tab('Control')
    loc('#levelClear').click()
    expect(loc('#levelState')).to_have_text('No surface probed')
    loc('#disconnect').click()
    print('PASS underside copper registered with the top and mirrored about the middle of the board')


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
