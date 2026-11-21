"""Exercise two-point drafting and corner trimming through the Studio workspace."""

import json
from pathlib import Path

from playwright.sync_api import expect


def run(page):
    loc = page.locator
    page.get_by_role("tab", name="Design", exact=True).click()
    page.evaluate("() => window.studioReady")
    loc("#width").fill("750")
    loc("#height").fill("400")

    def pick(x, y):
        pos = page.evaluate(
            """([x,y]) => {
          const p = new DOMPoint(x,Number(document.getElementById('height').value)-y)
            .matrixTransform(document.getElementById('canvas').getScreenCTM());
          return [p.x,p.y];
        }""",
            [x, y],
        )
        page.mouse.click(*pos)

    def line(a, b):
        loc("#cornerDraw").click()
        pick(*a)
        pick(*b)
        expect(loc("#cornerControls")).to_be_hidden()

    def paths():
        return page.evaluate("window.CNCProjects.document().objects.map(o=>o.paths)")

    line((110, 110), (370, 110))
    line((240, 40), (240, 200))
    originals = paths()
    assert len(originals) == 2
    loc("#cornerStart").click()
    pick(150, 110)
    pick(240, 175)
    expect(loc("#cornerApply")).to_be_enabled()
    expect(loc("#cornerOverlay path")).to_have_count(1)
    assert paths() == originals
    page.keyboard.press("Escape")
    expect(loc("#cornerControls")).to_be_hidden()
    loc("#cornerStart").click()
    pick(150, 110)
    pick(240, 175)
    loc("#cornerBack").click()
    pick(240, 70)
    loc("#cornerApply").click()
    joined = paths()
    assert len(joined) == 1
    path = joined[0][0]
    assert len(path) == 3 and path[0] == originals[0][0][0] and path[-1] == originals[1][0][0]
    assert all(abs(a - b) < 2 for a, b in zip(path[1], [240, 110]))
    loc("#undo").click()
    assert paths() == originals
    loc("#redo").click()
    assert paths() == joined
    loc("#preview").click()
    expect(loc("#gcode")).to_contain_text("G1")
    page.get_by_role("tab", name="Projects", exact=True).click()
    with page.expect_download() as result:
        loc("#projectDownload").click()
    document = json.loads(Path(result.value.path()).read_text())
    assert document["objects"][0]["paths"] == joined[0]
    page.reload()
    page.evaluate("() => window.studioReady")
    loc("#width").fill("750")
    loc("#height").fill("400")
    page.get_by_role("tab", name="Projects", exact=True).click()

    def upload(doc):
        loc("#loadProject").click()
        loc("#file").set_input_files(
            {"name": "joined.cncstudio.json", "mimeType": "application/json", "buffer": json.dumps(doc).encode()}
        )
        expect(loc("#message")).to_contain_text("Project opened")

    upload(document)
    assert paths() == joined
    # Distinct object operation settings must never silently collapse into one operation.
    document["objects"] = [
        {"name": "First", "paths": originals[0]},
        {"name": "Second", "paths": originals[1], "operationSettings": {"feed": 333}},
    ]
    upload(document)
    page.get_by_role("tab", name="Design", exact=True).click()
    loc("#cornerStart").click()
    pick(150, 110)
    pick(240, 70)
    expect(loc("#cornerPrompt")).to_contain_text("matching output")
    expect(loc("#cornerApply")).to_be_disabled()
    assert paths() == originals
    loc("#cornerCancel").click()
    print(
        "PASS mouse line drawing, four-arm trim preview, reselection, cancellation, undo/redo, CAM, portable reload and CAM ownership"
    )
