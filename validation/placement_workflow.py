"""Use the canvas to move a layout group and place a tab on a compensated contour."""

from playwright.sync_api import expect


def run(page):
    loc = page.locator
    tab = lambda name: page.get_by_role('tab', name=name, exact=True).click()
    page.evaluate('() => window.studioReady')
    tab('Design')
    for name, value in [
        ('width', 200),
        ('height', 120),
        ('shapeX', 20),
        ('shapeY', 20),
        ('shapeW', 45),
        ('shapeH', 30),
    ]:
        loc('#' + name).fill(str(value))
    loc('#rect').click()
    loc('#shapeX').fill('100')
    loc('#rect').click()
    loc('#layoutAll').click()
    loc('#placement-group').click()

    def coords(x, y):
        return page.evaluate(
            '''([x,y])=>{
          const p=new DOMPoint(x,Number(document.getElementById('height').value)-y)
          .matrixTransform(document.getElementById('canvas').getScreenCTM());return [p.x,p.y];}''',
            [x, y],
        )

    page.mouse.move(*coords(28, 25))
    page.mouse.down()
    page.mouse.move(*coords(38, 34), steps=4)
    page.mouse.up()
    items = page.evaluate('CNCProjects.document().objects')
    assert len(items) == 2 and items[0]['layoutGroup'] == items[1]['layoutGroup']
    mx = lambda o: min(p[0] for line in o['paths'] for p in line)
    assert abs(mx(items[0]) - 30) < 0.6 and abs(mx(items[1]) - 110) < 0.6, (mx(items[0]), mx(items[1]))
    tab('CAM')
    loc('#mode').select_option('router')
    loc('#operation').select_option('inside')
    loc('#tool_diameter').fill('2')
    loc('#depth').fill('3')
    loc('#tabStockThickness').fill('3')
    loc('#tabHeight').fill('1')
    loc('#tabWidth').fill('2')
    loc('#placeTabs').click()
    expect(loc('#tab-map')).to_be_visible()
    # The contour used here is the posted tool centre, 1 mm inside the drawing.
    m = loc('#tab-map-svg path').evaluate('(el)=>el.getBBox().width')
    assert abs(m - 43) < 0.03, m
    dot = loc('#tab-map-svg circle').first.bounding_box()
    pos = loc('#tab-map-svg path').evaluate('''el=>{
      const p=el.getPointAtLength(el.getTotalLength()*.2).matrixTransform(el.getScreenCTM());return [p.x,p.y];}''')
    page.mouse.move(dot['x'] + dot['width'] / 2, dot['y'] + dot['height'] / 2)
    page.mouse.down()
    page.mouse.move(*pos, steps=5)
    page.mouse.up()
    fraction = float(loc('#tab-station').input_value())
    assert abs(fraction - 20) < 1, fraction
    loc('#tab-keep').click()
    loc('#assignHoldingTabs').click()
    loc('#preview').click()
    expect(loc('#summary')).to_contain_text('Holding tabs retain material')
    assigned = page.evaluate(
        "CNCProjects.document().objects.find(o=>o.operationSettings?.tabs)?.operationSettings.tabs"
    )
    assert min(abs(v - 0.2) for v in assigned['positions']) < 0.01
    print('PASS rigid group mouse drag and interactive router tab projection, assignment and CAM')
