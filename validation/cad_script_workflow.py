"""Exercise typed drawing, script previews, solid history and portable project exchange."""

import json
import math
import struct
from pathlib import Path

from playwright.sync_api import expect


def run(page):
    loc = page.locator
    page.evaluate("() => window.studioReady")
    page.wait_for_function("window.KerfKiteCAD && window.KerfKiteViews")
    command = loc("#cad-command")
    message = loc("#cad-command-result")
    source = loc("#cad-script-source")
    editor = loc("#cad-script-dialog")
    feedback = loc("#cad-script-feedback")
    metrics = loc("#cad-script-metrics")
    objects = lambda: page.evaluate("CNCProjects.document().objects")
    model = lambda: page.evaluate("CNCSolidExchange.model()")
    view = lambda: page.evaluate("KerfKiteViews.state('3d')")
    tab = lambda name: page.get_by_role("tab", name=name, exact=True).click()

    def enter(text, expected):
        command.fill(text)
        command.press("Enter")
        expect(message).to_contain_text(expected)
        expect(command).to_have_value("")

    def upload(control, filename, content):
        loc(control).set_input_files({"name": filename, "mimeType": "text/plain", "buffer": content.encode()})

    errors = []
    page.on("pageerror", lambda error: errors.append(str(error)))
    tab("Design")
    loc("#canvas").focus()
    page.keyboard.press("/")
    expect(command).to_be_focused()
    enter("rect 18 12 3 4", "Added rectangle")
    enter("circle 3 24 9", "Added circle")
    enter("line 0 0 15 10", "Added line")
    assert [o["name"] for o in objects()] == ["Rectangle", "Circle", "Line"]
    assert objects()[0]["paths"][0][:2] == [[3, 4], [21, 4]]
    command.press("ArrowUp")
    expect(command).to_have_value("line 0 0 15 10")
    command.press("ArrowUp")
    expect(command).to_have_value("circle 3 24 9")
    command.press("ArrowDown")
    command.press("ArrowDown")
    expect(command).to_have_value("")
    loc("#cad-command-help").click()
    expect(loc("#cad-reference")).to_be_visible()
    loc("#cad-command-help").click()

    tab("Projects")
    name = loc("#projectName")
    name.fill("Script")
    name.press("End")
    name.press("/")
    expect(name).to_be_focused()
    expect(name).to_have_value("Script/")
    name.fill("Scripted bracket")
    tab("Design")
    drawing = "difference(){ square([20,12],center=true); circle(d=4,$fn=24); }"
    loc("#cad-script-open").click()
    expect(editor).to_be_visible()
    source.fill(drawing)
    source.press("End")
    source.press("Space")
    source.press("/")
    expect(source).to_be_focused()
    expect(source).to_have_value(drawing + " /")
    source.fill(drawing)
    source.press("F5")
    expect(metrics).to_contain_text("2D · 20 × 12 mm · 227.58 mm²")
    # The central hole is transparent in the real preview; material adjacent to it is filled.
    alphas = loc("#cad-script-canvas").evaluate("""c => {
      const ctx=c.getContext('2d'), sample=k=>ctx.getImageData(c.width*k,c.height/2,1,1).data[3];
      return [sample(.5),sample(.64)];
    }""")
    assert alphas == [0, 255], alphas
    assert len(objects()) == 3
    source.press("Control+Enter")
    expect(editor).not_to_be_visible()
    expect(message).to_contain_text("Applied drawing")
    assert len(objects()[-1]["paths"]) == 2
    assert page.evaluate("CNCLayout.extent(CNCProjects.document().objects.at(-1))") == [0, 0, 20, 12]

    tab("Properties")
    for name, value in (("propertyX", "40"), ("propertyY", "24")):
        loc("#" + name).fill(value)
        loc("#" + name).press("Tab")
    assert page.evaluate("CNCLayout.extent(CNCProjects.document().objects.at(-1))") == [40, 24, 60, 36]
    loc("#cad-script-open").click()
    expect(source).to_have_value(drawing)
    changed = drawing.replace("[20,12]", "[30,12]")
    source.fill(changed)
    source.press("F6")
    expect(editor).not_to_be_visible()
    updated = objects()[-1]
    assert updated["scad"]["source"] == changed
    assert page.evaluate("CNCLayout.extent(CNCProjects.document().objects.at(-1))") == [35, 24, 65, 36]

    # A one-line model expression is applied as a solid, then edited in its feature history.
    enter("cube([40,30,8]);", "Applied script")
    expect(loc("#solid-size")).to_have_text("40 × 30 × 8")
    enter("view front", "View · front")
    assert abs(view()["pitch"]) < 1e-9 and abs(view()["yaw"]) < 1e-9
    before_model = model()
    loc("#cad-script-open").click()
    expect(source).to_have_value("cube([40,30,8]);")
    source.fill("difference(){ cube([40,30,8]); sphere(-2); }")
    loc("#cad-script-apply").click()
    expect(feedback).to_contain_text("sphere")
    expect(editor).to_be_visible()
    assert model() == before_model and objects()[-1] == updated

    plate = """module bore(x,y){ translate([x,y,-1]) cylinder(h=10,d=6,$fn=48); }
difference(){ cube([40,30,8]); bore(20,15); }"""
    source.fill(plate)
    source.press("F5")
    expect(metrics).to_contain_text("3D · 40 × 30 × 8 mm")
    with page.expect_download() as source_download:
        loc("#cad-script-save").click()
    assert Path(source_download.value.path()).read_text() == plate
    loc("#cad-script-apply").click()
    expect(editor).not_to_be_visible()
    expect(loc("#solid-status")).to_have_text("Solid ready.")
    features = model()["features"]
    assert len(features) == 1 and features[0]["source"] == plate
    volume = float(loc("#solid-volume").inner_text().replace(",", ""))
    assert math.isclose(volume, 40 * 30 * 8 - 8 * (48 / 2) * 9 * math.sin(math.tau / 48), abs_tol=0.02)
    with page.expect_download() as download:
        loc("#solid-export").click()
    stl = Path(download.value.path()).read_bytes()
    count = struct.unpack_from("<I", stl, 80)[0]
    assert count > 48 and len(stl) == 84 + 50 * count
    vertices = [row[i : i + 3] for row in struct.iter_unpack("<12fH", stl[84:]) for i in (3, 6, 9)]
    assert [max(v[i] for v in vertices) - min(v[i] for v in vertices) for i in range(3)] == [40, 30, 8]
    assert any(abs(math.hypot(x - 20, y - 15) - 3) < 1e-5 and z == 8 for x, y, z in vertices)

    tab("Projects")
    with page.expect_download() as saved:
        loc("#projectDownload").click()
    document = Path(saved.value.path()).read_text()
    assert json.loads(document)["objects"][-1]["scad"]["source"] == changed
    page.reload()
    page.evaluate("() => window.studioReady")
    tab("Projects")
    loc("#loadProject").click()
    upload("#file", "keyboard.cncstudio.json", document)
    page.wait_for_function("CNCProjects.document().objects.length === 4")
    tab("3D CAD")
    expect(loc("#solid-size")).to_have_text("40 × 30 × 8")
    assert model()["features"][0]["source"] == plate
    assert objects()[-1] == updated

    loc("#cad-script-open").click()
    uploaded = "translate([4,6]) square([9,7]);"
    upload("#cad-script-file", "label.scad", uploaded)
    expect(source).to_have_value(uploaded)
    source.press("Control+Enter")
    expect(editor).not_to_be_visible()
    assert objects()[-1]["name"] == "label" and objects()[-1]["scad"]["source"] == uploaded
    assert model()["features"][0]["source"] == plate
    assert not errors, errors
    print(
        "PASS CAD commands and history, focus-safe shortcuts, measured 2D/3D builds, "
        "scripted edits after moving, compiler errors, STL, SCAD and project round trips"
    )
