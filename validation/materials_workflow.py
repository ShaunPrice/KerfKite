"""Exercise the materials guide through the CAM tab and the offline editor."""

import json
from pathlib import Path
from secrets import token_urlsafe
from urllib.parse import urlsplit

from playwright.sync_api import expect


def launch_guide(editor, mode, tool_id):
    editor.get_by_role("tab", name="CAM", exact=True).click()
    editor.locator("#mode").select_option(mode)
    before = [editor.locator(selector).input_value() for selector in ("#mode", "#operation", "#feed", "#power")]
    with editor.context.expect_page() as opened:
        editor.locator("#openMaterialsGuide").click()
    guide = opened.value
    expect(guide.locator("#guide-tool")).to_have_value(tool_id)
    assert urlsplit(guide.url).path.endswith("/materials-guide.html")
    after = [editor.locator(selector).input_value() for selector in ("#mode", "#operation", "#feed", "#power")]
    assert after == before
    return guide


def check_router_links(editor):
    editor.get_by_role("tab", name="CAM", exact=True).click()
    editor.locator("#mode").select_option("router")
    editor.locator("#manualToolShape").select_option("flat")
    for cam_op, tool, guide_op in (
        ("line", "flat", "engrave"),
        ("outside", "flat", "profile"),
        ("pocket", "flat", "pocket"),
        ("vcarve", "v", "engrave"),
    ):
        editor.locator("#operation").select_option(cam_op)
        with editor.context.expect_page() as opened:
            editor.locator("#openMaterialsGuide").click()
        guide = opened.value
        expect(guide.locator("#guide-tool")).to_have_value(tool)
        expect(guide.locator("#guide-operation")).to_have_value(guide_op)
        guide.close()
    editor.get_by_role("tab", name="3D relief", exact=True).click()
    for tool in ("flat", "ball"):
        editor.locator("#reliefTool").select_option(tool)
        with editor.context.expect_page() as opened:
            editor.locator("#reliefMaterialsGuide").click()
        guide = opened.value
        expect(guide.locator("#guide-tool")).to_have_value(tool)
        expect(guide.locator("#guide-operation")).to_have_value("relief")
        guide.close()
    print("PASS router engraving, profiles, pockets, V-carve and flat/ball relief guide links")


def check_notebook_backup(page, original_note):
    loc = page.locator
    entries = loc("#sample-entries article")
    with page.expect_download() as download:
        loc("#sample-export").click()
    backup = Path(download.value.path()).read_bytes()
    data = json.loads(backup)
    assert data["format"] == "kerfkite-material-samples/1"
    (sample,) = data["samples"]
    assert (sample["material"], sample["tool"], sample["operation"], sample["notes"]) == (
        "pvc-vinyl",
        "knife",
        "kiss",
        original_note,
    )
    file = {"name": download.value.suggested_filename, "mimeType": "application/json", "buffer": backup}

    entries.get_by_role("button", name="Edit sample").click()
    updated = "Rechecked under magnification; liner intact."
    loc("#sample-notes").fill(updated)
    loc("#sample-result").select_option("pass")
    loc("#sample-save").click()
    expect(entries).to_have_count(1)
    expect(entries).to_contain_text(updated)

    loc("#sample-import").set_input_files(file)
    expect(loc("#sample-status")).to_contain_text("Imported 0")
    expect(entries).to_have_count(1)
    expect(entries).to_contain_text(updated)  # An old backup must not roll back the edit.

    page.once("dialog", lambda prompt: prompt.dismiss())
    entries.get_by_role("button", name="Delete sample").click()
    expect(entries).to_have_count(1)
    page.once("dialog", lambda prompt: prompt.accept())
    entries.get_by_role("button", name="Delete sample").click()
    expect(entries).to_have_count(0)  # The row must disappear without a reload.
    expect(loc("#sample-status")).to_contain_text("deleted")

    loc("#sample-import").set_input_files(file)
    expect(loc("#sample-status")).to_contain_text("Imported 1")
    expect(entries).to_have_count(1)
    expect(entries).to_contain_text(original_note)
    expect(entries).to_contain_text("Sample sheet A")
    expect(entries).to_contain_text("0.15 mm")
    expect(entries).to_contain_text("Needs adjustment")


