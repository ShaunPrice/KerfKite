"""Build the public desktop installers on a macOS release workstation."""

import argparse
import os
import plistlib
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


# Every program the packages carry: the workstation server, the HTTPS editor, the MCP assistant server, the
# command-line CAM and the pendant's relay. The STEP add-on is optional, in step/.
NATIVE_PROGRAMS = ("kerfkite-studio", "kerfkite-web", "kerfkite-mcp", "kerfkite-cam", "kerfkite-pendant-tls")


def native_build(native, target):
    """The native programs of one target, <native>/<target>/: every program in NATIVE_PROGRAMS, and in step/
    the STEP add-on with Open CASCADE's libraries and notices where it was built. The
    .github/workflows/native-programs.yml workflow makes these folders."""
    folder = native / target
    if not folder.is_dir():
        folder = native / f"native-{target}"  # as gh run download names the workflow's artifacts
    suffix = ".exe" if target == "windows-amd64" else ""
    missing = [name + suffix for name in NATIVE_PROGRAMS if not (folder / (name + suffix)).is_file()]
    if missing:
        raise ValueError(f"{folder} lacks {', '.join(missing)}")
    return folder


def copy_native(folder, dest, step=True):
    """The server, its programs and the editor's files; the STEP add-on too unless the installer offers it apart.
    The assistant server (kerfkite-mcp) reads the template collection beside static/."""
    for item in folder.iterdir():
        if item.name == "step":
            if step:
                shutil.copytree(item, dest, dirs_exist_ok=True)
        elif item.is_dir():
            shutil.copytree(item, dest / item.name)
        else:
            shutil.copy2(item, dest)
    shutil.copytree(ROOT / "studio/static", dest / "static", ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copy2(ROOT / "studio/builtin_templates.kkart", dest)


def payload(dest, native, goos, arch):
    """The launcher and what it starts: the workstation server and its programs."""
    dest.mkdir(parents=True)
    copy_native(native, dest, step=goos != "windows")
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
    run(
        "go",
        "build",
        "-trimpath",
        "-buildvcs=false",
        "-ldflags",
        flags,
        "-o",
        exe,
        ".",
        cwd=HERE,
        env={**os.environ, "CGO_ENABLED": "0", "GO111MODULE": "off", "GOOS": goos, "GOARCH": arch},
    )
    return exe


def mac(work, out, native, arch, version):
    stage = work / "image"
    contents = stage / "KerfKite Studio.app/Contents"
    res = contents / "Resources"
    exe = payload(res, native, "darwin", arch)
    (contents / "MacOS").mkdir()
    exe.rename(contents / "MacOS/KerfKite")
    helper = contents / "MacOS/KerfKiteNative"
    run(sys.executable, HERE / "macos/build.py", arch, helper)
    run("codesign", "--force", "--sign", "-", helper)
    with Image.open(ICON) as im:
        im.save(res / "KerfKite.icns")
    (contents / "Info.plist").write_bytes(
        plistlib.dumps(
            {
                "CFBundleName": "KerfKite Studio",
                "CFBundleIdentifier": "org.kerfkite.studio",
                "CFBundleExecutable": "KerfKite",
                "CFBundleIconFile": "KerfKite.icns",
                "CFBundlePackageType": "APPL",
                "CFBundleVersion": version,
                "CFBundleShortVersionString": version,
                "LSUIElement": True,
            }
        )
    )
    run("codesign", "--force", "--sign", "-", contents.parent)
    run("codesign", "--verify", "--strict", contents.parent)
    (stage / "Applications").symlink_to("/Applications")
    suffix = "Apple-Silicon" if arch == "arm64" else "Intel"
    run(
        "hdiutil",
        "create",
        "-ov",
        "-format",
        "UDZO",
        "-volname",
        "KerfKite Studio",
        "-srcfolder",
        stage,
        out / f"KerfKite-macOS-{suffix}.dmg",
    )


def linux(work, out, native, arch, version):
    pkg = work / "package"
    dest = pkg / "opt/kerfkite"
    payload(dest, native, "linux", arch)
    # The GTK window, in C: it needs the GTK 3 headers to build, and finds WebKitGTK when it starts.
    window = work / "window"
    run("cmake", "-S", HERE / "linux", "-B", window, "-DCMAKE_BUILD_TYPE=Release")
    run("cmake", "--build", window, "--target", "kerfkite-native")
    helper = dest / "kerfkite-native"
    shutil.copyfile(window / "kerfkite-native", helper)
    helper.chmod(0o755)
    # The launcher points the menu entry and desktop shortcut of a portable copy at this icon.
    with Image.open(ICON) as im:
        im.resize((256, 256), Image.Resampling.LANCZOS).save(dest / "kerfkite-studio.png")
    desktop = pkg / "usr/share/applications/org.kerfkite.Studio.desktop"
    desktop.parent.mkdir(parents=True)
    desktop.write_text(
        "[Desktop Entry]\nType=Application\nName=KerfKite Studio\nComment=Design and simulate CNC jobs\n"
        "Exec=/opt/kerfkite/KerfKite\nIcon=kerfkite-studio\nTerminal=false\nCategories=Graphics;Engineering;\n"
    )
    icon = pkg / "usr/share/icons/hicolor/256x256/apps/kerfkite-studio.png"
    icon.parent.mkdir(parents=True)
    with Image.open(ICON) as im:
        im.resize((256, 256), Image.Resampling.LANCZOS).save(icon)
    meta = pkg / "DEBIAN/control"
    meta.parent.mkdir()
    revision = 2 if version == "0.2.1" else 1
    meta.write_text(
        f"Package: kerfkite-studio\nVersion: {version}-{revision}\nArchitecture: {arch}\nSection: graphics\n"
        "Priority: optional\nMaintainer: KerfKite Studio contributors <maintainers@users.noreply.github.com>\n"
        "Depends: xdg-utils, ca-certificates, libgl1, libgtk-3-0, libwebkit2gtk-4.1-0 | libwebkit2gtk-4.0-37\n"
        "Description: KerfKite CAD and machining simulator\n"
    )
    run("dpkg-deb", "--root-owner-group", "-Zxz", "--build", pkg, out / f"KerfKite-Linux-{arch}.deb")
    run(
        "tar",
        "-C",
        dest.parent,
        "-czf",
        out / f"KerfKite-Linux-{arch}.tar.gz",
        dest.name,
        env={**os.environ, "COPYFILE_DISABLE": "1"},
    )


def windows(work, out, native, version):
    payload(work / "payload", native, "windows", "amd64")
    if (native / "step").is_dir():
        # STEP import and export is a component the person chooses in Setup.
        shutil.copytree(native / "step", work / "step")
    run(sys.executable, HERE / "windows/build.py", work / "payload")
    with Image.open(ICON) as im:
        im.save(work / "app.ico", sizes=[(16, 16), (32, 32), (48, 48), (256, 256)])
    shutil.copy(HERE / "windows.nsi", work)
    nsis_args = [f"-DSTUDIO_VERSION={version}", "windows.nsi"]
    if shutil.which("makensis"):
        run("makensis", *nsis_args, cwd=work)
    else:
        run(
            "docker",
            "run",
            "--rm",
            "-v",
            f"{work}:/build",
            "-w",
            "/build",
            "kerfkite-installer-nsis",
            "makensis",
            *nsis_args,
        )
    (work / "KerfKite-Windows-x64-Setup.exe").replace(out / "KerfKite-Windows-x64-Setup.exe")


def studio_version():
    return (ROOT / "VERSION").read_text(encoding="utf-8").strip()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "programs",
        type=Path,
        help="Folder of the native programs, one folder per target as native-programs.yml uploads them",
    )
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "targets", nargs="+", choices=["mac-arm64", "mac-amd64", "linux-arm64", "linux-amd64", "windows-amd64"]
    )
    args = parser.parse_args()
    try:
        builds = {target: native_build(args.programs.resolve(), target) for target in args.targets}
    except ValueError as exc:
        parser.error(str(exc))
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    version = studio_version()
    for target, native in builds.items():
        with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
            work = Path(tmp)
            platform, arch = target.split("-")
            if platform == "windows":
                windows(work, out, native, version)
            else:
                {"mac": mac, "linux": linux}[platform](work, out, native, arch, version)
