"""Build an AppKit workstation helper with the macOS SDK, including cross-architecture builds."""

import argparse
import subprocess
from pathlib import Path


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('arch', choices=['arm64', 'amd64'])
    parser.add_argument('output', type=Path)
    parser.add_argument(
        '--smoke', action='store_true', help='build the simulator UI exercise instead of the application entry point'
    )
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    args.output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            'xcrun',
            'clang',
            '-arch',
            'x86_64' if args.arch == 'amd64' else 'arm64',
            '-mmacosx-version-min=12.0',
            '-fobjc-arc',
            '-O2',
            '-Wall',
            '-Wextra',
            '-Wno-unused-parameter',
            '-framework',
            'AppKit',
            '-framework',
            'WebKit',
            '-framework',
            'UniformTypeIdentifiers',
            *(['-weak_framework', 'ScreenCaptureKit'] if args.smoke else []),
            str(here / 'Workshop.m'),
            str(here / ('Smoke.m' if args.smoke else 'main.m')),
            '-o',
            str(args.output),
        ],
        check=True,
    )
