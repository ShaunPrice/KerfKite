"""Inspect selected text members of a LightBurn user bundle entirely in memory."""
import base64
import binascii
import io
import stat
import zipfile
import zlib
from pathlib import PurePosixPath

from flask import jsonify, request


MAX_ARCHIVE = 4_000_000
MAX_MEMBER = 4_000_000
MAX_TOTAL = 16_000_000
MAX_MEMBERS = 256
EXTENSIONS = {'.lbdev', '.lbprefs', '.clb', '.lbrn', '.lbrn2', '.lbset'}


def _eligible(entry):
    path = PurePosixPath(entry.filename)
    if (path.is_absolute() or '..' in path.parts or '\\' in entry.filename
            or len(entry.filename) > 512 or ':' in entry.filename
            or any(not c.isprintable() for c in entry.orig_filename)
            or path.suffix.lower() not in EXTENSIONS or entry.is_dir()):
        return False
    mode = entry.external_attr >> 16
    return (stat.S_IFMT(mode) in (0, stat.S_IFREG) and not (entry.flag_bits & 1)
            and entry.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED))


def inspect_bundle(payload):
    if not isinstance(payload, dict) or not isinstance(payload.get('content'), str):
        raise ValueError('Provide bundle content in base64')
    encoded = payload['content']
    if len(encoded) > 4 * ((MAX_ARCHIVE + 2) // 3):
        raise ValueError('Bundle exceeds the 4 MB archive limit')
    try:
        data = base64.b64decode(encoded, validate=True)
    except (binascii.Error, ValueError) as exc:
        raise ValueError('Bundle encoding is invalid') from exc
    if not data or len(data) > MAX_ARCHIVE:
        raise ValueError('Choose a bundle of up to 4 MB')
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as bundle:
            members = bundle.infolist()
            if len(members) > MAX_MEMBERS or sum(e.file_size for e in members) > MAX_TOTAL:
                raise ValueError('Bundle exceeds 256 members or 16 MB uncompressed')
            accepted = {i: e for i, e in enumerate(members) if _eligible(e)}
            if any(e.file_size > MAX_MEMBER for e in accepted.values()):
                raise ValueError('Bundle contains a supported file larger than 4 MB')
            if 'index' not in payload:
                return {'files': [{'index': i, 'name': PurePosixPath(e.filename).name}
                                  for i, e in accepted.items()],
                        'skipped': sum(not e.is_dir() for i, e in enumerate(members) if i not in accepted)}
            index = payload['index']
            if type(index) is not int or index not in accepted:
                raise ValueError('Select a supported member of this bundle')
            entry = accepted[index]
            with bundle.open(entry) as member:
                raw = member.read(MAX_MEMBER + 1)
            if len(raw) != entry.file_size or len(raw) > MAX_MEMBER:
                raise ValueError('Bundle member size mismatch')
            if raw.startswith((b'\xff\xfe', b'\xfe\xff')):
                text = raw.decode('utf-16')
            else:
                text = raw.decode('utf-8-sig')
            if '\0' in text:
                raise ValueError('Selected bundle member is binary')
            return {'name': PurePosixPath(entry.filename).name, 'text': text}
    except (zipfile.BadZipFile, UnicodeError, OSError, EOFError, zlib.error) as exc:
        raise ValueError('Cannot read the bundle or selected text member') from exc


def register(app):
    @app.post('/api/import-bundle')
    def import_bundle():
        return jsonify(inspect_bundle(request.get_json()))
