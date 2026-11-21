"""Create, dimension, machine, save and reload a two-section CAD loft."""

import struct
from pathlib import Path

from playwright.sync_api import expect


def run(page):
    loc = page.locator

    def tab(name):
        page.get_by_role('tab', name=name, exact=True).click()

    def field(name):
        return page.get_by_label(name, exact=True)

    def ready():
        expect(loc('#solid-status')).to_have_text('Solid ready.')

    tab('Projects')
    loc('#projectNew').click()
    tab('3D CAD')
    loc('#solid-kind').select_option('loft')
    loc('#solid-add').click()
    ready()
    loc('#solid-param-name').fill('outlet_dia')
    loc('#solid-add-param').click()
    field('outlet_dia').fill('18')
    field('Base width X').fill('40')
    field('Base depth Y').fill('24')
    field('Loft height').fill('20')
    field('Top centre offset X').fill('4')
    field('Top diameter').fill('outlet_dia')
    ready()
    loc('#solid-source').select_option('section')
    loc('#solid-slice').fill('10')
    loc('#solid-transfer').click()
    expect(loc('#propertyWidth')).to_have_value('29')
    expect(loc('#propertyHeight')).to_have_value('21')
    tab('3D CAD')
    field('outlet_dia').fill('22')
    ready()
    tab('Properties')
    expect(loc('#propertyWidth')).to_have_value('31')
    expect(loc('#propertyHeight')).to_have_value('23')
    tab('Projects')
    loc('#projectName').fill('Lofted transition')
    loc('#projectSaveLibrary').click()
    row = loc('#projectList p').filter(has_text='Lofted transition')
    expect(row).to_have_count(1)
    loc('#projectNew').click()
    row.get_by_role('button', name='Open', exact=True).click()
    tab('3D CAD')
    ready()
    expect(field('Top section')).to_have_value('circle')
    expect(field('Top diameter')).to_have_value('outlet_dia')
    expect(field('Top centre offset X')).to_have_value('4')
    expect(field('outlet_dia')).to_have_value('22')
    with page.expect_download() as output:
        loc('#solid-export').click()
    stl = Path(output.value.path()).read_bytes()
    (facets,) = struct.unpack_from('<I', stl, 80)
    assert facets >= 12 and len(stl) == 84 + 50 * facets
    loc('#cad-exchange-open').click()
    loc('#exchange-kind').select_option('part')
    with page.expect_download() as output:
        loc('#exchange-export').click()
    step = Path(output.value.path()).read_bytes()
    assert b'MANIFOLD_SOLID_BREP' in step and b'RATIONAL_B_SPLINE_SURFACE' in step
    loc('#exchange-close').click()
    print('PASS loft editor, linked section from 29 × 21 to 31 × 23 mm, project reopen, STL and exact STEP downloads')
