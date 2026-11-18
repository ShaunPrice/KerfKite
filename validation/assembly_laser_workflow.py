"""Additional workflows for the browser-based Studio simulator harness."""
from playwright.sync_api import expect


def run(page):
    tab = lambda name: page.get_by_role('tab', name=name, exact=True).click()
    loc = page.locator
    tab('Projects')
    loc('#projectNew').click()
    tab('3D CAD')
    loc('#assembly-open').click()
    loc('#assembly-example').click()
    state = loc('#assembly-status')
    expect(state).to_contain_text('No interference detected')
    loc('#assembly-parent-origin').fill('24,15,0')
    loc('#assembly-solve').click()
    expect(state).to_contain_text('Bearing plate × Shaft')
    loc('#assembly-parent-origin').fill('20,15,0')
    loc('#assembly-apply').click()
    expect(loc('#assembly-editor')).not_to_be_visible()
    assert page.evaluate("window.projectExtras.assembly.mates[0].parentFrame.origin[0]") == 20
    print('PASS concentric assembly mate, clearance, interference volume and saved assembly')

    tab('Images')
    loc('#rasterSample').click()
    loc('#rasterW').fill('8')
    loc('#rasterH').fill('6')
    loc('#rasterCropLeft').evaluate("el => el.closest('details').open=true")
    loc('#rasterCropLeft').fill('10')
    loc('#rasterMask').select_option('ellipse')
    loc('#rasterStyle').select_option('dither')
    loc('#rasterInvert').check()
    expect(loc('#rasterToneStatus')).to_contain_text('16 × 12')
    loc('#addRaster').click()
    expect(loc('#message')).to_contain_text('Raster added')
    raster = page.evaluate('CNCProjects.document().objects[0]')
    assert raster['rasterRecipe']['crop']['left'] == 10
    assert raster['powers'][0][2] == 0 and any(max(row) == 1 for row in raster['powers'])
    loc('#materialGridAdd').evaluate("el => el.closest('details').open=true")
    loc('#materialGridAdd').click()
    expect(loc('#message')).to_contain_text('9 material test cells')
    loc('#preview').click()
    expect(loc('#message')).to_contain_text('Preview ready')
    jobs = page.evaluate('CNCProjects.document().settings.operations')
    assert len(jobs) == 10 and jobs[-1]['feed'] == 1200 and jobs[-1]['power'] == 300
    tab('Projects')
    loc('#projectName').fill('Bearing and tonal coupon')
    loc('#projectSaveLibrary').click()
    row=loc('#projectList p').filter(has_text='Bearing and tonal coupon')
    expect(row).to_have_count(1)
    loc('#projectNew').click()
    row.get_by_role('button', name='Open', exact=True).click()
    expect(loc('#projectName')).to_have_value('Bearing and tonal coupon')
    restored=page.evaluate('CNCProjects.document()')
    assert restored['settings']['operations'] == jobs
    assert restored['objects'][0]['powers'] == raster['powers']
    assert restored['extras']['assembly']['mates'][0]['kind'] == 'concentric'
    print('PASS image crop, masked dithering, speed/power grid, preview and saved machining settings')
    tab('Images')
    loc('#camera-reg-demo').click()
    expect(loc('#camera-reg-info')).to_contain_text('Teaching card solved')
    overlay=page.locator('#stock + image')
    expect(overlay).to_have_attribute('opacity','0.45')
    loc('#camera-reg-save').click()
    expect(loc('#camera-reg-info')).to_contain_text('Calibration saved')
    loc('#camera-reg-marker').select_option('0')
    loc('#camera-reg-wx').fill('12')
    expect(loc('#camera-reg-add')).to_be_disabled()
    loc('#camera-reg-recall').click()
    expect(loc('#camera-reg-wx')).to_have_value('0')
    loc('#tracePreview').click()
    loc('#camera-reg-add').click()
    expect(loc('#camera-reg-info')).to_contain_text('1 registered')
    registered=page.evaluate('CNCProjects.document().objects.at(-1)')
    assert registered['name'].startswith('Registered trace')
    assert registered['paths'][0][0]==registered['paths'][0][-1]
    assert registered['paths'][0][0][0]>100
    print('PASS camera teaching card, projective bed overlay, profile recall and registered trace placement')
    tab('Projects')
    loc('#projectNew').click()
    tab('Design')
    for x,y,w in [(5,5,40),(16,16,4),(16,16,4)]:
        for selector,value in [('shapeX',x),('shapeY',y),('shapeW',w),('shapeH',w)]:
            loc('#'+selector).fill(str(value))
        loc('#rect').click()
    tab('CAM')
    loc('#mode').select_option('laser')
    loc('#operation').select_option('line')
    tab('Layers')
    loc('#laser-order-enabled').check()
    loc('#laser-order-dedupe').check()
    expect(loc('#laser-order-report')).to_contain_text('Removed 1 duplicate')
    loc('#preview').click()
    expect(loc('#message')).to_contain_text('Preview ready')
    planned=page.evaluate('CNCProjects.document()')
    assert len(planned['objects'])==3 and len(planned['settings']['operations'])==1
    assert [p[0] for p in planned['settings']['paths']]==[[16,16],[5,5]]
    print('PASS layer-aware laser planner, nested first, exact duplicate removal and real preview')
