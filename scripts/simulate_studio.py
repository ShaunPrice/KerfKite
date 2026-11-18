#!/usr/bin/env python3
"""Run the CAD-to-machine acceptance fixture using Studio's local simulator."""
import argparse
from collections import Counter
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def check(condition, message):
    if not condition:
        raise ValueError(message)


def fixture(width):
    return {
        'version': 1,
        'parameters': {'width': width, 'height': 'width / 2', 'stock': 3},
        'features': [
            {'id': 'plate', 'kind': 'box', 'op': 'add', 'size': ['width', 'height', 'stock'], 'origin': [10, 10, 0]},
            {'id': 'slot', 'kind': 'box', 'op': 'cut', 'size': [6, 4, 'stock + 2'], 'origin': [15, 15, -1]},
        ],
    }


def replay(job, project):
    """Interpret the linear, absolute GRBL dialect emitted by the fixture."""
    mode = project['mode']
    xyz = dict.fromkeys('XYZ')
    enabled = False
    power = 0
    moves = []
    for line in job['lines']:
        command, *words = line.split()
        values = {word[0]: float(word[1:]) for word in words}
        if command in ('M3', 'M4'):
            check(mode != 'knife', 'Knife program enabled spindle output')
            check(command == ('M3' if mode == 'router' else 'M4'), 'Wrong spindle mode')
            enabled = True
        elif command == 'M5':
            enabled = False
        elif command not in ('G0', 'G1', 'G4', 'G21', 'G90', 'G17', 'G94', 'G54'):
            raise ValueError(f'Unexpected instruction in GRBL fixture: {line}')
        power = values.get('S', power)
        if command not in ('G0', 'G1'):
            continue
        check(all(math.isfinite(value) for value in values.values()), 'Nonfinite motion')
        old = xyz.copy()
        xyz.update({axis: values[axis] for axis in xyz if axis in values})
        if mode == 'laser':
            check('Z' not in values, 'Laser job moved Z')
        elif xyz['Z'] is not None:
            check(xyz['Z'] >= -project['depth'] - .001, 'Tool exceeds programmed depth')
        if 'X' not in values and 'Y' not in values:
            continue
        check(0 <= xyz['X'] <= project['width'] and 0 <= xyz['Y'] <= project['height'], 'XY move leaves work area')
        if command == 'G0':
            if mode == 'laser':
                check(not enabled, 'Laser rapid with output enabled')
            else:
                check(old['Z'] == project['safe'] == xyz['Z'], 'Rapid XY below clearance')
        else:
            check(mode == 'knife' or enabled, 'Cut with output disabled')
            check(values.get('F', 0) > 0, 'Missing feed')
            if mode == 'laser':
                check(0 <= power <= project['power'], 'Laser power outside job range')
            moves.append([old['X'], old['Y'], xyz['X'], xyz['Y'], xyz['Z'] or 0])
    check(not enabled, 'Output still active at end')
    if mode != 'laser':
        check(xyz['Z'] == project['safe'], 'Tool was not retracted')
    check(moves and len(moves) == len(job['segments']), 'Motion count differs from preview')
    for actual, segment in zip(moves, job['segments']):
        check(all(abs(a - b) <= .00051 for a, b in zip(actual, segment[:5])), 'Posted motion differs from preview')
    return len(moves)


class StudioFixture:
    def __init__(self):
        from studio.device import DeviceController
        from studio.server import create_app
        self.device = DeviceController()
        self.client = create_app(controller=self.device, allow_hardware=False).test_client()
        session = self.client.get('/api/session').get_json()
        check(session['hardware_enabled'] is False, 'Simulator must use disabled hardware')
        self.headers = {'X-Studio-Token': session['token']}

    def request(self, route, data, binary=False):
        response = self.client.post('/api/' + route, headers=self.headers, json=data)
        if response.status_code != 200:
            raise ValueError(f'{route} returned {response.status_code}: {response.get_data(as_text=True)[:300]}')
        return response.data if binary else response.get_json()

    def solid(self):
        from studio.operations import region
        for width in (32, 40):
            model = fixture(width)
            rebuilt = self.request('solid-cad', {'model': model})
            area = width * width / 2 - 24
            check(math.isclose(rebuilt['volume'], area * 3, abs_tol=.01), 'Solid volume differs from plate minus slot')
            profiles = rebuilt['profiles']
            check(len(profiles) == 2 and math.isclose(region(profiles).area, area, abs_tol=.01), 'Projection lost slot or dimensions')
        raw = self.request('solid-cad/stl', {'model': model}, binary=True)
        facets, = struct.unpack_from('<I', raw, 80)
        check(facets == len(rebuilt['triangles']) and len(raw) == 84 + 50 * facets, 'STL triangle records differ from preview')
        self.profiles = profiles
        return {'widths_mm': [32, 40], 'volume_mm3': rebuilt['volume'], 'stl_facets': facets, 'projected_area_mm2': area}

    def head(self, mode):
        project = {'mode': mode, 'width': 75, 'height': 60, 'paths': self.profiles,
                   'operation': 'outside' if mode == 'router' else 'line', 'power': 600,
                   'safe': 4, 'feed': 300, 'plunge': 100, 'step': 1.5, 'depth': .2 if mode == 'knife' else 3,
                   'tool_diameter': 2, 'stock_thickness': 3, 'knife_offset': .25, 'knife_lead': 1}
        job = self.request('generate', project)
        result = {'cutting_moves': replay(job, project), 'posted_lines': len(job['lines'])}
        if mode == 'router':
            surface = self.request('simulate', {**project, 'segments': job['segments'], 'resolution': 80})
            heights = [z for row in surface['heights'] for z in row]
            check(math.isclose(min(heights), -3) and max(heights) == 0, 'Unexpected remaining stock surface')
            result['stock_min_z_mm'] = min(heights)
        self.request('connect', {'port': 'simulator', 'protocol': 'grbl'})
        try:
            check(self.device.status()['simulated'], 'Transport is not simulated')
            self.request('run', {'project': project, 'confirmed': True})
            self.device.command('hold')
            progress = self.device.status()['progress']
            time.sleep(.04)
            check(self.device.status()['progress'] == progress and self.device.status()['active'], 'Hold did not retain job')
            self.device.command('resume')
            deadline = time.monotonic() + 10 + len(job['lines']) * .04
            while self.device.status()['active'] and time.monotonic() < deadline:
                time.sleep(.02)
            state = self.device.status()
            check(state['state'] == 'Idle' and state['progress'] == 100 and not state['error'], 'Simulator failed to finish')
            result['controller'] = state['state']
        finally:
            self.device.disconnect()
        return result


