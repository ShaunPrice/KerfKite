"""Draw a finger-jointed box from the wizard, cut it in the simulator, dog-bone a slot, make an inlay and nest."""

import re

from playwright.sync_api import expect


def run(page):
    loc = page.locator
    tab = lambda title: page.get_by_role('tab', name=title, exact=True).click()
    opened = lambda name: page.evaluate('id => { document.getElementById(id).open = true; }', name)
    count = lambda: page.evaluate('() => objects.length')
    page.evaluate('() => window.studioReady')
    tab('CAM')
    loc('#mode').select_option('router')
    loc('#depth').fill('6')
    loc('#step').fill('3')
    tab('Design')
    loc('#width').fill('400')
    loc('#width').dispatch_event('change')
    loc('#height').fill('300')
    loc('#height').dispatch_event('change')

    # The box: five panels with outside profiles and dog-boned slots, inside the work area.
    tab('Templates')
    opened('projectWizards')
    loc('#wizardAdd').click()
    expect(loc('#wizardStatus')).to_contain_text(
        'Box added: 5 panels, 7 × 5 × 3 fingers, inside 108.0 × 68.0 × 54.0 mm'
    )
    assert count() == 5
    parts = page.evaluate(
        """() => objects.map(o => ({name: o.name, operation: o.operationSettings.operation,
            box: CNCLayout.extent(o), closed: o.paths.every(p => p[0][0] === p.at(-1)[0] && p[0][1] === p.at(-1)[1])}))"""
    )
    assert [p['name'] for p in parts] == ['Box · ' + n for n in ('Bottom', 'Front', 'Back', 'Left side', 'Right side')]
    assert all(p['operation'] == 'outside' and p['closed'] for p in parts)
    assert all(b >= 0 for p in parts for b in p['box'][:2]) and all(
        p['box'][2] <= 400 and p['box'][3] <= 300 for p in parts
    )
    for a in parts:
        for b in parts:
            if a is not b:
                apart = (
                    a['box'][2] <= b['box'][0]
                    or b['box'][2] <= a['box'][0]
                    or a['box'][3] <= b['box'][1]
                    or b['box'][3] <= a['box'][1]
                )
                assert apart, (a['name'], b['name'])
    loc('#preview').click()
    expect(loc('#summary')).to_contain_text('lines', timeout=30000)
    program = loc('#gcode').text_content()
    assert 'Z-6.000' in program and 'Z-3.000' in program, program[:300]
    print(
        f'PASS the box wizard added 5 panels that do not overlap, and their outside profiles preview to full depth ({program.count(chr(10))} lines)'
    )

    # Undo takes the whole project away in one step.
    page.evaluate("() => document.getElementById('undo').click()")
    assert count() == 0
    page.evaluate("() => document.getElementById('redo').click()")
    assert count() == 5
    print('PASS one undo removes the project and redo restores it')

    # A lidded box without dog-bones for a laser has six plain panels.
    loc('#boxLid').check()
    loc('#boxDogbones').uncheck()
    loc('#boxWidth').fill('2')
    loc('#wizardAdd').click()
    expect(loc('#message')).to_have_text('Box width must be between 18 and 2000')
    page.evaluate('() => { objects = []; invalidate(); draw(); }')
    loc('#boxWidth').fill('120')
    loc('#wizardAdd').click()
    expect(loc('#wizardStatus')).to_contain_text('Box added: 6 panels')
    expect(loc('#wizardStatus')).not_to_contain_text('dog-bones')
    print('PASS a lidded box has six panels, and an impossible box is refused with its reason')

    # Dividers and a sign.
    page.evaluate('() => { objects = []; invalidate(); draw(); }')
    loc('#wizardKind').select_option('dividers')
    loc('#wizardAdd').click()
    expect(loc('#wizardStatus')).to_contain_text('3 dividers added; each compartment is 64.7 × 73.5 mm')
    loc('#wizardKind').select_option('sign')
    loc('#wizardAdd').click()
    names = page.evaluate('() => objects.map(o => o.name)')
    assert names[-2:] == ['Sign hanging holes', 'Sign blank'], names
    print('PASS dividers and a sign blank are added, the holes ahead of the outline')

    # Dog-bones on a drawn slot, then an inlay from it.
    page.evaluate('() => { objects = []; invalidate(); draw(); }')
    tab('Design')
    for key, value in {'shapeX': 20, 'shapeY': 20, 'shapeW': 40, 'shapeH': 24}.items():
        loc('#' + key).fill(str(value))
    loc('#rect').click()
    opened('joineryPanel')
    expect(loc('#dogboneRadius')).to_have_value('1.64')
    loc('#dogboneRadius').fill('1')
    loc('#dogboneApply').click()
    expect(loc('#message')).to_contain_text("at least the cutter's radius")
    loc('#dogboneRadius').fill('1.64')
    loc('#dogboneApply').click()
    expect(loc('#message')).to_contain_text('4 dog-bone corners added')
    slot = page.evaluate(
        '() => ({name: objects[0].name, points: objects[0].paths[0].length, box: CNCLayout.extent(objects[0])})'
    )
    assert slot['name'] == 'Rectangle · dog-bones' and slot['points'] > 40
    assert slot['box'][0] < 20 and slot['box'][2] > 60, slot  # the reliefs reach past the corners
    page.evaluate("() => document.getElementById('undo').click()")
    assert page.evaluate('() => objects[0].paths[0].length') == 5
    print('PASS a slot takes four dog-bones no smaller than the cutter, and undo restores the rectangle')

    loc('#inlayMake').click()
    expect(loc('#message')).to_contain_text('Pocket and plug added')
    made = page.evaluate(
        """() => objects.slice(1).map(o => ({name: o.name, operation: o.operationSettings.operation, box: CNCLayout.extent(o)}))"""
    )
    assert [m['operation'] for m in made] == ['pocket', 'outside']
    pocket, plug = made[0]['box'], made[1]['box']
    # The plug is the pocket less the clearance of 0.1 mm all round.
    assert (
        abs((pocket[2] - pocket[0]) - (plug[2] - plug[0]) - 0.2) < 1e-6
        and abs((pocket[3] - pocket[1]) - (plug[3] - plug[1]) - 0.2) < 1e-6
    ), made
    assert plug[0] > pocket[2]
    corner = page.evaluate('() => objects[2].paths[0].length')
    assert corner > 20  # rounded to the cutter
    print('PASS the inlay makes a pocket and a plug 0.1 mm smaller all round with corners rounded to the cutter')

    # Nesting gathers the parts at the corner of the sheet with the gap asked for.
    loc('#nestGap').fill('5')
    loc('#nestMargin').fill('3')
    loc('#nestApply').click()
    expect(loc('#message')).to_contain_text('3 parts nested in')
    boxes = page.evaluate('() => objects.map(o => CNCLayout.extent(o))')
    assert min(b[0] for b in boxes) == 3 and min(b[1] for b in boxes) == 3, boxes
    for i, a in enumerate(boxes):
        for b in boxes[i + 1 :]:
            assert (
                a[2] + 5 <= b[0] + 1e-6 or b[2] + 5 <= a[0] + 1e-6 or a[3] + 5 <= b[1] + 1e-6 or b[3] + 5 <= a[1] + 1e-6
            )
    used = re.search(r'nested in ([\d.]+) × ([\d.]+) mm', loc('#message').text_content())
    assert float(used.group(1)) <= 400 and float(used.group(2)) <= 300
    print(f'PASS three parts nested in {used.group(1)} × {used.group(2)} mm with a 5 mm gap')


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
