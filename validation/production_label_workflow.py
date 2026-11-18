"""Exercise single-line text, serial codes, layout groups and snap placement."""
import json
from pathlib import Path
from playwright.sync_api import expect


def run(page):
    find = page.locator
    tab = lambda name: page.get_by_role('tab', name=name, exact=True).click()
    data = lambda: page.evaluate('() => CNCProjects.document()')
    page.evaluate('() => window.studioReady')
    tab('Control')
    find('#machine-library summary').click()
    find('#machine-name').fill('Sample bench')
    find('#machine-width').fill('500')
    find('#machine-height').fill('240')
    find('#machine-z').fill('50')
    find('#machine-dialect').select_option('grbl')
    find('#machine-save').click()
    expect(find('#profileHint')).to_contain_text('Sample bench')
    assert data()['extras']['machineProfile']['zTravel']==50
    tab('Layers')
    find('#designLayerName').fill('Plate numbers')
    find('#designLayerCreate').click()
    tab('Text')
    find('#textFont').select_option('Workshop Mono · single line')
    find('#textSize').fill('5')
    find('#seriesLabels summary').click()
    find('#label-count').fill('3')
    find('#label-start').fill('17')
    find('#label-columns').fill('3')
    find('#label-add').click()
    expect(find('#label-summary')).to_contain_text('PART-017')
    inserted = data()['objects']
    assert [o['textOptions']['textValue'] for o in inserted]==['PART-017','PART-018','PART-019']
    assert len({o['designLayerId'] for o in inserted})==1 and inserted[0]['designLayerId']!='default'
    assert all(len(path)==len(depths) for o in inserted for path,depths in zip(o['paths'],o['depths']))

    find('#editText').click()
    find('#textValue').fill('AA-019')
    find('#replaceText').click()
    assert data()['objects'][2]['textOptions']['textValue']=='AA-019'
    assert data()['objects'][2]['designLayerId']==inserted[2]['designLayerId']

    tab('Design')
    find('#layoutAll').click()
    find('#placement-group').click()
    group=data()['objects'][0]['layoutGroup']
    find('#placement-x').fill('25')
    find('#placement-y').fill('40')
    find('#placement-move').click()
    grouped=data()['objects']
    assert all(o['layoutGroup']==group for o in grouped)
    assert min(p[1] for o in grouped for path in o['paths'] for p in path)==40
    # Mouse pick tests check actual snapping; exact coordinate entry stays exact.
    find('#placementSnapping summary').click()
    find('#placement-grid').check()
    find('#placement-pitch').fill('5')
    find('#cornerDraw').click()
    def pick(x,y):
        a,b=page.evaluate('''([x,y])=>{
            const p=new DOMPoint(x,Number(document.getElementById('height').value)-y)
             .matrixTransform(document.getElementById('canvas').getScreenCTM());return [p.x,p.y];}''',[x,y])
        page.mouse.click(a,b)
    pick(100.7,60.3)
    pick(128.4,73.8)
    assert data()['objects'][-1]['paths']==[[[100,60],[130,75]]],data()['objects'][-1]

    tab('Text')
    find('#label-kind').select_option('qr')
    find('#label-pattern').fill('Gear-17')
    find('#label-count').fill('1')
    find('#label-add').click()
    assert data()['objects'][-1]['name']=='QR · Gear-17'
    assert len(data()['objects'][-1]['paths'])>100
    tab('CAM')
    find('#mode').select_option('laser')
    find('#preview').click()
    expect(find('#gcode')).to_contain_text('M4')
    tab('Quote')
    find('#cyclePanel summary').click()
    find('#time-estimate').click()
    expect(find('#time-result')).to_contain_text('uncalibrated')
    find('#time-measured').fill('5.2')
    find('#time-calibrate').click()
    expect(find('#time-result')).to_contain_text('5.20 min')
    find('#time-quote').click()
    expect(find('#cost_minutes')).to_have_value('5.2')
    expect(find('#cost_timeFactor')).to_have_value('1')
    tab('Projects')
    with page.expect_download() as export:
        find('#projectDownload').click()
    raw=Path(export.value.path()).read_bytes()
    assert json.loads(raw)['objects'][0]['layoutGroup']==group
    assert json.loads(raw)['extras']['cycleTime']['sample']['minutes']==5.2
    assert json.loads(raw)['extras']['placementSnap']['grid'] is True
    find('#projectNew').click()
    with page.expect_file_chooser() as choose:
        find('#loadProject').click()
    choose.value.set_files({'name':'Labels.cncstudio.json','mimeType':'application/json','buffer':raw})
    page.wait_for_function("window.CNCProjects.document().objects.length===5")
    assert data()['objects'][-1]['name']=='QR · Gear-17'
    assert data()['extras']['machineProfile']['name']=='Sample bench'
    print('PASS single-line text, numbered batch and layer, group placement, snap drafting, QR engraving, calibrated quote and project round trip')
    from tool_finder_workflow import run as check_finder
    check_finder(page)
