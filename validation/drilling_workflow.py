"""A saved peck-drill pattern retains its centres, depths and stock preview."""
import json
from pathlib import Path
from playwright.sync_api import expect


def check(page):
    loc=page.locator
    tab=lambda title:page.get_by_role('tab',name=title,exact=True).click()
    page.evaluate('() => window.studioReady')
    tab('CAM')
    loc('#mode').select_option('router')
    values={'tool_diameter':4,'depth':2.5,'safe':3,'plunge':80,'power':9000,
            'drillX':25,'drillY':30,'drillCols':2,'drillRows':2,'drillDX':12,'drillDY':10,
            'drill_peck':1,'drill_point_angle':118}
    for key,value in values.items():
        loc('#'+key).fill(str(value))
    loc('#addDrillHoles').click()
    expect(loc('#message')).to_contain_text('4 drill locations')
    loc('#preview').click()
    expect(loc('#gcode')).to_contain_text('G1 Z-2.500 F80.0')
    gcode=loc('#gcode').text_content()
    assert gcode.count('G0 X')==4
    assert gcode.count('G1 Z-2.500 F80.0')==4 and gcode.count('G0 Z3.000')==13
    expect(loc('#toolpaths circle')).to_have_count(4)
    page.get_by_text('Rotatable 3D toolpath preview',exact=True).click()
    loc('#simulateStock').click()
    page.wait_for_function('window.stockPreview && Math.min(...window.stockPreview.heights.flat()) < -2')
    minimum=page.evaluate('Math.min(...window.stockPreview.heights.flat())')
    tab('Projects')
    with page.expect_download() as event:
        loc('#projectDownload').click()
    raw=Path(event.value.path()).read_bytes()
    doc=json.loads(raw)
    settings=doc['objects'][0]['operationSettings']
    assert settings['operation']=='drill' and settings['drill_peck']==1 and settings['tool_diameter']==4
    loc('#loadProject').click()
    loc('#file').set_input_files({'name':'bore-grid.cncstudio.json','mimeType':'application/json','buffer':raw})
    expect(loc('#message')).to_contain_text('Project opened')
    loc('#preview').click()
    expect(loc('#gcode')).to_have_text(gcode)
    tab('CAM')
    loc('#operation').select_option('drill')
    with page.context.expect_page() as linked:
        loc('#openMaterialsGuide').click()
    guide=linked.value
    expect(guide.locator('#guide-tool')).to_have_value('drill')
    expect(guide.locator('#guide-operation')).to_have_value('peck')
    guide.locator('#guide-material').select_option('maple')
    expect(guide.locator('#schematic-caption')).to_contain_text('clearance plane')
    guide.close()
    return gcode,minimum


def run(page):
    root=Path(__file__).resolve().parents[1]
    results=[]
    for offline in (False,True):
        with page.context.browser.new_context(viewport={'width':1440,'height':980},accept_downloads=True) as ctx:
            if offline:ctx.set_offline(True)
            editor=ctx.new_page();editor.on('dialog',lambda dialog:dialog.accept())
            editor.goto((root/'studio/static/index.html').as_uri() if offline else page.url)
            results.append(check(editor))
    assert results[0][0]==results[1][0] and abs(results[0][1]-results[1][1])<1e-7
    print('PASS four-hole peck cycle, tip-angle stock simulation, save/reopen and equal offline output')
