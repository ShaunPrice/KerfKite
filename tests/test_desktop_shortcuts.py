"""The desktop packages put KerfKite Studio where a new user looks for it."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DESKTOP = ROOT / "packaging/desktop"


def go(*args, **env):
    return subprocess.run(
        ["go", *args],
        cwd=DESKTOP,
        env={**os.environ, "GO111MODULE": "off", "CGO_ENABLED": "0", **env},
        capture_output=True,
        text=True,
        timeout=600,
    )


class WindowsInstallerTests(unittest.TestCase):
    script = (DESKTOP / "windows.nsi").read_text()

    def test_start_menu_is_always_installed_and_desktop_is_a_choice(self):
        studio = self.script.split('Section "KerfKite Studio and its Start menu shortcut" SecStudio', 1)[1]
        studio = studio.split("SectionEnd", 1)[0]
        self.assertIn("SectionIn RO", studio)
        self.assertIn('CreateShortcut "$SMPROGRAMS\\KerfKite Studio.lnk"', studio)
        desktop = self.script.split('Section "Desktop shortcut" SecDesktop', 1)[1].split("SectionEnd", 1)[0]
        self.assertIn('CreateShortcut "$DESKTOP\\KerfKite Studio.lnk"', desktop)
        self.assertIn("!insertmacro MUI_PAGE_COMPONENTS", self.script)
        self.assertLess(self.script.index("MUI_PAGE_COMPONENTS"), self.script.index("MUI_PAGE_INSTFILES"))

    def test_uninstall_and_reinstall_remove_the_desktop_shortcut(self):
        uninstall = self.script.split('Section "Uninstall"', 1)[1]
        self.assertIn('Delete "$SMPROGRAMS\\KerfKite Studio.lnk"', uninstall)
        self.assertIn('Delete "$DESKTOP\\KerfKite Studio.lnk"', uninstall)
        studio = self.script.split("SecStudio", 1)[1].split("SectionEnd", 1)[0]
        self.assertIn('Delete "$DESKTOP\\KerfKite Studio.lnk"', studio)

    @unittest.skipUnless(shutil.which("makensis"), "NSIS is not installed")
    def test_installer_compiles(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            (work / "payload").mkdir()
            for name in ("KerfKite.exe", "LICENSE"):
                (work / "payload" / name).write_text("stand-in\n")
            from PIL import Image

            Image.new("RGBA", (256, 256), (17, 99, 104, 255)).save(work / "app.ico", sizes=[(16, 16), (256, 256)])
            shutil.copy(DESKTOP / "windows.nsi", work)
            run = subprocess.run(
                ["makensis", "-V2", "-WX", "-DSTUDIO_VERSION=0.0.0", "windows.nsi"],
                cwd=work,
                capture_output=True,
                text=True,
                timeout=300,
            )
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertTrue((work / "KerfKite-Windows-x64-Setup.exe").is_file())


@unittest.skipUnless(shutil.which("go"), "Go is not installed")
class LauncherTests(unittest.TestCase):
    def test_launcher_shortcut_logic(self):
        for args in (["vet", "."], ["test", "-count=1", "."]):
            run = go(*args)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_launcher_builds_for_each_release_platform(self):
        with tempfile.TemporaryDirectory() as tmp:
            targets = (
                ("windows", "amd64"),
                ("darwin", "arm64"),
                ("darwin", "amd64"),
                ("linux", "arm64"),
                ("linux", "amd64"),
            )
            for goos, arch in targets:
                run = go("build", "-o", str(Path(tmp) / f"{goos}-{arch}"), ".", GOOS=goos, GOARCH=arch)
                self.assertEqual(run.returncode, 0, f"{goos}/{arch}: {run.stderr}")


class LinuxPackageTests(unittest.TestCase):
    def test_portable_archive_carries_the_shortcut_icon(self):
        source = (DESKTOP / "build.py").read_text()
        linux = source.split("def linux(", 1)[1].split("\ndef ", 1)[0]
        self.assertIn('dest / "kerfkite-studio.png"', linux)
        self.assertLess(linux.index('dest / "kerfkite-studio.png"'), linux.index('"tar",'))


if __name__ == "__main__":
    unittest.main()
