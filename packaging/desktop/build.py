"""Build the public desktop installers on a macOS release workstation."""
import argparse
import os
import plistlib
import shutil
import subprocess
import tempfile
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ICON = ROOT / "studio/static/app-icon.png"


def run(*cmd, **kwargs):
    subprocess.run(cmd, check=True, **kwargs)


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
    meta.write_text(f"Package: kerfkite-studio\nVersion: {version}-1\nArchitecture: {arch}\nSection: graphics\n"
                    "Priority: optional\nMaintainer: KerfKite Studio contributors <maintainers@users.noreply.github.com>\n"
                    "Depends: xdg-utils, ca-certificates\nDescription: KerfKite CAD and machining simulator\n")
    run("dpkg-deb","--root-owner-group","-Zxz","--build",pkg,out / f"KerfKite-Linux-{arch}.deb")
    run("tar","-C",dest.parent,"-czf",out / f"KerfKite-Linux-{arch}.tar.gz",dest.name,env={**os.environ,"COPYFILE_DISABLE":"1"})


def windows(work, out, wheel):
    payload(work / "payload", wheel, "windows", "amd64")
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
    parser.add_argument("wheel",type=Path)
    parser.add_argument("output",type=Path)
    parser.add_argument("targets",nargs="+",choices=["mac-arm64","mac-amd64","linux-arm64","linux-amd64","windows-amd64"])
    args=parser.parse_args()
    wheel=args.wheel.resolve()
    if not wheel.name.endswith("-py3-none-any.whl"):
        parser.error("Provide the portable Studio wheel")
    version=wheel.name.split("-")[1]
    out=args.output.resolve()
    out.mkdir(parents=True,exist_ok=True)
    for target in args.targets:
        with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
            work=Path(tmp)
            platform,arch=target.split("-")
            if platform=="windows":
                windows(work,out,wheel)
            else:
                {"mac":mac,"linux":linux}[platform](work,out,wheel,arch,version)
