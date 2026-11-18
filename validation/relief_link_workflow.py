"""Exercise associative 3D relief and independent same-name STL input in Studio."""
import json
from pathlib import Path

from playwright.sync_api import expect


def run(page):
    loc = page.locator
    tab = lambda name: page.get_by_role("tab",name=name,exact=True).click()
    saved = lambda: page.evaluate("CNCProjects.document().objects")
    page.evaluate("() => window.studioReady")
    doc = page.evaluate("CNCProjects.document()")
    doc["extras"]["solidCAD"] = {"version":1,"parameters":{"span":36},"features":[
        {"id":"part","kind":"box","op":"add","size":["span",20,4]},
        {"id":"groove","kind":"box","op":"cut","size":[18,22,3],"origin":[8,-1,2]}]}

    def open_doc(content):
        tab("Projects")
        loc("#loadProject").click()
        loc("#file").set_input_files({"name":"grooved-plate.json","mimeType":"application/json","buffer":json.dumps(content).encode()})
        expect(loc("#message")).to_contain_text("Project opened")

    open_doc(doc)
    tab("3D CAD")
    loc("#solid-process").select_option("relief")
    expect(loc("#solid-link")).to_be_visible()
    loc("#solid-x").fill("22")
    loc("#solid-y").fill("33")
    loc("#solid-transfer").click()
    expect(loc("#reliefFile")).to_have_value(r"C:\fakepath\solid-part.stl")
    loc("#shapeW").fill("18")
    loc("#shapeH").fill("10")
    loc("#reliefDepth").fill("2")
    loc("#reliefResolution").fill("20")
    loc("#makeRelief").click()
    expect(loc("#message")).to_contain_text("relief.")
    page.wait_for_function("CNCProjects.document().objects.length === 1")
    o, = saved()
    assert o["solidLink"]["kind"] == "relief"
    assert o["solidLink"]["scale"] == [.5,.5,.5]
    loc("#cornerStart").click()
    loc("#cornerControls summary").click()
    loc("#cornerX").fill("26")
    loc("#cornerY").fill("33")
    loc("#cornerPick").click()
    expect(loc("#cornerPrompt")).to_contain_text("Detach its CAD link")
    expect(loc("#cornerApply")).to_be_disabled()
    loc("#cornerCancel").click()
    tab("Layers")
    loc("#designLayerName").fill("Relief finishing")
    loc("#designLayerCreate").click()
    loc("#designLayerAssign").click()
    tab("CAM")
    loc("#feed").fill("415")
    loc("#assignOperation").click()
    expected_owner, = saved()
    assert expected_owner["operationSettings"]["feed"] == 415
    loc("#preview").click()
    expect(loc("#message")).to_contain_text("Preview ready")
    tab("3D CAD")
    page.get_by_label("span",exact=True).fill("72")
    expect(loc("#solid-size")).to_have_text("72 × 20 × 4")
    expect(loc("#solid-status")).to_have_text("Solid ready.")
    expect(loc("#gcode")).to_contain_text("Generate a new preview")
    rebuilt, = saved()
    assert rebuilt["surface"]["width"] == 36 and rebuilt["surface"]["height"] == 10 and rebuilt["surface"]["depth"] == 2
    assert rebuilt["surface"]["x"] == 22 and rebuilt["surface"]["y"] == 33
    assert rebuilt["solidLink"]["tool"] == o["solidLink"]["tool"]
    assert rebuilt["designLayerId"] == expected_owner["designLayerId"]
    assert rebuilt["operationSettings"] == expected_owner["operationSettings"]
    assert rebuilt["solidLink"]["revision"] != o["solidLink"]["revision"]
    assert len(rebuilt["depths"]) == len(rebuilt["paths"])
    tab("3D relief")
    loc("#makeRelief").click()
    expect(loc("#message")).to_contain_text("earlier CAD revision")
    assert len(saved()) == 1
    tab("3D CAD")
    with page.expect_download() as download:
        loc("#solid-export").click()
    source = Path(download.value.path()).read_bytes()
    tab("3D relief")
    loc("#reliefFile").set_input_files({"name":"solid-part.stl","mimeType":"model/stl","buffer":source})
    loc("#makeRelief").click()
    expect(loc("#message")).to_contain_text("relief.")
    page.wait_for_function("CNCProjects.document().objects.length === 2")
    assert "solidLink" not in saved()[-1]
    tab("3D CAD")
    loc("#solid-link").uncheck()
    loc("#solid-transfer").click()
    loc("#makeRelief").click()
    expect(loc("#message")).to_contain_text("relief.")
    page.wait_for_function("CNCProjects.document().objects.length === 3")
    assert len(saved()) == 3 and "solidLink" not in saved()[-1]
    print("PASS linked top relief updates its scaled dimensions with its tool recipe, placement and CAM ownership intact; stale staging is blocked, independent imports stay independent")
