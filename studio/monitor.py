"""Paired phone status and bounded job actions; no arbitrary commands."""
import secrets
from pathlib import Path
from flask import Flask, jsonify, request, send_from_directory


def create_monitor_app(controller, token, phone_jobs=None):
    if not isinstance(token, str) or len(token) < 32:
        raise ValueError('Monitor token must be at least 32 characters')
    app = Flask('cnc_monitor', static_folder=None)

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
            if not secrets.compare_digest(supplied.encode('utf-8'), ('Bearer ' + token).encode('utf-8')):
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
        action = data.get('action')
        if action == 'start':
            phone_jobs.start()
        elif action in ('pause', 'resume', 'stop'):
            if action == 'stop':
                phone_jobs.disarm()
            controller.command({'pause': 'hold', 'resume': 'resume', 'stop': 'reset'}[action])
        else:
            raise ValueError('Only start, pause, resume and stop are supported')
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
        state = controller.status()
        # Explicit allowlist: no saved machine configuration or serial-port inventory.
        result = {key: state.get(key) for key in ('connected', 'simulated', 'state', 'active', 'progress', 'position', 'error', 'log')}
        result.update(controls=phone_jobs is not None, armed=phone_jobs.status()['armed'] if phone_jobs else False)
        return jsonify(result)

    return app
