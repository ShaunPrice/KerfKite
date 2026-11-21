"""Loopback-only monitor fixture; never opens a physical controller."""

from werkzeug.serving import make_server
from studio.controller import Controller
from studio.monitor import create_monitor_app

ctl = Controller()
ctl.connect('simulator')
server = make_server(
    '127.0.0.1', 0, create_monitor_app(ctl, 'monitor-demo-token-for-local-test-only-0000'), threaded=True
)
print(server.server_port, flush=True)
server.serve_forever()