def python_suite():
    suite = unittest.defaultTestLoader.discover(str(ROOT / 'tests'))
    stream = io.StringIO()
    result = unittest.TextTestRunner(stream=stream, verbosity=1).run(suite)
    check(result.testsRun > 0 and result.wasSuccessful(), stream.getvalue()[-5000:])
    return {'tests_run': result.testsRun, 'skipped': [(str(test), why) for test, why in result.skipped]}


def js_suite():
    node = shutil.which('node')
    check(node, 'Node.js is required for --all')
    files = sorted(set((ROOT / 'tests').rglob('*.test.cjs')) | set((ROOT / 'tests').glob('test*.cjs')) |
                   set((ROOT / 'validation').glob('*.test.js')) | set((ROOT / 'validation').glob('test*.cjs')))
    check(files, 'No JavaScript test files discovered')
    proc = subprocess.run([node, '--test', '--test-reporter=tap', *map(str, files)], cwd=ROOT,
                          capture_output=True, text=True, timeout=300,
                          env={**os.environ, 'CNC_TEST_PYTHON': sys.executable})
    if proc.returncode:
        failures = re.findall(r'^not ok .*?(?=^# Subtest:|^1\.\.|\Z)', proc.stdout, re.M | re.S)
        raise ValueError('\n'.join(failures)[:6000] + proc.stderr[-2000:])
    summary = {key: int(value) for key, value in re.findall(r'^# (tests|pass|fail|skipped|todo) (\d+)$', proc.stdout, re.M)}
    check(summary.get('tests', 0) > 0, 'Missing Node TAP summary')
    return {'files': [str(f.relative_to(ROOT)) for f in files], **summary}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--require-step', action='store_true', help='fail if STEP exchange is unavailable')
    parser.add_argument('--all', action='store_true', help='also run the Python and JavaScript regression suites')
    parser.add_argument('--json', type=Path, help='write a machine-readable result file')
    args = parser.parse_args()
    cases = []

    def run(name, function):
        try:
            evidence = function()
        except Exception as exc:
            cases.append({'name': name, 'status': 'failed', 'error': str(exc)})
            print('FAIL', name, exc, file=sys.stderr)
            return False
        cases.append({'name': name, 'status': 'passed', 'evidence': evidence})
        print('PASS', name)
        return True

    fixture_state = []

    def build():
        simulation = StudioFixture()
        fixture_state.append(simulation)
        return simulation.solid()

    built = run('Rebuild plate, resize and export STL', build)
    for head in ('router', 'laser', 'knife'):
        if built:
            run(head, lambda head=head: fixture_state[0].head(head))
        else:
            cases.append({'name': head, 'status': 'skipped', 'reason': 'CAD fixture failed'})
    import simulate_interchange as extra
    if fixture_state:
        for name, function in [('Assembly mate and interference', extra.assembly),
                               ('Parametric surface patch', extra.surface),
                               ('Swept holder and clamp', extra.collision),
                               ('Plotter import to knife program', extra.program_import)]:
            run(name, lambda function=function: function(fixture_state[0]))
        from importlib.util import find_spec
        if find_spec('OCP') is not None or args.require_step:
            run('STEP surface and solid exchange', lambda: extra.step(fixture_state[0]))
        else:
            cases.append({'name':'STEP surface and solid exchange', 'status':'skipped', 'reason':'Install .[step] or enforce with --require-step'})
    for name, function in [('Python regression suite', python_suite), ('JavaScript regression suite', js_suite)]:
        if args.all:
            run(name, function)
        else:
            cases.append({'name': name, 'status': 'skipped', 'reason': 'Use --all'})
    report = {'format': 'cnc-studio-simulation/1', 'transport': 'in-process simulator', 'cases': cases,
              'totals': dict(Counter(case['status'] for case in cases)),
              'outside_simulation': ['Physical travel limits, tool calibration, material response and measured machine clearances',
                                     'GUI interaction and visual tutorial acceptance', 'External controller execution of exported files']}
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(report['totals'])
    return int(any(case['status'] == 'failed' for case in cases))


if __name__ == '__main__':
    raise SystemExit(main())
