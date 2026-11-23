"""Browser acceptance for surface edits, STEP exchange and CAM transfers.

STEP documents are read and written by the workstation server the page came from, through its STEP routes (the
Open CASCADE add-on), with the editor's session token.
"""

import base64
import json
import urllib.parse
from pathlib import Path

from playwright.sync_api import expect


def call(page, route, payload):
    """A route of the page's workstation server, as the editor calls it."""
    address = urllib.parse.urlsplit(page.url)
    origin = f'{address.scheme}://{address.netloc}/'
    token = page.request.get(origin + 'api/session').json()['token']
    headers = {'Content-Type': 'application/json', 'X-Studio-Token': token}
    response = page.request.post(origin + route, data=json.dumps(payload), headers=headers)
    assert response.ok, (route, response.status, response.text()[:300])
    return response


def inspect_step(page, content):
    """What the server's STEP import makes of a document: its solids and faces."""
    return call(page, 'api/solid-cad/step/import', {'content': base64.b64encode(content).decode()}).json()


def write_step(page, model):
    return call(page, 'api/solid-cad/step/export', {'model': model}).body()


def run(page):
    loc = page.locator

    def open_exchange():
        page.get_by_role('tab', name='3D CAD', exact=True).click()
        loc('#cad-exchange-open').click()

    open_exchange()
    expect(loc('#exchange-export')).to_be_enabled()
    loc('#exchange-guide').click()
    loc('#exchange-Z').fill('12')
    loc('#exchange-apply').click()
    expect(loc('#exchange-instruction')).to_contain_text('Orbit')
    with page.expect_download() as saved:
        loc('#exchange-export').click()
    sheet = Path(saved.value.path()).read_bytes()
    assert inspect_step(page, sheet)['solids'] == 0
    loc('#exchange-lesson-keep').click()
    expect(loc('#exchange-lesson')).to_be_hidden()
    assert page.evaluate('window.projectExtras.cadExchange.surface.poles[1][1][2]') == 12
    loc('#exchange-basis').select_option('bspline')
    loc('#exchange-du').fill('2')
    loc('#exchange-weight').fill('2.5')
    loc('#exchange-apply').click()
    page.wait_for_function("window.projectExtras.cadExchange.surface.weights?.[1][1] === 2.5")
    loc('#exchange-row').click()
    page.wait_for_function("window.projectExtras.cadExchange.surface.poles.length === 5")
    with page.expect_download() as weighted:
        loc('#exchange-export').click()
    assert b'RATIONAL_B_SPLINE_SURFACE' in Path(weighted.value.path()).read_bytes()
    print('PASS multispan NURBS edit, pole weight, net extension and analytic export')

    loc('#exchange-append').click()
    expect(loc('#exchange-status')).to_contain_text('2 faces')
    loc('#exchange-continuity').select_option('tangent')
    page.wait_for_function("window.projectExtras.cadExchange.surface.patches[1].continuity === 'tangent'")
    expect(loc('#exchange-driven')).to_be_visible()
    loc('#exchange-net summary').click()
    loc('#exchange-u0').fill('.1')
    loc('#exchange-u1').fill('.9')
    loc('#exchange-trim').click()
    page.wait_for_function("window.projectExtras.cadExchange.surface.trim[0] === .1")
    loc('#exchange-outline').click()
    loc('#outline-square').click()
    loc('#outline-hole').click()
    # The hole crosses the seam between the two patches.
    loc('#outline-points').fill('0.35,0.25\n0.68,0.25\n0.54,0.56')
    loc('#outline-apply').click()
    expect(loc('#outline-heading')).to_be_hidden()
    with page.expect_download() as sewed:
        loc('#exchange-export').click()
    exact = inspect_step(page, Path(sewed.value.path()).read_bytes())
    assert exact['faces'] == 2 and exact['solids'] == 0
    loc('#exchange-relief').click()
    expect(loc('#reliefFile')).to_have_value(r'C:\fakepath\geometry.stl')
    page.get_by_role('tab', name='Projects', exact=True).click()
    loc('#projectName').fill('Trimmed joined sheet')
    loc('#projectSaveLibrary').click()
    row = loc('#projectList p').filter(has_text='Trimmed joined sheet')
    expect(row).to_have_count(1)
    loc('#projectNew').click()
    row.get_by_role('button', name='Open', exact=True).click()
    open_exchange()
    expect(loc('#exchange-status')).to_contain_text('2 faces')
    assert page.evaluate('window.projectExtras.cadExchange.surface.boundary[1].length') == 3
    print('PASS C1 patch seam, shaped trim hole, exact sewn STEP, relief transfer and project reopen')

    loc('#exchange-file').set_input_files({'name': 'patch.step', 'mimeType': 'model/step', 'buffer': sheet})
    expect(loc('#exchange-status')).to_contain_text('0 solids')
    expect(loc('#exchange-add')).to_be_disabled()
    loc('#exchange-relief').click()
    expect(loc('#reliefFile')).to_have_value(r'C:\fakepath\geometry.stl')
    print('PASS guided surface edit, retained control point, STEP download/import, open-patch relief')

    # Start a clean part so the dimension check also detects accidental geometry unions.
    page.get_by_role('tab', name='Projects', exact=True).click()
    loc('#projectNew').click()
    open_exchange()
    raw = write_step(page, {'version': 1, 'features': [{'id': 'plate', 'kind': 'box', 'size': [40, 20, 5]}]})
    loc('#exchange-file').set_input_files({'name': 'plate.stp', 'mimeType': 'model/step', 'buffer': raw})
    expect(loc('#exchange-status')).to_contain_text('plate.stp')
    loc('#exchange-add').click()
    expect(loc('#solid-size')).to_have_text('40 × 20 × 5')
    print('PASS imported STEP to editable part feature')
