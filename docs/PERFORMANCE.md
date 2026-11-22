# Native CAM and editor verification

The Studio 0.6.1 simulator benchmark uses a 256 × 256 height map, a ball end mill and a cylindrical router setup. Three fresh processes per backend generated the same 135,886 planar instructions and the same 136,691 rotary instructions. The largest difference between corresponding stock-preview cells was 2.7 × 10⁻¹⁴ mm. These timings were measured on an Apple Silicon Mac during integration testing.

| Median | Python reference | Accelerated path |
| --- | ---: | ---: |
| Relief, planar CAM and stock preview | 2.124 s | 0.416 s |
| Stock preview stage | 0.889 s | 0.0097 s |
| Rotary job generation | 2.586 s | 1.288 s |
| Process peak resident memory before rotary generation | 94.4 MiB | 90.4 MiB |

Run `python -m validation.benchmark_native result.json` from the checkout with the extension installed. The benchmark requires matching G-code hashes and a maximum stock-cell difference below 1 × 10⁻¹⁰ mm. For the viewport, `python validation/benchmark_viewport.py --browser PATH` compares the same camera orbit through WebGL2 and Canvas. On Apple M4 using Metal, median frame intervals for an 18,432-triangle mesh were 16.7 ms and 50.0 ms respectively.

The indexed artwork catalog retained all 1,768 original entries and all 1,358,040 coordinate pairs in a value-for-value comparison. An isolated Mac process searching metadata and loading one template peaked at 32.6 MiB with the index. The earlier eager loader peaked at 432.8 MiB. The compressed catalog payload decreased from 18.89 MiB to 15.11 MiB.

Pocket's signed Android app was exercised on an ARM64 Android API 37 emulator configured with 2,048 MiB of RAM. The editor passed file import, reference-preserving export, resizing, SVG conversion, recovery, and library persistence after a cold restart. The paired pendant passed jog, home, zero, start, pause, resume and stop against the Studio simulator over virtual LAN and ADB reverse. The APK minimum is API 26. On a physical Galaxy S23 Ultra running Android 17, the published Pocket 0.4.1 APK started cold in 273 ms and 499 ms, used 41 to 97 MiB proportional memory while editing, and rendered 648 frames with a 5 ms median and a 13 ms 90th percentile. Pi 4/5 performance for this revision and material-cutting tests require their respective devices.

## Relief contact on threads and the GPU

The relief contact shares its rasters out over the processor's threads and, for large jobs on a computer with a discrete GPU, may run on the GPU, with the same depths to the last bit either way. On a 24-thread laptop a relief from a 1024-point map with a 12 mm ball takes 116 ms on the threads, 104 ms on its RTX 5090 Laptop GPU and 1.77 s on one thread. The measurements, the rules for choosing and the checks are in [Threads and the GPU](NATIVE.md#threads-and-a-gpu-for-large-reliefs); `validation/benchmark_relief.py` repeats them.

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

