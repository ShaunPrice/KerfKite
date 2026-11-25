"""Draw a finger-jointed box from the wizard, cut it in the simulator, dog-bone a slot, make inlays and nest."""

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

    # Two L-shaped parts nest into each other when they may be turned, and not when they may not.
    page.evaluate(
        """() => { const ell = [[0,0],[60,0],[60,20],[20,20],[20,60],[0,60],[0,0]];
            objects = [0, 1].map(k => ({name: 'Bracket ' + (k + 1), designLayerId: 'default',
                paths: [ell.map(([x, y]) => [x + 100 + 150 * k, y + 100])]})); selected = 0; invalidate(); draw(); }"""
    )
    loc('#nestGap').fill('4')
    loc('#nestMargin').fill('2')
    loc('#nestTurns').select_option('none')
    loc('#nestApply').click()
    expect(loc('#message')).to_contain_text('2 parts nested in')
    plain = float(re.search(r'nested in ([\d.]+) ×', loc('#message').text_content()).group(1))
    loc('#nestTurns').select_option('quarter')
    loc('#nestApply').click()
    expect(loc('#message')).to_contain_text('1 of them turned')
    turned = float(re.search(r'nested in ([\d.]+) ×', loc('#message').text_content()).group(1))
    assert turned < plain - 20, (plain, turned)
    # The outlines themselves keep the gap: no corner of one bracket is inside or within 4 mm of the other.
    apart = page.evaluate(
        """() => { const [a, b] = objects.map(o => o.paths[0]); let least = Infinity;
            const d = (p, q, r) => { const t = Math.max(0, Math.min(1, ((p[0]-q[0])*(r[0]-q[0]) + (p[1]-q[1])*(r[1]-q[1])) / ((r[0]-q[0])**2 + (r[1]-q[1])**2)));
                return Math.hypot(p[0] - q[0] - t*(r[0]-q[0]), p[1] - q[1] - t*(r[1]-q[1])); };
            for (const [m, n] of [[a, b], [b, a]]) for (const p of m) for (let k = 0; k + 1 < n.length; k++) least = Math.min(least, d(p, n[k], n[k+1]));
            return least; }"""
    )
    assert apart >= 4 - 1e-6, apart
    print(
        f'PASS two brackets nest in {turned:.1f} mm of width when one is turned, against {plain:.1f} mm upright, {apart:.2f} mm apart'
    )

    # A V-carved inlay of a star-like shape: clearing and carving for the pocket and for the mirrored plug.
    page.evaluate(
        """() => { objects = [{name: 'Arrow', designLayerId: 'default', paths: [[[30,30],[80,30],[80,50],[100,50],[55,90],[10,50],[30,50],[30,30]]]}];
            selected = 0; invalidate(); draw(); }"""
    )
    loc('#inlayKind').select_option('v')
    expect(loc('#inlayV')).to_be_visible()
    expect(loc('#inlayToolLabel')).to_have_text('Clearing cutter diameter · mm')
    loc('#inlayStart').fill('4')
    loc('#inlayMake').click()
    expect(loc('#message')).to_contain_text('to leave a gap for glue')
    loc('#inlayStart').fill('3')
    loc('#inlayVDiameter').fill('4')
    loc('#inlayMake').click()
    expect(loc('#message')).to_contain_text('use a wider bit')
    loc('#inlayVDiameter').fill('12.7')
    loc('#inlayMake').click()
    expect(loc('#message')).to_contain_text(
        'V inlay added: a pocket 4 mm deep and a mirrored plug that goes in 3 mm', timeout=30000
    )
    made = page.evaluate(
        """() => objects.slice(1).map(o => ({name: o.name, s: o.operationSettings, box: CNCLayout.extent(o), paths: o.paths.length}))"""
    )
    assert [m['s']['operation'] for m in made] == ['pocket', 'vcarve', 'pocket', 'vcarve'], [m['name'] for m in made]
    pocket_clear, pocket_carve, plug_clear, plug_carve = made
    assert pocket_carve['s']['depth'] == 4 and 'v_start_depth' not in pocket_carve['s']
    # The plug's outline is carved 0.2 ÷ tan 30° = 0.346 mm deeper than the 3 mm it goes in, for the sampling
    # step the pocket's carve stays short of its wall.
    assert plug_carve['s']['depth'] == 4.846 and plug_carve['s']['v_start_depth'] == 3.346 and plug_carve['paths'] == 2
    assert pocket_carve['s']['v_resolution'] == plug_carve['s']['v_resolution'] == 0.2
    assert (
        pocket_carve['s']['tool_id']
        == plug_carve['s']['tool_id']
        != pocket_clear['s']['tool_id']
        == plug_clear['s']['tool_id']
    )
    # The pocket's floor is inset by depth × tan(30°) + the sampling step = 2.509 mm; the plug is the mirror image
    # beside the shape.
    assert abs(pocket_clear['box'][1] - (30 + 4 * 0.57735 + 0.2)) < 0.01, pocket_clear['box']
    width = pocket_carve['box'][2] - pocket_carve['box'][0]
    assert plug_carve['box'][0] > pocket_carve['box'][2] and plug_carve['box'][2] - plug_carve['box'][0] > width
    mirrored = page.evaluate(
        '() => { const a = objects[0].paths[0], b = objects[4].paths[1]; const ax = a.map(p => p[0]), bx = b.map(p => p[0]); return [Math.max(...ax) - a[4][0], b.find(p => Math.abs(p[1] - 90) < 1e-6)[0] - Math.min(...bx)]; }'
    )
    assert abs(mirrored[0] - mirrored[1]) < 1e-6, mirrored
    # The plug's carve previews alone: its outline is cut 3 mm down, so no cutting move is above that.
    tab('CAM')
    loc('#mode').select_option('router')
    # (In one pass straight down, so that the only depths in the program are the carve's own.)
    page.evaluate(
        "() => { objects.forEach((o, i) => { o.output = i === 4; }); Object.assign(objects[4].operationSettings, {step: 4.846, router_entry: 'plunge'}); invalidate(); draw(); }"
    )
    loc('#preview').click()
    expect(loc('#summary')).to_contain_text('lines', timeout=60000)
    program = loc('#gcode').text_content().splitlines()
    depths = [float(w[1:]) for line in program if line.startswith('G1 X') for w in line.split() if w[0] == 'Z']
    assert depths and max(depths) <= -3.346 + 1e-9 and min(depths) == -4.846, (max(depths), min(depths))
    print(
        f'PASS a V inlay makes clearing and carving for a 4 mm pocket and a mirrored plug carved from {-max(depths):.2f} to {-min(depths):.2f} mm'
    )


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
