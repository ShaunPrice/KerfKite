"""Exercise foreign file review, CAM regeneration and measured fixture checks in the editor."""
import io
import zipfile

from playwright.sync_api import expect


def run(host_page):
    # Use a separate browser store and project so this exercise is repeatable.
    with host_page.context.browser.new_context(viewport={'width': 1450, 'height': 960}) as context:
        page = context.new_page()
        page.on('dialog', lambda dialog: dialog.accept())
        page.goto(host_page.url)
        page.wait_for_function('!!window.studioSession')
        tab = lambda name: page.get_by_role('tab', name=name, exact=True).click()
        loc = page.locator
        tab('Projects')
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as bundle:
            bundle.writestr('device/example.lbdev', '{"DeviceList":[{"Width":220,"Height":120}]}')
            bundle.writestr('library/sample.clb', '<LightBurnLibrary/>')
            bundle.writestr('ignored.txt', 'unmapped')
        loc('#exchange-source-file').set_input_files({'name':'example.lbzip', 'mimeType':'application/zip',
                                                     'buffer':archive.getvalue()})
        expect(loc('#import-status')).to_contain_text('2 supported members · 1 skipped')
        loc('#import-member').select_option(label='example.lbdev')
        loc('#import-read-member').click()
        expect(loc('#import-summary')).to_contain_text('1 setups')
        loc('#import-commit').click()
        loc('#import-use').click()
        expect(loc('#width')).to_have_value('220')
        expect(loc('#height')).to_have_value('120')
        print('PASS select LightBurn bundle member, inspect device and apply workspace')

        def inspect(name, text):
            loc('#exchange-source-file').set_input_files({'name':name, 'mimeType':'text/plain',
                                                         'buffer':text.encode()})
            expect(loc('#import-status')).to_have_text('Review ready.')
        inspect('motif.plt','IN;SP1;VS2;FS5;PU400,400;PD800,400,800,800,400,800,400,400;PU;SP2;PU600,600;CI80;PU;')
        expect(loc('#import-summary')).to_contain_text('2 objects')
        loc('#import-commit').click()
        expect(loc('#import-status')).to_contain_text('Added 2 objects')
        assert page.evaluate('project().operations.length') == 0
        expect(loc('#mode')).to_have_value('knife')
        tab('Layers')
        row = loc('#designLayerList .buttons').filter(has=page.get_by_role('button',name='Pen 1',exact=True))
        row.get_by_label('Output',exact=True).check()
        assert page.evaluate('project().operations.length') == 1
        tab('Projects')
        inspect('workspace.ini','[TRAJ]\nLINEAR_UNITS=metric\n[AXIS_X]\nMIN_LIMIT=-50\nMAX_LIMIT=150\n[AXIS_Y]\nMIN_LIMIT=10\nMAX_LIMIT=110')
        loc('#import-commit').click()
        loc('#import-setup').select_option(label='LinuxCNC workspace')
        loc('#import-use').click()
        expect(loc('#width')).to_have_value('200')
        expect(loc('#height')).to_have_value('100')
        print('PASS HP-GL pen layers and output gating; LinuxCNC soft-limit workspace')
        tab('CAM')
        loc('#preview').click()
        expect(loc('#message')).to_contain_text('Preview ready')
        panel = loc('#fixtureCollisionPanel')
        panel.locator('summary').first.click()
        panel.get_by_text('Try the clamp clearance lesson').click()
        panel.locator('#fixtureLesson').click()
        expect(panel.locator('[data-lesson-results]')).to_contain_text('holder touches Bridge clamp')
        panel.locator('#fixtureLessonY').fill('40')
        panel.locator('#fixtureLesson').click()
        expect(panel.locator('[data-lesson-results]')).to_contain_text('Clear of 1')
        for key, val in zip(('Tool envelope diameter', 'Tip to holder base', 'Holder diameter',
                             'Holder length'), (2, 4, 12, 15)):
            panel.get_by_label(key,exact=True).fill(str(val))
        for el,val in zip(panel.locator('[data-fields="start"] input').all(), (0, 0, 4)):
            el.fill(str(val))
        panel.locator('#fixtureAdd').click()
        clamp = panel.locator('[data-fixtures]')
        clamp.get_by_label('Fixture name').fill('Workshop clamp')
        for key, val in zip(('X minimum', 'X maximum', 'Y minimum','Y maximum', 'Z minimum','Z maximum'),
                            (10, 11, 10, 12, -1, 10)):
            clamp.get_by_label(key).fill(str(val))
        panel.locator('#fixtureCheck').click()
        expect(panel.locator('[data-results]')).to_contain_text('touches Workshop clamp')
        tab('Projects')
        loc('#projectName').fill('Workshop fixture')
        loc('#projectSaveLibrary').click()
        expect(loc('#projectList')).to_contain_text('Workshop fixture')
        loc('#projectNew').click()
        loc('#projectList').get_by_role('button',name='Open',exact=True).click()
        tab('CAM')
        expect(panel.get_by_label('Fixture name')).to_have_value('Workshop clamp')
        expect(panel.get_by_label('Holder diameter')).to_have_value('12')
        assert page.evaluate("project().operations.length") == 1
        print('PASS clamp lesson contact/clear, actual knife CAM contact, project save/reopen')
