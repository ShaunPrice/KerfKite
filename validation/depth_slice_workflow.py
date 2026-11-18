"""Verify exposure-layer engraving through the real image UI and both model/energy representations."""
from playwright.sync_api import expect


def run(page):
    page.wait_for_function('!!window.studioSession')
    page.locator('#design-tab-images').click()
    raster = page.locator('details').filter(has=page.locator('#rasterStyle')).last
    if not raster.get_attribute('open') and not raster.locator('#rasterStyle').is_visible():
        raster.locator(':scope > summary').click()
    page.locator('#rasterStyle').select_option('sliced')
    for field,value in [('W','16'),('H','16'),('Interval','1'),('Slices','4')]:
        page.locator('#raster'+field).fill(value)
    page.locator('#rasterDome').click()
    expect(page.locator('#rasterToneStatus')).to_contain_text('4 exposures')
    page.locator('#addRaster').click()
    expect(page.locator('#message')).to_contain_text('4 exposure levels')
    o=page.evaluate('CNCProjects.document().objects.at(-1)')
    assert o['rasterRecipe']['style']=='sliced' and o['rasterRecipe']['slices']==4
    assert o['operationSettings']['passes']==1 and len(o['paths'])>16
    page.locator('#machine-tab-cam').click()
    page.locator('#preview').click()
    expect(page.locator('#message')).to_contain_text('Preview ready')
    # At the centre, four distinct passes traverse the same surface location with power.
    def spans_center(seg):
        return (seg[5]>0 and max(seg[0],seg[2])>18>min(seg[0],seg[2])
                and abs(seg[1]-18.5)<1e-6)
    assert sum(spans_center(s) for s in page.evaluate('preview.segments'))==4
    assert 0 in {s[5] for s in page.evaluate('preview.segments')}
    print('PASS four-level laser depth engraving, source recipe and cumulative exposure at the centre')
