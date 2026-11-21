# Optional C++ relief backend

`studio/_relief_native.cpp` is an in-process CPython extension. It accelerates conservative flat/ball cutter contact; Python retains input validation, work caps, path construction, metadata and the reference implementation. There are no runtime compiler or subprocess calls. The kernel releases the GIL during its bounded numerical loop.

Install with a compiler and Python headers, then `python -m pip install .`. Developers can use `python -m pip install -e .`, which builds the extension in place with the setuptools release the project requires, or `python setup.py build_ext --inplace` where setuptools 77 or later is already installed (the release bundled with a new Python 3.10 environment is older and rejects the licence field). Windows uses MSVC `/O2`; other builds use `-O3 -std=c++11`, without fast-math. The extension is optional, so inspect the backend rather than assuming compilation succeeded.

```python
from studio.relief import prepare_relief
result = prepare_relief(data)  # auto: native if importable, otherwise Python
print(result['surface']['backend'])  # cpp or python
reference = prepare_relief(data, backend='python')
accelerated = prepare_relief(data, backend='native')  # explicit error if absent
```

The UI identifies accelerated relief results. Restart an already-running server after installing the extension. Browser-only file mode does not load a CPython extension.

Native and Python parity tests cover planes, ramps, domes, peaks, random fields, both cutter shapes, allowances, malformed input and the fallback path. The installed Pi wheel passed ten relief/native tests; six complete benchmark jobs produced the same G-code hash. The original standalone C++ executable remains only as a historical benchmark fixture.

Installed Pi medians: 1.142 s Python relief versus 0.086 s C++; 2.864 s total versus 1.818 s. About 37% less total calculation time for that fixture. C++ was chosen because its implementation was directly measured; no Rust performance comparison was performed. Stock preview is the largest remaining stage.

Evidence: `validation/native-installed-pi5-2026-09-11.json` and `validation/native-installed-mac-2026-09-11.json`. The Mac evidence exercises the local compiled module; the Pi evidence imports from the temporary environment's installed site-packages. No persistent Pi service was deployed.

## G-code and rotary loops

`studio/_gcode_native.cpp` is compiled into the same extension. It ports the per-move loops that were the largest remaining Python stages:

| Function | Replaces |
| --- | --- |
| `plain_points`, `plain_depths` | the per-point input checks, for designs made only of plain in-range numbers |
| `gcode_moves` | the loop that writes each cutting move, its preview segment and the cut length |
| `rotary_run` | the translation of each generated cutting run to rotary blocks |
| `segment_rows` | unpacking packed segments at the JSON boundary |

**Python stays the reference.** Each function handles only the plain case. When it meets anything else (an unexpected type, a non-finite number, a fixture or travel limit) it returns `None` and changes nothing, and the caller runs the Python loop from the same state. Every error message, and every unusual input, is therefore handled by the reference by construction.

**The output is identical, not merely close.** Three choices make that hold on every platform:

* Fixed-point text is produced with exact integer arithmetic, rounding half to even on the exact binary value as Python does. The C library is not used, because its rounding has differed between platforms.
* Squares use the same `pow` call that Python's `**` operator makes.
* The rotary cut length is computed by the Python function itself, called from C++, because Python's `hypot` and `asinh` are its own implementations. Laser path preparation stays in Python for the same reason (`math.dist`).

```python
from studio.cam import generate
from studio.rotary import generate_rotary
job = generate(project)                      # auto: C++ loops if importable, otherwise Python
reference = generate(project, backend='python')
accelerated = generate(project, backend='native')  # explicit error if the extension is absent
rotary = generate_rotary(project, options, backend='python')
```

Set `KERFKITE_GCODE_BACKEND=python` in the server's environment to make `auto` use the Python loops without reinstalling. The functions hold the GIL.

Verification on Linux x86-64 (GCC, Python 3.13):

* 10.4 million formatted and rounded values, including decimal ties, binary fractions and random bit patterns, matched Python with no difference.
* `tests/test_native_gcode.py` compares both backends for router, relief, laser, perforated and bridged jobs, in list, packed and rotary forms, with number types included; 96 rotary option combinations; and the limit conditions that make the translator hand a run back.
* Every generator call made by the regression suite (578 calls, 174 of which must fail) returns the same result or error as the Python-only baseline. The suite also passes with the Python loops forced and with the extension removed.
* The extension was rebuilt with the address and undefined-behaviour sanitizers and the CAM tests and benchmark jobs were run under them without a report.

