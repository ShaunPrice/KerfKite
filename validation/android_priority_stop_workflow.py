"""Exercise one signed Pocket, a real browser workstation and the simulated machine together.

The workstation server is kerfkite-studio (--server, KERFKITE_STUDIO or build/native) with its plain HTTP phone
monitor on the loopback interface. The browser editor and the emulator each reach it through a proxy that records
what they send and holds a request where a check needs it held.
"""

import argparse
import json
from pathlib import Path
import re
from threading import Event
from xml.etree import ElementTree
import time

from playwright.sync_api import sync_playwright

from android_pendant_workflow import MONITOR, Phone, wait_until
from native_servers import Proxy, studio
from phone_arm_workflow import arm_preview, connect_editor
from pocket_app import PACKAGE, component


def touch(phone, point):
    assert point is not None
    phone.run("shell", "input", "tap", *map(str, point))


def button_location(phone, label, dialog=False):
    # A disabled idle Stop occupies its future hit target even before a jog is sent.
    phone.run("shell", "uiautomator", "dump", phone.hierarchy)
    layout = ElementTree.fromstring(phone.run("shell", "cat", phone.hierarchy))
    for node in layout.iter("node"):
        if node.get("class") != "android.widget.Button" or node.get("text", "").casefold() != label.casefold():
            continue
        if dialog != (node.get("resource-id") == "android:id/button1"):
            continue
        left, top, right, bottom = map(int, re.findall(r"\d+", node.get("bounds")))
        return (left + right) // 2, (top + bottom) // 2
    return None


