"""Build a reproducible ZIP of the KerfKite workshop assistant skill."""

import argparse
from pathlib import Path
from tempfile import NamedTemporaryFile
from zipfile import ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parents[1]
SKILL = ROOT / 'skills' / 'kerfkite-workshop'


def build(destination):
    entries = {
        'LICENSE': ROOT / 'LICENSE',
        'SKILL.md': SKILL / 'SKILL.md',
        'agents/openai.yaml': SKILL / 'agents' / 'openai.yaml',
    }
    destination.parent.mkdir(parents=True, exist_ok=True)
    with NamedTemporaryFile(dir=destination.parent, delete=False) as temporary:
        stage = Path(temporary.name)
    try:
        with ZipFile(stage, 'w') as archive:
            for name, source in entries.items():
                # Fixed metadata and uncompressed text produce identical bytes across hosts.
                entry = ZipInfo(f'{SKILL.name}/{name}', date_time=(1980, 1, 1, 0, 0, 0))
                entry.create_system = 3
                entry.external_attr = 0o100644 << 16
                archive.writestr(entry, source.read_bytes())
        stage.replace(destination)
    finally:
        stage.unlink(missing_ok=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    build(parser.parse_args().destination)
