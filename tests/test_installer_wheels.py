import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile

script = Path(__file__).resolve().parents[1] / 'packaging' / 'desktop' / 'build.py'
spec = importlib.util.spec_from_file_location('desktop_build', script)
desktop_build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(desktop_build)


class InstallerWheelTests(unittest.TestCase):
    def test_each_installer_selects_its_own_native_wheel(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            targets = {'mac-arm64': 'macosx_11_0_universal2', 'mac-amd64': 'macosx_11_0_universal2',
                       'linux-arm64': 'manylinux_2_28_aarch64', 'linux-amd64': 'manylinux2014_x86_64',
                       'windows-amd64': 'win_amd64'}
            (folder / 'kerfkite_studio-1-py3-none-any.whl').touch()
            for tag in set(targets.values()):
                suffix = '.pyd' if tag.startswith('win') else '.so'
                with zipfile.ZipFile(folder / f'kerfkite_studio-1-cp312-cp312-{tag}.whl', 'w') as wheel:
                    wheel.writestr('studio/_relief_native.cp312' + suffix, b'')
            for target, tag in targets.items():
                self.assertEqual(desktop_build.release_wheel(folder, target).name,
                                 f'kerfkite_studio-1-cp312-cp312-{tag}.whl')

    def test_reject_portable_wrong_arch_and_missing_extension(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            portable = folder / 'kerfkite_studio-1-py3-none-any.whl'
            portable.touch()
            native = folder / 'kerfkite_studio-1-cp312-cp312-win_amd64.whl'
            with zipfile.ZipFile(native, 'w') as wheel:
                wheel.writestr('studio/simulation.py', '')
            for path, target in ((portable, 'windows-amd64'), (native, 'linux-amd64'), (native, 'windows-amd64')):
                with self.subTest(path=path, target=target), self.assertRaises(ValueError):
                    desktop_build.release_wheel(path, target)
