"""Read the pairing QR codes Studio draws with the decoder Pocket uses.

    python validation/pairing_qr_check.py --zxing path/to/core-3.5.3.jar

Studio's own encoder (studio/static/pairing-qr.js) writes the symbols for a pinned and a plain pairing link.
Pocket's QrFrames class then decodes them from simulated camera frames: several sizes, a quarter turn,
off-centre in the scanning square, and rows padded as a camera pads them. A code outside the square and an
empty frame must read as nothing.

Needs Node.js, a JDK, and the ZXing core jar that the Android build uses. After an Android build, Gradle keeps
the jar under ~/.gradle/caches/modules-2/files-2.1/com.google.zxing/core/. No phone or camera is involved.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'android/app/src'
PACKAGE = 'au/cncstudio/pocket'
TOKEN = 'kT3vYw0Qm1x_Zr8N-5sHc2LbJ7dUeGfPaVo4iWq9XyA'
LINKS = [
    'https://192.168.1.20:8766/#token=' + TOKEN + '&fp=' + 'ab' * 32,
    'http://192.168.1.20:8766/#token=' + TOKEN,
    'https://[fd00::20]:8766/#token=' + TOKEN + '&fp=' + '0f' * 32,
]
ENCODE = """
const fs = require('node:fs'), vm = require('node:vm'), path = require('node:path');
const [root, out, ...links] = process.argv.slice(1);
const scope = vm.createContext({});
for (const script of ['vendor/qrcodegen.js', 'pairing-qr.js'])
  vm.runInContext(fs.readFileSync(path.join(root, script), 'utf8'), scope);
fs.writeFileSync(out, links.map(link => [link, ...scope.KerfPairingQr.modules(link)
  .map(row => Array.from(row, dark => dark ? '1' : '0').join(''))].join('\\n')).join('\\n\\n') + '\\n');
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--zxing', required=True, type=Path, help='ZXing core jar used by the Android build')
    args = parser.parse_args()
    for tool in ('node', 'javac', 'java'):
        if not shutil.which(tool):
            raise SystemExit(f'{tool} is required.')
    if not args.zxing.is_file():
        raise SystemExit(f'ZXing core jar not found: {args.zxing}')
    with tempfile.TemporaryDirectory() as scratch:
        symbols = Path(scratch) / 'symbols.txt'
        subprocess.run(['node', '-e', ENCODE, str(ROOT / 'studio/static'), str(symbols), *LINKS], check=True)
        files = [
            SOURCES / 'main/java' / PACKAGE / 'QrFrames.java',
            SOURCES / 'test/java' / PACKAGE / 'QrFramesCheck.java',
        ]
        classes = Path(scratch) / 'classes'
        subprocess.run(['javac', '-cp', str(args.zxing), '-d', str(classes), *map(str, files)], check=True)
        classpath = os.pathsep.join([str(classes), str(args.zxing)])
        subprocess.run(['java', '-cp', classpath, 'au.cncstudio.pocket.QrFramesCheck', str(symbols)], check=True)
    print('PASS Pocket reads the pairing codes Studio draws.')


if __name__ == '__main__':
    main()
