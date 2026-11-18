import argparse
import math
import atexit
import secrets
from flask import Flask, jsonify, request, send_from_directory, g
from .cam import generate, number
from .design_api import register_design_routes
from .device import DeviceController as Controller
from .profiles import PROFILES
from .phone_jobs import PhoneJobs
from .phone_monitor_service import PhoneMonitor
from .protocols import get_protocol


def create_app(controller=None, phone_jobs=None, allow_hardware=False, phone_monitor=None):
    app = Flask(__name__, static_folder='static')
    app.config['MAX_CONTENT_LENGTH'] = 8 * 1024 * 1024
    ctl = controller or Controller()
    jobs = phone_jobs or PhoneJobs(ctl, allow_physical=allow_hardware)
    token = secrets.token_urlsafe(32)
    atexit.register(ctl.disconnect)
    monitor = phone_monitor or PhoneMonitor(ctl, jobs)
    atexit.register(monitor.disable)

    @app.before_request
    def protect():
        # Local UI only. Host validation blocks DNS rebinding; token prevents cross-origin writes.
        if request.host.split(':')[0] not in ('127.0.0.1', 'localhost'):
            return jsonify(error='Use localhost on the Raspberry Pi'), 403
        if request.method == 'POST' and request.headers.get('X-Studio-Token') != token:
            return jsonify(error='Reload this page before using controls'), 403
        if request.method=='POST' and request.path in ('/api/connect','/api/disconnect','/api/arm','/api/disarm','/api/run','/api/command','/api/head'):
            ctl.lock.acquire();g.machine_lock=True

    @app.teardown_request
    def release_machine_lock(error):
        if getattr(g,'machine_lock',False):g.machine_lock=False;ctl.lock.release()

    @app.errorhandler(ValueError)
    @app.errorhandler(RuntimeError)
    def bad_request(exc):
        return jsonify(error=str(exc)), 400

    @app.get('/<path:asset>')
    def asset(asset):
        return send_from_directory(app.static_folder,asset)

    @app.get('/')
    def index():
        return send_from_directory(app.static_folder, 'index.html')

    @app.get('/api/session')
    def session():
        return jsonify(token=token, depth_paths=True, advanced_cam=True, holding_tabs=True, laser_patterns=True, router_entries=True, hardware_enabled=allow_hardware)

    @app.get('/api/status')
    def status():
        return jsonify(ctl.status())

    @app.get('/api/profiles')
    def profiles():
        return jsonify(PROFILES)

    @app.get('/api/ports')
    def ports():
        return jsonify(ctl.ports())

    def validate_machine(project, job):
        status=ctl.status()
        profile=get_protocol(project.get('protocol','grbl'))
        if not profile.live_transport:raise ValueError('File export only; live execution is unavailable for '+profile.name)
        if project.get('protocol','grbl')!=status.get('protocol','grbl'):raise ValueError('Job protocol differs from connected controller')
        if not status.get('simulated') and profile.family=='grbl':
            settings=status.get('settings',{})
            if any(not math.isfinite(float(v)) for v in settings.values()):raise ValueError('Invalid controller settings readback; reconnect')
            laser=settings.get('32',settings.get('$32'));maximum=settings.get('30',settings.get('$30'))
            if laser is None or maximum is None:raise ValueError('Wait for GRBL $30/$32 readback before running')
            if float(maximum)<=0:raise ValueError('Invalid controller $30 maximum')
            if float(laser)!=(1 if project['mode']=='laser' else 0):raise ValueError('Controller $32 does not match the selected head; configure GRBL separately')
            if float(project['power'])>float(maximum):raise ValueError('Power/RPM exceeds controller $30')
            for field,key in [('width','130'),('height','131')]:
                if key in settings and float(project[field])>float(settings[key]):raise ValueError('Work area exceeds configured controller travel')
            for operation in project.get('operations',[]):
                if float(operation.get('power',project['power']))>float(maximum):raise ValueError('Operation power exceeds controller $30')
        if not status.get('simulated') and project['mode']!='laser':
            depth=max([-float(segment[4]) for segment in job['segments']]+[0])
            stock=number(project.get('stock_thickness'),'Stock thickness',.01,200)
            if depth>stock:raise ValueError('Cut exceeds entered stock thickness')
            settings=status.get('settings',{})
            safe=max([float(project.get('safe',5))]+[float(op.get('safe',project.get('safe',5))) for op in project.get('operations',[])])
            if '132' in settings and depth+safe>float(settings['132']):raise ValueError('Depth plus clearance exceeds configured Z travel')
        return status

    @app.post('/api/phone-monitor')
    def configure_phone():
        action = (request.get_json() or {}).get('action')
        if action not in ('status','enable','disable'):
            raise ValueError('Select a phone pairing action.')
        try:
            info = {'status': monitor.info, 'enable': monitor.enable, 'disable': monitor.disable}[action]()
        except OSError as exc:
            raise ValueError('Phone listener could not start. Check the monitor port and TLS certificate.') from exc
        response = jsonify(info)
        response.headers['Cache-Control'] = 'no-store'
        return response

    @app.post('/api/arm')
    def arm_phone():
        data = request.get_json()
        if data.get('confirmed') is not True:
            raise ValueError('Confirm setup at the Pi before arming')
        job=generate(data['project'])
        validate_machine(data['project'],job)
        jobs.arm(job['lines'], validator=lambda:validate_machine(data['project'],job))
        return jsonify(ok=True)

    @app.post('/api/disarm')
    def disarm_phone():
        jobs.disarm()
        return jsonify(ok=True)

    @app.post('/api/connect')
    def connect():
        port = request.get_json()['port']
        if port != 'simulator' and not allow_hardware:
            raise ValueError('Physical ports are disabled. Start with --enable-hardware only after completing machine setup and commissioning; otherwise use Simulator.')
        jobs.disarm()
        if hasattr(ctl,'select_protocol'):ctl.select_protocol(request.get_json().get('protocol','grbl'))
        ctl.connect(port)
        return jsonify(ok=True)

    @app.post('/api/disconnect')
    def disconnect():
        jobs.disarm()
        ctl.disconnect()
        return jsonify(ok=True)

    @app.post('/api/generate')
    def preview():
        return jsonify(generate(request.get_json()))

    @app.post('/api/run')
    def run():
        data = request.get_json()
        if data.get('confirmed') is not True:
            raise ValueError('Confirm work zero, tool and clear work area')
        job = generate(data['project'])
        validate_machine(data['project'],job)
        jobs.disarm()
        ctl.start(job['lines'])
        return jsonify(ok=True)

    @app.post('/api/head')
    def apply_head():
        data=request.get_json();mode=data.get('mode')
        if mode not in ('router','laser','knife') or data.get('confirmed') is not True:raise ValueError('Confirm the mounted head before applying mode')
        jobs.disarm()
        state=ctl.status()
        if not state.get('connected') or state.get('active') or state.get('state')!='Idle':raise ValueError('Head mode changes require an idle connected controller')
        commands=['M5']
        if state.get('protocol')=='fluidnc':raise ValueError('Configure the mounted spindle or laser in FluidNC and reconnect for readback; Studio does not change FluidNC YAML')
        if get_protocol(state.get('protocol','grbl')).family=='grbl':commands += ['$32='+('1' if mode=='laser' else '0'),'$$']
        ctl.start(commands)
        return jsonify(ok=True,message='Outputs off and mode requested. Wait for readback; re-establish tool Z/focus and work origin.')

    @app.post('/api/command')
    def command():
        data = request.get_json()
        if data.get('action') not in ('hold', 'resume'):
            jobs.disarm()
        ctl.command(**data)
        return jsonify(ok=True)

    register_design_routes(app)
    return app


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--enable-hardware', action='store_true', help='Operator opt-in to physical controller ports; requires machine setup and supervised commissioning')
    parser.add_argument('--monitor', action='store_true', help='Enable paired phone monitor and job controls on the LAN')
    parser.add_argument('--monitor-port', type=int, default=8766)
    parser.add_argument('--monitor-cert', help='HTTPS certificate for the phone monitor')
    parser.add_argument('--monitor-key', help='Matching HTTPS private key')
    args = parser.parse_args()
    ctl = Controller()
    jobs = PhoneJobs(ctl, allow_physical=args.enable_hardware)
    if bool(args.monitor_cert) != bool(args.monitor_key):
        parser.error('Provide both --monitor-cert and --monitor-key')
    monitor = PhoneMonitor(ctl, jobs, args.monitor_port, args.monitor_cert, args.monitor_key)
    if args.monitor:
        pairing = monitor.enable()
        print(f"Phone pairing: {pairing['scheme']}://<PI-LAN-IP>:{pairing['port']}/#token={pairing['token']}", flush=True)
        print('Open Control → Phone pendant to view or revoke pairing.', flush=True)
    create_app(ctl, jobs, allow_hardware=args.enable_hardware, phone_monitor=monitor).run(host='127.0.0.1', port=args.port, threaded=True, debug=False)


if __name__ == '__main__':
    main()
