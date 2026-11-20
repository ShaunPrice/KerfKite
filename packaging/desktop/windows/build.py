#!/usr/bin/env python3
"""Build the x64 WinForms desktop using Roslyn on Windows, Linux or macOS."""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

HERE = Path(__file__).resolve().parent
PACKAGES = {
    'Microsoft.Web.WebView2': ('1.0.4314.35', '90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4'),
    'Microsoft.NETFramework.ReferenceAssemblies.net48': (
        '1.0.3',
        '8a7e348538e7eb91351696911689f49e3d4f63f8bab517432bbe159b8b1104a2',
    ),
}
REFERENCES = ('mscorlib', 'System', 'System.Core', 'System.Windows.Forms', 'System.Drawing', 'System.Web.Extensions')
WEB = ('Microsoft.Web.WebView2.Core.dll', 'Microsoft.Web.WebView2.WinForms.dll')


def package(name, cache):
    version, digest = PACKAGES[name]
    filename = f'{name.lower()}.{version}.nupkg'
    target = cache / filename
    if not target.is_file() or hashlib.sha256(target.read_bytes()).hexdigest() != digest:
        url = f'https://api.nuget.org/v3-flatcontainer/{name.lower()}/{version}/{filename}'
        with urllib.request.urlopen(url, timeout=90) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f'NuGet checksum mismatch for {name}')
        with tempfile.NamedTemporaryFile(dir=cache, delete=False) as temporary:
            temporary.write(data)
        os.replace(temporary.name, target)
    return zipfile.ZipFile(target)


def build(output, smoke=False, smoke_only=False):
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cache = Path(os.environ.get('KERFKITE_NATIVE_BUILD_CACHE', Path.home() / '.cache' / 'kerfkite-native'))
    cache.mkdir(parents=True, exist_ok=True)
    dotnet = shutil.which('dotnet')
    if not dotnet:
        raise RuntimeError('Windows desktop builds require a .NET SDK with the Roslyn C# compiler on PATH.')
    sdks = subprocess.check_output([dotnet, '--list-sdks'], text=True).splitlines()
    for line in reversed(sdks):
        version, root = line.split(' [', 1)
        compiler = Path(root.rstrip(']')) / version / 'Roslyn' / 'bincore' / 'csc.dll'
        if compiler.is_file():
            break
    else:
        raise RuntimeError('Install the .NET SDK (the runtime alone does not include Roslyn).')
    with tempfile.TemporaryDirectory() as temp:
        reference_dir = Path(temp)
        with package('Microsoft.NETFramework.ReferenceAssemblies.net48', cache) as sdk:
            for name in REFERENCES:
                (reference_dir / f'{name}.dll').write_bytes(sdk.read(f'build/.NETFramework/v4.8/{name}.dll'))
        with package('Microsoft.Web.WebView2', cache) as webview:
            for name in WEB:
                data = webview.read('lib/net462/' + name)
                if smoke_only:
                    if data != (output / name).read_bytes():
                        raise ValueError(f'Installed WebView2 assembly differs from pinned SDK: {name}')
                else:
                    (output / name).write_bytes(data)
            for source, name in [
                ('runtimes/win-x64/native/WebView2Loader.dll', 'WebView2Loader.dll'),
                ('LICENSE.txt', 'WebView2-LICENSE.txt'),
                ('NOTICE.txt', 'WebView2-NOTICE.txt'),
            ]:
                data = webview.read(source)
                if smoke_only:
                    if data != (output / name).read_bytes():
                        raise ValueError(f'Installed dependency differs from pinned SDK: {name}')
                else:
                    (output / name).write_bytes(data)
        compiler_args = [
            dotnet,
            str(compiler),
            '/nologo',
            '/nostdlib+',
            '/noconfig',
            '/deterministic+',
            '/platform:x64',
            '/optimize+',
            '/target:winexe',
            '/win32manifest:' + str(HERE / 'app.manifest'),
        ]
        compiler_args += ['/reference:' + str(reference_dir / (name + '.dll')) for name in REFERENCES]
        compiler_args += ['/reference:' + str(output / name) for name in WEB]
        sources = [str(HERE / 'Workbench.cs'), str(HERE / 'StudioSession.cs')]
        if not smoke_only:
            from PIL import Image

            icon = reference_dir / 'studio.ico'
            with Image.open(HERE.parents[2] / 'studio/static/app-icon.png') as artwork:
                artwork.save(icon, sizes=[(16, 16), (32, 32), (48, 48), (256, 256)])
            subprocess.run(
                [*compiler_args, '/win32icon:' + str(icon), '/out:' + str(output / 'KerfKiteNative.exe'), *sources],
                check=True,
            )
        if smoke or smoke_only:
            subprocess.run(
                [
                    *compiler_args,
                    '/main:KerfKite.Smoke',
                    '/out:' + str(output / 'KerfKiteNativeSmoke.exe'),
                    '/reference:' + str(output / 'KerfKiteNative.exe'),
                    str(HERE / 'Smoke.cs'),
                ],
                check=True,
            )


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--smoke', action='store_true', help='also compile the Windows simulator test runner')
    parser.add_argument(
        '--smoke-only', action='store_true', help='compile only the simulator runner against an installed desktop'
    )
    args = parser.parse_args()
    build(args.output, args.smoke, args.smoke_only)
