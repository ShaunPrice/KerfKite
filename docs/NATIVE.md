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

`native/` holds the start of the application's move to C++: a C++17 library and the `kerfkite-cam` command-line tool, which write G-code from a project file with no Python installed. It currently reproduces router line jobs, laser line jobs (leads, power ramps, perforation, holding bridges and raster powers), drilling, drag-knife paths, inside and outside offsets, pockets, hatch fills, ramp entries, helical entries into pockets, holding tabs, ordered multi-operation jobs and controller post-processing; everything else (V-carving is the one operation of a job still missing) is declined, never approximated. It contains no third-party code and its build downloads nothing. `native/README.md` lists what is ported and how to build it.

Probe levelling (`native/core/src/leveling.cpp`) is the first part of the application written only in the core: it has no Python version. The extension compiles that file and exposes it as `core_call`, which `studio/leveling.py` wraps; the extension is therefore built as C++17 and compiles the core's JSON reader and writer with it. See [Probe levelling](PROBE-LEVELING.md).

Circuit board import (`gerber.cpp`, `excellon.cpp`, `pcb.cpp`) is written the same way, compiled into the extension and wrapped by `studio/pcb_import.py`. So are isolation toolpaths (`isolation.cpp`), for which the extension also compiles the polygon kernel (`polygon/`). See [Circuit boards](PCB.md).

Offsets, pockets and hatch fills are computed by the core's own polygon kernel (`native/core/src/polygon/`, `native/core/src/operations.cpp`; see [The polygon kernel](POLYGON-KERNEL.md)). This is the third part with a single implementation: the extension compiles the kernel and the operations, and `studio/operations.py` calls them through `core_call` after checking the job's settings in Python, so Studio and `kerfkite-cam` cut the same paths by construction. GEOS, which the core built for these operations at first, has been removed from the native build with its options.

```python
from studio import operations
operations.polygon_backend()   # 'kernel' with the extension, 'shapely' without it
```

Without the extension (the portable wheel) the same operations run on Shapely, as they did before the kernel. The fallback cuts the same regions, but its vertices are not the kernel's: arcs are approximated from inside the true arc and not from outside, rings start elsewhere, pockets have many more vertices, and a contour that closes only to within the tolerance is usually refused. `KERFKITE_POLYGON_BACKEND=shapely` selects the fallback where the extension is installed, and `=kernel` refuses to run without the extension. Kernel coordinates are whole millionths of a millimetre; each crosses the JSON boundary as the shortest text of the double `n / 1e6` and is read back as exactly that double, which `tests/test_polygon_operations.py` checks.

`validation/check_installed_cam.py` pins these operations on every platform a wheel is built for: an inside offset, an outside offset, a pocket and a cross-hatch fill at 33.3 degrees, generated through the installed extension, must give the recorded digest. On Linux x86-64 the extension (GCC 13) and `kerfkite-cam` built with GCC 13, with Clang 18 and with the address and undefined-behaviour sanitizers wrote the same four programs.

Solid modelling is the fourth part computed by the core for Studio. The extension compiles the solid kernel (`native/core/src/solid/`, `solid_command.cpp`) and the polygon kernel's own command (`polygon/command.cpp`), and `studio/solid_kernel.py` gives them the interface Studio's 3D CAD modules call: parts, CAD scripts and their 2D drawings, lofts, STEP tessellations and assemblies. Unlike the operations above, a model is built in many steps, so the extension has a second entry beside `core_call`: `solid_call(request, kept)` answers with the JSON and with the solid itself, which Python keeps and passes back to later requests, so no mesh crosses as text between steps.

```python
from studio import solid_kernel
solid_kernel.solid_backend()   # 'kernel' with the extension, 'manifold' without it
```

