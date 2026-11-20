# Optional C++ relief backend

`studio/_relief_native.cpp` is an in-process CPython extension. It accelerates conservative flat/ball cutter contact; Python retains input validation, work caps, path construction, metadata and the reference implementation. There are no runtime compiler or subprocess calls. The kernel releases the GIL during its bounded numerical loop.

Install with a compiler and Python headers, then `python -m pip install .`. Developers can use `python setup.py build_ext --inplace`. Windows uses MSVC `/O2`; other builds use `-O3 -std=c++11`, without fast-math. The extension is optional, so inspect the backend rather than assuming compilation succeeded.

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

