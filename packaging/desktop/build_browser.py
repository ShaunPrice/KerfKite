"""Package the KerfKite local-file editor for desktop browsers on SBCs, from this source tree's editor files."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parents[2]


def build(output, static=ROOT / "studio/static", version=None):
    version = version or (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        package = root / "package"
        share = package / "usr/share"
        editor = share / "kerfkite-browser"
        shutil.copytree(static, editor, ignore=shutil.ignore_patterns("__pycache__"))

        for name in ("index.html", "app.js", "offline-cam.js", "app-icon.png", "vendor/Studio-MIT.txt"):
            if not (editor / name).is_file():
                raise ValueError(f"The editor's files lack {name}")
        with ZipFile(output / "KerfKite-Browser-Editor.zip", "w", ZIP_DEFLATED) as portable:
            for file in sorted(editor.rglob("*")):
                if file.is_file():
                    portable.write(file, Path("KerfKite Browser Editor") / file.relative_to(editor))

        menu = share / "applications/org.kerfkite.BrowserEditor.desktop"
        menu.parent.mkdir()
        menu.write_text(
            "[Desktop Entry]\nType=Application\nName=KerfKite Browser Editor\n"
            "Comment=Draw and export toolpaths offline in your browser\n"
            "Exec=xdg-open file:///usr/share/kerfkite-browser/index.html\n"
            "Icon=/usr/share/kerfkite-browser/app-icon.png\nTerminal=false\n"
            "Categories=Graphics;Engineering;\n"
        )
        docs = share / "doc/kerfkite-browser"
        docs.mkdir(parents=True)
        shutil.copyfile(editor / "vendor/Studio-MIT.txt", docs / "copyright")
        control = package / "DEBIAN/control"
        control.parent.mkdir()
        control.write_text(
            f"Package: kerfkite-browser\nVersion: {version}-1\n"
            "Architecture: all\nSection: graphics\nPriority: optional\n"
            "Maintainer: KerfKite Studio contributors <maintainers@users.noreply.github.com>\n"
            "Depends: xdg-utils\nRecommends: chromium | firefox-esr | firefox\n"
            "Description: Offline KerfKite drawing and contour CAM in a desktop browser\n"
        )
        subprocess.run(
            ["dpkg-deb", "--root-owner-group", "-Zxz", "--build", package, output / "KerfKite-Browser-Editor-all.deb"],
            check=True,
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    build(args.output)
