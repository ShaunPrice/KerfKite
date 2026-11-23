# Native CAM and editor verification

These measurements were made while Studio's Python reference existed, as each part moved into the native core. The benchmarks that ran the reference (`validation/benchmark_native.py`, `benchmark_gcode.py` and the others) are kept with it at the reference commit ([The Python reference](DEVELOPER.md#the-python-reference)); `kerfkite_solid_bench` and `validation/benchmark_viewport.py` run from this tree.

The Studio 0.6.1 simulator benchmark uses a 256 × 256 height map, a ball end mill and a cylindrical router setup. Three fresh processes per backend generated the same 135,886 planar instructions and the same 136,691 rotary instructions. The largest difference between corresponding stock-preview cells was 2.7 × 10⁻¹⁴ mm. These timings were measured on an Apple Silicon Mac during integration testing.

| Median | Python reference | Accelerated path |
| --- | ---: | ---: |
| Relief, planar CAM and stock preview | 2.124 s | 0.416 s |
| Stock preview stage | 0.889 s | 0.0097 s |
| Rotary job generation | 2.586 s | 1.288 s |
| Process peak resident memory before rotary generation | 94.4 MiB | 90.4 MiB |

The reference's `python -m validation.benchmark_native result.json`, run with its extension installed, requires matching G-code hashes and a maximum stock-cell difference below 1 × 10⁻¹⁰ mm. Since the stock preview moved into the native core it is the reference's to the bit, so that difference is now zero. For the viewport, `python validation/benchmark_viewport.py --browser PATH` compares the same camera orbit through WebGL2 and Canvas. On Apple M4 using Metal, median frame intervals for an 18,432-triangle mesh were 16.7 ms and 50.0 ms respectively.

The indexed artwork catalog retained all 1,768 original entries and all 1,358,040 coordinate pairs in a value-for-value comparison. An isolated Mac process searching metadata and loading one template peaked at 32.6 MiB with the index. The earlier eager loader peaked at 432.8 MiB. The compressed catalog payload decreased from 18.89 MiB to 15.11 MiB.

Pocket's signed Android app was exercised on an ARM64 Android API 37 emulator configured with 2,048 MiB of RAM. The editor passed file import, reference-preserving export, resizing, SVG conversion, recovery, and library persistence after a cold restart. The paired pendant passed jog, home, zero, start, pause, resume and stop against the Studio simulator over virtual LAN and ADB reverse. The APK minimum is API 26. On a physical Galaxy S23 Ultra running Android 17, the published Pocket 0.4.1 APK started cold in 273 ms and 499 ms, used 41 to 97 MiB proportional memory while editing, and rendered 648 frames with a 5 ms median and a 13 ms 90th percentile. Pi 4/5 performance for this revision and material-cutting tests require their respective devices.

## Relief contact on threads and the GPU

Reliefs are prepared in the native core, whose contact shares its rasters out over the processor's threads and, for large jobs on a computer with a GPU that has double precision, may give part or all of them to the GPU, with the same depths to the last bit whichever computes what. Studio measures the threads and the GPU each time it starts and shares large jobs so that both finish together. On a 24-thread laptop a whole relief from a 1024-point image with a 12 mm ball takes 140 ms on the threads, 126 ms on its RTX 5090 Laptop GPU, 103 ms on both at once and 1.88 s on one thread; capped at 4 threads, as a small processor would be, it takes 472 ms on the threads and 116 ms shared, and `/api/relief` answers a relief from that image in half the time it took before the relief moved into the core. The measurements, the rules for choosing and the checks are in [Threads and the GPU](NATIVE.md#threads-and-a-gpu-for-large-reliefs); `validation/benchmark_relief.py` repeats them.

## G-code and rotary generation after 0.6.1

The relief and stock-preview kernels are native, which left G-code and rotary generation in Python as the largest stages. Their per-move loops were tightened without changing the output: plain in-range numbers skip the general validator, per-block sets are built once per job, and packed rows are written through a bound function. `validation/benchmark_gcode.py` gives best-of-nine times and a hash of each generated program on the 256 × 256 relief fixture.

| Stage (Linux x86-64, Python 3.13) | Before | After | Program hash, unchanged |
| --- | ---: | ---: | --- |
| Relief job, 135,886 lines | 291 ms | 200 ms | `08c0811b116f` |
| Flat six-level job, 135,886 lines | 201 ms | 143 ms | `31c8a9794b61` |
| Laser job, 67,945 lines | 269 ms | 213 ms | `6636b83bda44` |
| Rotary job, 136,691 lines | 1,060 ms | 773 ms | `cfe252049e5c` |

Every generator, relief and stock-preview call made by the regression suite (578 calls, including 174 that must fail) returned the same result or the same error before and after. Start-up was measured and left alone: importing the server takes about 0.2 s warm and 48 MiB, with the geometry, PDF, DXF and STEP libraries already loaded on first use.

### C++ move loops

The same jobs with the per-move loops ported to C++ (`studio/_gcode_native.cpp`; see [Native](NATIVE.md)). Best of nine, Linux x86-64, Python 3.13. Both backends produce the program hash shown.

| Stage | Python loops | C++ loops | Gain | Program hash |
| --- | ---: | ---: | ---: | --- |
| Relief job, packed segments | 209 ms | 47 ms | 4.5× | `08c0811b116f` |
| Relief job, list rows | 194 ms | 71 ms | 2.7× | `08c0811b116f` |
| Flat six-level job | 146 ms | 31 ms | 4.7× | `31c8a9794b61` |
| Laser job | 202 ms | 128 ms | 1.6× | `6636b83bda44` |
| Rotary job | 729 ms | 167 ms | 4.4× | `cfe252049e5c` |

Against the 0.6.1 figures above, the relief job is 6.3 times faster and the rotary job 6.4 times. The laser job gains least because preparing laser paths remains in Python, as does the rotary cut-length calculation; both depend on Python's own `hypot` and `dist`, and porting them would change the last digits of some values.

### The native core's CAM

Studio now generates the whole job in the native core (see [Native](NATIVE.md#g-code-and-rotary-output)), and the C++ loops above are retired. The core's rotary post reads its tokens in place, and its post-processor takes the program's lines over instead of copying them. Best of nine, Linux x86-64 on a two-core machine, Python 3.13; the same jobs, with the program hashes unchanged:

| Stage | Python | C++ loops | Native core | Program hash |
| --- | ---: | ---: | ---: | --- |
| Relief job, packed segments | 150 ms | 35 ms | 43 ms | `08c0811b116f` |
| Relief job, list rows | 136 ms | 54 ms | 52 ms | `08c0811b116f` |
| Flat six-level job | 103 ms | 23 ms | 26 ms | `31c8a9794b61` |
| Laser job | 143 ms | 93 ms | 26 ms | `6636b83bda44` |
| Rotary job | 535 ms | 129 ms | 114 ms | `cfe252049e5c` |

The laser job is 3.6 times faster than with the C++ loops, because its path preparation is now in C++ as well; the rotary job is 12 per cent faster. Packed segments, which only the tests ask for, are a few milliseconds slower: the job's paths and lines are made as Python objects, where the loops reused the caller's. A preview also returns as the core's own JSON text, which for these jobs is most of the request:

| Preview response | C++ loops, then `jsonify` | Core's job and text |
| --- | ---: | ---: |
| Relief job | 329 ms | 129 ms |
| Laser job | 228 ms | 65 ms |
| Rotary job | 412 ms | 193 ms |

