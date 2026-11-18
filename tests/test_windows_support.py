import argparse
import contextlib
import io
import subprocess
import sys
import unittest
from unittest.mock import Mock, patch

from studio import windows_support


class WindowsSupportTests(unittest.TestCase):
    def test_supported_releases_do_not_need_wmi(self):
        for build in (10240, 19045, 22000, 26100, 26300, 28000):
            with self.subTest(build=build):
                self.assertTrue(windows_support._eligible((10, 0, build, 1), lambda _: True,
                                                          self.fail))

    def test_unknown_workstation_build_uses_caption(self):
        for caption, accepted in [
            ("Microsoft Windows 11 Professionnel", True), ("Windows 10 Enterprise", True),
            ("Microsoft Windows 12 Pro", False), ("Windows 110", False),
            ("Windows Server 2025", False), ("", False),
        ]:
            with self.subTest(caption=caption):
                self.assertEqual(windows_support._eligible((10, 0, 30000, 1), lambda _: True,
                                                          lambda: caption), accepted)

    def test_os_and_architecture_preconditions_apply_to_every_build(self):
        for version, x64 in [((6, 3, 9600, 1), True), ((10, 1, 30000, 1), True),
                             ((10, 0, 9600, 1), True), ((10, 0, 26100, 2), True),
                             ((10, 0, 26100, 3), True), ((10, 0, 19045, 1), False),
                             ((10, 0, 30000, 1), False)]:
            with self.subTest(version=version, x64=x64):
                self.assertFalse(windows_support._eligible(version, lambda _: x64, self.fail))

    def test_failure_to_identify_new_build_denies_startup(self):
        with patch.object(sys, "platform", "win32"), \
                patch.object(windows_support, "_windows_version", return_value=(10, 0, 30000, 1)), \
                patch.object(windows_support, "_native_x64", return_value=True), \
                patch.object(windows_support, "_product_caption", side_effect=subprocess.TimeoutExpired("WMI", 10)), \
                contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            windows_support.require_supported_windows()
        self.assertEqual(error.exception.code, 1150)

    def test_all_source_commands_reject_server_before_startup(self):
        from studio import desktop_server, mcp_server, server, web_server

        for module in (server, web_server, mcp_server, desktop_server):
            with self.subTest(command=module.__name__), patch.object(sys, "platform", "win32"), \
                    patch.object(windows_support, "_windows_version", return_value=(10, 0, 26100, 3)), \
                    patch.object(argparse, "ArgumentParser") as parser, \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                module.main()
            self.assertEqual(error.exception.code, 1150)
            parser.assert_not_called()

    def test_other_platforms_skip_windows_probes(self):
        for name in ["darwin", "linux"]:
            with patch.object(sys, "platform", name), patch.object(windows_support, "_windows_version") as probe:
                windows_support.require_supported_windows()
                probe.assert_not_called()

    def test_early_windows10_fallback_and_emulation(self):
        def query(process, process_result, native_result):
            process_result._obj.value = 0x8664
            native_result._obj.value = 0xaa64
            return 1

        kernel = Mock(spec=["IsWow64Process2"])
        kernel.IsWow64Process2 = query
        windows = Mock(kernel32=kernel)
        with patch.object(windows_support.ctypes, "windll", windows, create=True):
            self.assertFalse(windows_support._native_x64(26100))
            windows.kernel32 = Mock(spec=["GetNativeSystemInfo"])
            windows.kernel32.GetNativeSystemInfo.side_effect = lambda p: setattr(p._obj, "raw", b"\x09\0" + bytes(62))
            self.assertTrue(windows_support._native_x64(10240))
            self.assertFalse(windows_support._native_x64(26100))
