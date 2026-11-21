"""Import original-size and fitted SVGs in the server and file:// editors."""

import json
from pathlib import Path

from playwright.sync_api import expect


SAMPLE = b'''<svg xmlns="http://www.w3.org/2000/svg" width="2in" height="25.4mm" viewBox="10 20 96 48">
 <defs><path id="two" d="M0 0 h12 v12 z m24 12 12 0 0 6"/></defs>
 <g transform="translate(10,20)"><use href="#two" x="2" y="3"/>
 <g style="display:none"><path d="M1000 5000 1001 6000"/></g></g>
</svg>'''


def exercise(page):
    page.evaluate("() => window.studioReady")
    loc = page.locator
    tab = lambda label: page.get_by_role("tab", name=label, exact=True).click()
    tab("Design")
    loc("#shapeX").fill("5")
    loc("#shapeY").fill("6")
    loc("#shapeW").fill("84")
    loc("#shapeH").fill("36")
    tab("Images")
    loc("#svgSizing").select_option("physical")

    def upload(data):
        with page.expect_file_chooser() as chooser:
            loc("#svgImport").click()
        chooser.value.set_files({"name": "sample.svg", "mimeType": "image/svg+xml", "buffer": data})

    paths = lambda: page.evaluate("window.CNCProjects.document().objects.map(o=>o.paths)")
    upload(SAMPLE)
    expect(loc("#message")).to_contain_text("2 separate paths")
    imported = paths()[0]
    scale = 50.8 / 96
    source = [[(2, 3), (14, 3), (14, 15), (2, 3)], [(26, 15), (38, 15), (38, 21)]]
    assert len(imported) == 2
    for path, original in zip(imported, source, strict=True):
        assert len(path) == len(original)
        for (x, y), (a, b) in zip(path, original, strict=True):
            assert abs(x - (5 + a * scale)) < 1e-6 and abs(y - (6 + 25.4 - b * scale)) < 1e-6, (x, y, a, b, imported)
    loc("#preview").click()
    expect(loc("#gcode")).to_contain_text("G1")
    assert page.evaluate("preview.segments.length") > 0
    for seg in page.evaluate("preview.segments"):
        # There is a gap of 12 source units between the two subpaths.
        assert not min(seg[0], seg[2]) < 5 + 20 * scale < max(seg[0], seg[2])
    tab("Projects")
    with page.expect_download() as download:
        loc("#projectDownload").click()
    doc = json.loads(Path(download.value.path()).read_text())
    assert doc["objects"][0]["paths"] == imported
    tab("Images")
    loc("#svgSizing").select_option("fit")
    upload(SAMPLE)
    expect(loc("#drawing path[data-object]")).to_have_count(2)
    flat = sum(paths()[1], [])
    assert min(p[0] for p in flat) == 5 and max(p[0] for p in flat) == 89
    assert min(p[1] for p in flat) == 6 and max(p[1] for p in flat) == 42
    # The bundled gear has one compound path for the teeth and bore.
    gear = Path(__file__).resolve().parents[1] / "studio/static/icons/gear.svg"
    upload(gear.read_bytes())
    expect(loc("#drawing path[data-object]")).to_have_count(3)
    gear_paths = paths()[-1]
    assert len(gear_paths) == 2 and all(path[0] == path[-1] for path in gear_paths)
    before = paths()
    upload(b'<svg><path clip-path="url(#trim)" d="M0 0 L1 1"/></svg>')
    expect(loc("#message")).to_contain_text("clip-path")
    assert paths() == before
    results = page.evaluate("""() => {
      const options={physical:true,bed:[200,200],box:[0,0,60,60]};
      const parse = inner => KerfKiteSvg.parse('<svg width="96px" height="72pt" viewBox="0 0 96 96">'+inner+'</svg>',options);
      const curve=parse('<path d="M8 16 Q16 8 24 16T40 16 C44 8 48 8 52 16S62 24 64 16 A8 8 0 0172 24Z"/>')[0];
      const transformed=parse('<g transform="translate(12 14) rotate(90)"><rect width="8" height="4"/></g>')[0];
      const alignment = KerfKiteSvg.parse('<svg width="50mm" height="100mm" viewBox="0 0 50 50"><path d="M0 0 50 50"/></svg>',options)[0];
      const rejected=[];
      for(const text of ['<use href="elsewhere.svg#a"/>','<defs><g id="a"><use href="#a"/></g></defs><use href="#a"/>','<path mask="url(#a)" d="M0 0 4 4"/>','<path d="M1 1 C2 3"/>','<script/>']){
        try{parse(text);rejected.push(false);}catch{rejected.push(true);}
      }
      return {curve,transformed,alignment,rejected};
    }""")
    assert results["curve"][0] == results["curve"][-1] and len(results["curve"]) > 40
    assert abs(results["transformed"][0][0] - 12 * 25.4 / 96) < 1e-7
    assert results["alignment"] == [[0, 75], [50, 25]] and all(results["rejected"])


def run(editor):
    root = Path(__file__).resolve().parents[1]
    for offline in (False, True):
        with editor.context.browser.new_context(
            accept_downloads=True, viewport={"width": 1360, "height": 900}
        ) as context:
            context.set_offline(offline)
            page = context.new_page()
            page.goto((root / "studio/static/index.html").as_uri() if offline else editor.url)
            exercise(page)
            print(
                "PASS compound SVG import, physical units, transforms, arc sampling, CAM gaps, saved geometry and rejected effects in",
                "offline editor" if offline else "server editor",
            )
