"""Self-signed certificate for the phone pendant listener, pinned by fingerprint in the pairing link.

The listener is reached by private IP address, so no certificate authority can vouch for it. Studio instead
creates a certificate for each pairing and places its SHA-256 fingerprint in the pairing link; Pocket accepts
that one certificate and nothing else. Requires the optional ``cryptography`` package (Studio's web extra).
"""

import datetime
import hashlib
import ipaddress
import os
import ssl
import tempfile
from importlib.util import find_spec
from pathlib import Path


def available():
    return find_spec('cryptography') is not None


def self_signed(hosts, days=30):
    """Return (certificate PEM, private key PEM, SHA-256 fingerprint of the DER certificate as lowercase hex)."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    names, seen = [], set()
    for host in [*hosts, 'localhost', '127.0.0.1']:
        if not isinstance(host, str) or not host or host in seen:
            continue
        seen.add(host)
        try:
            names.append(x509.IPAddress(ipaddress.ip_address(host)))
        except ValueError:
            names.append(x509.DNSName(host))
    private = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'KerfKite Studio phone pendant')])
    now = datetime.datetime.now(datetime.timezone.utc)
    certificate = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(subject)
        .public_key(private.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(minutes=5))
        .not_valid_after(now + datetime.timedelta(days=days))
        .add_extension(x509.SubjectAlternativeName(names), False)
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), True)
        .sign(private, hashes.SHA256())
    )
    der = certificate.public_bytes(serialization.Encoding.DER)
    key = private.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()
    )
    return certificate.public_bytes(serialization.Encoding.PEM), key, hashlib.sha256(der).hexdigest()


def server_context(certificate, key):
    """TLS 1.2+ server context for PEM bytes. The key touches disk only inside a private temporary directory."""
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    with tempfile.TemporaryDirectory() as directory:
        cert_path, key_path = Path(directory) / 'pendant.pem', Path(directory) / 'pendant-key.pem'
        cert_path.write_bytes(certificate)
        with os.fdopen(os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'wb') as handle:
            handle.write(key)
        context.load_cert_chain(cert_path, key_path)
    return context
