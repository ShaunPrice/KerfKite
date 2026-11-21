"""Windows workstation admission for source installations and the Python entry points."""

import ctypes
from pathlib import Path
import re
import subprocess
import sys


_RELEASED_BUILDS = {
    10240,
    10586,
    14393,
    15063,
    16299,
    17134,
    17763,
    18362,
    18363,
    19041,
    19042,
    19043,
    19044,
    19045,
    22000,
    22621,
    22631,
    26100,
    26200,
    26300,
    28000,
}
_UNSUPPORTED = "KerfKite requires Windows 10 or Windows 11 on an x64 PC with 64-bit x64 Python."


def _windows_version():
    class Version(ctypes.Structure):
        _fields_ = [
            ("size", ctypes.c_uint32),
            ("major", ctypes.c_uint32),
            ("minor", ctypes.c_uint32),
            ("build", ctypes.c_uint32),
            ("platform", ctypes.c_uint32),
            ("service_pack", ctypes.c_wchar * 128),
            ("sp_major", ctypes.c_uint16),
            ("sp_minor", ctypes.c_uint16),
            ("suite", ctypes.c_uint16),
            ("product_type", ctypes.c_ubyte),
            ("reserved", ctypes.c_ubyte),
        ]

    version = Version()
    version.size = ctypes.sizeof(version)
    if ctypes.windll.ntdll.RtlGetVersion(ctypes.byref(version)) != 0:
        raise OSError("Windows version query failed")
    return version.major, version.minor, version.build, version.product_type


def _native_x64(build):
    if ctypes.sizeof(ctypes.c_void_p) != 8:
        return False
    kernel32 = ctypes.windll.kernel32
    try:
        query = kernel32.IsWow64Process2
    except AttributeError:  # Windows 10 before 1709 does not provide this API.
        if build >= 16299:
            return False
        # SYSTEM_INFO starts with a WORD containing the native processor architecture.
        info = ctypes.create_string_buffer(64)
        kernel32.GetNativeSystemInfo(ctypes.byref(info))
        return ctypes.c_uint16.from_buffer(info).value == 9
    process_machine, native_machine = ctypes.c_uint16(), ctypes.c_uint16()
    if not query(ctypes.c_void_p(-1), ctypes.byref(process_machine), ctypes.byref(native_machine)):
        raise OSError("Native Windows architecture query failed")
    return process_machine.value == 0 and native_machine.value == 0x8664


def _product_caption():
    # Windows 10 and 11 share NT version 10.0. Ask the OS for its product
    # caption on an unfamiliar build so feature updates and Insider builds work.
    directory = ctypes.create_unicode_buffer(32768)
    length = ctypes.windll.kernel32.GetSystemDirectoryW(directory, len(directory))
    if not 0 < length < len(directory):
        raise OSError("Windows system directory query failed")
    shell = Path(directory.value) / "WindowsPowerShell/v1.0/powershell.exe"
    result = subprocess.run(
        [
            str(shell),
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false); "
            "(Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction Stop).Caption",
        ],
        check=True,
        capture_output=True,
        timeout=10,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    return result.stdout.decode("utf-8-sig").strip()


def _eligible(version, native_x64, caption):
    major, minor, build, product_type = version
    if (major, minor, product_type) != (10, 0, 1) or build < 10240 or not native_x64(build):
        return False
    if build in _RELEASED_BUILDS:
        return True
    return bool(re.match(r"^(?:Microsoft\s+)?Windows\s+(?:10|11)(?:\s|$)", caption(), re.IGNORECASE))


def require_supported_windows():
    if sys.platform != "win32":
        return
    try:
        if _eligible(_windows_version(), _native_x64, _product_caption):
            return
    except (AttributeError, OSError, subprocess.SubprocessError, UnicodeError):
        pass
    print(_UNSUPPORTED, file=sys.stderr)
    raise SystemExit(1150)  # ERROR_OLD_WIN_VERSION, shared with Windows Setup.


if __name__ == "__main__":
    require_supported_windows()