Without the extension the same modules model with the third-party library manifold3d, as before. `KERFKITE_SOLID_BACKEND=manifold` selects it where the extension is installed, and `=kernel` refuses to run without the extension. The two give the same shapes within nanometres, except where they facet differently (spheres, round offsets, twisted extrusions), and the kernel is slower. When the kernel refuses a result as too fine for its grid, Studio reports the error; it never switches library for one model. [The solid kernel](SOLID-KERNEL.md#how-studio-uses-the-kernel) has the details and the measurements.

`validation/check_installed_cam.py` pins this too: a part of seven features and a drawing, modelled through Studio's entry points with the installed extension, must give the recorded digest of every vertex, triangle and outline. On Linux x86-64 the extension built with GCC 13, with Clang 18, and with the portable 128-bit integers a compiler without a 128-bit type uses, gave the same digest. Windows (MSVC), macOS and ARM64 builds have not been run; the check is what tests them.

The exact number formatting described above now lives in one header, `native/core/include/kerfkite/number_text.hpp`, used by both the Python extension and the core.

The core reads and writes JSON with its own code (`native/core/include/kerfkite/json.hpp`, `native/core/src/json.cpp`), which replaced the vendored JSON for Modern C++ header; the core has no third-party source. It supports what the core uses: all of RFC 8259 on input, with every escape and surrogate pairs; numbers that keep their kind, so that `1` stays a whole number and `1.0` a float as the Python reference requires; objects with their names in byte order, the last value standing where a name repeats; and one-line output. Its limits: arrays and objects nest at most 200 deep in text that is read, text must be well-formed UTF-8 in both directions, a literal beyond the range of a double is refused, and there is no indented output, comment syntax or binary format. A float is read as the nearest double and written as the shortest text that reads back as the same double, by exact integer arithmetic in `native/core/include/kerfkite/decimal.hpp`; the C library's conversions and the C locale are not involved, and `repr` in `number_text.hpp` and `float()` of text in a project use the same code. Floats are therefore written as Python writes them. The old library wrote two things differently: a float from 1e15 up to 1e16 in exponent form (`1e+15`, now `1000000000000000.0`), and, for roughly one double in a thousand, digits that were not the shortest (`23.343139999999998`, now `23.34314`). Both forms read back as the same double.

It was checked on Linux x86-64 (GCC 13 and Clang 18) in four ways. `native/tests/json_selftest.cpp`, which CTest runs on every platform, covers each construct, the number kinds, the limits and malformed input, holds tables of doubles computed by Python, and compares every short cut in the number conversions with the exact arithmetic behind it on some 300,000 doubles and the decimals written from them. Against Python, 5 million decimal texts (ties between doubles, decimals one digit either side of a tie hundreds of places along, subnormals, exponents far outside the range) gave the double `float()` gives, and 5 million doubles gave the text `repr()` gives, with the short cuts and without them. Against the library it replaced, while that was still in the tree, 440,000 documents (generated, damaged at random, and real: fixtures, projects recorded from the test suite and the answers to them) were read by both: the same were accepted and refused, with the same values, kinds and order; equality between 87,000 pairs of values agreed; and the 26,215 jobs of the parity comparison below gave answers equal in every value and type, 97.7% of them byte for byte, the rest differing only as described above. The one deliberate difference in what is accepted, apart from the depth limit, is that text after a NUL byte is refused, where the old library stopped reading at the NUL. Under the address and undefined-behaviour sanitizers, the self-tests, 500,000 mutated documents, the comparison corpus and 2,500 jobs through `kerfkite-cam` ran without a report. Windows (MSVC), macOS and ARM64 builds have not been run; the self-test is the check for them.

`python validation/native_core_parity.py --cli <kerfkite-cam>` compares the core with the Python reference: number text, every generator call the test suite makes, and thousands of generated edge cases. A case must be reproduced exactly or declined; a different answer fails. With laser jobs ported, the measurement on Linux x86-64 is: 300,000 numbers and 360,000 distances and sums identical; 124 of 202 recorded suite calls reproduced (61%); 11,225 of 12,000 generated cases reproduced (94%), among them 1,137 laser programs; and no differences. The three benchmark programs written by `kerfkite-cam` have the same hashes as Python's.

Drilling (`native/core/src/drilling.cpp`) and drag-knife paths (`native/core/src/knife.cpp`) are ported as well. The generated cases gained 4,000 drilling and knife projects. On Linux x86-64, with those two parts: 160 of 205 recorded suite calls reproduced (78%, from 62%); 15,562 of 16,000 generated cases reproduced (97%), among them 1,290 drilling and 1,634 drag-knife programs; and no differences. The one knife case declined then was a path that returns to a point through steps too short to keep, where the Python divided by zero. That was a defect in the reference: it compared each point with the one before it in the drawing, kept or not. Both now keep a point only when it is further than 0.1 nm from the last point kept, and refuse a path that is then a single point, so the core reproduces every knife job and declines none. Knife swivels use the platform's `atan2`, `sin` and `cos`, as Python does, so they are compared on each platform against the Python there.

Ramp entries and holding tabs on router line jobs are reproduced too (`native/core/src/router_entry.cpp`, `tabs.cpp`). Before either, the reference asks Shapely whether each contour crosses itself; `simple_line.cpp` answers by the rules GEOS applies, including its double-double orientation test, and declines the rare contour for which that test and exact arithmetic disagree. A helix entry needs a pocket, so on a line job it is rejected as the reference rejects it. The ramp slope is `math.tan` of the entry angle, one C library call on each side. With these ported, the measurement on Linux x86-64 is: 161 of 205 recorded suite calls reproduced (79%); 11,432 of the same 12,000 generated cases (95%); and 5,951 of 6,005 ramp, helix and tab cases, among them 3,254 programs, the rest of those declined because they also ask for an offset, a pocket or drilling; no differences.

Helical entries into pockets are reproduced as well (`native/core/src/router_entry.cpp`). The reference placed each helix with Shapely: the pocket's region, two insets of it, the nearest point of an inset to the first point of each pass, a distance to the boundary and a covering test for the feed from the helix to the pass. The vertices of a GEOS inset cannot be reproduced to the last bit by other code, so this part was moved instead, as the offsets and pockets were: `native/core/src/pocket_entry.cpp` places the helix on the core's own polygon kernel by the same rules, the extension exposes it as the `helix` operation of `core_call('operation', ...)`, and `studio/router_entry.py` calls it through `studio.operations.kernel_pocket_entries` whenever the extension is installed. Studio and `kerfkite-cam` therefore place the same helix by construction, and the Shapely code remains as the fallback without the extension (or with `KERFKITE_POLYGON_BACKEND=shapely`). The centre of a helix is a double worked out with `+ - * /` alone, the same on every platform; the start angle is the platform's `atan2`, and the helix points its `sin` and `cos`, as in Python. The move also put right a fault that came with the kernel's pockets: Studio had been checking the kernel's passes, which lie on a grid of a millionth of a millimetre, against a Shapely inset to a hundredth of that, and refused most pockets with walls at an angle.

That the cutter stays clear is checked independently of both codes. `tests/test_router_entry.py` generates pockets of many shapes, with and without islands, and measures with Shapely the distance from every cutter position of the job to the contours as drawn: the centre of each helix is at least the cutter radius plus the helix radius from every wall, with no tolerance, and the feeds and passes are within 0.000002 mm of the cutter radius, the two half steps of the kernel's grid. The core's self-test makes the same check on generated pockets with a distance function of its own and compares a digest of the centres with a recorded value, and `validation/check_installed_cam.py` pins the centres an installed wheel gives.

With these, and with the drag-knife fix, the measurement on Linux x86-64 (GCC 13, Python 3.13) is: 368 of 369 recorded suite calls reproduced (219 programs and 149 rejections; the one declined is V-carving); 19,986 of 20,000 generated cases; all 6,005 ramp, helix and tab cases; and 4,998 of 5,004 pockets entered by a helix, a new group (2,189 programs and 2,809 rejections; the six declined are untidy cases whose operation was replaced with V-carving). Nothing differs, and V-carving is the only feature declined (21 cases in all).

The test of whether a contour crosses itself (`native/core/src/simple_line.cpp`) has a bounded worst case. It used to apply GEOS's test to every pair of segments whose bounding boxes overlap. For a contour of long segments side by side that is every pair: a comb of 20,000 parallel strokes took 2.1 s and one of 100,000, the most a job may hold, about a minute; a fan of rays and a square spiral behaved alike. (A contour that does cross was never slow: the first crossing found ends the test.) Two things bound it now.

* A tree over the segments, walked two branches at a time. A pair of branches is left out whole when GEOS's test is certain to pass every pair of segments in it: when the boxes of the two branches are apart, or when every end point in one branch is clear of, and on one side of, the line of every segment in the other. The second holds because GEOS passes two segments as soon as its orientation test puts both ends of one strictly on one side of the other, and that test is exact whenever its floating-point filter settles the sign; the tree asks for three times the margin the filter needs, at every corner of the boxes of end points, and the determinant is of the first degree in each coordinate, so the corners bound it. The branches are split on whichever coordinate of the segments' first or second end points is most spread, which keeps those boxes small for combs, fans and spirals alike. The same contours of 100,000 segments now take 0.05 to 0.12 s.
* A count of the work, in fixed units of a few nanoseconds each. Past 400 million units, about a second, the contour is declined as not ported. The count does not depend on the machine, so the tests bound it without timing anything: the self-test requires a multiple of n log n for long simple lines of five kinds, and that a contour past a small budget is declined at once.

The answer is unchanged. `validation/native_core_parity.py` compares it with Shapely's `is_simple` for 20,000 lines by each of three methods (the usual one, the tree forced on short lines, and every pair): points on coarse grids, random points, points in a line, and regular lines of 50 to 400 points spoilt by one touch, crossing, overlap, repeated vertex or a vertex a few units in the last place off another segment, with strokes from far apart down to a few units in the last place apart. A further 400,000 lines from other seeds were compared the same way while this was written. The tree and the test of every pair meet the pairs in different orders, so on the rare contour whose answer depends on rounding inside GEOS one may decline where the other answers; where both answer they agree.
