"""Write the Python hosted editor's store (web.sqlite3) that native/tests/web_selftest.cpp moves to the C++ store.

The accounts, token and files are synthetic: alice's password is "correct horse battery staple" (an scrypt hash,
as studio.web_store writes it), bob's "bob's long test password" (a PBKDF2 hash in werkzeug's format), and alice
has the upload token below. Enough files are stored that the files table has interior pages and overflow pages.

Usage:
    PYTHONPATH=$PWD python validation/web_store_fixture.py native/tests/data/web-store
"""

import hashlib
import sys
from pathlib import Path

from werkzeug.security import generate_password_hash

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from studio.web_store import WebStore  # noqa: E402

TOKEN = 'kerfkite-selftest-upload-token-0123456789abcdefghij'


def main():
    folder = Path(sys.argv[1])
    folder.mkdir(parents=True, exist_ok=True)
    database = folder / 'web.sqlite3'
    if database.exists():
        database.unlink()
    store = WebStore(folder)
    store.add_user('alice', 'correct horse battery staple')
    with store.db() as db:
        db.execute(
            'INSERT INTO users VALUES(?,?)',
            ('bob', generate_password_hash("bob's long test password", method='pbkdf2:sha256:600000', salt_length=32)),
        )
        db.execute(
            'INSERT INTO tokens VALUES(?,?,?)', (hashlib.sha256(TOKEN.encode()).hexdigest(), 'alice', 1795000000.5)
        )
        db.execute(
            'INSERT INTO sessions VALUES(?,?,?,?,?)', ('0' * 64, 'alice', 'csrf', 1795000000.0 + 8 * 3600, 1795000000.0)
        )
        db.execute('INSERT INTO attempts VALUES(?,?,?)', ('ip:192.0.2.7', 3, 1795000000.0))
        for index in range(60):
            content = (f'G1 X{index} Y{index * 2}\n' * (index + 3)).encode()
            db.execute(
                'INSERT INTO files VALUES(?,?,?,?,?,?,?)',
                (
                    f'{index:032x}',
                    'alice' if index % 3 else 'bob',
                    f'part-{index}.nc',
                    content,
                    hashlib.sha256(content).hexdigest(),
                    1795000000.0 + index,
                    'web' if index % 2 else 'upload',
                ),
            )
        big = ('<svg xmlns="http://www.w3.org/2000/svg">' + '<path d="M0 0L10 10"/>' * 1200 + '</svg>').encode()
        db.execute(
            'INSERT INTO files VALUES(?,?,?,?,?,?,?)',
            ('f' * 32, 'alice', 'big.svg', big, hashlib.sha256(big).hexdigest(), 1795000100.25, 'web'),
        )
    print(database, database.stat().st_size, 'bytes')


if __name__ == '__main__':
    main()
