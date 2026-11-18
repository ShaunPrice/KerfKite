"""Build the public desktop installers on a macOS release workstation."""
import argparse
import os
import plistlib
import re
import zipfile
import shutil
import subprocess
import tempfile
import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ICON = ROOT / "studio/static/app-icon.png"


def run(*cmd, **kwargs):
    subprocess.run(cmd, check=True, **kwargs)


def release_wheel(source, target):
    """Select the CPython wheel for the runtime installed by the launcher."""
    target_platforms = {
        "mac-arm64": r"macosx_\d+_\d+_(arm64|universal2)",
        "mac-amd64": r"macosx_\d+_\d+_(x86_64|universal2)",
        "linux-arm64": r"(linux|manylinux(?:\d+|_\d+_\d+))_aarch64",
        "linux-amd64": r"(linux|manylinux(?:\d+|_\d+_\d+))_x86_64",
        "windows-amd64": r"win_amd64",
    }
    candidates = source.glob("kerfkite_studio-*.whl") if source.is_dir() else [source]
    matches = []
    for wheel in candidates:
        tags = re.fullmatch(r"kerfkite_studio-[^-]+-cp312-cp312-([^.]+(?:\.[^.]+)*)\.whl", wheel.name)
        if tags and any(re.fullmatch(target_platforms[target], tag) for tag in tags[1].split(".")):
            matches.append(wheel)
    if len(matches) != 1:
        raise ValueError(f"Expected one native CPython 3.12 wheel for {target} in {source}, found {len(matches)}")
    with zipfile.ZipFile(matches[0]) as archive:
        suffix = ".pyd" if target == "windows-amd64" else ".so"
        if not any(n.startswith("studio/_relief_native.") and n.endswith(suffix) for n in archive.namelist()):
            raise ValueError(f"Wheel for {target} is missing the native CAM extension")
    return matches[0]


def payload(dest, wheel, goos, arch):
    dest.mkdir(parents=True)
    shutil.copy(wheel, dest)
    shutil.copy(HERE / "requirements.lock", dest)
    shutil.copy(HERE / "step.lock", dest)
    shutil.copy(ROOT / "LICENSE", dest)
    goroot = Path(subprocess.check_output(["go", "env", "GOROOT"], text=True).strip())
    notices = dest / "runtime-notices"
    notices.mkdir()
    shutil.copy(goroot / "LICENSE", notices / "Go-LICENSE")
    # The standard library includes vendored packages with additional notices.
    vendor = goroot / "src/vendor"
    for notice in vendor.rglob("*"):
        if notice.is_file() and notice.name.upper().startswith(("LICENSE", "COPYING", "PATENTS", "NOTICE")):
            target = notices / notice.relative_to(vendor)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(notice, target)
    exe = dest / ("KerfKite.exe" if goos == "windows" else "KerfKite")
    flags = "-s -w" + (" -H=windowsgui" if goos == "windows" else "")
    run("go", "build", "-trimpath", "-buildvcs=false", "-ldflags", flags, "-o", exe, ".",
        cwd=HERE, env={**os.environ, "CGO_ENABLED":"0", "GO111MODULE":"off", "GOOS":goos, "GOARCH":arch})
    return exe


def mac(work, out, wheel, arch, version):
    stage = work / "image"
    contents = stage / "KerfKite Studio.app/Contents"
    res = contents / "Resources"
    exe = payload(res, wheel, "darwin", arch)
    (contents / "MacOS").mkdir()
    exe.rename(contents / "MacOS/KerfKite")
    helper = contents / "MacOS/KerfKiteNative"
    run(sys.executable, HERE / "macos/build.py", arch, helper)
    run("codesign", "--force", "--sign", "-", helper)
    with Image.open(ICON) as im:
        im.save(res / "KerfKite.icns")
    (contents / "Info.plist").write_bytes(plistlib.dumps({
        "CFBundleName":"KerfKite Studio", "CFBundleIdentifier":"org.kerfkite.studio",
        "CFBundleExecutable":"KerfKite", "CFBundleIconFile":"KerfKite.icns",
        "CFBundlePackageType":"APPL", "CFBundleVersion":version, "CFBundleShortVersionString":version,
        "LSUIElement":True,
    }))
    run("codesign", "--force", "--sign", "-", contents.parent)
    run("codesign", "--verify", "--strict", contents.parent)
    (stage / "Applications").symlink_to("/Applications")
    suffix = "Apple-Silicon" if arch=="arm64" else "Intel"
    run("hdiutil","create","-ov","-format","UDZO","-volname","KerfKite Studio","-srcfolder",stage,
        out / f"KerfKite-macOS-{suffix}.dmg")


