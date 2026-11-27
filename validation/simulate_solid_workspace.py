"""Exercise the CAD workspace using the real editor and the workstation server in simulation.

Build the native programs (native/README.md), install Playwright, then run:
    python validation/simulate_solid_workspace.py --browser /path/to/chromium
It starts kerfkite-studio (--server, KERFKITE_STUDIO or build/native) on a free port and runs the lessons, STEP
exchange among them when the Open CASCADE add-on (kerfkite-step) is beside it. With --url
the lessons run against a server that is already running instead; the phone arm is then left out, and so is STEP
exchange (the exchange lesson and the loft lesson's STEP download) unless --step says the server has it.
"""

import argparse
import contextlib
import struct
from pathlib import Path

from playwright.sync_api import expect, sync_playwright

from native_servers import beside, program, studio


def run(page):
    loc = page.locator
    tab = lambda name: page.get_by_role("tab", name=name, exact=True).click()
    ready = lambda: expect(loc("#solid-status")).to_have_text("Solid ready.")
    field = lambda name: page.get_by_label(name, exact=True)
    errors = []
    page.on("pageerror", lambda error: errors.append(str(error)))
    tab("3D CAD")
    loc("#solid-guide").click()
    ready()
    field("width").fill("80")
    expect(loc("#solid-size")).to_have_text("80 × 32 × 6")
    loc("#solid-tree button").filter(has_text="bore").click()
    page.get_by_role("button", name="Suppress", exact=True).click()
    expect(loc("#solid-instruction")).to_contain_text("Choose Enable")
    page.get_by_role("button", name="Enable", exact=True).click()
    expect(loc("#solid-lesson-title")).to_have_text("Plate tutorial complete")
    loc("#solid-keep").click()
    ready()
    print("PASS guided plate and suppression")
    field("width").fill("bad_dimension")
    expect(loc("#solid-status")).to_contain_text("Unknown parameter")
    expect(loc("#solid-transfer")).to_be_disabled()
    field("width").fill("80")
    ready()
    tab("Projects")
    loc("#projectName").fill("Browser test plate")
    loc("#projectSaveLibrary").click()
    row = loc("#projectList p").filter(has_text="Browser test plate")
    expect(row).to_have_count(1)
    loc("#projectNew").click()
    row.get_by_role("button", name="Open", exact=True).click()
    tab("3D CAD")
    expect(loc("#solid-size")).to_have_text("80 × 32 × 6")
    print("PASS invalidation and CAD-only project round trip")
    loc("#solid-tree button").filter(has_text="bore").click()
    page.get_by_text("Pattern copies", exact=True).click()
    field("Pattern").select_option("linear")
    ready()
    field("Instance count").fill("2")
    ready()
    with page.expect_download() as saved:
        loc("#solid-export").click()
    data = Path(saved.value.path()).read_bytes()
    assert len(data) == 84 + 50 * struct.unpack_from("<I", data, 80)[0]
    print("PASS linear feature pattern and binary STL")
    for process in ("laser", "knife", "router"):
        tab("3D CAD")
        ready()
        loc("#solid-source").select_option("section")
        loc("#solid-slice").fill("3")
        loc("#solid-process").select_option(process)
        loc("#solid-transfer").click()
        expect(loc("#mode")).to_have_value(process)
        loc("#preview").click()
        expect(loc("#message")).to_contain_text("Preview ready")
        print("PASS horizontal section to", process)
    tab("3D CAD")
    ready()
    loc("#solid-process").select_option("relief")
    loc("#solid-transfer").click()
    expect(loc("#reliefDepth")).to_have_value("6")
    expect(loc("#reliefFile")).to_have_value(r"C:\fakepath\solid-part.stl")
    loc("#makeRelief").click()
    expect(loc("#message")).to_contain_text("relief.", timeout=30000)
    print("PASS solid to top relief")
    tab("Projects")
    loc("#projectNew").click()
    tab("3D CAD")
    loc("#solid-param-name").fill("peak")
    loc("#solid-add-param").click()
    field("peak").fill("20")
    loc("#solid-kind").select_option("extrude")
    loc("#solid-add").click()
    ready()
    field("Y 3").fill("peak / 2")
    ready()
    page.get_by_role("button", name="Edit sketch on plane").click()
    dialog = page.get_by_role("dialog", name="Sketch plane editor")
    expect(dialog).to_be_visible()
    node = dialog.locator('[data-index="2"]')
    node_box = node.bounding_box()
    x = node_box["x"] + node_box["width"] / 2
    y = node_box["y"] + node_box["height"] / 2
    page.mouse.move(x, y)
    page.mouse.down()
    page.mouse.move(x - 20, y - 30, steps=5)
    page.mouse.up()
    expect(dialog.locator("[data-position]")).to_contain_text("peak / 2 = 10")
    first = dialog.locator('[data-index="0"]').bounding_box()
    page.mouse.click(first["x"] - 12, (first["y"] + y) / 2)
    expect(dialog.locator("[data-vertex] option")).to_have_count(4)
    dialog.locator("[data-axis]").select_option(index=1)
    dialog.locator("[data-assign]").click()
    expect(dialog.locator("[data-position]")).to_contain_text("peak = 20")
    dialog.locator("[data-apply]").click()
    expect(dialog).not_to_be_visible()
    ready()
    expect(field("Y 3")).to_have_value("peak / 2")
    expect(field("Y 4")).to_have_value("peak")
    field("peak").fill("28")
    ready()
    page.get_by_role("button", name="Edit sketch on plane").click()
    expect(dialog.locator("[data-position]")).to_contain_text("peak = 28")
    dialog.locator("[data-remove]").click()
    dialog.locator("[data-cancel]").click()
    expect(field("Y 4")).to_have_value("peak")
    print("PASS sketch insertion, snapped drag, driven coordinate, parameter rebuild and cancel")
    page.set_viewport_size({"width": 390, "height": 844})
    tab("3D CAD")
    ready()
    box = loc("#solid-canvas").bounding_box()
    assert box and box["width"] > 200 and box["width"] <= 390
    assert page.evaluate("document.documentElement.scrollWidth <= window.innerWidth")
    loc("#solid-canvas").focus()
    page.keyboard.press("ArrowRight")
    page.keyboard.press("+")
    assert not errors, errors
    print("PASS narrow workspace and keyboard viewport")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", help="Path to a Chromium executable")
    parser.add_argument("--server", help="kerfkite-studio to start (default: KERFKITE_STUDIO or build/native)")
    parser.add_argument("--url", help="A Studio server that is already running, such as http://127.0.0.1:8765/")
    parser.add_argument("--step", action="store_true", help="The server at --url has STEP exchange")
    args = parser.parse_args()
    with contextlib.ExitStack() as stack:
        server = None
        if args.url:
            url, step = args.url, args.step
        else:
            path = program("kerfkite-studio", args.server, "KERFKITE_STUDIO")
            step = beside(path, "kerfkite-step")
            server = stack.enter_context(studio(path, args=("--monitor-port", "0")))
            url = server.url
        lessons(args, url, step, server)


