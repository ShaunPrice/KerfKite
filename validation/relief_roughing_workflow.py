"""Rough out a relief from an STL before finishing it: levels of roughing ahead of the finishing pass."""

from playwright.sync_api import expect


def pyramid():
    """An ASCII STL of a square pyramid 40 × 40 mm at its base and 10 mm high, on a base plate."""
    apex, corners = (20, 20, 10), [(0, 0, 0), (40, 0, 0), (40, 40, 0), (0, 40, 0)]
    facets = [(corners[i], corners[(i + 1) % 4], apex) for i in range(4)]
    facets += [(corners[0], corners[2], corners[1]), (corners[0], corners[3], corners[2])]
    text = 'solid pyramid\n'
    for facet in facets:
        text += (
            'facet normal 0 0 1\nouter loop\n'
            + ''.join(f'vertex {x} {y} {z}\n' for x, y, z in facet)
            + 'endloop\nendfacet\n'
        )
    return (text + 'endsolid pyramid\n').encode()


def run(page):
    loc = page.locator
    tab = lambda name: page.get_by_role('tab', name=name, exact=True).click()
    page.evaluate('() => window.studioReady')
    tab('Design')
    for field, value in (('#shapeX', '20'), ('#shapeY', '20'), ('#shapeW', '80'), ('#shapeH', '80')):
        loc(field).fill(value)
    tab('CAM')
    loc('#mode').select_option('router')
    loc('#tool_diameter').fill('3.175')
    loc('#stepover').fill('1')
    tab('3D relief')
    loc('#reliefFile').evaluate("input => input.closest('details').open = true")
    loc('#makeRoughing').click()
    expect(loc('#message')).to_have_text('Choose a grayscale image or STL')
    loc('#reliefFile').set_input_files({'name': 'pyramid.stl', 'mimeType': 'model/stl', 'buffer': pyramid()})
    loc('#reliefDepth').fill('8')
    loc('#reliefTool').select_option('ball')
    loc('#reliefResolution').fill('48')
    loc('#makeRelief').click()
    expect(loc('#message')).to_contain_text('C++ accelerated relief', timeout=30000)
    assert page.evaluate('() => objects.map(o => o.name)') == ['Relief · pyramid.stl']

    loc('#roughStepover').fill('9')
    loc('#makeRoughing').click()
    expect(loc('#message')).to_contain_text('no more than the cutter diameter')
    loc('#roughStepover').fill('2.5')
    loc('#roughStep').fill('2')
    loc('#roughAllowance').fill('0.5')
    with page.expect_request(lambda request: request.url.endswith('/api/relief-roughing')) as sent:
        loc('#makeRoughing').click()
    expect(loc('#message')).to_contain_text('Roughing added: 4 levels of 2 mm', timeout=30000)
    payload = sent.value.post_data_json
    assert (
        payload['rough_step'] == 2
        and payload['tool_shape'] == 'flat'
        and payload['tool_diameter'] == 6.35
        and payload['allowance'] == 0.5
    )
    assert 'content_base64' in payload
    made = page.evaluate(
        """() => objects.map(o => ({name: o.name, passes: o.paths.length, lowest: Math.min(...o.depths.flat()),
            highest: Math.max(...o.depths.flat()), settings: o.operationSettings || null}))"""
    )
    assert [m['name'] for m in made] == ['Relief roughing · pyramid.stl', 'Relief · pyramid.stl'], made
    rough, finish = made
    assert rough['settings']['tool_diameter'] == 6.35 and rough['settings']['operation'] == 'line'
    # The roughing stops at least the allowance short of the finishing depth, and no pass goes above the stock.
    assert finish['lowest'] + 0.5 - 1e-9 <= rough['lowest'] < -4, (rough['lowest'], finish['lowest'])
    assert rough['highest'] <= 1e-9
    print(
        f"PASS {rough['passes']} roughing passes in 4 levels to {-rough['lowest']:.2f} mm sit ahead of the finishing relief ({-finish['lowest']:.2f} mm)"
    )

    assert rough['settings']['tool_id'] == 1 and rough['settings']['tool_name'] == 'Roughing · 6.35 mm flat'
    assert (
        finish['settings']['tool_id'] == 2
        and finish['settings']['tool_shape'] == 'ball'
        and finish['settings']['tool_diameter'] == 3.175
    )

    # Two cutters cannot share one job: the roughing previews alone, and a tool-change plan has a stage for each.
    tab('CAM')
    loc('#preview').click()
    expect(loc('#message')).to_contain_text('Tool identity or geometry differs between operations')
    page.evaluate('() => { objects[1].output = false; invalidate(); draw(); }')
    loc('#preview').click()
    expect(loc('#summary')).to_contain_text('lines', timeout=60000)
    program = loc('#gcode').text_content()
    assert (
        f"Z{rough['lowest']:.3f}" in program and 'Z-2.000' in program and f"Z{finish['lowest']:.3f}" not in program
    ), program[:300]
    print(
        f'PASS the roughing previews alone as a router job of {program.count(chr(10))} lines, no deeper than {-rough["lowest"]:.2f} mm'
    )
    page.evaluate('() => { objects[1].output = true; invalidate(); draw(); }')
    page.evaluate("() => { document.getElementById('toolChangesPanel').open = true; }")
    loc('#generateToolPlan').click()
    expect(loc('#manualToolStages')).to_contain_text('Roughing · 6.35 mm flat', timeout=60000)
    expect(loc('#manualToolStages')).to_contain_text('Finishing · 3.175 mm ball')
    print('PASS the tool-change plan has a roughing stage and a finishing stage')

    # Adaptive clearing for the same relief: a width of cut to keep instead of a stepover.
    tab('3D relief')
    loc('#roughStrategy').select_option('adaptive')
    expect(loc('#roughStepoverLabel')).to_have_text('Width of cut to keep · mm')
    loc('#roughStepover').fill('0.3')
    loc('#makeRoughing').click()
    expect(loc('#message')).to_contain_text('at least 8% of the cutter diameter')
    loc('#roughStepover').fill('2.5')
    with page.expect_request(lambda request: request.url.endswith('/api/relief-roughing')) as sent:
        loc('#makeRoughing').click()
    expect(loc('#message')).to_contain_text('adaptive clearing keeping about 2.5 mm of cut', timeout=30000)
    assert sent.value.post_data_json['rough_strategy'] == 'adaptive'
    names = page.evaluate('() => objects.map(o => o.name)')
    assert names[:2] == ['Relief roughing · pyramid.stl', 'Relief roughing (adaptive) · pyramid.stl'], names
    adaptive = page.evaluate(
        """() => { const o = objects[1], z = o.depths.flat();
            return {lowest: Math.min(...z), highest: Math.max(...z), paths: o.paths.length, points: z.length,
                    box: CNCLayout.extent(o)}; }"""
    )
    assert rough['lowest'] - 1e-9 <= adaptive['lowest'] < -4 and adaptive['highest'] <= 1e-9, adaptive
    assert adaptive['box'][0] >= 20 - 1e-6 and adaptive['box'][2] <= 100 + 1e-6, adaptive
    tab('CAM')
    page.evaluate('() => { objects.forEach((o, i) => { o.output = i === 1; }); invalidate(); draw(); }')
    loc('#preview').click()
    expect(loc('#summary')).to_contain_text('lines', timeout=60000)
    program = loc('#gcode').text_content()
    assert f"Z{adaptive['lowest']:.3f}" in program
    print(
        f"PASS adaptive clearing adds {adaptive['paths']} paths of {adaptive['points']} points to {-adaptive['lowest']:.2f} mm and previews as a job of {program.count(chr(10))} lines"
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
