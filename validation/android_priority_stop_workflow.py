"""Exercise one signed Pocket, a real browser workstation and the simulated machine together."""

import argparse
from contextlib import contextmanager
import logging
from pathlib import Path
import secrets
import re
import sys
from threading import Event, Thread
from xml.etree import ElementTree
import time

from flask import request
from playwright.sync_api import sync_playwright
from werkzeug.serving import make_server

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from pocket_app import PACKAGE, component
from android_pendant_workflow import Phone, wait_until
from phone_arm_workflow import arm_preview, connect_editor
from studio.device import DeviceController
from studio.monitor import create_monitor_app
from studio.phone_jobs import PhoneJobs
from studio.server import create_app


@contextmanager
def serve(app):
    server = make_server("127.0.0.1", 0, app, threaded=True)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_port
    finally:
        server.shutdown()
        thread.join()
        server.server_close()


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


def run(phone, apk, chrome):
    assert phone.run("shell", "getprop", "ro.kernel.qemu") == "1"
    phone.run("install", "-r", str(apk))
    controller = DeviceController()
    jobs = PhoneJobs(controller)
    token = secrets.token_urlsafe(32)
    desktop = create_app(controller, phone_jobs=jobs)
    monitor = create_monitor_app(controller, token, jobs)
    delay_confirm, saw_confirm, release_confirm = Event(), Event(), Event()
    delay_jog, saw_jog, release_jog = Event(), Event(), Event()
    legacy, stop_done = Event(), Event()
    responses = []
    requests = []
    confirmations = []
    action_errors = []

    @desktop.before_request
    def delay_workstation_reply():
        if delay_confirm.is_set() and request.path == "/api/arm/confirm":
            saw_confirm.set()
            release_confirm.wait(6)

    @desktop.after_request
    def observe_workstation(response):
        if request.path == "/api/arm/confirm":
            confirmations.append(response.status_code)
        return response

    @monitor.before_request
    def observe_motion():
        if request.method == "POST":
            data = request.get_json()
            requests.append(data.copy())
            if data["action"] == "jog" and delay_jog.is_set():
                saw_jog.set()
                release_jog.wait(6)

    @monitor.after_request
    def observe_result(response):
        if request.method == "POST":
            action = request.get_json()["action"]
            responses.append((action, response.status_code))
            if response.status_code != 200:
                action_errors.append(response.get_json().get("error", ""))
            if action == "stop" and response.status_code == 200:
                stop_done.set()
        if legacy.is_set() and request.path == "/api/status":
            status = response.get_json()
            status.pop("control_epoch", None)
            response.set_data(monitor.json.dumps(status))
        return response

    try:
        with serve(desktop) as desk_port, serve(monitor) as phone_port, sync_playwright() as pw:
            browser = pw.chromium.launch(executable_path=chrome)
            page = browser.new_page()
            page.goto(f"http://127.0.0.1:{desk_port}/")
            connect_editor(page)
            phone.run("shell", "am", "force-stop", PACKAGE)
            phone.run("shell", "am", "start", "-W", "-n", component())
            phone.tap("CNC pendant", button=True)
            phone.tap("Monitor pairing link")
            phone.run("shell", "input", "text", f"http://10.0.2.2:{phone_port}/#token={token}")
            phone.run("shell", "input", "keyevent", "KEYCODE_BACK")
            phone.tap("Pair", button=True)
            phone.confirm()
            wait_until(lambda: phone.find("Simulator · Idle · grbl"), "Pairing did not complete.")
            arm_preview(page)
            phone.tap("Start", button=True)
            phone.confirm()
            wait_until(lambda: responses == [("start", 200)], "Workstation-confirmed Start failed.")
            assert confirmations == [200] and not jobs.status()["armed"]
            wait_until(lambda: not controller.status()["active"], "Simulated job did not finish.")
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
            assert not jobs.status()["armed"] and not controller.status()["active"]
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
            assert not any("$J=" in entry for entry in controller.status()["log"])
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
            wait_until(lambda: not jobs.status()["armed"], "Stop failed on legacy status.")
            assert [r["action"] for r in requests] == ["start", "start", "stop", "jog", "stop", "stop"]
            assert len({r["request_id"] for r in requests}) == len(requests)
            assert all(("control_epoch" in r) == (r["action"] != "stop") for r in requests)
            phone.tap("Forget pairing")
            print("PASS older receiver motion controls disabled, Stop preserved, no automatic retries", flush=True)
            browser.close()
    finally:
        release_confirm.set()
        release_jog.set()
        controller.disconnect()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--chrome", help="Installed Chrome executable; defaults to Playwright's Chromium")
    args = parser.parse_args()
    logging.getLogger("werkzeug").setLevel(logging.ERROR)
    run(Phone(args.adb, args.serial), args.apk.resolve(), args.chrome)
