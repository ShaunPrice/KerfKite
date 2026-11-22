import argparse
import math
import atexit
import secrets
from flask import Flask, jsonify, request, send_from_directory, g
from .windows_support import require_supported_windows
from . import leveling
from . import pcb_import
from . import relief
from .cam import generate, job_json, number
from .design_api import register_design_routes
from .machine_setup import describe as describe_machine
from .device import DeviceController as Controller
from .profiles import PROFILES
from .rotary import generate_rotary, validate_rotary_machine
from .phone_jobs import PhoneJobs
from .phone_monitor_service import PhoneMonitor
from .protocols import get_protocol

# The only fields a controller command may carry; jog is the one action with arguments.
COMMAND_FIELDS = frozenset(('action', 'axis', 'distance', 'feed'))
# Probe settings the workstation may pass through to the probing plan.
PROBE_FIELDS = ('spacing', 'margin', 'clearance', 'depth', 'feed', 'plate')
# A probe line is acknowledged only when its motion ends, so it has to end well inside the
# acknowledgement timeout of the transport.
PROBE_SECONDS = 10


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
        if request.method == 'POST' and not secrets.compare_digest(
            request.headers.get('X-Studio-Token', '').encode('utf-8'), token.encode('utf-8')
        ):
            return jsonify(error='Reload this page before using controls'), 403
        if request.method == 'POST' and request.path in (
            '/api/connect',
            '/api/disconnect',
            '/api/arm',
            '/api/disarm',
            '/api/run',
            '/api/command',
            '/api/head',
            '/api/level/probe',
            '/api/level/collect',
            '/api/level/clear',
        ):
            ctl.lock.acquire()
            g.machine_lock = True

    @app.teardown_request
    def release_machine_lock(error):
        if getattr(g, 'machine_lock', False):
            g.machine_lock = False
            ctl.lock.release()

    @app.errorhandler(ValueError)
    @app.errorhandler(RuntimeError)
    def bad_request(exc):
        return jsonify(error=str(exc)), 400

    def payload(*required):
        # Machine-control routes accept only a JSON object; anything else is a 400, never a 500.
        data = request.get_json(silent=True)
        if not isinstance(data, dict):
            raise ValueError('Send a JSON object')
        missing = [key for key in required if key not in data]
        if missing:
            raise ValueError('Missing field: ' + ', '.join(missing))
        return data

    @app.get('/<path:asset>')
    def asset(asset):
        return send_from_directory(app.static_folder, asset)

    @app.get('/')
    def index():
        return send_from_directory(app.static_folder, 'index.html')

    @app.get('/api/session')
    def session():
        return jsonify(
            token=token,
            depth_paths=True,
            advanced_cam=True,
            holding_tabs=True,
            laser_patterns=True,
            router_entries=True,
            stock_wire_v1=True,
            phone_arm_challenge=True,
            rotary_stream=True,
            probe_leveling=leveling.available(),
            pcb_import=pcb_import.isolation_available(),
            relief_map=relief.map_limit(),
            machine_readback=True,
            hardware_enabled=allow_hardware,
        )

    @app.get('/api/status')
    def status():
        return jsonify(ctl.status())

    @app.get('/api/machine')
    def machine():
        # What the controller reports about its machine. Reading only: nothing is written to it.
        return jsonify(describe_machine(ctl.status()))

    @app.get('/api/profiles')
    def profiles():
        return jsonify(PROFILES)

    @app.get('/api/ports')
    def ports():
        return jsonify(ctl.ports())

    def validate_machine(project, job):
        status = ctl.status()
        profile = get_protocol(project.get('protocol', 'grbl'))
        if not profile.live_transport:
            raise ValueError('File export only; live execution is unavailable for ' + profile.name)
        if project.get('protocol', 'grbl') != status.get('protocol', 'grbl'):
            raise ValueError('Job protocol differs from connected controller')
        rotary = job.get('rotary')
        if rotary:
            validate_rotary_machine(rotary, status)
        if not status.get('simulated') and profile.family == 'grbl':
            settings = status.get('settings', {})
            if any(not math.isfinite(float(v)) for v in settings.values()):
                raise ValueError('Invalid controller settings readback; reconnect')
            laser = settings.get('32', settings.get('$32'))
            maximum = settings.get('30', settings.get('$30'))
            if laser is None or maximum is None:
                raise ValueError('Wait for GRBL $30/$32 readback before running')
            if rotary and profile.id == 'xpro-v5':
                spindle_type = status.get('spindle_type')
                if not spindle_type or (spindle_type == 'LASER') != (project['mode'] == 'laser'):
                    raise ValueError('Verify xPRO V5 Spindle/Type for the mounted head and reconnect for readback')
                if project['mode'] == 'laser':
                    maximum = settings.get('Laser/FullPower')
                    if maximum is None or float(maximum) <= 0:
                        raise ValueError('Wait for xPRO V5 Laser/FullPower readback before running a rotary laser job')
            if float(maximum) <= 0:
                raise ValueError('Invalid controller maximum power readback')
            if float(laser) != (1 if project['mode'] == 'laser' else 0):
                raise ValueError('Controller $32 does not match the selected head; configure GRBL separately')
            if float(project['power']) > float(maximum):
                raise ValueError('Power/RPM exceeds controller $30 or active laser full-power limit')
            for field, key in [('width', '130'), ('height', '131')]:
                if not rotary and key in settings and float(project[field]) > float(settings[key]):
                    raise ValueError('Work area exceeds configured controller travel')
            for operation in project.get('operations', []):
                if float(operation.get('power', project['power'])) > float(maximum):
                    raise ValueError('Operation power exceeds controller $30 or active laser full-power limit')
        if not status.get('simulated') and project['mode'] != 'laser':
            lowest = min((min(segment[4], segment[6]) for segment in job['segments']), default=0.0)
            if rotary:
                lowest = min(lowest, rotary['radial_bounds'][0])
            depth = max(0.0, -lowest)
            stock = number(project.get('stock_thickness'), 'Stock thickness', 0.01, 200)
            if depth > stock:
                raise ValueError('Cut exceeds entered stock thickness')
            settings = status.get('settings', {})
            safe = max(
                [float(project.get('safe', 5))]
                + [float(op.get('safe', project.get('safe', 5))) for op in project.get('operations', [])]
            )
            if not rotary and '132' in settings and depth + safe > float(settings['132']):
                raise ValueError('Depth plus clearance exceeds configured Z travel')
        return status

    @app.post('/api/phone-monitor')
    def configure_phone():
        data = payload()
        action = data.get('action')
        if action not in ('status', 'enable', 'disable'):
            raise ValueError('Select a phone pairing action.')
        if action == 'enable' and 'plaintext' in data:
            monitor.set_plaintext(data['plaintext'])
        try:
            info = {'status': monitor.info, 'enable': monitor.enable, 'disable': monitor.disable}[action]()
        except OSError as exc:
            raise ValueError('Phone listener could not start. Check the monitor port and TLS certificate.') from exc
        response = jsonify(info)
        response.headers['Cache-Control'] = 'no-store'
        return response

    def prepared(data):
        project = data.get('project')
        if not isinstance(project, dict):
            raise ValueError('Supply a design project')
        if project.get('rotary') and 'rotary' not in data:
            raise ValueError('Preview rotary CAM and explicitly confirm the rotary setup before running')
        if 'rotary' not in data:
            return generate(project)
        if data.get('rotary_confirmed') is not True:
            raise ValueError(
                'Confirm independent rotary drive, calibration in degrees, tool axes, seam zero and fixture clearance.'
            )
        return generate_rotary(project, data['rotary'])

    # Probe levelling. The probing plan and the surface measured with it are kept here, for one placement
    # of the stock: anything that can move work zero or the tool length forgets them.
    level = {'plan': None, 'surface': None}

    def forget_level():
        level['plan'] = level['surface'] = None

    def levelling_project(data):
        project = data.get('project')
        if not isinstance(project, dict):
            raise ValueError('Supply a design project')
        if project.get('mode') != 'router':
            raise ValueError('Probe levelling applies to router jobs')
        if project.get('rotary') or 'rotary' in data:
            raise ValueError('Probe levelling does not apply to rotary jobs')
        if get_protocol(project.get('protocol', 'grbl')).family != 'grbl':
            raise ValueError('Probe levelling needs a Grbl-family controller (G38.2)')
        return project

    def levelled(job, project, status):
        surface = level['surface']
        if surface is None:
            raise ValueError('Probe the stock surface before running a levelled job')
        result = leveling.level(job['lines'], surface)
        if not status.get('simulated'):
            lowest = min((min(segment[4], segment[6]) for segment in job['segments']), default=0.0)
            stock = number(project.get('stock_thickness'), 'Stock thickness', 0.01, 200)
            if -(lowest + min(surface['offsets'])) > stock:
                raise ValueError('Levelled cut exceeds entered stock thickness')
        return result['lines']

    def level_state():
        plan, surface = level['plan'], level['surface']
        return dict(
            available=leveling.available(),
            plan=plan and dict(grid=plan['grid'], points=plan['points'], probed=ctl.status().get('probe_count', 0)),
            surface=surface and dict(surface, lowest=min(surface['offsets']), highest=max(surface['offsets'])),
        )

    @app.get('/api/level')
    def level_status():
        return jsonify(level_state())

    @app.post('/api/level/probe')
    def level_probe():
        data = payload()
        if data.get('confirmed') is not True:
            raise ValueError(
                'Confirm that the probe is connected and tested, the spindle is stopped and work zero is set at the stock surface'
            )
        project = levelling_project(data)
        job = generate(project)
        validate_machine(project, job)
        plan = leveling.probe_plan(
            dict(
                {key: data[key] for key in PROBE_FIELDS if key in data},
                bounds=job['bounds'],
                width=project['width'],
                height=project['height'],
                zero=True,
            )
        )
        if (plan['clearance'] + plan['depth']) / plan['feed'] * 60 > PROBE_SECONDS:
            raise ValueError(
                f'Each probe must finish within {PROBE_SECONDS} seconds; raise the probe feed or lower the clearance and depth'
            )
        jobs.disarm()
        forget_level()
        ctl.start(plan['lines'])
        level['plan'] = plan
        return jsonify(ok=True, **level_state())

    @app.post('/api/level/collect')
    def level_collect():
        data = payload()
        plan = level['plan']
        if plan is None:
            raise ValueError('Probe the stock surface first')
        status = ctl.status()
        if status.get('active'):
            raise ValueError('Probing is still running')
        if status.get('error'):
            raise ValueError('Probing did not finish: ' + str(status['error']))
        reports = ctl.probe_reports()
        if len(reports) != len(plan['points']):
            raise ValueError(f'Probing stopped after {len(reports)} of {len(plan["points"])} points; probe again')
        level['surface'] = leveling.height_map(plan, reports=reports, limit=data.get('limit'))
        return jsonify(ok=True, **level_state())

    @app.post('/api/level/clear')
    def level_clear():
        forget_level()
        return jsonify(ok=True, **level_state())

    @app.post('/api/level/program')
    def level_program():
        data = payload()
        project = levelling_project(data)
        job = generate(project)
        return jsonify(lines=levelled(job, project, {'simulated': True}))

    @app.post('/api/arm')
    def arm_phone():
        data = payload()
        if data.get('confirmed') is not True:
            raise ValueError('Confirm setup at the Pi before arming')
        if data.get('phone_arm_challenge') is not True:
            raise ValueError('Reload the updated workstation before arming a phone Start.')
        job = prepared(data)
        validate_machine(data['project'], job)
        arm_id = jobs.arm(
            job['lines'], validator=lambda: validate_machine(data['project'], job), require_workstation=True
        )
        return jsonify(ok=True, arm_id=arm_id)

    def owner_arm_id(data):
        arm_id = data.get('arm_id') if isinstance(data, dict) else None
        if not isinstance(arm_id, str) or not arm_id:
            raise ValueError('Use the workstation tab that armed the preview.')
        return arm_id

    @app.post('/api/arm/check')
    def check_phone_arm():
        return jsonify(jobs.check_preview(owner_arm_id(payload())))

    @app.post('/api/arm/confirm')
    def confirm_phone_arm():
        data = payload()
        jobs.confirm_preview(owner_arm_id(data), data.get('challenge'))
        return jsonify(ok=True)

    @app.post('/api/disarm')
    def disarm_phone():
        return jsonify(ok=True, removed=jobs.disarm(owner_arm_id(payload())))

    @app.post('/api/connect')
    def connect():
        data = payload('port')
        port = data['port']
        if not isinstance(port, str) or not port:
            raise ValueError('Select a controller port')
        if port != 'simulator' and not allow_hardware:
            raise ValueError(
                'Physical ports are disabled. Start with --enable-hardware only after completing machine setup and commissioning; otherwise use Simulator.'
            )
        jobs.invalidate_controls()
        forget_level()
        if hasattr(ctl, 'select_protocol'):
            ctl.select_protocol(data.get('protocol', 'grbl'))
        ctl.connect(port)
        return jsonify(ok=True)

    @app.post('/api/disconnect')
    def disconnect():
        jobs.invalidate_controls()
        forget_level()
        ctl.disconnect()
        return jsonify(ok=True)

    @app.post('/api/generate')
    def preview():
        project = payload()
        # The core writes the job as JSON itself: for a large job that is most of the time of the request.
        text = job_json(project)
        if text is not None:
            return app.response_class(text, mimetype='application/json')
        return jsonify(generate(project, backend='python'))

    @app.post('/api/run')
    def run():
        data = payload()
        if data.get('confirmed') is not True:
            raise ValueError('Confirm work zero, tool and clear work area')
        if not isinstance(data.get('leveled', False), bool):
            raise ValueError('Levelled must be true or false')
        job = prepared(data)
        status = validate_machine(data['project'], job)
        lines = job['lines']
        if data.get('leveled'):
            lines = levelled(job, levelling_project(data), status)
        jobs.disarm()
        ctl.start(lines)
        return jsonify(ok=True, lines=len(lines))

    @app.post('/api/head')
    def apply_head():
        data = payload()
        mode = data.get('mode')
        if mode not in ('router', 'laser', 'knife') or data.get('confirmed') is not True:
            raise ValueError('Confirm the mounted head before applying mode')
        jobs.disarm()
        forget_level()
        state = ctl.status()
        if not state.get('connected') or state.get('active') or state.get('state') != 'Idle':
            raise ValueError('Head mode changes require an idle connected controller')
        commands = ['M5']
        if state.get('protocol') == 'fluidnc':
            raise ValueError(
                'Configure the mounted spindle or laser in FluidNC and reconnect for readback; Studio does not change FluidNC YAML'
            )
        if get_protocol(state.get('protocol', 'grbl')).family == 'grbl':
            commands += ['$32=' + ('1' if mode == 'laser' else '0'), '$$']
        ctl.start(commands)
        return jsonify(
            ok=True,
            message='Outputs off and mode requested. Wait for readback; re-establish tool Z/focus and work origin.',
        )

    @app.post('/api/command')
    def command():
        data = payload('action')
        action = data['action']
        if not isinstance(action, str):
            raise ValueError('Select a controller action')
        unknown = sorted(set(data) - COMMAND_FIELDS)
        if unknown:
            raise ValueError('Unsupported command field: ' + ', '.join(unknown))
        if action == 'reset':
            jobs.invalidate_controls()
        elif action not in ('hold', 'resume'):
            jobs.disarm()
        if action in ('reset', 'home', 'unlock', 'zero'):
            forget_level()  # work zero may no longer be where the surface was probed
        ctl.command(**data)
        return jsonify(ok=True)

    register_design_routes(app)
    return app