def lessons(args, url, step, server):
    """The lessons in one browser, against the server at url."""
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(executable_path=args.browser)
        try:
            page = browser.new_page(viewport={"width": 1440, "height": 980}, accept_downloads=True)
            page.on("dialog", lambda dialog: dialog.accept())
            page.goto(url)
            run(page)
            from cad_script_workflow import run as cad_script
            from rotary_workflow import run as rotary
            from depth_slice_workflow import run as depth_slices
            from cad_macro_workflow import run as cad_macros
            from editor_performance_workflow import run as editor_performance
            from cad_views_workflow import run as cad_views
            from desktop_workflow import run as desktop
            from router_entry_workflow import run as router_entries
            from svg_import_workflow import run as svg_import
            from placement_workflow import run as placement
            from production_label_workflow import run as production_labels
            from drilling_workflow import run as drilling
            from laser_pattern_workflow import run as laser_patterns
            from corner_workflow import run as corner
            from relief_link_workflow import run as linked_relief
            from image_relief_workflow import run as image_relief
            from sketch_join_workflow import run as sketch_join
            from machining_lessons_workflow import run as machining_lessons
            from loft_workflow import run as loft
            from cad_link_workflow import run as linked_cad
            from materials_workflow import run as materials
            from lightburn_workflow import run as lightburn
            from import_fixture_workflow import run as fixture_imports
            from assembly_laser_workflow import run as assembly_laser
            from simulate_exchange_workspace import run as exchange
            from probe_leveling_workflow import run as probe_leveling
            from pcb_workflow import run as circuit_board
            from tool_library_workflow import run as tool_library
            from joinery_workflow import run as joinery
            from setup_tools_workflow import run as setup_tools
            from tiling_workflow import run as tiling
            from relief_roughing_workflow import run as relief_roughing
            from pen_workflow import run as pen_drawing

            # Each lesson can modify the example or viewport.
            for lesson in (
                cad_views,
                editor_performance,
                cad_script,
                cad_macros,
                depth_slices,
                rotary,
                desktop,
                lightburn,
                fixture_imports,
                assembly_laser,
                *((exchange,) if step else ()),
                materials,
                corner,
                linked_cad,
                *((loft,) if step else ()),
                linked_relief,
                image_relief,
                sketch_join,
                production_labels,
                laser_patterns,
                drilling,
                placement,
                router_entries,
                svg_import,
                machining_lessons,
                probe_leveling,
                circuit_board,
                tool_library,
                joinery,
                setup_tools,
                tiling,
                relief_roughing,
                pen_drawing,
            ):
                with browser.new_context(viewport={"width": 1440, "height": 980}, accept_downloads=True) as context:
                    workshop = context.new_page()
                    workshop.on("dialog", lambda dialog: dialog.accept())
                    workshop.goto(page.url)
                    lesson(workshop)
            from phone_arm_workflow import run as check_phone_arm

            if server is None:
                return
            with browser.new_context(viewport={"width": 1440, "height": 980}) as context:
                workstation = context.new_page()
                workstation.on("dialog", lambda dialog: dialog.accept())
                workstation.goto(page.url)
                check_phone_arm(workstation, server)
        finally:
            browser.close()


if __name__ == "__main__":
    main()
