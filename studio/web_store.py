"""Accounts, revocable sessions, scoped upload tokens and private artifact storage."""

from contextlib import contextmanager
import hashlib
import re
import secrets
import sqlite3
import time
from pathlib import Path
from werkzeug.security import generate_password_hash, check_password_hash


def digest(value):
    return hashlib.sha256(value.encode('utf-8')).hexdigest()


def username(value):
    if not isinstance(value, str) or not re.fullmatch(r'[a-zA-Z0-9_.@-]{1,64}', value):
        raise ValueError('Username must be 1–64 letters, digits or _.@-')
    return value.lower()


def password_hash(password):
    if not isinstance(password, str) or not 15 <= len(password) <= 256:
        raise ValueError('Choose a password of 15–256 characters or use the secure generator')
    return generate_password_hash(password, method='scrypt:32768:8:1', salt_length=32)


class WebStore:
    def __init__(self, data_dir):
        self.root = Path(data_dir).resolve()
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.path = self.root / 'web.sqlite3'
        with self.db() as db:
            db.executescript('''
              CREATE TABLE IF NOT EXISTS users(name TEXT PRIMARY KEY, password TEXT NOT NULL);
              CREATE TABLE IF NOT EXISTS sessions(id TEXT PRIMARY KEY, user TEXT NOT NULL, csrf TEXT NOT NULL, expires REAL NOT NULL, last_seen REAL NOT NULL);
              CREATE TABLE IF NOT EXISTS attempts(key TEXT PRIMARY KEY, count INTEGER NOT NULL, start REAL NOT NULL);
              CREATE TABLE IF NOT EXISTS tokens(id TEXT PRIMARY KEY, user TEXT NOT NULL, created REAL NOT NULL);
              CREATE TABLE IF NOT EXISTS files(id TEXT PRIMARY KEY, user TEXT NOT NULL, name TEXT NOT NULL, content BLOB NOT NULL, sha TEXT NOT NULL, created REAL NOT NULL, source TEXT NOT NULL);
            ''')
        self.path.chmod(0o600)
        # Unknown accounts take the same password-hash verification path.
        self.dummy = generate_password_hash(secrets.token_urlsafe(32), method='scrypt:32768:8:1')

    @contextmanager
    def db(self):
        db = sqlite3.connect(self.path, timeout=10)
        db.row_factory = sqlite3.Row
        try:
            with db:
                yield db
        finally:
            db.close()

    def has_users(self):
        with self.db() as db:
            return db.execute('SELECT 1 FROM users LIMIT 1').fetchone() is not None

    def add_user(self, name, password, replace=False):
        name = username(name)
        hashed = password_hash(password)
        with self.db() as db:
            if replace:
                if not db.execute('SELECT 1 FROM users WHERE name=?', (name,)).fetchone():
                    raise ValueError('Unknown user')
                db.execute('UPDATE users SET password=? WHERE name=?', (hashed, name))
                db.execute('DELETE FROM sessions WHERE user=?', (name,))
                db.execute('DELETE FROM tokens WHERE user=?', (name,))
            else:
                try:
                    db.execute('INSERT INTO users VALUES(?,?)', (name, hashed))
                except sqlite3.IntegrityError:
                    raise ValueError('User already exists; use reset-password') from None

    def check_login(self, name, password, address):
        now = time.time()
        name = name.lower() if isinstance(name, str) else ''
        keys = ['ip:' + str(address), 'user:' + name[:64]]
        # Reserve an attempt before hashing so concurrent requests cannot bypass the limit.
        with self.db() as db:
            db.execute('BEGIN IMMEDIATE')
            db.execute('DELETE FROM attempts WHERE start<?', (now - 900,))
            for key in keys:
                row = db.execute('SELECT count FROM attempts WHERE key=?', (key,)).fetchone()
                if row and row['count'] >= 10:
                    return None, True
            for key in keys:
                db.execute(
                    'INSERT INTO attempts VALUES(?,1,?) ON CONFLICT(key) DO UPDATE SET count=count+1', (key, now)
                )
            row = db.execute('SELECT password FROM users WHERE name=?', (name,)).fetchone()
        if not isinstance(password, str) or len(password) > 256:
            password = ''
        valid = check_password_hash(row['password'] if row else self.dummy, password)
        if not row or not valid:
            return None, False
        sid = secrets.token_urlsafe(32)
        csrf = secrets.token_urlsafe(32)
        with self.db() as db:
            db.execute('BEGIN IMMEDIATE')
            current = db.execute('SELECT password FROM users WHERE name=?', (name,)).fetchone()
            if not current or current['password'] != row['password']:
                return None, False
            db.execute('DELETE FROM sessions WHERE expires<? OR last_seen<?', (now, now - 1800))
            db.execute('INSERT INTO sessions VALUES(?,?,?,?,?)', (digest(sid), name, csrf, now + 8 * 3600, now))
            # Release only this successful reservation; retain unrelated failed attempts.
            db.execute('UPDATE attempts SET count=max(0,count-1) WHERE key=?', ('ip:' + str(address),))
            db.execute('DELETE FROM attempts WHERE key=?', ('user:' + name,))
        return {'sid': sid, 'csrf': csrf, 'user': name}, False

    def session(self, sid):
        if not isinstance(sid, str) or len(sid) > 128:
            return None
        now = time.time()
        with self.db() as db:
            row = db.execute(
                'SELECT * FROM sessions WHERE id=? AND expires>? AND last_seen>?', (digest(sid), now, now - 1800)
            ).fetchone()
            if row:
                db.execute('UPDATE sessions SET last_seen=? WHERE id=?', (now, row['id']))
        return dict(row) if row else None

    def logout(self, sid):
        with self.db() as db:
            db.execute('DELETE FROM sessions WHERE id=?', (digest(sid),))

    def issue_token(self, name):
        name = username(name)
        token = secrets.token_urlsafe(32)
        with self.db() as db:
            if not db.execute('SELECT 1 FROM users WHERE name=?', (name,)).fetchone():
                raise ValueError('Unknown user')
            db.execute('INSERT INTO tokens VALUES(?,?,?)', (digest(token), name, time.time()))
        return token

    def token_user(self, token):
        if len(token) > 128:
            return None
        with self.db() as db:
            row = db.execute('SELECT user FROM tokens WHERE id=?', (digest(token),)).fetchone()
        return row['user'] if row else None

    def revoke_tokens(self, name):
        with self.db() as db:
            db.execute('DELETE FROM tokens WHERE user=?', (username(name),))

    def put_file(self, user, name, content, source):
        identifier = secrets.token_hex(16)
        sha = hashlib.sha256(content).hexdigest()
        with self.db() as db:
            db.execute('BEGIN IMMEDIATE')
            count, size = db.execute(
                'SELECT count(*),coalesce(sum(length(content)),0) FROM files WHERE user=?', (user,)
            ).fetchone()
            if count >= 200 or size + len(content) > 64 * 1024 * 1024:
                raise ValueError('Account file quota reached; download and delete unused files')
            db.execute(
                'INSERT INTO files VALUES(?,?,?,?,?,?,?)', (identifier, user, name, content, sha, time.time(), source)
            )
        return {'id': identifier, 'name': name, 'sha256': sha, 'bytes': len(content), 'status': 'stored_for_review'}

    def files(self, user):
        with self.db() as db:
            return [
                dict(r)
                for r in db.execute(
                    'SELECT id,name,sha AS sha256,length(content) AS bytes,created,source FROM files WHERE user=? ORDER BY created DESC',
                    (user,),
                )
            ]

    def file(self, user, identifier):
        with self.db() as db:
            r = db.execute('SELECT * FROM files WHERE user=? AND id=?', (user, identifier)).fetchone()
        return dict(r) if r else None

    def delete_file(self, user, identifier):
        with self.db() as db:
            db.execute('DELETE FROM files WHERE user=? AND id=?', (user, identifier))
