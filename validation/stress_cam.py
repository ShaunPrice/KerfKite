"""Reproducible CAM-only stress test. Never imports a controller or opens devices.

Run from repository root: python -m validation.stress_cam --output report.json
The standalone relief executable is a historical prototype. The native backend
selects the installed application kernels for relief and stock simulation.
"""

import argparse, concurrent.futures, hashlib, json, math, os, platform
import statistics, subprocess, sys, threading, time

try:
    import resource
except ImportError:  # Windows
    resource = None
from pathlib import Path
from studio.relief import prepare_relief
from studio.cam import generate
from studio.simulation import simulate


def fixture(n=256):
    matrix = []
    for j in range(n):
        row = []
        for i in range(n):
            x = (i / (n - 1) - 0.5) * 2
            y = (j / (n - 1) - 0.5) * 2
            r = math.hypot(x, y)
            a = math.atan2(y, x)
            row.append(
                max(
                    0.02,
                    min(
                        0.98,
                        0.48
                        + 0.19 * math.cos(28 * r + 3 * math.sin(8 * a))
                        + 0.16 * math.sin(17 * x) * math.cos(21 * y)
                        + 0.12 * math.exp(-60 * ((x - 0.3) ** 2 + (y + 0.2) ** 2)),
                    ),
                )
            )
        matrix.append(row)
    return dict(
        heightmap=matrix,
        width=300,
        height=180,
        x=10,
        y=10,
        depth=6,
        tool_diameter=3.175,
        tool_shape='ball',
        stepover=0.9,
    )


def memory_mib():
    if resource is None:
        return None
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / (1048576 if sys.platform == 'darwin' else 1024)


def native_relief(data, executable):
    n = len(data['heightmap'])
    width = data['width']
    height = data['height']
    content = f"{n} {n} {width} {height} {data['depth']} {data['tool_diameter']} {data['stepover']}\n"
    content += '\n'.join(' '.join(map(str, row)) for row in data['heightmap'])
    p = subprocess.run([str(executable)], input=content, text=True, capture_output=True, check=True, timeout=60)
    depths = list(map(float, p.stdout.split()))
    nx = math.ceil(width / min(width / (n - 1), data['stepover']))
    ny = math.ceil(height / data['stepover'])
    assert len(depths) == (nx + 1) * (ny + 1)
    paths = []
    for j in range(ny + 1):
        path = [[data['x'] + width * i / nx, data['y'] + height * j / ny] for i in range(nx + 1)]
        if j % 2:
            path.reverse()
        paths.append(path)
    return dict(paths=paths, path_depths=[depths[j * (nx + 1) : (j + 1) * (nx + 1)] for j in range(ny + 1)])


def workload(native=None, backend="python", capture_grid=False, measure_rotary=False):
    stages = {}
    data = fixture()

    def timed(name, fn):
        t = time.perf_counter()
        result = fn()
        stages[name] = time.perf_counter() - t
        return result

    relief = timed('relief', lambda: native_relief(data, native) if native else prepare_relief(data, backend=backend))
    project = dict(
        mode='router',
        protocol='grbl',
        width=680,
        height=240,
        feed=1200,
        plunge=300,
        power=1000,
        safe=5,
        depth=6,
        step=3,
        tool_diameter=3.175,
        paths=relief['paths'],
        path_depths=relief['path_depths'],
    )
    job = timed('gcode', lambda: generate(project, packed_segments=backend == 'native'))
    preview = timed(
        'stock_preview',
        lambda: simulate(
            dict(
                segments=job['segments'],
                width=320,
                height=200,
                resolution=96,
                stock_thickness=12,
                tool_diameter=3.175,
                tool_shape='ball',
            ),
            backend=backend,
        ),
    )
    text = '\n'.join(job['lines'])
    assert len(job['lines']) <= 200000
    assert all(math.isfinite(v) and -6 <= v <= 0 for row in relief['path_depths'] for v in row)
    assert all(math.isfinite(v) and -12 <= v <= 0 for row in preview['heights'] for v in row)
    result = dict(
        stages=stages,
        vertices=sum(map(len, relief['paths'])),
        lines=len(job['lines']),
        segments=len(job['segments']),
        gcode_bytes=len(text.encode()),
        sha256=hashlib.sha256(text.encode()).hexdigest(),
        peak_rss_mib=memory_mib(),
        **({'grid': preview['heights']} if capture_grid else {}),
    )
    if measure_rotary:
        from studio.rotary import generate_rotary, wrap_job

        del job, text
        options = dict(diameter=100, wrap_axis='X', postprocessor='xpro-v5', output_axis='A')
        start = time.perf_counter()
        rotary = generate_rotary(project, options) if backend == 'native' else wrap_job(generate(project), options)
        result['rotary_seconds'] = time.perf_counter() - start
        result['rotary_lines'] = len(rotary['lines'])
        result['rotary_sha256'] = hashlib.sha256('\n'.join(rotary['lines']).encode()).hexdigest()
    return result


