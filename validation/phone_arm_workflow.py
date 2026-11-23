"""Exercise current-preview confirmation and concurrent Stop across two real browser sessions.

The workstation server is a native_servers.studio(): pairing is switched on for the run, and the phone is a second
browser session on the monitor's loopback address with the pairing token.
"""

from playwright.sync_api import expect


def connect_editor(editor):
    editor.evaluate('() => window.studioReady')
    editor.locator('#port').select_option('simulator')
    editor.locator('#connect').click()
    expect(editor.locator('#state')).to_contain_text('SIMULATOR')
    editor.get_by_role('tab', name='Design', exact=True).click()
    editor.locator('#rect').click()


def arm_preview(editor):
    editor.locator('#preview').click()
    expect(editor.locator('#message')).to_contain_text('Preview ready')
    editor.get_by_role('tab', name='Control', exact=True).click()
    editor.locator('#confirmed').check()
    editor.locator('#armPhone').click()
    expect(editor.locator('#message')).to_contain_text('Preview armed')


def run(editor, server):
    active = lambda: server.status()['active']
    armed = lambda: server.monitor_status()['armed']
    server.enable_monitor()
    with editor.context.browser.new_context() as phone_context:
        phone = phone_context.new_page()
        phone.on('dialog', lambda dialog: dialog.accept())
        phone.goto(f'{server.monitor}#token={server.token}')
        connect_editor(editor)

        def wait_for_new_arm():
            # Start displays the epoch from the last real status poll. Wait for this
            # workstation preview, even if the previous arm had also enabled Start.
            phone.wait_for_function(
                "epoch => lastState?.armed && lastState.control_epoch === epoch",
                arg=server.monitor_status()['control_epoch'],
            )
            expect(phone.locator('#start')).to_be_enabled()

        arm_preview(editor)
        wait_for_new_arm()
        # A phone seeing the earlier armed state cannot authorize a replacement preview.
        previous_status = phone.evaluate('() => lastState')
        phone.route('**/api/status', lambda route: route.fulfill(json=previous_status))
        arm_preview(editor)
        phone.locator('#start').click()
        expect(phone.locator('#actionResult')).to_contain_text('Controller controls changed')
        assert not active() and armed()
        phone.unroute('**/api/status')
        wait_for_new_arm()
        editor.get_by_role('tab', name='CAM', exact=True).click()
        # A temporarily failed disarm can outlast a normal status poll after reconnection.
        editor.route('**/api/disarm', lambda route: route.abort())
        editor.context.set_offline(True)
        editor.locator('#feed').fill('211')
        expect(editor.locator('#gcode')).to_contain_text('new preview')
        expect(editor.locator('#message')).to_contain_text('Waiting to confirm disarm')
        editor.context.set_offline(False)
        expect(editor.locator('#state')).to_contain_text('SIMULATOR')
        phone.locator('#start').click()
        expect(phone.locator('#actionResult')).to_contain_text('No job started', timeout=5000)
        assert not active()
        assert not armed()
        editor.unroute('**/api/disarm')

        arm_preview(editor)
        wait_for_new_arm()
        phone.locator('#start').click()
        expect(phone.locator('#actionResult')).to_contain_text('start accepted', timeout=5000)
        expect(phone.locator('#start')).to_be_disabled()
        assert not armed()
        expect(phone.locator('#state')).to_have_text('Idle')

        arm_preview(editor)
        wait_for_new_arm()
        # Stop is available on the same phone while an owner reply is pending.
        checks = []
        editor.route('**/api/arm/check', lambda route: checks.append(route))
        phone.locator('#start').click()
        expect(phone.locator('#actionResult')).to_contain_text('Waiting')
        expect(phone.locator('#stop')).to_be_enabled()
        phone.locator('#stop').click()
        expect(phone.locator('#actionResult')).to_contain_text('stop accepted')
        for route in checks:
            route.continue_()
        editor.unroute('**/api/arm/check')
        expect(phone.locator('#start')).to_be_disabled()
        assert not armed()
        assert not active()
        expect(phone.locator('#actionResult')).to_contain_text('stop accepted')
        arm_preview(editor)
        wait_for_new_arm()
        editor.context.set_offline(True)
        editor.close()
        phone.locator('#start').click()
        expect(phone.locator('#actionResult')).to_contain_text('No job started', timeout=5000)
        assert not active()
    server.disable_monitor()
    print('PASS live browser owner confirmation, lost disarm after offline edit, one-use phone Start and priority Stop')
