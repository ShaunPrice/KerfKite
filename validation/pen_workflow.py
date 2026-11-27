"""Draw with a pen: single-line lettering and a shape, by the Z axis and by a servo, with a broad nib and hatching."""

from playwright.sync_api import expect


def run(page):
    loc = page.locator
    tab = lambda title: page.get_by_role('tab', name=title, exact=True).click()
    page.evaluate('() => window.studioReady')
    page.evaluate('() => { objects = []; selected = -1; invalidate(); draw(); }')
    tab('Design')
    for key, value in {'width': 200, 'height': 150}.items():
        loc('#' + key).fill(str(value))
        loc('#' + key).dispatch_event('change')
    for key, value in {'shapeX': 20, 'shapeY': 60, 'shapeW': 60, 'shapeH': 40}.items():
        loc('#' + key).fill(str(value))
    loc('#rect').click()
    # Lettering a pen writes in single strokes: the Text tool's own single-line face.
    tab('Text')
    loc('#textValue').fill('Pen')
    loc('#textFont').select_option('Workshop Mono · single line')
    loc('#textSize').fill('12')
    loc('#addText').click()
    strokes = page.evaluate('() => objects.at(-1).paths.length')
    assert page.evaluate('() => objects.length') == 2 and strokes >= 4, strokes

    tab('CAM')
    loc('#mode').select_option('pen')
    loc('#mode').dispatch_event('input')
    expect(loc('#penSettings')).to_be_visible()
    expect(loc('#depthLabel')).to_contain_text('Pen pressed below Z zero')
    expect(loc('#safeLabel')).to_contain_text('Pen lift')
    expect(loc('#powerLabel')).to_contain_text('not used')
    expect(loc('#penDwellLabel')).to_be_hidden()
    expect(loc('#modeHint')).to_contain_text('The spindle stays off')
    loc('#penDefaults').click()
    expect(loc('#message')).to_contain_text('Pen settings applied')
    assert [loc('#' + key).input_value() for key in ('depth', 'safe', 'plunge', 'feed', 'operation')] == [
        '1',
        '3',
        '600',
        '2000',
        'line',
    ]
    loc('#preview').click()
    expect(loc('#gcode')).to_contain_text('G1 Z-1.000 F600.0')
    program = loc('#gcode').text_content().splitlines()
    assert not any(line.startswith(('M3', 'M4')) for line in program), 'the spindle is started for a pen'
    assert sum(line == 'G1 Z-1.000 F600.0' for line in program) == 1 + strokes
    assert all(float(word[1:]) in (3.0, -1.0) for line in program for word in line.split() if word[0] == 'Z')
    # The editor's own generator, used when there is no server, writes the same program.
    offline = page.evaluate('() => CNCOffline.generate(project()).lines')
    assert offline == program, 'the offline program differs from the server\'s'
    print(
        f'PASS a Z pen draws a rectangle and {strokes} strokes of lettering, pressed 1 mm and lifted 3 mm, with the spindle off'
    )

    # Draw it on the simulator.
    tab('Control')
    loc('#port').select_option('simulator')
    loc('#connect').click()
    expect(loc('#state')).to_contain_text('Idle')
    loc('#confirmed').check()
    with page.expect_response(lambda response: response.url.endswith('/api/run')) as submitted:
        loc('#run').click()
    assert submitted.value.json()['lines'] == len(program), submitted.value.json()
    expect(loc('#message')).to_contain_text('Job submitted')
    for _ in range(300):
        status = page.evaluate("async () => await api('status')")
        if not status['active'] and status['state'] == 'Idle':
            break
        page.wait_for_timeout(100)
    assert status['error'] is None and not status['active'], status
    height = status['position']['z'] if isinstance(status['position'], dict) else status['position'][2]
    assert abs(height - 3) < 1e-6, status['position']  # left lifted
    print('PASS the pen job runs to its end on the simulator and leaves the pen lifted')
    loc('#disconnect').click()

    # A pen lifted by a servo on the spindle or laser output: no Z at all.
    tab('CAM')
    loc('#pen_lift').select_option('servo')
    loc('#pen_lift').dispatch_event('input')
    expect(loc('#routerSettings')).to_be_hidden()
    expect(loc('#penDwellLabel')).to_be_visible()
    expect(loc('#powerLabel')).to_contain_text('Pen-down S value')
    loc('#power').fill('800')
    loc('#pen_dwell').fill('0.3')
    loc('#preview').click()
    expect(loc('#gcode')).to_contain_text('M3 S800')
    program = loc('#gcode').text_content().splitlines()
    assert not any(' Z' in line for line in program), 'a servo pen moves Z'
    # Each of the two objects starts by raising the holder, then every stroke lowers and raises it.
    assert program.count('M3 S800') == 1 + strokes and program.count('G4 P0.300') == 2 * (1 + strokes) + 2
    down = False
    for line in program:
        if line.startswith('M3'):
            down = True
        elif line == 'M5':
            down = False
        elif line.startswith('G0'):
            assert not down, f'travel with the pen down: {line}'
    assert page.evaluate('() => CNCOffline.generate(project()).lines') == program
    print('PASS a servo pen is lowered with M3 S800 and raised with M5, pausing 0.3 s each time, and Z is never moved')

    # A broad nib: every stroke four times side by side.
    loc('#pen_lift').select_option('z')
    loc('#pen_lift').dispatch_event('input')
    loc('#pen_nib').fill('1.2')
    loc('#pen_nib_angle').fill('45')
    loc('#pen_tip').fill('0.4')
    loc('#pen_tip').dispatch_event('input')
    loc('#preview').click()
    expect(loc('#gcode')).to_contain_text('G1 Z-1.000 F600.0')
    program = loc('#gcode').text_content().splitlines()
    assert sum(line == 'G1 Z-1.000 F600.0' for line in program) == 4 * (1 + strokes)
    print(f'PASS a 1.2 mm nib with a 0.4 mm pen draws each of the {1 + strokes} strokes four times')

    # Hatching fills the rectangle; the lettering is left out of this job.
    loc('#pen_nib').fill('0')
    loc('#pen_nib').dispatch_event('input')
    page.evaluate('() => { objects.at(-1).output = false; invalidate(); draw(); }')
    loc('#operation').select_option('fill')
    loc('#operation').dispatch_event('input')
    page.evaluate(
        "() => { const i = document.getElementById('interval'); i.value = 2; i.dispatchEvent(new Event('input', {bubbles: true})); }"
    )
    loc('#preview').click()
    expect(loc('#gcode')).to_contain_text('G1 Z-1.000 F600.0')
    program = loc('#gcode').text_content().splitlines()
    rows = sorted(
        {float(word[1:]) for line in program if line.startswith('G1 X') for word in line.split() if word[0] == 'Y'}
    )
    assert rows == [60.0 + 2 * k for k in range(21)], rows
    print(f'PASS hatching fills the 60 × 40 mm rectangle with {len(rows)} lines 2 mm apart')
    loc('#operation').select_option('line')
    loc('#operation').dispatch_event('input')
    loc('#mode').select_option('router')
    loc('#mode').dispatch_event('input')


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
