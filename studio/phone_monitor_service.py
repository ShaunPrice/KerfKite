"""Session-scoped phone listener managed by the local workstation."""

import ipaddress
import secrets
import socket
import ssl
import sys
import threading

from socketserver import ThreadingMixIn
from wsgiref.simple_server import make_server, WSGIServer, WSGIRequestHandler

from . import monitor_tls
from .monitor import create_monitor_app


class ThreadedHTTPServer(ThreadingMixIn, WSGIServer):
    daemon_threads = True

    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(10)
        return connection, address

    def handle_error(self, request, client_address):
        # A phone that drops the connection, or fails the TLS handshake, is routine; anything else is reported.
        if not isinstance(sys.exc_info()[1], OSError):
            super().handle_error(request, client_address)


class PinnedHTTPSServer(ThreadedHTTPServer):
    """TLS on the standard-library listener. The handshake runs in the request thread, not the accept loop."""

    def process_request_thread(self, request, client_address):
        try:
            request.do_handshake()
        except (OSError, ssl.SSLError):
            self.shutdown_request(request)
            return
        super().process_request_thread(request, client_address)


class MonitorRequestHandler(WSGIRequestHandler):
    def log_message(self, fmt, *args):
        pass


class PhoneMonitor:
    def __init__(self, controller, jobs, port=8766, certificate=None, key=None, plaintext=False):
        self.controller, self.jobs = controller, jobs
        self.port, self.certificate, self.key = port, certificate, key
        # plaintext keeps the HTTP listener for Pocket 0.4.1 and earlier, which cannot verify a pinned certificate.
        self.plaintext = plaintext
        self.fingerprint = None
        self.lock = threading.RLock()
        self.listener = self.revoked = self.token = self.worker = None
        self.bound_port = port

    @staticmethod
    def routed_address():
        """The address this machine uses to reach other networks, or None. Connecting a UDP socket sends nothing."""
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.connect(('192.0.2.1', 9))  # documentation address: only the route is looked up
                address = probe.getsockname()[0]
        except OSError:
            return None
        return None if ipaddress.ip_address(address).is_loopback or address == '0.0.0.0' else address  # noqa: S104

    @staticmethod
    def addresses():
        """This machine's IPv4 addresses, the likeliest one for a phone first.

        The workstation offers the first address in the pairing link and its QR code. Virtual adapters (WSL,
        Hyper-V, container bridges) often sort first numerically but are unreachable from a phone, so the address
        on the default route leads and self-assigned 169.254 addresses trail.
        """
        try:
            ips = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
            found = {ip[4][0] for ip in ips if not ipaddress.ip_address(ip[4][0]).is_loopback}
        except OSError:
            found = set()
        routed = PhoneMonitor.routed_address()
        if routed:
            found.add(routed)
        return sorted(
            found, key=lambda ip: (ip != routed, ipaddress.ip_address(ip).is_link_local, ipaddress.ip_address(ip))
        )

    def pinned(self):
        """Whether enabling the listener creates a certificate pinned by the pairing link."""
        return not self.certificate and not self.plaintext and monitor_tls.available()

    def info(self):
        with self.lock:
            encryption = 'certificate' if self.certificate else 'pinned' if self.fingerprint else 'none'
            return dict(
                enabled=self.listener is not None,
                port=self.bound_port,
                hosts=self.addresses() if self.listener else [],
                scheme='http' if encryption == 'none' else 'https',
                token=self.token,
                encryption=encryption,
                fingerprint=self.fingerprint,
                fragment=self.fragment(),
                # What the next enable will do, so the workstation can offer the unencrypted option.
                can_encrypt=bool(self.certificate) or monitor_tls.available(),
                plaintext=self.plaintext,
            )

    def set_plaintext(self, plaintext):
        """Choose HTTP or the pinned certificate for the next pairing. Refused while a pairing is live."""
        if not isinstance(plaintext, bool):
            raise ValueError('Choose encrypted or unencrypted pairing.')
        with self.lock:
            if plaintext != self.plaintext:
                if self.listener:
                    raise ValueError('Disable phone pairing before changing its encryption.')
                if plaintext and self.certificate:
                    raise ValueError('This server was started with a monitor certificate; pairing stays on HTTPS.')
                self.plaintext = plaintext

    def fragment(self):
        """The pairing link's fragment: the bearer token and, for a pinned listener, its certificate fingerprint."""
        if not self.token:
            return None
        return 'token=' + self.token + ('&fp=' + self.fingerprint if self.fingerprint else '')

    def enable(self):
        with self.lock:
            if self.listener:
                return self.info()
            fingerprint = None
            token = secrets.token_urlsafe(32)
            revoked = threading.Event()
            app = create_monitor_app(self.controller, token, self.jobs, revoked)
            address = ('0.0.0.0', self.port)
            if self.certificate:
                try:
                    from cheroot.wsgi import Server
                    from cheroot.ssl.builtin import BuiltinSSLAdapter
                except ImportError as exc:
                    raise ValueError('Install the Studio web extra for the HTTPS phone listener.') from exc
                listener = Server(address, app, numthreads=4, max=8)
                listener.ssl_adapter = BuiltinSSLAdapter(self.certificate, self.key)
                listener.ssl_adapter.context.minimum_version = ssl.TLSVersion.TLSv1_2
                listener.prepare()
                run = listener.serve
                port = listener.socket.getsockname()[1]
            elif self.pinned():
                try:
                    pem, private_key, fingerprint = monitor_tls.self_signed([socket.gethostname(), *self.addresses()])
                    context = monitor_tls.server_context(pem, private_key)
                except (ImportError, ValueError, ssl.SSLError) as exc:
                    raise ValueError('The phone listener could not create its certificate.') from exc
                listener = make_server(
                    *address, app, server_class=PinnedHTTPSServer, handler_class=MonitorRequestHandler
                )
                listener.socket = context.wrap_socket(listener.socket, server_side=True, do_handshake_on_connect=False)
                listener.base_environ['HTTPS'] = 'on'
                run = listener.serve_forever
                port = listener.server_port
            else:
                listener = make_server(
                    *address, app, server_class=ThreadedHTTPServer, handler_class=MonitorRequestHandler
                )
                run = listener.serve_forever
                port = listener.server_port
            self.worker = threading.Thread(target=run, daemon=True)
            try:
                self.worker.start()
            except RuntimeError:
                if self.certificate:
                    listener.stop()
                else:
                    listener.server_close()
                raise
            self.bound_port = port
            self.listener, self.token, self.revoked, self.fingerprint = listener, token, revoked, fingerprint
            return self.info()

    def disable(self):
        with self.lock:
            if self.listener:
                with self.jobs.lock:
                    self.revoked.set()
                    self.jobs.invalidate_controls()
                if self.certificate:
                    self.listener.stop()
                else:
                    self.listener.shutdown()
                    self.listener.server_close()
                self.worker.join(2)
                self.listener = self.worker = self.revoked = self.token = self.fingerprint = None
            return self.info()
