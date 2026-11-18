"""Exercise an installed browser-only editor with Debian Chromium and Selenium."""
import argparse
import json
import os
from pathlib import Path
import tempfile

from selenium import webdriver
from selenium.webdriver.chrome.service import Service
from selenium.webdriver.common.by import By
from selenium.webdriver.support import expected_conditions as EC
from selenium.webdriver.support.ui import Select, WebDriverWait


def check(index, downloads):
    opts = webdriver.ChromeOptions()
    opts.add_argument("--headless")
    opts.add_argument("--disable-dev-shm-usage")
    opts.add_argument("--window-size=1440,1000")
    if os.getuid() == 0:  # Isolated CI container.
        opts.add_argument("--no-sandbox")
    opts.add_experimental_option("prefs", {"download.default_directory": str(downloads)})
    with webdriver.Chrome(options=opts, service=Service("/usr/bin/chromedriver")) as browser:
        browser.get(index.resolve().as_uri())
        wait = WebDriverWait(browser, 15)
        element = lambda name: browser.find_element(By.ID, name)
        wait.until(lambda _: "LOCAL FILE" in element("state").get_attribute("textContent"))
        element("rect").click()
        element("design-tab-projects").click()
        element("saveProject").click()
        project = wait.until(lambda _: next(downloads.glob("*.json"), None))
        data = json.loads(project.read_text())
        assert data["format"] == "pi-cnc-studio/1" and len(data["objects"]) == 1
        browser.refresh()
        element("design-tab-projects").click()
        element("loadProject").click()
        element("file").send_keys(str(project))
        prompt = wait.until(EC.alert_is_present())
        assert "Replace the current design" in prompt.text
        prompt.accept()
        wait.until(lambda _: element("drawing").find_elements(By.TAG_NAME, "path"))
        for mode in ("router", "laser", "knife"):
            Select(element("mode")).select_by_value(mode)
            element("preview").click()
            wait.until(lambda _: "G21" in element("gcode").get_attribute("textContent"))
            code = element("gcode").get_attribute("textContent").strip()
            assert "G1 X" in code and ("M3 S" in code) == (mode == "router")
            assert ("M4 S" in code) == (mode == "laser")
            element("export").click()
            exported = wait.until(lambda _: next(downloads.glob("*.nc"), None))
            assert exported.read_text().strip() == code
            exported.unlink()
        print("Offline project round trip and router, laser and knife exports passed:", index.name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("index", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        check(args.index, Path(tmp))
