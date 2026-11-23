"""Exercise a freshly installed desktop launcher without connecting any machine.

The package's workstation server (kerfkite-studio) must start without setting anything up or downloading it, and
with --step its STEP add-on must write and read a part.
"""

import argparse
import base64
from contextlib import contextmanager
import json
import math
import os
from pathlib import Path
import re
import platform
import sys
import socket
import subprocess
import tempfile
import time
from urllib.error import URLError
from urllib.parse import urlencode, urlsplit
from urllib.request import Request, urlopen

LAUNCH = "http://127.0.0.1:18764/"
STUDIO = "http://127.0.0.1:18765/"


def read(url, **kwargs):
    with urlopen(Request(url, **kwargs), timeout=8) as response:
        return response.read()


def page_when(proc, needle):
    deadline = time.monotonic() + 300
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError("Launcher exited before becoming ready")
        try:
            page = read(LAUNCH).decode()
        except (OSError, URLError):
            time.sleep(0.5)
            continue
        if "Setup needs attention" in page:
            raise RuntimeError(re.search(r"<h1>.*?</h1><p>(.*?)</p>", page).group(1))
        if needle in page:
            return page
        time.sleep(0.5)
    raise RuntimeError("Timed out waiting for " + needle)


def submit(page, action, **fields):
    token = re.search(r'name="token" value="([^"]+)"', page).group(1)
    return read(
        LAUNCH,
        headers={"Origin": LAUNCH.rstrip("/")},
        data=urlencode(dict(action=action, token=token, **fields)).encode(),
    )


def wait_for_studio_exit():
    endpoint = urlsplit(STUDIO)
    deadline = time.monotonic() + 12
    while True:
        try:
            # On Windows, loopback refusal can take several seconds while
            # Winsock retries the SYN. Require the explicit refusal to pass.
            with socket.create_connection((endpoint.hostname, endpoint.port), timeout=5):
                pass
        except ConnectionRefusedError:
            return
        except TimeoutError:
            pass
        if time.monotonic() >= deadline:
            raise RuntimeError("Studio listener shutdown could not be confirmed after its launcher exited")
        time.sleep(0.2)


@contextmanager
def install_directory():
    directory = tempfile.TemporaryDirectory(prefix="kerfkite clean install ")
    try:
        yield Path(directory.name)
    finally:
        # Windows can briefly retain mapped Python extension DLLs after shutdown.
        deadline = time.monotonic() + 30
        while True:
            try:
                directory.cleanup()
                break
            except OSError as error:
                if (
                    os.name != "nt"
                    or getattr(error, "winerror", None) not in (5, 32, 33)
                    or time.monotonic() >= deadline
                ):
                    raise
                time.sleep(0.5)


def exercise_native(exe, scratch):
    sources = Path(__file__).resolve().parents[1] / "packaging/desktop"
    env = os.environ.copy()
    if sys.platform.startswith("linux"):
        host = exe.with_name("kerfkite-native")
        # The installed window finds WebKitGTK; the smoke runner, built from the same sources, drives it.
        subprocess.run([host, "--check"], check=True, timeout=60)
        build = scratch / "window"
        subprocess.run(["cmake", "-S", sources / "linux", "-B", build, "-DCMAKE_BUILD_TYPE=Release"], check=True)
        subprocess.run(["cmake", "--build", build, "--target", "kerfkite-native-smoke"], check=True)
        command = [build / "kerfkite-native-smoke", STUDIO]
    elif sys.platform == "darwin":
        host = exe.with_name("KerfKiteNative")
        subprocess.run(["codesign", "--verify", "--strict", host], check=True)
        runner = scratch / "MacWindowTest"
        subprocess.run(
            [
                sys.executable,
                sources / "macos/build.py",
                "arm64" if platform.machine() == "arm64" else "amd64",
                runner,
                "--smoke",
            ],
            check=True,
        )
        command = [runner, STUDIO]
    elif os.name == "nt":
        subprocess.run([sys.executable, sources / "windows/build.py", "--smoke-only", exe.parent], check=True)
        command = [exe.with_name("KerfKiteNativeSmoke.exe"), STUDIO, scratch]
    else:
        raise RuntimeError("Unsupported native test platform")
    try:
        process = subprocess.run(command, timeout=180, env=env)
        if os.name == "nt" and (scratch / "receipt.txt").exists():
            print((scratch / "receipt.txt").read_text(encoding="utf-8-sig"), flush=True)
        process.check_returncode()
    finally:
        if os.name == "nt":
            exe.with_name("KerfKiteNativeSmoke.exe").unlink(missing_ok=True)


