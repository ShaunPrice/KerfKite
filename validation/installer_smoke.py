"""Exercise a freshly installed desktop launcher without connecting any machine."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
from urllib.error import URLError
from urllib.parse import urlencode
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
        project={"mode":"laser","protocol":"grbl","paths":[[[10,10],[30,10],[30,20]]],
                 "width":80,"height":55,"operation":"line","feed":400,"power":500,"passes":1}
        job=json.loads(read(STUDIO+"api/generate", data=json.dumps(project).encode(),
                       headers={"Content-Type":"application/json","X-Studio-Token":session["token"]}))
        assert len(job["segments"])>=2 and job["cut_length"]==30
        if step:
            python=next(data.glob("runtime-*/Scripts/python.exe" if os.name=="nt" else "runtime-*/bin/python"))
            subprocess.run([python,"-I","-c","from OCP.STEPControl import STEPControl_Writer; STEPControl_Writer()"],check=True)
        submit(page,"quit")
        assert proc.wait(timeout=12)==0
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=10)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable",type=Path)
    parser.add_argument("--step",action="store_true")
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="kerfkite clean install ") as tmp:
        data=Path(tmp)
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