def run(phone, apk, chrome, program=None):
    assert phone.run("shell", "getprop", "ro.kernel.qemu") == "1"
    phone.run("install", "-r", str(apk))
    delay_confirm, saw_confirm, release_confirm = Event(), Event(), Event()
    delay_jog, saw_jog, release_jog = Event(), Event(), Event()
    legacy, stop_done = Event(), Event()
    responses = []
    requests = []
    confirmations = []
    action_errors = []

    def route(seen):
        return seen.path.split("?")[0]

    def delay_workstation_reply(seen):
        if delay_confirm.is_set() and route(seen) == "/api/arm/confirm":
            saw_confirm.set()
            release_confirm.wait(6)

    def observe_workstation(seen, status, body):
        if route(seen) == "/api/arm/confirm":
            confirmations.append(status)

    def observe_motion(seen):
        if seen.method == "POST":
            data = seen.json()
            requests.append(data)
            if data["action"] == "jog" and delay_jog.is_set():
                saw_jog.set()
                release_jog.wait(6)

    def observe_result(seen, status, body):
        if seen.method == "POST":
            action = seen.json()["action"]
            responses.append((action, status))
            if status != 200:
                action_errors.append(json.loads(body).get("error", ""))
            if action == "stop" and status == 200:
                stop_done.set()
        if legacy.is_set() and route(seen) == "/api/status":
            # A receiver from before control generations: the same status without control_epoch.
            state = json.loads(body)
            state.pop("control_epoch", None)
            return json.dumps(state).encode()
        return None

    try:
        with (
            studio(program, args=MONITOR) as server,
            Proxy(server.url, before=delay_workstation_reply, after=observe_workstation) as desk,
            Proxy(server.monitor, before=observe_motion, after=observe_result) as monitor,
            sync_playwright() as pw,
        ):
            status = server.status

            def armed():
                return server.monitor_status()["armed"]

            browser = pw.chromium.launch(executable_path=chrome)
            page = browser.new_page()
            page.goto(f"http://127.0.0.1:{desk.port}/")
            connect_editor(page)
            phone.run("shell", "am", "force-stop", PACKAGE)
            phone.run("shell", "am", "start", "-W", "-n", component())
            phone.tap("CNC pendant", button=True)
            phone.tap("Monitor pairing link")
            phone.run("shell", "input", "text", f"http://10.0.2.2:{monitor.port}/#token={server.token}")
            phone.run("shell", "input", "keyevent", "KEYCODE_BACK")
            phone.tap("Pair", button=True)
            phone.confirm()
            wait_until(lambda: phone.find("Simulator · Idle · grbl"), "Pairing did not complete.")
            arm_preview(page)
            phone.tap("Start", button=True)
            phone.confirm()
            wait_until(lambda: responses == [("start", 200)], "Workstation-confirmed Start failed.")
            # The proxy records the workstation's confirmation once its answer is back, which can be just after the
            # phone's answer.
            wait_until(lambda: confirmations == [200], "The workstation's confirmation was not recorded.")
            assert not armed()
            wait_until(lambda: not status()["active"], "Simulated job did not finish.")
            print(
                "PASS signed Pocket Start with browser-generated preview and live workstation confirmation", flush=True
            )

            wait_until(
                lambda: phone.find("Manual controls enabled · No job armed"), "First native job status did not settle."
            )
            arm_preview(page)
            # Locate real native buttons before measuring the short confirmation interval:
            # UIAutomator's hierarchy dump waits for an idle display.
            stop = wait_until(lambda: phone.find("Stop", button=True), "Missing armed Stop.")
            touch(phone, stop)
            positive = wait_until(lambda: button_location(phone, "Stop", dialog=True), "Missing Stop confirmation.")
            phone.run("shell", "input", "keyevent", "KEYCODE_BACK")
            phone.tap("Start", button=True)
            start_confirm = wait_until(
                lambda: button_location(phone, "Start", dialog=True), "Missing Start confirmation."
            )
            delay_confirm.set()
            touch(phone, start_confirm)
            assert saw_confirm.wait(5), (
                "Missing pending Start",
                [r["action"] for r in requests],
                responses,
                action_errors,
            )
            touch(phone, stop)
            time.sleep(0.20)  # Native dialog opening animation.
            touch(phone, positive)
            assert stop_done.wait(1.3), "Same phone could not stop its own pending Start."
            release_confirm.set()
            wait_until(
                lambda: ("start", 400) in responses and 400 in confirmations,
                "Overtaken Start/confirmation was accepted.",
            )
            assert not armed() and not status()["active"]
            wait_until(lambda: phone.find("Receiver acknowledged stop."), "Late Start replaced the Stop status.")
            print(
                "PASS same phone cancels pending Start, late browser confirmation rejected, fresh Stop result retained",
                flush=True,
            )

            # Hold a normal jog request before the server operation lock, then deliver
            # the ordinary POST after Stop has rotated the receiver's control generation.
            delay_jog.set()
            stop_done.clear()
            jog = wait_until(lambda: phone.find("Jog X positive", button=True), "Idle jog unavailable.")
            stop = button_location(phone, "Stop")
            touch(phone, jog)
            assert saw_jog.wait(1.2)
            touch(phone, stop)
            time.sleep(0.20)
            touch(phone, positive)
            assert stop_done.wait(1.3), "Stop was blocked by the pending jog."
            release_jog.set()
            wait_until(lambda: ("jog", 400) in responses, "Delayed jog passed the Stop boundary.")
            assert not any("$J=" in entry for entry in status()["log"])
            wait_until(lambda: phone.find("Receiver acknowledged stop."), "Late jog replaced the Stop status.")
            print("PASS same phone preempts delayed jog and receiver rejects old motion", flush=True)

            delay_confirm.clear()
            legacy.set()
            arm_preview(page)
            wait_until(
                lambda: phone.find("Update the Studio receiver for motion controls · Job armed"),
                "Receiver without control generation did not prompt for update.",
            )
            assert phone.find("Start", button=True) is None and phone.find("Jog X positive", button=True) is None
            phone.tap("Stop", button=True)
            phone.confirm()
            wait_until(lambda: not armed(), "Stop failed on legacy status.")
            assert [r["action"] for r in requests] == ["start", "start", "stop", "jog", "stop", "stop"]
            assert len({r["request_id"] for r in requests}) == len(requests)
            assert all(("control_epoch" in r) == (r["action"] != "stop") for r in requests)
            phone.tap("Forget pairing")
            print("PASS older receiver motion controls disabled, Stop preserved, no automatic retries", flush=True)
            browser.close()
    finally:
        release_confirm.set()
        release_jog.set()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--chrome", help="Installed Chrome executable; defaults to Playwright's Chromium")
    parser.add_argument("--server", help="kerfkite-studio (default: KERFKITE_STUDIO, then build/native)")
    args = parser.parse_args()
    run(Phone(args.adb, args.serial), args.apk.resolve(), args.chrome, args.server)
