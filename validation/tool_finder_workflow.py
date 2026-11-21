"""Exercise tool discovery without executing the matched machine or drawing command."""

from pathlib import Path
from playwright.sync_api import expect


def run(editor):
    root = Path(__file__).resolve().parents[1]
    for offline in (False, True):
        with editor.context.browser.new_context(viewport={"width": 390, "height": 844}) as context:
            context.set_offline(offline)
            page = context.new_page()
            page.goto((root / "studio/static/index.html").as_uri() if offline else editor.url)
            page.evaluate("() => window.studioReady")
            locate = page.locator
            trigger = locate("#toolFinderOpen")
            query = locate("#toolFinderQuery")
            dialog = locate("#toolFinder")
            page.evaluate("""() => {
              window.unwantedToolActivations=[];
              for(const id of ['label-add','cornerStart','connect']){
                document.getElementById(id).addEventListener('click',()=>unwantedToolActivations.push(id));
              }
            }""")
            page.get_by_role("tab", name="Images", exact=True).click()
            trigger.click()
            expect(query).to_be_focused()
            box = dialog.bounding_box()
            assert box and box["x"] >= 0 and box["x"] + box["width"] <= 390
            assert page.evaluate("document.documentElement.scrollWidth <= 390")
            first = query.get_attribute("aria-activedescendant")
            query.press("ArrowDown")
            assert query.get_attribute("aria-activedescendant") != first
            query.press("ArrowUp")
            assert query.get_attribute("aria-activedescendant") == first
            query.fill("this-tool-doesnt-exist-74583")
            expect(locate("#toolFinderEmpty")).to_be_visible()
            query.press("Escape")
            expect(dialog).not_to_be_visible()
            expect(trigger).to_be_focused()

            page.keyboard.press("Control+k")
            query.fill("Insert labels")
            expect(locate("#toolFinderResults [aria-selected=true]")).to_contain_text("Production labels")
            page.keyboard.down("Enter")
            page.keyboard.down("Enter")  # a held key cannot fire the newly focused button
            page.keyboard.up("Enter")
            expect(locate("#label-add")).to_be_focused()
            expect(page.get_by_role("tab", name="Text", exact=True)).to_have_attribute("aria-selected", "true")
            assert page.evaluate("document.getElementById('seriesLabels').open")

            page.keyboard.press("Meta+k")
            query.fill("crossing lines join")
            locate("#toolFinderResults [aria-selected=true]").click()
            expect(locate("#cornerStart")).to_be_focused()
            expect(locate("#cornerControls")).to_be_hidden()
            if not offline:
                page.keyboard.press("Control+k")
                query.fill("Connect")
                query.press("Enter")
                expect(locate("#connect")).to_be_focused()
            assert page.evaluate("unwantedToolActivations") == []
            assert page.evaluate("CNCProjects.document().objects.length") == 0
            print(
                "PASS 390 px focus-only tool navigation, modal keyboard controls, contextual search and held Enter in",
                "offline editor" if offline else "server editor",
            )
