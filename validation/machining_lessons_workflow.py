"""Check the standalone toolpath, join and loft exercises in a real browser."""

from pathlib import Path
from urllib.parse import urljoin

from playwright.sync_api import expect


def run(page):
    hosted = urljoin(page.url, 'machining-lessons.html')
    local = Path(__file__).resolve().parents[1] / 'studio/static/machining-lessons.html'
    for address in (hosted, local.as_uri()):
        page.goto(address + '#loft-practice')
        loc = page.locator
        expect(loc('#loft-size')).to_have_text('29 × 21 mm · centre X 2 mm')
        loc('#loft-diameter').fill('22')
        expect(loc('#loft-feedback')).to_contain_text('Target reached')
        box = loc('#loft-outline').evaluate('(p) => {const b=p.getBBox(); return [b.x,b.y,b.width,b.height]}')
        assert all(abs(a - b) < 0.001 for a, b in zip(box, [-13.5, -11.5, 31, 23])), box
        loc('#loft-offset').fill('-8')
        expect(loc('#loft-size')).to_have_text('31 × 23 mm · centre X -4 mm')
        for key, number, dimensions in [
            ('Home', 0, '40 × 24 mm · centre X 0 mm'),
            ('End', 20, '22 × 22 mm · centre X -8 mm'),
        ]:
            loc('#loft-level').press(key)
            expect(loc('#loft-level-value')).to_have_text(f'{number} mm')
            expect(loc('#loft-size')).to_have_text(dimensions)
        loc('#loft-diameter').fill('')
        expect(loc('#loft-feedback')).to_contain_text('8 to 36 mm')
        expect(loc('#loft-drawing')).to_be_hidden()
        loc('#loft-diameter').fill('18')
        expect(loc('#loft-drawing')).to_be_visible()
        for head, setting in [('router', '1'), ('laser', '5'), ('knife', '.5')]:
            loc('#lesson').select_option(head)
            loc('#setting').fill(setting)
            page.get_by_role('button', name='Generate lesson preview').click()
            expect(loc('#feedback')).to_contain_text('Lesson complete')
            assert loc('#motion path').nth(1).evaluate('(p) => p.getTotalLength()') > 0
            expect(loc('#step')).to_be_enabled()
        loc('#trim-horizontal').select_option('left')
        loc('#trim-vertical').select_option('upper')
        expect(loc('#trim-feedback')).to_contain_text('Inside corner complete')
    print(
        'PASS hosted and local-file machining lessons, measured loft outline, target feedback, toolpath playback and trim exercise'
    )
