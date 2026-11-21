"""Verify Pocket's opt-in window flag, real inactivity and signed upgrade on a disposable emulator."""

import argparse
from pathlib import Path
import re
import time
import uuid
from xml.etree import ElementTree

from android_pendant_workflow import Phone, wait_until
from android_projects_workflow import replace_text
from pocket_app import PACKAGE, component

EDITOR = "WorkshopActivity"
PENDANT = "PendantActivity"
LABEL = "Keep screen awake"


def flags(phone, activity):
    windows = phone.run("shell", "dumpsys", "window", "windows")
    blocks = re.split(r"(?m)^\s*Window #\d+ ", windows)[1:]
    for block in blocks:
        if component(activity) in block.splitlines()[0]:
            found = re.search(r"(?m)^\s*fl=(.*)$", block)
            if found:
                return set(found.group(1).split())
    return None


def check_window(phone, activity, awake, secure=False):
    def expected():
        window = flags(phone, activity)
        return window is not None and ("KEEP_SCREEN_ON" in window) == awake and ("SECURE" in window) == secure

    wait_until(expected, f"{activity} keep-awake={awake}, secure={secure} did not match.")


def check_switch(phone, checked):
    phone.run("shell", "uiautomator", "dump", phone.hierarchy)
    root = ElementTree.fromstring(phone.run("shell", "cat", phone.hierarchy))
    control = next((n for n in root.iter("node") if n.get("text") == LABEL), None)
    assert control is not None and control.get("checkable") == "true"
    assert control.get("checked") == ("true" if checked else "false")


def launch(phone):
    phone.run("shell", "input", "keyevent", "KEYCODE_WAKEUP")
    phone.run("shell", "wm", "dismiss-keyguard")
    phone.run("shell", "am", "start", "-W", "-n", component(EDITOR))


def awake(phone):
    match = re.search(r"(?m)^\s*mWakefulness=(\w+)", phone.run("shell", "dumpsys", "power"))
    assert match, "Android did not report power-manager wakefulness."
    return match[1] == "Awake"


def test_timeout(phone):
    keys = [("global", "stay_on_while_plugged_in"), ("system", "screen_off_timeout")]
    original = [phone.run("shell", "settings", "get", group, key) for group, key in keys]
    try:
        # This setup is confined to the disposable emulator. The APK only changes its window.
        phone.run("shell", "settings", "put", "global", "stay_on_while_plugged_in", "0")
        phone.run("shell", "settings", "put", "system", "screen_off_timeout", "10000")
        power = phone.run("shell", "dumpsys", "power")
        minimum = re.search(r"mMinimumScreenOffTimeoutConfig=(\d+)", power)
        duration = max(10, int(minimum[1]) / 1000 if minimum else 10)
        assert duration <= 30, "Use an emulator with a short minimum display timeout."
        time.sleep(duration + 4)
        assert awake(phone), "The foreground opted-in editor fell asleep."
        phone.tap(LABEL)
        check_window(phone, EDITOR, False)
        wait_until(
            lambda: not awake(phone), "The normal display timeout failed after switching off.", timeout=duration + 12
        )
        print("PASS real emulator timeout with Keep screen awake on and off", flush=True)
    finally:
        for (group, key), value in zip(keys, original):
            if value == "null":
                phone.run("shell", "settings", "delete", group, key)
            else:
                phone.run("shell", "settings", "put", group, key, value)
        launch(phone)


def source_version():
    """Version name and code of the app in this checkout, so the check follows each release."""
    gradle = (Path(__file__).resolve().parents[1] / "android/app/build.gradle").read_text()
    return re.search(r"versionName '([^']+)'", gradle)[1], re.search(r"versionCode (\d+)", gradle)[1]


def installed_version(phone):
    return re.search(r"versionName=(\S+)", phone.run("shell", "dumpsys", "package", PACKAGE))[1]


def exercise(phone, apk, previous):
    assert phone.run("shell", "getprop", "ro.kernel.qemu") == "1"
    assert phone.run("shell", "getprop", "sys.boot_completed") == "1"
    if previous:
        phone.run("install", "-r", str(previous))
        earlier = installed_version(phone)
        launch(phone)
        project = "AwakeCheck-" + uuid.uuid4().hex[:8]
        phone.tap("My projects")
        phone.tap("New project")
        replace_text(phone, "Project name", project)
        phone.confirm()
        phone.tap("Add shape")
        phone.confirm()
        assert phone.find("Rectangle · 40.0 × 25.0 mm")
    phone.run("install", "-r", str(apk))
    pkg = phone.run("shell", "dumpsys", "package", PACKAGE)
    name, code = source_version()
    assert f"versionName={name}" in pkg and f"versionCode={code} " in pkg, (
        "The APK does not match android/app/build.gradle."
    )
    assert "android.permission.WAKE_LOCK" not in pkg
    launch(phone)
    if previous:
        assert phone.find(project) and phone.find("Rectangle · 40.0 × 25.0 mm")
        assert earlier != name, "Supply an earlier release as --previous."
        print(f"PASS signed {earlier} → {name} upgrade preserves the drawing", flush=True)
    check_window(phone, EDITOR, False)
    check_switch(phone, False)
    phone.tap(LABEL)
    check_window(phone, EDITOR, True)
    phone.tap("CNC pendant")
    check_window(phone, PENDANT, True, secure=True)
    check_switch(phone, True)
    phone.tap(LABEL)
    check_window(phone, PENDANT, False, secure=True)
    phone.run("shell", "input", "keyevent", "KEYCODE_BACK")
    check_window(phone, EDITOR, False)
    check_switch(phone, False)
    print("PASS shared switch, back navigation and secure pairing window", flush=True)
    phone.tap(LABEL)
    check_window(phone, EDITOR, True)
    phone.run("shell", "input", "keyevent", "KEYCODE_HOME")
    # Android can retain the paused window or remove it altogether.
    wait_until(
        lambda: "KEEP_SCREEN_ON" not in (flags(phone, EDITOR) or set()),
        "The background editor retained its keep-awake flag.",
    )
    phone.run("shell", "am", "force-stop", PACKAGE)
    launch(phone)
    check_window(phone, EDITOR, True)
    check_switch(phone, True)
    print("PASS background release and persistence after process restart", flush=True)
    test_timeout(phone)
    check_switch(phone, False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--apk", required=True, type=Path)
    parser.add_argument("--previous", type=Path, help="Signed Pocket 0.4.0 on a fresh emulator")
    args = parser.parse_args()
    exercise(Phone(args.adb, args.serial), args.apk.resolve(), args.previous)
