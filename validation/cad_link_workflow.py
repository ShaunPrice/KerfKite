"""Exercise rebuilt contours, secondary assembly mates and the local pairing panel."""

from playwright.sync_api import expect


def run(page):
    loc = page.locator
    tab = lambda name: page.get_by_role('tab', name=name, exact=True).click()
    field = lambda name: page.get_by_label(name, exact=True)
    ready = lambda: expect(loc('#solid-status')).to_have_text('Solid ready.')
    tab('3D CAD')
    loc('#solid-kind').select_option('box')
    loc('#solid-add').click()
    ready()
    loc('#solid-source').select_option('section')
    loc('#solid-transfer').click()
    expect(loc('#solid-detach')).to_be_visible()
    expect(loc('#propertyWidth')).to_have_value('60')
    tab('3D CAD')
    field('Length X').fill('74')
    ready()
    tab('Properties')
    expect(loc('#propertyWidth')).to_have_value('74')
    tab('3D CAD')
    field('Length X').fill('bad_width')
    expect(loc('#solid-status')).to_contain_text('Unknown parameter')
    tab('Design')
    loc('#preview').click()
    expect(loc('#message')).to_contain_text('needs its CAD source rebuilt')
    tab('3D CAD')
    field('Length X').fill('65')
    ready()
    tab('Properties')
    expect(loc('#propertyWidth')).to_have_value('65')
    loc('#solid-detach').click()
    expect(loc('#solid-detach')).to_be_hidden()
    tab('3D CAD')
    field('Length X').fill('72')
    ready()
    tab('Properties')
    expect(loc('#propertyWidth')).to_have_value('65')
    print('PASS associated section refresh, stale preview restriction and deliberate detach')
    tab('3D CAD')
    loc('#assembly-open').click()
    loc('#assembly-example').click()
    expect(loc('#assembly-status')).to_contain_text('No interference')
    page.get_by_text('Additional mates', exact=True).click()
    loc('#assembly-extra-parent').fill('0,0,8')
    loc('#assembly-extra-child').fill('0,0,2')
    loc('#assembly-add-relation').click()
    expect(loc('#assembly-status')).to_contain_text('No interference')
    loc('#assembly-apply').click()
    expect(loc('#assembly-editor')).to_be_hidden()
    loc('#assembly-open').click()
    expect(loc('#assembly-relations')).to_contain_text('Shaft')
    loc('#assembly-close').click()
    print('PASS two independent assembly mate conditions saved and reopened')
    tab('Control')
    page.get_by_text('Phone pendant', exact=True).click()
    expect(loc('#phone-enable')).to_be_visible()
    loc('#phone-enable').click()
    loc('#phone-host').fill('127.0.0.1')
    pattern = __import__('re').compile
    expect(loc('#phone-link')).to_have_value(pattern(r'^https?://127\.0\.0\.1:\d+/#token='))

    def code():
        # The QR code is the link shown beside it, drawn square and large enough to scan from a screen.
        expect(loc('#phone-qr svg path')).to_be_visible()
        assert page.evaluate(
            "KerfPairingQr.symbol(document.querySelector('#phone-link').value).path === document.querySelector('#phone-qr path').getAttribute('d')"
        )
        box = loc('#phone-qr svg').bounding_box()
        assert abs(box['width'] - box['height']) < 1 and box['width'] >= 180, box
        return int(loc('#phone-qr svg').get_attribute('viewBox').split()[-1])

    modules = code()
    loc('#phone-host').fill('')
    expect(loc('#phone-qr')).to_be_hidden()
    loc('#phone-host').fill('127.0.0.1')
    if loc('#phone-link').input_value().startswith('https:'):
        # With the web extra the listener is encrypted and the link carries the certificate fingerprint.
        expect(loc('#phone-link')).to_have_value(pattern(r'^https://127\.0\.0\.1:\d+/#token=[^&]+&fp=[0-9a-f]{64}$'))
        expect(loc('#phone-pair-status')).to_contain_text('Encrypted pairing')
        loc('#phone-disable').click()
        expect(loc('#phone-link')).to_be_hidden()
        loc('#phone-plaintext').check()
        loc('#phone-enable').click()
        expect(loc('#phone-link')).to_have_value(pattern(r'^http://'))
        assert code() < modules, 'The plain link has no fingerprint, so its code should be smaller.'
    expect(loc('#phone-link')).to_have_value(pattern(r'^http://127\.0\.0\.1:\d+/#token=[^&]+$'))
    expect(loc('#phone-pair-status')).to_contain_text('Pairing uses HTTP')
    loc('#phone-disable').click()
    expect(loc('#phone-link')).to_be_hidden()
    # A revoked pairing leaves neither the link nor its code in the page.
    assert (
        page.evaluate("document.querySelector('#phone-qr').innerHTML + document.querySelector('#phone-link').value")
        == ''
    )
    print('PASS local phone pendant pairing, its QR code and revocation')
