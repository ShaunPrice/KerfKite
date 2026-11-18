"""Build an original two-page PDF and review its second page through Studio."""

from io import BytesIO
from pypdf import PdfWriter
from pypdf.generic import DecodedStreamObject, NameObject


def add_page(writer, operators):
    page = writer.add_blank_page(width=144, height=180)
    content = DecodedStreamObject()
    content.set_data(operators)
    page[NameObject('/Contents')] = writer._add_object(content)
    return page


def example_pdf():
    writer = PdfWriter()
    add_page(writer, b'BT (Label) Tj ET')
    add_page(writer, b'q 2 0 0 2 18 18 cm 0 0 m 36 0 l S Q\n18 36 m 30 72 52 72 68 36 c S')
    target = BytesIO()
    writer.write(target)
    return target.getvalue()


def run(page):
    from playwright.sync_api import expect

    loc = page.locator
    page.get_by_role('tab', name='Projects', exact=True).click()
    loc('#projectNew').click()
    loc('#exchange-source-file').set_input_files(
        {'name': 'Workshop curves.pdf', 'mimeType': 'application/pdf', 'buffer': example_pdf()}
    )
    expect(loc('#import-pdf-size')).to_contain_text('Page 1 of 2')
    expect(loc('#import-commit')).to_be_disabled()
    expect(loc('#import-notes')).to_contain_text('text objects omitted')
    loc('#import-pdf-page').fill('2')
    loc('#import-pdf-page').press('Tab')
    expect(loc('#import-pdf-size')).to_contain_text('Page 2 of 2')
    expect(loc('#import-summary')).to_contain_text('2 objects')
    loc('#import-commit').click()
    expect(loc('#import-status')).to_contain_text('Added 2 objects')
    assert page.evaluate('project().operations.length') == 0
    page.get_by_role('tab', name='Layers', exact=True).click()
    row = loc('#designLayerList .buttons').filter(
        has=page.get_by_role('button', name='PDF page 2', exact=True)
    )
    row.get_by_label('Output', exact=True).check()
    assert page.evaluate('project().operations.length') == 2
    page.get_by_role('tab', name='CAM', exact=True).click()
    loc('#preview').click()
    expect(loc('#message')).to_contain_text('Preview ready')
    print('PASS select PDF page, review omitted text and cubic paths, enable layer and preview CAM')
