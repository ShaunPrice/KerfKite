"""Inspect selected text members of a LightBurn user bundle entirely in memory.

The native core reads bundles to the bit (native/core/src/bundle_import.cpp) and is used where the compiled
extension is installed; this module is the reference the parity harness compares it with, and the reader without
the extension. The core declines nothing: where zipfile raises something other than its read errors (a newer
entry version, patched or strongly encrypted data, a header before the data) it refuses the bundle as unreadable.
"""

import base64
import binascii
import io
import stat
import zipfile
import zlib
from pathlib import PurePosixPath

from flask import jsonify, request

try:  # The native core's port of this module, in the compiled extension.
    from ._relief_native import core_cam as _core_cam, core_cam_text as _core_cam_text
except ImportError:
    _core_cam = _core_cam_text = None

MAX_ARCHIVE = 4_000_000
MAX_MEMBER = 4_000_000
MAX_TOTAL = 16_000_000
MAX_MEMBERS = 256
EXTENSIONS = {'.lbdev', '.lbprefs', '.clb', '.lbrn', '.lbrn2', '.lbset'}


def _eligible(entry):
    path = PurePosixPath(entry.filename)
    if (
        path.is_absolute()
        or '..' in path.parts
        or '\\' in entry.filename
        or len(entry.filename) > 512
        or ':' in entry.filename
        or any(not c.isprintable() for c in entry.orig_filename)
        or path.suffix.lower() not in EXTENSIONS
        or entry.is_dir()
    ):
        return False
    mode = entry.external_attr >> 16
    return (
        stat.S_IFMT(mode) in (0, stat.S_IFREG)
        and not (entry.flag_bits & 1)
        and entry.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)
    )


def import_json(payload):
    """inspect_bundle(payload) as JSON text written by the core, for a web response, or None: then inspect_bundle."""
    if _core_cam_text is None:
        return None
    try:
        return _core_cam_text('bundle-import', payload)
    except NotImplementedError:
        return None


def inspect_bundle(payload, *, backend='auto'):
    """The supported members of a bundle, or the text of one. backend='auto' reads in the native core where it
    can and here otherwise; 'python' reads here; 'native' raises NotImplementedError where the core cannot answer."""
    if backend not in ('auto', 'python', 'native'):
        raise ValueError('Bundle import backend must be auto, python or native')
    if backend != 'python':
        if _core_cam is not None:
            try:
                answer = _core_cam('bundle-import', payload)
            except NotImplementedError:
                if backend == 'native':
                    raise
                answer = None
            if answer is not None:
                return answer
            if backend == 'native':
                raise NotImplementedError('The bundle request cannot be passed to the native core unchanged')
        elif backend == 'native':
            raise NotImplementedError('The native bundle import is not installed')
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
                return {
                    'files': [{'index': i, 'name': PurePosixPath(e.filename).name} for i, e in accepted.items()],
                    'skipped': sum(not e.is_dir() for i, e in enumerate(members) if i not in accepted),
                }
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
        payload = request.get_json()
        # The core's own JSON text is the response; the Python answers what the core declines.
        text = import_json(payload)
        if text is not None:
            return app.response_class(text, mimetype='application/json')
        return jsonify(inspect_bundle(payload, backend='python'))