On Windows x64 (MSVC 2022 Build Tools, Python 3.10 and 3.12) the extension builds without compiler warnings, the parity tests and the installed-wheel check pass, and the five benchmark programs have the same hashes as on Linux. macOS and ARM builds have not been run.

`validation/benchmark_gcode.py` times both backends and fails if their program hashes differ; `validation/check_installed_cam.py` repeats the parity check on an installed wheel.

## Python-free core

`native/` holds the start of the application's move to C++: a C++17 library and the `kerfkite-cam` command-line tool, which write G-code from a project file with no Python installed. It currently reproduces router line jobs, laser line jobs (leads, power ramps, perforation, holding bridges and raster powers), drilling, drag-knife paths, ordered multi-operation jobs and controller post-processing; everything else is declined, never approximated. `native/README.md` lists what is ported and how to build it.

Probe levelling (`native/core/src/leveling.cpp`) is the first part of the application written only in the core: it has no Python version. The extension compiles that file and exposes it as `core_call`, which `studio/leveling.py` wraps; the extension is therefore built as C++17 and includes the vendored JSON header. See [Probe levelling](PROBE-LEVELING.md).

Circuit board import (`gerber.cpp`, `excellon.cpp`, `pcb.cpp`) is written the same way, compiled into the extension and wrapped by `studio/pcb_import.py`. See [Circuit boards](PCB.md).

Offsets, pockets and hatch fills are computed with GEOS 3.14.1, the release inside the Shapely 2.2.0 wheels that the Python code uses, through the same calls with the same arguments (`native/core/src/geometry.hpp`, `native/core/src/operations.cpp`). The build fetches the pinned GEOS release and checks its SHA-256; `native/README.md` gives the options, the build cost and the limits of what has been measured, and [Licences](LICENSING.md) covers GEOS's LGPL terms. On Linux x86-64 the generated comparison adds 4,000 contour jobs, of which about 2,400 produce programs (offsets, pockets and fills) and the rest rejections; all are reproduced or declined, none differs.

The exact number formatting described above now lives in one header, `native/core/include/kerfkite/number_text.hpp`, used by both the Python extension and the core.

`python validation/native_core_parity.py --cli <kerfkite-cam>` compares the core with the Python reference: number text, every generator call the test suite makes, and thousands of generated edge cases. A case must be reproduced exactly or declined; a different answer fails. With laser jobs ported, the measurement on Linux x86-64 is: 300,000 numbers and 360,000 distances and sums identical; 124 of 202 recorded suite calls reproduced (61%); 11,225 of 12,000 generated cases reproduced (94%), among them 1,137 laser programs; and no differences. The three benchmark programs written by `kerfkite-cam` have the same hashes as Python's.

Drilling (`native/core/src/drilling.cpp`) and drag-knife paths (`native/core/src/knife.cpp`) are ported as well. The generated cases gained 4,000 drilling and knife projects. On Linux x86-64, with those two parts: 160 of 205 recorded suite calls reproduced (78%, from 62%); 15,562 of 16,000 generated cases reproduced (97%), among them 1,290 drilling and 1,634 drag-knife programs; and no differences. The one knife case declined is a path that returns to a point through steps too short to keep, where the Python divides by zero. Knife swivels use the platform's `atan2`, `sin` and `cos`, as Python does, so they are compared on each platform against the Python there.

Ramp entries and holding tabs on router line jobs are reproduced too (`native/core/src/router_entry.cpp`, `tabs.cpp`). Before either, the reference asks Shapely whether each contour crosses itself; `simple_line.cpp` answers by the rules GEOS applies, including its double-double orientation test, and declines the rare contour for which that test and exact arithmetic disagree. A helix entry needs a pocket, so on a line job it is rejected as the reference rejects it. The ramp slope is `math.tan` of the entry angle, one C library call on each side. With these ported, the measurement on Linux x86-64 is: 161 of 205 recorded suite calls reproduced (79%); 11,432 of the same 12,000 generated cases (95%); and 5,951 of 6,005 ramp, helix and tab cases, among them 3,254 programs, the rest of those declined because they also ask for an offset, a pocket or drilling; no differences.