def main():
    require_supported_windows()
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument(
        '--enable-hardware',
        action='store_true',
        help='Operator opt-in to physical controller ports; requires machine setup and supervised commissioning',
    )
    parser.add_argument(
        '--monitor', action='store_true', help='Enable paired phone monitor and job controls on the LAN'
    )
    parser.add_argument('--monitor-port', type=int, default=8766)
    parser.add_argument('--monitor-cert', help='HTTPS certificate for the phone monitor')
    parser.add_argument('--monitor-key', help='Matching HTTPS private key')
    parser.add_argument(
        '--monitor-plaintext',
        action='store_true',
        help='Serve the phone monitor over HTTP. Needed for Pocket 0.4.1 and earlier, which cannot verify a pinned certificate',
    )
    args = parser.parse_args()
    ctl = Controller()
    jobs = PhoneJobs(ctl, allow_physical=args.enable_hardware)
    if bool(args.monitor_cert) != bool(args.monitor_key):
        parser.error('Provide both --monitor-cert and --monitor-key')
    if args.monitor_plaintext and args.monitor_cert:
        parser.error('Use either --monitor-plaintext or a monitor certificate')
    monitor = PhoneMonitor(
        ctl, jobs, args.monitor_port, args.monitor_cert, args.monitor_key, plaintext=args.monitor_plaintext
    )
    if args.monitor:
        pairing = monitor.enable()
        print(f"Phone pairing: {pairing['scheme']}://<PI-LAN-IP>:{pairing['port']}/#{pairing['fragment']}", flush=True)
        if pairing['encryption'] == 'none':
            print('The phone monitor is using HTTP: the pairing token and commands are readable on this network.')
        print('Open Control → Phone pendant to view or revoke pairing.', flush=True)
    app = create_app(ctl, jobs, allow_hardware=args.enable_hardware, phone_monitor=monitor)
    relief.calibrate_in_background()
    app.run(host='127.0.0.1', port=args.port, threaded=True, debug=False)


if __name__ == '__main__':
    main()
