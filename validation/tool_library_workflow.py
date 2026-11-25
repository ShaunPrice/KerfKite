"""Choose a cutter and a material in the tool library, take its starting feeds, and keep a tool of one's own."""

import json

from playwright.sync_api import expect


def run(page):
    loc = page.locator
    tab = lambda title: page.get_by_role('tab', name=title, exact=True).click()
    page.evaluate('() => window.studioReady')
    tab('Design')
    loc('#shapeW').fill('60')
    loc('#shapeH').fill('40')
    loc('#rect').click()
    tab('CAM')
    loc('#mode').select_option('router')
    opened = lambda name: page.evaluate('id => { document.getElementById(id).open = true; }', name)
    opened('toolLibraryPanel')
    expect(loc('#toolLibraryTool option')).to_have_count(12)
    loc('#toolLibraryTool').select_option('6.35 mm (1/4 in) two-flute end mill')
    loc('#toolLibraryMaterial').select_option('plywood')
    expect(loc('#toolLibraryResult')).to_contain_text('rev/min · feed')
    wanted = page.evaluate(
        """() => { const lib = KerfToolLibrary, start = lib.starter();
        return lib.fields(lib.recommend(start.tools[1], 'plywood', start.machine)); }"""
    )
    loc('#toolLibraryApply').click()
    expect(loc('#message')).to_contain_text('starting settings entered')
    shown = {key: float(loc('#' + key).input_value()) for key in wanted}
    assert shown == wanted, (shown, wanted)
    expect(loc('#manualToolName')).to_have_value('6.35 mm (1/4 in) two-flute end mill')
    print(f'PASS the quarter-inch end mill in plywood fills the CAM fields: {json.dumps(wanted)}')

    # The preview posts the spindle speed and feeds the library gave.
    loc('#operation').select_option('outside')
    loc('#depth').fill('3')
    loc('#preview').click()
    expect(loc('#gcode')).to_contain_text(f"M3 S{int(wanted['power'])}")
    program = loc('#gcode').text_content()
    assert f"F{wanted['feed']:.1f}" in program and f"F{wanted['plunge']:.1f}" in program, program[:400]
    print('PASS the previewed program carries the spindle speed, feed and plunge of the library')

    # A lighter machine lowers the feed; the setting is kept for the next visit.
    before = wanted['feed']
    opened('toolLibraryMachine')
    loc('#toolLibraryRigidity').select_option('light')
    loc('#toolLibraryApply').click()
    assert float(loc('#feed').input_value()) < before
    expect(loc('#gcode')).to_have_text('Generate a new preview after editing.')
    print('PASS a light machine takes a lower feed, and applying it invalidates the preview')

    # A tool of the operator's own, saved, found again after a reload, and exported.
    opened('toolLibraryEdit')
    loc('#toolLibraryName').fill('Shop compression bit')
    loc('#toolLibraryShape').select_option('flat')
    loc('#toolLibraryDiameter').fill('8')
    loc('#toolLibraryFlutes').fill('2')
    loc('#toolLibraryNumber').fill('7')
    loc('#toolLibrarySave').click()
    expect(loc('#toolLibraryTool option')).to_have_count(13)
    loc('#toolLibraryDiameter').fill('0')
    loc('#toolLibrarySave').click()
    expect(loc('#message')).to_have_text('Cutter diameter must be between 0.05 and 100')
    with page.expect_download() as saved:
        loc('#toolLibraryExport').click()
    exported = json.load(open(saved.value.path(), encoding='utf-8'))
    assert exported['format'] == 'kerfkite-tools/1' and exported['machine']['rigidity'] == 'light'
    assert exported['tools'][-1] == {
        'name': 'Shop compression bit',
        'shape': 'flat',
        'diameter': 8,
        'flutes': 2,
        'number': 7,
    }
    page.reload()
    page.evaluate('() => window.studioReady')
    tab('CAM')
    expect(loc('#toolLibraryTool option')).to_have_count(13)
    expect(loc('#toolLibraryRigidity')).to_have_value('light')
    loc('#toolLibraryTool').select_option('Shop compression bit')
    loc('#mode').select_option('laser')
    opened('toolLibraryPanel')
    loc('#toolLibraryApply').click()
    expect(loc('#message')).to_have_text('Feeds and speeds from the tool library are for router mode')
    print('PASS a saved tool survives a reload and an export, a bad diameter is refused, and laser mode is left alone')


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
