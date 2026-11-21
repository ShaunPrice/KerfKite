"""Exercise the native Pocket library and Android document picker on a disposable emulator."""

import argparse
import json
from pathlib import Path
from xml.etree import ElementTree
import tempfile
import uuid

from android_pendant_workflow import Phone, wait_until

ACTIVITY = "au.cncstudio.pocket/.WorkshopActivity"


def select_folder(phone, folder):
    phone.run("shell", "uiautomator", "dump", phone.hierarchy)
    ui = ElementTree.fromstring(phone.run("shell", "cat", phone.hierarchy))
    toolbar = next((n for n in ui.iter("node") if n.get("resource-id", "").endswith(":id/toolbar")), None)
    if toolbar is not None and any(n.get("text") == folder for n in toolbar.iter("node")):
        return
    phone.tap("Show roots")
    # Browse shared storage directly. The Downloads collection may wait for a media scan.
    phone.tap(phone.run("shell", "getprop", "ro.product.model"))
    phone.tap("Download")
    phone.tap(folder)


def open_file(phone, folder, filename):
    phone.tap("Open file")
    select_folder(phone, folder)
    phone.tap(filename)


def save_file(phone, folder, format_button):
    phone.tap(format_button)
    select_folder(phone, folder)
    phone.confirm()
    wait_until(lambda: phone.find("Design exported."), "The document provider did not finish saving.")


def replace_text(phone, field_label, value):
    field = wait_until(lambda: phone.find(field_label), "Missing field: " + field_label)
    phone.run("shell", "input", "tap", *(str(v) for v in field))
    phone.run("shell", "input", "keycombination", "KEYCODE_CTRL_LEFT", "KEYCODE_A")
    phone.run("shell", "input", "text", value)


def exercise(phone, apk, previous=None):
    assert phone.run("shell", "getprop", "ro.kernel.qemu") == "1"
    assert phone.run("shell", "getprop", "sys.boot_completed") == "1"
    if previous:
        phone.run("install", "-r", str(previous))
        phone.run("shell", "am", "start", "-W", "-f", "0x10008000", "-n", ACTIVITY)
        phone.tap("Add shape")
        phone.confirm()
        assert phone.find("Rectangle · 40.0 × 25.0 mm")
    phone.run("install", "-r", str(apk))
    phone.run("shell", "am", "force-stop", "au.cncstudio.pocket")
    phone.run("shell", "am", "start", "-W", "-f", "0x10008000", "-n", ACTIVITY)
    if previous:
        assert phone.find("Pocket design")
        assert phone.find("Rectangle · 40.0 × 25.0 mm")
        print("PASS signed legacy draft migration", flush=True)

    folder = "Pocket-" + uuid.uuid4().hex[:10]
    remote = "/sdcard/Download/" + folder
    desktop_name = folder + "-reference"
    paths = [[[5, 9], [30, 11]], [[4, 3], [8, 3], [4, 9], [4, 3]]]
    document = {
        "format": "pi-cnc-studio/1",
        "metadata": {"name": desktop_name},
        "settings": {"width": 750, "height": 400, "mode": "laser", "power": 200, "paths": paths},
        "objects": [{"name": "Line and triangle", "paths": paths, "output": False}],
        "extras": {"custom": {"notes": "Keep this desktop setting exactly"}},
    }
    raw = json.dumps(document, ensure_ascii=False, indent=2) + "\n"
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp)
        phone.run("shell", "mkdir", remote)
        for name, content in [("desktop.json", raw), ("broken.json", "{broken")]:
            local = scratch / name
            local.write_text(content)
            phone.run("push", str(local), remote + "/" + name)
        open_file(phone, folder, "desktop.json")
        phone.confirm()
        assert phone.find(desktop_name)
        assert phone.find("750 × 400 mm   ·   Desktop reference · read only")
        assert phone.find("Edit", button=True) is None
        save_file(phone, folder, "Studio project")
        reference = scratch / "reference.json"
        phone.run("pull", remote + "/" + desktop_name + ".cncstudio.json", str(reference))
        assert reference.read_text() == raw
        print("PASS SAF desktop import and byte-preserving reference export", flush=True)

        phone.tap("My projects")
        phone.tap("Extract editable artwork")
        phone.confirm()
        artwork = desktop_name + " artwork"
        assert phone.find(artwork)
        phone.tap("Edit")
        replace_text(phone, "26.0", "52")
        phone.confirm()
        save_file(phone, folder, "Studio project")
        exported = scratch / "artwork.json"
        phone.run("pull", remote + "/" + artwork + ".cncstudio.json", str(exported))
        copy = json.loads(exported.read_text())
        scaled = [[[4 + (x - 4) * 2, y] for x, y in path] for path in paths]
        assert copy["objects"][0]["paths"] == scaled and copy["objects"][0]["output"] is False
        assert copy["settings"]["paths"] == [] and copy["metadata"]["pocketArtwork"] is True
        save_file(phone, folder, "SVG")
        svg = scratch / "drawing.svg"
        phone.run("pull", remote + "/" + artwork + ".svg", str(svg))
        assert svg.read_text().count("<path ") == 2 and svg.read_text().count('Z"/>') == 1
        print("PASS independent artwork extraction, resized path compatibility and open/closed SVG", flush=True)

        open_file(phone, folder, artwork + ".cncstudio.json")
        phone.confirm()
        assert phone.find(artwork + " (2)")
        open_file(phone, folder, "broken.json")
        assert phone.find("Project could not be opened")
        phone.confirm()
        assert phone.find(artwork + " (2)")
        print("PASS reopen Pocket export, duplicate name allocation and invalid import recovery", flush=True)

        phone.tap("My projects")
        phone.tap("New project")
        phone.confirm()
        phone.tap("Add shape")
        phone.confirm()
        phone.tap("My projects")
        phone.tap("Rename current")
        replace_text(phone, "Project name", folder)
        phone.confirm()
        assert phone.find(folder)
        phone.tap("My projects")
        phone.tap("Duplicate current")
        phone.confirm()
        assert phone.find(folder + " copy")
        phone.tap("My projects")
        phone.tap("Delete current")
        phone.confirm()
        phone.tap("My projects")
        phone.tap(folder, button=True)
        assert phone.find("Rectangle · 40.0 × 25.0 mm")
        phone.run("shell", "am", "force-stop", "au.cncstudio.pocket")
        phone.run("shell", "am", "start", "-W", "-f", "0x10008000", "-n", ACTIVITY)
        assert phone.find(folder) and phone.find("Rectangle · 40.0 × 25.0 mm")
        print("PASS create, rename, duplicate, delete, switch and active project persistence", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--apk", required=True, type=Path)
    parser.add_argument("--previous", type=Path, help="Previous signed Pocket APK, for upgrade on a fresh emulator")
    args = parser.parse_args()
    exercise(Phone(args.adb, args.serial), args.apk, args.previous)
