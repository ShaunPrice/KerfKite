"""Session-scoped phone listener managed by the local workstation."""

import ipaddress
import secrets
import socket
import ssl
import threading

from socketserver import ThreadingMixIn
from wsgiref.simple_server import make_server, WSGIServer, WSGIRequestHandler

from .monitor import create_monitor_app


class ThreadedHTTPServer(ThreadingMixIn, WSGIServer):
    daemon_threads = True

    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(10)
        return connection, address


class MonitorRequestHandler(WSGIRequestHandler):
    def log_message(self, fmt, *args):
        pass


class PhoneMonitor:
    def __init__(self, controller, jobs, port=8766, certificate=None, key=None):
        self.controller, self.jobs = controller, jobs
        self.port, self.certificate, self.key = port, certificate, key
        self.lock = threading.RLock()
        self.listener = self.revoked = self.token = self.worker = None
        self.bound_port = port

    def info(self):
        with self.lock:
            addresses = []
            if self.listener:
                try:
                    ips = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
                    addresses = sorted({ip[4][0] for ip in ips if not ipaddress.ip_address(ip[4][0]).is_loopback})
                except OSError:
                    pass
            return dict(
                enabled=self.listener is not None,
                port=self.bound_port,
                hosts=addresses,
                scheme='https' if self.certificate else 'http',
                token=self.token,
            )

    def enable(self):
        with self.lock:
            if self.listener:
                return self.info()
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
            self.listener, self.token, self.revoked = listener, token, revoked
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
                self.listener = self.worker = self.revoked = self.token = None
            return self.info()