def check_step(post):
    """STEP through the package's Open CASCADE add-on: a part written and read back."""
    plate = {
        "version": 1,
        "features": [
            {"id": "base", "kind": "box", "size": [40, 30, 6]},
            {"id": "bore", "kind": "cylinder", "op": "cut", "radius": 4, "height": 8, "origin": [20, 15, -1]},
        ],
    }
    request = Request(
        STUDIO + "api/solid-cad/step/export",
        data=json.dumps({"model": plate}).encode(),
        headers=post.headers,
    )
    with urlopen(request, timeout=60) as response:
        document = response.read()
    assert document.startswith(b"ISO-10303-21;"), document[:40]
    shape = post("api/solid-cad/step/import", {"content": base64.b64encode(document).decode()})
    assert shape["solids"] == 1 and shape["faces"] == 7 and shape["solidOnly"] is True
    assert abs(shape["volume"] - (40 * 30 * 6 - math.pi * 16 * 6)) < 1e-6 * shape["volume"]


def check(exe, data, offline, step, native=False, target=None):
    env = {**os.environ, "KERFKITE_DATA_DIR": str(data)}
    name = "kerfkite-studio.exe" if os.name == "nt" else "kerfkite-studio"
    # kerfkite-studio beside the launcher, or in the Mac app's Resources.
    assert (exe.parent / name).is_file() or (exe.parent.parent / "Resources" / name).is_file(), "no workstation server"
    places = data / "shortcut-check"
    if sys.platform.startswith("linux"):
        # Keep the menu entry and desktop shortcut out of the runner's own folders.
        (places / "Desktop").mkdir(parents=True, exist_ok=True)
        env.update(XDG_DATA_HOME=str(places / "share"), XDG_DESKTOP_DIR=str(places / "Desktop"))
    proc = subprocess.Popen([exe], env=env)
    try:
        page = page_when(proc, "Choose your workshop")
        # STEP comes with the package or not at all; the launcher says which.
        fields = {}
        assert ("STEP import and export is installed." in page) == step, "the package's STEP add-on"
        asked = 'name="shortcuts" value="offered"' in page
        if asked and sys.platform.startswith("linux"):
            fields.update(shortcuts="offered", menu="yes", icon="yes")
        submit(page, "launch", **fields)
        page = page_when(proc, "Your workshop is ready")
        if asked and sys.platform.startswith("linux"):
            shortcut = (places / "Desktop/kerfkite-studio.desktop").read_text()
            assert f'Exec="{exe}"' in shortcut and os.access(places / "Desktop/kerfkite-studio.desktop", os.X_OK)
            portable = places / "share/applications/org.kerfkite.Studio.Portable.desktop"
            # An installed package already has its menu entry; a portable copy gets one.
            assert portable.exists() != Path("/usr/share/applications/org.kerfkite.Studio.desktop").exists()
            assert "Open it from there next time" in page or not portable.exists()
        session = json.loads(read(STUDIO + "api/session"))
        assert session["hardware_enabled"] is False
        assert b"KerfKite Studio" in read(STUDIO)
        headers = {"Content-Type": "application/json", "X-Studio-Token": session["token"]}

        def post(path, payload):
            return json.loads(read(STUDIO + path, data=json.dumps(payload).encode(), headers=headers))

        post.headers = headers

        sketch = post(
            "api/cad-script",
            {"source": "difference(){square([w,20]);translate([5,5]) circle(r=2);}", "overrides": {"w": 42}},
        )
        assert sketch["bounds"] == [0, 0, 42, 20] and len(sketch["profiles"]) == 2
        block = {"id": "Spacer", "kind": "script", "source": "cube([w,5,3]);", "scriptParameters": {"w": 12}}
        part = post("api/solid-cad", {"model": {"version": 1, "parameters": {"w": 99}, "features": [block]}})
        assert part["bounds"] == [0, 0, 0, 12, 5, 3] and abs(part["volume"] - 180) < 1e-8
        assert b"KerfKiteMacros" in read(STUDIO + "cad-macros.js")
        project = {
            "mode": "laser",
            "protocol": "grbl",
            "paths": [[[10, 10], [30, 10], [30, 20]]],
            "width": 80,
            "height": 55,
            "operation": "line",
            "feed": 400,
            "power": 500,
            "passes": 1,
        }
        assert post("api/generate", project)["cut_length"] == 30
        project.update(laser_pattern="perforate", laser_dash=3, laser_gap=2)
        laser = post("api/generate", project)
        assert {segment[5] for segment in laser["segments"]} == {0, 500}

        feature = {
            "id": "inlet",
            "kind": "loft",
            "baseShape": "rectangle",
            "baseWidth": 40,
            "baseDepth": 24,
            "topShape": "circle",
            "topWidth": "outlet",
            "height": 20,
        }
        model = {"version": 1, "parameters": {"outlet": 18}, "features": [feature]}
        loops = post("api/solid-cad/section", {"model": model, "z": 10})["profiles"]
        assert len(loops) == 1
        x, y = zip(*loops[0])
        assert abs(max(x) - min(x) - 29) < 0.01 and abs(max(y) - min(y) - 21) < 0.01
        # Nothing was set up: no runtime, and nothing downloaded.
        assert not list(data.glob("runtime-*")), "the launcher made a Python runtime"
        if step:
            check_step(post)
        if native and not offline:
            with install_directory() as scratch:
                exercise_native(exe, scratch)
        submit(page, "quit")
        assert proc.wait(timeout=12) == 0
        wait_for_studio_exit()
    finally:
        if proc.poll() is None:
            # Let the launcher stop the server even when a native GUI check fails.
            try:
                submit(read(LAUNCH).decode(), "quit")
                proc.wait(timeout=12)
                wait_for_studio_exit()
            except (OSError, ValueError, AttributeError, subprocess.TimeoutExpired):
                if proc.poll() is None:
                    proc.terminate()
                    proc.wait(timeout=10)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--step", action="store_true")
    parser.add_argument("--native", action="store_true", help="exercise the platform window on a graphical session")
    parser.add_argument(
        "--also-intel", type=Path, help="On Apple Silicon, test the Intel launcher sharing the same user data"
    )
    args = parser.parse_args()
    if args.also_intel and (sys.platform != "darwin" or platform.machine() != "arm64"):
        parser.error("--also-intel requires an Apple Silicon Mac with Rosetta")
    with install_directory() as data:
        try:
            launches = [(args.executable, None)]
            if args.also_intel:
                launches = [(args.executable, "darwin-arm64"), (args.also_intel, "darwin-amd64")]
            for offline in (False, True):
                for executable, target in launches:
                    check(executable.resolve(), data, offline, args.step, args.native, target)
                    result = "Offline relaunch passed" if offline else "Fresh installation and CAM preview passed"
                    print(f"{target or executable.name}: {result}", flush=True)
        except Exception:
            log = data / "setup.log"
            if log.exists():
                # Avoid leaking HTTP request information, index credentials or local paths.
                lines = log.read_text(errors="replace").splitlines()
                safe = [line for line in lines if "error" in line.lower() and "://" not in line and "@" not in line]
                print("Setup diagnostics:", " ".join(safe[-3:])[:500])
            raise
