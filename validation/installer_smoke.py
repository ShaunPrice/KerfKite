"""Exercise a freshly installed desktop launcher without connecting any machine."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
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
            time.sleep(.5)
            continue
        if "Setup needs attention" in page:
            raise RuntimeError(re.search(r"<h1>.*?</h1><p>(.*?)</p>", page).group(1))
        if needle in page:
            return page
        time.sleep(.5)
    raise RuntimeError("Timed out waiting for " + needle)


def submit(page, action, **fields):
    token = re.search(r'name="token" value="([^"]+)"', page).group(1)
    return read(LAUNCH, headers={"Origin": LAUNCH.rstrip("/")},
                data=urlencode(dict(action=action,token=token,**fields)).encode())


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
        time.sleep(.2)


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
                if (os.name != "nt" or getattr(error, "winerror", None) not in (5, 32, 33)
                        or time.monotonic() >= deadline):
                    raise
                time.sleep(.5)


def check(exe, data, offline, step):
    env = {**os.environ, "KERFKITE_DATA_DIR": str(data)}
    if offline:
        env["UV_OFFLINE"]="1"
    proc = subprocess.Popen([exe], env=env)
    try:
        page = page_when(proc, "Choose your workshop")
        submit(page, "launch", **({"step":"yes"} if step else {}))
        page = page_when(proc, "Your workshop is ready")
        session=json.loads(read(STUDIO+"api/session"))
        assert session["hardware_enabled"] is False
        assert b"KerfKite Studio" in read(STUDIO)
        headers = {"Content-Type": "application/json", "X-Studio-Token": session["token"]}
        def post(path, payload):
            return json.loads(read(STUDIO + path, data=json.dumps(payload).encode(), headers=headers))

        project = {"mode": "laser", "protocol": "grbl", "paths": [[[10, 10], [30, 10], [30, 20]]],
                   "width": 80, "height": 55, "operation": "line", "feed": 400, "power": 500, "passes": 1}
        assert post("api/generate", project)["cut_length"] == 30
        project.update(laser_pattern="perforate", laser_dash=3, laser_gap=2)
        laser = post("api/generate", project)
        assert {segment[5] for segment in laser["segments"]} == {0, 500}

        feature = {"id": "inlet", "kind": "loft", "baseShape": "rectangle", "baseWidth": 40,
                   "baseDepth": 24, "topShape": "circle", "topWidth": "outlet", "height": 20}
        model = {"version": 1, "parameters": {"outlet": 18}, "features": [feature]}
        loops = post("api/solid-cad/section", {"model": model, "z": 10})["profiles"]
        assert len(loops) == 1
        x, y = zip(*loops[0])
        assert abs(max(x) - min(x) - 29) < .01 and abs(max(y) - min(y) - 21) < .01
        if step:
            python=next(data.glob("runtime-*/Scripts/python.exe" if os.name=="nt" else "runtime-*/bin/python"))
            subprocess.run([python,"-I","-c","from pypdf import PdfReader; from OCP.STEPControl import STEPControl_Writer; STEPControl_Writer()"],check=True)
        submit(page,"quit")
        assert proc.wait(timeout=12)==0
        wait_for_studio_exit()
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=10)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable",type=Path)
    parser.add_argument("--step",action="store_true")
    args=parser.parse_args()
    with install_directory() as data:
        try:
            for offline in (False,True):
                check(args.executable.resolve(),data,offline,args.step)
                print("Offline relaunch passed" if offline else "Fresh installation and CAM preview passed",flush=True)
        except Exception:
            log=data/"setup.log"
            if log.exists():
                # Avoid leaking HTTP request information, index credentials or local paths.
                lines=log.read_text(errors="replace").splitlines()
                safe=[line for line in lines if "error" in line.lower() and "://" not in line and "@" not in line]
                print("Setup diagnostics:", " ".join(safe[-3:])[:500])
            raise
