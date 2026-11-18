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

Pocket's signed Android app was exercised on an ARM64 Android API 37 emulator configured with 2,048 MiB of RAM. The editor passed file import, reference-preserving export, resizing, SVG conversion, recovery, and library persistence after a cold restart. The paired pendant passed jog, home, zero, start, pause, resume and stop against the Studio simulator over virtual LAN and ADB reverse. The APK minimum is API 26. Physical handset, Pi 4/5 performance for this revision and material-cutting tests require their respective devices.