def thermal():
    p = Path('/sys/class/thermal/thermal_zone0/temp')
    return float(p.read_text()) / 1000 if p.exists() else None


def sustained(seconds):
    started = time.monotonic()
    count = 0
    hashes = set()
    times = []
    while time.monotonic() - started < seconds:
        temp = thermal()
        if temp is not None and temp >= 78:
            return dict(stopped='temperature >=78C', runs=count)
        r = workload()
        count += 1
        hashes.add(r['sha256'])
        times.append(sum(r['stages'].values()))
    return dict(
        runs=count,
        elapsed=time.monotonic() - started,
        hashes=sorted(hashes),
        median_pipeline_seconds=statistics.median(times),
        peak_rss_mib=memory_mib(),
    )


def native_comparison(executable):
    errors = []
    results = []
    for n in (32, 128, 256):
        data = fixture(n)
        py_times = []
        cpp_times = []
        parse_times = []
        content = f"{n} {n} 300 180 6 3.175 .9\n" + '\n'.join(' '.join(map(str, row)) for row in data['heightmap'])
        for _ in range(3):
            t = time.perf_counter()
            expected = prepare_relief(data, backend="python")
            py_times.append(time.perf_counter() - t)
            t = time.perf_counter()
            p = subprocess.run([str(executable)], input=content, text=True, capture_output=True, check=True, timeout=60)
            cpp_times.append(time.perf_counter() - t)
            t = time.perf_counter()
            actual = list(map(float, p.stdout.split()))
            parse_times.append(time.perf_counter() - t)
            flat = [v for row in expected['path_depths'] for v in row]
            assert len(actual) == len(flat)
            error = max(abs(a - b) for a, b in zip(actual, flat))
            errors.append(error)
            assert error < 1e-9, error
        results.append(
            dict(
                grid=n,
                python_median_seconds=statistics.median(py_times),
                cpp_process_including_text_io_seconds=statistics.median(cpp_times),
                python_output_parse_seconds=statistics.median(parse_times),
                max_depth_error_mm=max(errors),
            )
        )
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', default='stress-results.json')
    parser.add_argument('--native')
    parser.add_argument('--single-seconds', type=int, default=60)
    parser.add_argument('--parallel-seconds', type=int, default=30)
    args = parser.parse_args()
    samples = []
    stop = threading.Event()

    def sample():
        while not stop.is_set():
            samples.append(
                dict(
                    time=time.time(),
                    temperature_c=thermal(),
                    load=os.getloadavg(),
                    meminfo=Path('/proc/meminfo').read_text() if Path('/proc/meminfo').exists() else None,
                )
            )
            stop.wait(1)

    threading.Thread(target=sample, daemon=True).start()
    result = dict(
        platform=platform.platform(),
        python=sys.version,
        cpus=os.cpu_count(),
        fixture='256x256 rosette/ripples, 300x180mm, 6mm depth, 3.175mm ball, .9mm stepover, 3mm stepdown',
        model=Path('/proc/device-tree/model').read_text().strip('\x00')
        if Path('/proc/device-tree/model').exists()
        else None,
    )
    try:
        result['baseline'] = [workload() for _ in range(3)]
        print('Baseline complete', flush=True)
        if args.native:
            result['native'] = native_comparison(Path(args.native).resolve())
            print('Native parity complete', flush=True)
        result['sustained_single'] = sustained(args.single_seconds)
        print('Single process stress complete', flush=True)
        if args.parallel_seconds:
            with concurrent.futures.ProcessPoolExecutor(max_workers=4) as pool:
                result['sustained_four'] = list(pool.map(sustained, [args.parallel_seconds] * 4))
        hashes = {v['sha256'] for v in result['baseline']}
        for record in [result['sustained_single'], *result.get('sustained_four', [])]:
            hashes.update(record.get('hashes', []))
        result['all_gcode_hashes_match'] = len(hashes) == 1
        assert len(hashes) == 1
    finally:
        stop.set()
        result['telemetry'] = samples
        Path(args.output).write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if k != 'telemetry'}, indent=2), flush=True)


if __name__ == '__main__':
    main()