def exercise_guide(page):
    loc = page.locator
    loc("#guide-search").fill("PVC")
    loc("#guide-material").select_option("pvc-vinyl")
    expect(loc("#guide-verdict")).to_have_attribute("data-status", "avoid")
    expect(loc("#guide-schematic")).to_be_hidden()
    loc("#guide-tool").select_option("knife")
    loc("#guide-operation").select_option("kiss")
    expect(loc("#guide-verdict")).to_have_attribute("data-status", "candidate")
    expect(loc("#guide-schematic")).to_be_visible()
    expect(loc("#schematic-caption")).to_contain_text("liner")
    group = loc("#detail-family").inner_text()
    loc("#guide-group").select_option(group)
    expect(loc("#guide-material")).to_have_value("pvc-vinyl")

    loc("#guide-search").fill("no-such-coupon-stock")
    expect(loc("#guide-empty")).to_be_visible()
    loc("#guide-reset").click()
    expect(loc("#guide-empty")).to_be_hidden()
    expect(loc("#guide-search")).to_have_value("")
    loc("#guide-tool").select_option("diode")
    loc("#guide-operation").select_option("engrave")
    loc("#guide-material").select_option("anodised-aluminium")
    expect(loc("#guide-verdict")).to_have_attribute("data-status", "conditional")
    loc("#guide-compare").select_option("clear-acrylic")
    expect(loc("#guide-comparison")).to_be_visible()
    expect(loc("#guide-comparison")).to_have_attribute("data-status", "avoid")
    loc("#guide-operation").select_option("cut")
    expect(loc("#guide-verdict")).to_have_attribute("data-status", "avoid")
    expect(loc("#guide-advice")).not_to_be_empty()
    assert loc("#guide-sources a[href^='https://']").count() > 0
    save = loc("#sample-save")
    assert not save.is_visible() or save.is_disabled()
    loc("#guide-status").select_option("conditional")
    assert "anodised-aluminium" not in loc("#guide-material option").evaluate_all("(items) => items.map(i => i.value)")
    loc("#guide-reset").click()

    loc("#guide-tool").select_option("knife")
    loc("#guide-operation").select_option("kiss")
    loc("#guide-material").select_option("pvc-vinyl")
    loc("#sample-stock").fill("Sample sheet A")
    loc("#sample-thickness").fill("0.15")
    loc("#sample-result").select_option("adjust")
    note = "Synthetic coupon observation: slight edge lift on the smallest letter."
    loc("#sample-notes").fill(note)
    loc("#sample-save").click()
    expect(loc("#sample-entries")).to_contain_text(note)
    page.reload()
    # Restore the selection explicitly for browsers that restrict history updates on file URLs.
    loc("#guide-tool").select_option("knife")
    loc("#guide-operation").select_option("kiss")
    loc("#guide-material").select_option("pvc-vinyl")
    expect(loc("#sample-entries")).to_contain_text(note)
    check_notebook_backup(page, note)
    loc("#guide-material").select_option("cardstock")
    expect(loc("#sample-entries")).not_to_contain_text(note)
    loc("#guide-operation").select_option("through")
    expect(loc("#guide-verdict")).to_have_attribute("data-status", "candidate")
    page.set_viewport_size({"width": 390, "height": 844})
    expect(loc("#guide-title")).to_be_visible()
    assert page.evaluate("document.documentElement.scrollWidth <= innerWidth")


def hosted_notebooks(browser):
    from native_servers import web

    accounts = {name: token_urlsafe(24) for name in ("sample-maker", "sample-reader")}
    with web(users=accounts.items()) as server:
        url = server.origin
        with browser.new_context(ignore_https_errors=True) as context:
            editor = context.new_page()
            for index, name in enumerate(("sample-maker", "sample-reader", "sample-maker")):
                context.clear_cookies()  # Simulate an expired login, retaining the same browser database.
                editor.goto(url)
                editor.get_by_label("Username").fill(name)
                editor.get_by_label("Password").fill(accounts[name])
                editor.get_by_role("button", name="Log in").click()
                guide = launch_guide(editor, "knife", "knife")
                loc = guide.locator
                loc("#guide-operation").select_option("kiss")
                loc("#guide-material").select_option("pvc-vinyl")
                notes = ["Bench A, test sheet passed.", "Bench B, adjust blade."]
                if index < 2:
                    loc("#sample-stock").fill("Sign film offcut")
                    loc("#sample-result").select_option("pass")
                    loc("#sample-notes").fill(notes[index])
                    loc("#sample-save").click()
                    expect(loc("#sample-entries")).to_contain_text(notes[index])
                expect(loc("#sample-entries")).to_contain_text(notes[index % 2])
                expect(loc("#sample-entries")).not_to_contain_text(notes[1 - index % 2])
                guide.close()
            print("PASS separate hosted HTTPS sample notebooks across account changes in one browser")


def run(editor):
    root = Path(__file__).resolve().parents[1]
    for offline in (False, True):
        with editor.context.browser.new_context(viewport={"width": 1360, "height": 900}) as context:
            start = context.new_page()
            if offline:
                context.set_offline(True)
            start.goto((root / "studio/static/index.html").as_uri() if offline else editor.url)
            errors = []
            requests = []

            def observe(page):
                page.on("pageerror", lambda exc: errors.append(str(exc)))
                page.on("request", lambda req: requests.append((req.method, req.url)))

            context.on("page", observe)
            guide = launch_guide(start, "laser", "diode")
            exercise_guide(guide)
            if not offline:
                check_router_links(start)
            assert not errors, errors
            for method, url in requests:
                dest = urlsplit(url)
                assert method == "GET", (method, url)
                if offline:
                    assert dest.scheme in {"file", "data", "blob"}, url
                elif dest.path.startswith("/api/"):
                    assert dest.path == "/api/session", url
            print(
                "PASS materials comparison, tool/operation restrictions and sample persistence in",
                "offline editor" if offline else "simulator editor",
            )
    hosted_notebooks(editor.context.browser)
