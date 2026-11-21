"""Local combined UI and paired-phone fixture; only simulator is available."""

import secrets
import logging
import threading
from werkzeug.serving import make_server
from studio.controller import Controller
from studio.phone_jobs import PhoneJobs
from studio.server import create_app
from studio.monitor import create_monitor_app

logging.getLogger('werkzeug').setLevel(logging.ERROR)
ctl = Controller()
jobs = PhoneJobs(ctl)
monitor = make_server('127.0.0.1', 0, create_monitor_app(ctl, secrets.token_urlsafe(32), jobs), threaded=True)
threading.Thread(target=monitor.serve_forever, daemon=True).start()
server = make_server('127.0.0.1', 50312, create_app(ctl, jobs), threaded=True)
print('workspace=50312 monitor=' + str(monitor.server_port), flush=True)
server.serve_forever()