def linux(work, out, wheel, arch, version):
    pkg = work / "package"
    dest = pkg / "opt/kerfkite"
    payload(dest, wheel, "linux", arch)
    helper = dest / "kerfkite-native"
    shutil.copyfile(HERE / "linux/kerfkite-native", helper)
    helper.chmod(0o755)
    desktop = pkg / "usr/share/applications/org.kerfkite.Studio.desktop"
    desktop.parent.mkdir(parents=True)
    desktop.write_text("[Desktop Entry]\nType=Application\nName=KerfKite Studio\nComment=Design and simulate CNC jobs\n"
                       "Exec=/opt/kerfkite/KerfKite\nIcon=kerfkite-studio\nTerminal=false\nCategories=Graphics;Engineering;\n")
    icon = pkg / "usr/share/icons/hicolor/256x256/apps/kerfkite-studio.png"
    icon.parent.mkdir(parents=True)
    with Image.open(ICON) as im:
        im.resize((256,256),Image.Resampling.LANCZOS).save(icon)
    meta = pkg / "DEBIAN/control"
    meta.parent.mkdir()
    revision = 2 if version == "0.2.1" else 1
    meta.write_text(f"Package: kerfkite-studio\nVersion: {version}-{revision}\nArchitecture: {arch}\nSection: graphics\n"
                    "Priority: optional\nMaintainer: KerfKite Studio contributors <maintainers@users.noreply.github.com>\n"
                    "Depends: xdg-utils, ca-certificates, libgl1, python3, python3-gi, gir1.2-gtk-3.0, gir1.2-webkit2-4.1 | gir1.2-webkit2-4.0\nDescription: KerfKite CAD and machining simulator\n")
    run("dpkg-deb","--root-owner-group","-Zxz","--build",pkg,out / f"KerfKite-Linux-{arch}.deb")
    run("tar","-C",dest.parent,"-czf",out / f"KerfKite-Linux-{arch}.tar.gz",dest.name,env={**os.environ,"COPYFILE_DISABLE":"1"})


def windows(work, out, wheel):
    payload(work / "payload", wheel, "windows", "amd64")
    run(sys.executable, HERE / "windows/build.py", work / "payload")
    with Image.open(ICON) as im:
        im.save(work / "app.ico", sizes=[(16,16),(32,32),(48,48),(256,256)])
    shutil.copy(HERE / "windows.nsi",work)
    if shutil.which("makensis"):
        run("makensis","windows.nsi",cwd=work)
    else:
        run("docker","run","--rm","-v",f"{work}:/build","-w","/build","kerfkite-installer-nsis","makensis","windows.nsi")
    (work / "KerfKite-Windows-x64-Setup.exe").replace(out / "KerfKite-Windows-x64-Setup.exe")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wheels",type=Path,help="Directory of platform wheels, or one matching native wheel")
    parser.add_argument("output",type=Path)
    parser.add_argument("targets",nargs="+",choices=["mac-arm64","mac-amd64","linux-arm64","linux-amd64","windows-amd64"])
    args=parser.parse_args()
    try:
        wheels = {target: release_wheel(args.wheels.resolve(), target) for target in args.targets}
    except ValueError as exc:
        parser.error(str(exc))
    if len({wheel.name.split("-")[1] for wheel in wheels.values()}) != 1:
        parser.error("All desktop targets must use the same Studio version")
    out=args.output.resolve()
    out.mkdir(parents=True,exist_ok=True)
    for target, wheel in wheels.items():
        version = wheel.name.split("-")[1]
        with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
            work=Path(tmp)
            platform,arch=target.split("-")
            if platform=="windows":
                windows(work,out,wheel)
            else:
                {"mac":mac,"linux":linux}[platform](work,out,wheel,arch,version)
