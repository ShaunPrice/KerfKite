"""Paired phone status and bounded job actions; no arbitrary commands."""
import math
import secrets
from collections import OrderedDict
from uuid import UUID
from pathlib import Path
from flask import Flask, jsonify, request, send_from_directory


def create_monitor_app(controller, token, phone_jobs=None, revoked=None):
    if not isinstance(token, str) or len(token) < 32:
        raise ValueError('Monitor token must be at least 32 characters')
    app = Flask('cnc_monitor', static_folder=None)
    app.config['MAX_CONTENT_LENGTH'] = 2048
    recent = OrderedDict()

    def begin_action(data, required):
        if revoked is not None and revoked.is_set():
            raise ValueError('Phone pairing has ended.')
        if not isinstance(data, dict):
            raise ValueError('Supply a phone action object.')
        identifier = data.get('request_id')
        if not required and identifier is None:
            return  # Compatibility with the browser monitor.
        try:
            identifier = str(UUID(identifier))
        except (ValueError, TypeError, AttributeError):
            raise ValueError('Supply a unique request ID for this action.') from None
        if identifier in recent:
            raise ValueError('This request has already been handled. Read the controller status.')
        recent[identifier] = True
        if len(recent) > 2048:
            recent.popitem(last=False)

    @app.after_request
    def headers(response):
        response.headers['Cache-Control'] = 'no-store'
        response.headers['Referrer-Policy'] = 'no-referrer'
        response.headers['X-Content-Type-Options'] = 'nosniff'
        response.headers['Content-Security-Policy'] = "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'"
        return response

    @app.before_request
    def authorize():
        if request.path.startswith('/api/'):
            supplied = request.headers.get('Authorization', '')
            if (revoked is not None and revoked.is_set()) or not secrets.compare_digest(supplied.encode('utf-8'), ('Bearer ' + token).encode('utf-8')):
                return jsonify(error='Pair this phone using the monitor link from the Pi'), 401

    @app.errorhandler(ValueError)
    @app.errorhandler(RuntimeError)
    def bad_request(exc):
        return jsonify(error=str(exc)), 400

    @app.post('/api/action')
    def action():
        if phone_jobs is None:
            return jsonify(error='Phone controls are not enabled on this monitor'), 403
        data = request.get_json()
        with phone_jobs.lock:
            begin_action(data, required=False)
            action = data.get('action')
            if action != 'stop':
                phone_jobs.require_current_controls(data.get('control_epoch'))
            if action == 'start':
                phone_jobs.start()
            elif action in ('pause', 'resume', 'stop'):
                if action == 'stop':
                    phone_jobs.invalidate_controls()
                controller.command({'pause': 'hold', 'resume': 'resume', 'stop': 'reset'}[action])
            else:
                raise ValueError('Only start, pause, resume and stop are supported')
        return jsonify(ok=True)

    @app.post('/api/pendant')
    def pendant():
        if phone_jobs is None:
            return jsonify(error='Pendant controls are disabled'), 403
        data = request.get_json()
        with phone_jobs.lock:
            begin_action(data, required=True)
            phone_jobs.require_current_controls(data.get('control_epoch'))
            state = controller.status()
            if not state['connected'] or state['active'] or state['state'] != 'Idle' or state.get('error'):
                raise ValueError('Connect an idle, ready controller before using the pendant.')
            if not state['simulated'] and not phone_jobs.allow_physical:
                raise ValueError('Physical control is disabled on this server.')
            action = data.get('action')
            args = {}
            if action == 'jog':
                axis, distance, feed = (data.get(k) for k in ('axis','distance','feed'))
                if axis not in ('X','Y','Z') or any(type(n) not in (float,int) or not math.isfinite(n) for n in (distance,feed)):
                    raise ValueError('Supply one XYZ axis and finite numeric jog settings.')
                if not .001 <= abs(distance) <= 10 or not .1 <= feed <= 1000:
                    raise ValueError('Jog distance is 0.001–10 mm and feed is 0.1–1000 mm/min.')
                args = dict(axis=axis, distance=distance, feed=feed)
            elif action not in ('home', 'zero'):
                raise ValueError('Only a single incremental jog, home or work zero is supported.')
            if set(data) != {'action','request_id','control_epoch',*args}:
                raise ValueError('Unexpected pendant action fields.')
            phone_jobs.disarm()
            controller.command(action, **args)
        return jsonify(ok=True)

    @app.get('/')
    def index():
        return send_from_directory(Path(__file__).parent / 'static', 'monitor.html')

    @app.get('/monitor.js')
    def script():
        return send_from_directory(Path(__file__).parent / 'static', 'monitor.js')

    @app.get('/monitor.css')
    def css():
        return send_from_directory(Path(__file__).parent / 'static', 'monitor.css')

    @app.get('/api/status')
    def status():
        supplied = request.headers.get('Authorization', '')
        if not secrets.compare_digest(supplied.encode('utf-8'), ('Bearer ' + token).encode('utf-8')):
            return jsonify(error='Pair this phone using the monitor link from the Pi'), 401
        if phone_jobs is not None:
            with phone_jobs.lock:
                state = controller.status()
                phone = dict(**phone_jobs.status(), control_epoch=phone_jobs.control_epoch)
        else:
            state = controller.status()
            phone = {'armed': False}
        # Explicit allowlist: no saved machine configuration or serial-port inventory.
        result = {key: state.get(key) for key in ('connected', 'simulated', 'state', 'active', 'progress', 'position', 'error', 'log', 'protocol')}
        result.update(pendant=phone_jobs is not None and (bool(state.get('simulated')) or phone_jobs.allow_physical), controls=phone_jobs is not None, **phone)
        return jsonify(result)

    return app
