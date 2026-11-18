# Pi CAM stress test — 11 September 2026

The existing Python CAM pipeline completed the complex fixture in a median **2.89 seconds** on the real Raspberry Pi 5 with 8 GB RAM. Replacing only the relief contact calculation with a benchmark C++ prototype reduced the complete calculation pipeline to **1.95 seconds** (approximately **32% less time / 1.48× throughput**).

## Workload and environment

- Verified host: `benchmark host`, Raspberry Pi 5 Model B Rev 1.0, four CPU cores, 8,063 MiB OS-visible RAM.
- Debian 13, Linux 6.12.47+rpt-rpi-2712 aarch64, Python 3.13.5, Shapely 2.1.2, NumPy 2.5.3; GCC 14.2.0 with `-O3 -std=c++17`, without fast-math.
- Synthetic 256×256 height field with rosette, intersecting ripples and a localized raised feature, covering 300×180 mm, maximum depth 6 mm, 3.175 mm ball cutter, 0.9 mm stepover and 3 mm step-down.
- 67,335 path vertices; 135,886 G-code lines; 134,268 preview segments; 4,749,814 bytes of G-code; 96×96 sampled stock preview.
- Other services remained running. A background Python process was consuming CPU before the test. Benchmark processes used niceness 10. This represents this Pi's observed conditions, not an idle-machine hardware comparison.
- Only CAM modules and benchmark files were copied to an isolated temporary directory. No controller modules, device access, GPIO operations or motion commands were used. The user confirmed the CNC was disconnected.

## Three-run medians

| Stage | Existing Python pipeline | C++ contact calculation plus Python pipeline |
|---|---:|---:|
| Relief paths and depths | 1.154 s | 0.220 s |
| G-code generation | 0.523 s | 0.524 s |
| Sampled stock preview | 1.209 s | 1.205 s |
| Complete calculation pipeline | 2.888 s | 1.954 s |

The complete-pipeline median is calculated from each run's total, rather than the sum of stage medians. Fixture construction, hashing and report writing are excluded in both pipelines. The hybrid relief stage includes fixture serialization, executable startup, native computation, output parsing and reconstruction of the XY/depth arrays. It is not a measurement of an integrated native library.

The native depth-only comparison also tested 32×32, 128×128 and 256×256 input grids, three runs each. All calculated depths agreed within **7.3×10^-14 mm**, below the benchmark's 10^-9 mm tolerance. All three hybrid jobs produced byte-identical G-code to Python.

## Sustained load

- Three baseline jobs, 20 further jobs over 60.9 seconds in one process, then 36 jobs over approximately 31.3 seconds across four workers: **59 Python pipeline jobs, all matching**.
- A further three hybrid pipeline jobs also matched, giving **62 complete Pi jobs** with the same G-code hash.
- Single-process sustained median calculation time: 2.908 s/job. Four-worker medians: 3.246–3.322 s/job; concurrent throughput approximately 1.15 jobs/second.
- Highest sampled temperature: **72.7°C**. Samples approximately once per second; the test stops between jobs if temperature reaches 78°C. No temperature stop occurred.
- `vcgencmd get_throttled` was `0x0` before, during spot checks and after testing: no current or latched throttling/undervoltage flags reported.
- Minimum sampled available system memory: **6,387.9 MiB (6.24 GiB)**. Maximum sampled swap use: **0 MiB**.
- Python parent process high-water RSS: **100.5 MiB**; each concurrent worker: approximately **89.5–90.4 MiB**. These are per-process measurements, not summed private memory and not the full browser/application footprint.
- The same fixture on the Mac produced the identical G-code hash. Different Python versions and background loads mean this is a consistency check rather than a controlled hardware shoot-out.

SHA-256 of newline-joined G-code, without a final newline:

`08c0811b116f76b17866ad928bc4484293a43a86f6b02050ad0d85e46dd67bbc`

## Interpretation and limits

The Pi 5 with 8 GB has ample memory for this tested workload. Compiling the relief calculation is worthwhile: the full relief stage was approximately 5.3× faster, while the full calculation pipeline was approximately 1.48× faster. Stock preview is the largest remaining stage in the hybrid run and is the next useful profiling/optimization target.

This is a bounded synthetic stress test within the application's existing 256×256 height-map, 100,000-vertex and 200,000-line limits. It does not establish support for arbitrarily large meshes, full-resolution stock simulation, browser responsiveness, concurrent network/control performance, physical machining accuracy, or long-duration thermal equilibrium. The sampled preview remains approximate. No Rust speedup has been measured.

`relief_benchmark.cpp` is a benchmark-only implementation for trusted square height fields, ball cutters and zero allowance. It does not implement the production module's complete validation, metadata and feature set. It has **not been enabled in the application**. Integrating a native backend requires broader correctness testing and packaged builds for each supported platform.

## Reproduction and evidence

From the project root, in an environment with the CAM dependencies installed:

```sh
c++ -O3 -std=c++17 validation/relief_benchmark.cpp -o /tmp/cnc-relief-benchmark
python -m validation.stress_cam --native /tmp/cnc-relief-benchmark --output stress-results.json
```

The default test includes three baseline runs, native depth comparisons, 60 seconds of single-process load and 30 seconds with four workers. The three hybrid full-pipeline measurements were run separately with `workload('./relief_benchmark')` from `validation.stress_cam`.

- `stress-pi5-2026-09-11.json`: raw timings, per-process high-water memory and time-series telemetry.
- `stress-pi5-hybrid-2026-09-11.json`: three complete hybrid jobs with matching hashes.
- `stress-mac-2026-09-11.json`: corresponding local fixture and depth-comparison evidence.
- `stress_cam.py` and `relief_benchmark.cpp`: repeatable fixture, invariants and native prototype.
- Existing six relief correctness tests passed locally after adding the benchmark. Application CAM code was unchanged.
