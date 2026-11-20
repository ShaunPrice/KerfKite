"""Operator-configured destinations only; never fetch a URL supplied by a browser."""

import hashlib
import json
import ssl
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from werkzeug.utils import secure_filename


def validate_upload(data):
    if not isinstance(data, dict):
        raise ValueError('Upload must be a JSON object')
    name = data.get('name', '')
    content = data.get('content')
    if not isinstance(name, str) or len(name) > 128 or name != secure_filename(name):
        raise ValueError('Use a simple file name without folders')
    if not name.lower().endswith(('.nc', '.gcode', '.svg', '.cncstudio.json', '.templates.json', '.paths3d.json')):
        raise ValueError('Supported files: .nc, .gcode, .svg, .cncstudio.json, .templates.json')
    if not isinstance(content, str) or '\x00' in content:
        raise ValueError('Upload must contain text, without NUL characters')
    encoded = content.encode('utf-8')
    if not 1 <= len(encoded) <= 8 * 1024 * 1024:
        raise ValueError('File must contain 1 byte to 8 MB')
    expected = data.get('sha256')
    if expected and expected != hashlib.sha256(encoded).hexdigest():
        raise ValueError('Upload checksum mismatch')
    return name, encoded


def validate_destination(entry):
    parsed = urllib.parse.urlsplit(entry.get('url', ''))
    if (
        parsed.scheme not in ('https', 'http')
        or not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.fragment
        or parsed.query
        or parsed.path not in ('', '/')
    ):
        raise ValueError('Destination must be an HTTP/S server origin, without credentials, path or query')
    if parsed.scheme == 'http' and not entry.get('allow_http', False):
        raise ValueError('Plain HTTP needs explicit --allow-http; HTTPS is the default')
    if not isinstance(entry.get('token'), str) or len(entry['token']) < 32:
        raise ValueError('Configure the receiver-generated upload token')
    if entry.get('ca_file') and not Path(entry['ca_file']).is_file():
        raise ValueError('CA certificate file does not exist')
    return entry


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def send_to_receiver(entry, name, content):
    validate_destination(entry)
    context = ssl.create_default_context(cafile=entry.get('ca_file') or None)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({}), NoRedirect(), urllib.request.HTTPSHandler(context=context)
    )
    sha = hashlib.sha256(content).hexdigest()
    payload = json.dumps({'name': name, 'content': content.decode('utf-8'), 'sha256': sha}).encode()
    req = urllib.request.Request(
        entry['url'].rstrip('/') + '/api/receive',
        data=payload,
        headers={'Authorization': 'Bearer ' + entry['token'], 'Content-Type': 'application/json'},
        method='POST',
    )
    try:
        with opener.open(req, timeout=15) as response:
            raw = response.read(65537)
            if len(raw) > 65536:
                raise ValueError('Receiver response exceeded limit')
            result = json.loads(raw)
    except (urllib.error.URLError, ValueError, TimeoutError, OSError) as exc:
        # Never echo credentials or claim a retry is safe after an ambiguous POST.
        raise ValueError(
            'Upload result could not be verified. Check the receiver before retrying; verify its URL, token and TLS trust.'
        ) from exc
    if (
        not isinstance(result, dict)
        or result.get('sha256') != sha
        or result.get('bytes') != len(content)
        or result.get('status') != 'stored_for_review'
    ):
        raise ValueError('Receiver did not confirm the expected file checksum and size; inspect it before retrying')
    return {'id': result.get('id'), 'name': name, 'sha256': sha, 'bytes': len(content), 'status': 'stored_for_review'}
