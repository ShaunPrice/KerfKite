# KerfKite native core

A C++17 library and command-line tool that produce KerfKite's machine output without Python. This is the
first stage of moving the application to C++: the core first, the editors last.

The Python package remains the reference until the core reproduces all of it. A part of the Python code is
removed only when its C++ replacement gives the same output to the byte on every case that can be compared.

## Build

    cmake -S native -B build/native -DCMAKE_BUILD_TYPE=Release
    cmake --build build/native --config Release
    ctest --test-dir build/native -C Release

A C++17 compiler and CMake 3.16 or later are the only requirements. There is nothing to install: the JSON
parser is vendored in `third_party/` (JSON for Modern C++ 3.11.3, MIT).

## Use

    kerfkite-cam generate project.json -o program.nc

`project.json` is the object the Studio editor sends to `/api/generate`. The exit status is 0 when the
program was written, 2 when the project was rejected (the message is the one Studio gives), and 3 when the
project needs a feature that is not ported yet. `--json` writes the whole job: lines, preview segments, cut
length and time, bounds and warnings.

    kerfkite-cam probe-plan request.json
    kerfkite-cam height-map request.json
    kerfkite-cam level request.json

are the three steps of probe levelling: a probing program over the cutting bounds of a job, the surface
heights from the probe results, and a program corrected to follow that surface. Each takes a JSON request
(`-` reads standard input) and writes a JSON answer. See `docs/PROBE-LEVELING.md`.

## What is ported

| Area | State |
| --- | --- |
| Number text: fixed-point formatting, `round()`, `repr()` | Done. Shared with the Python extension through `core/include/kerfkite/number_text.hpp`. |
| Project validation and its error messages | Done for every setting of a router line job. |
| Router line jobs, with constant depth or a depth at each vertex | Done. |
| Laser line jobs: straight and arc leads, power ramps, perforation, holding bridges, raster powers, passes | Done. |
| Python's `math.dist` and `sum()` arithmetic (`core/include/kerfkite/py_math.hpp`) | Done, to the last bit, as CPython 3.12 and later compute them. |
| Ordered multi-operation jobs | Done; each operation must itself be ported. |
| Controller post-processing (Grbl family, Marlin, LinuxCNC, Mach3, Mach4, UCCNC) | Done. The table is read from `studio/static/protocols.json` at build time. |
| Probe levelling: probing plan, height map, levelled program (`core/src/leveling.cpp`) | Done. New in the core: there is no Python version, and Studio calls this code through its extension. |
| Ramp entry on router line jobs (`core/src/router_entry.cpp`) | Done, with or without holding tabs. A helix entry is accepted only on a pocket, so on a line job it is rejected with the reference's message; on a pocket it waits for pockets and is declined (`helical entry into pockets`). |
| Holding tabs on router line jobs (`core/src/tabs.cpp`) | Done: tab count or explicit positions, tabs across the seam, and every rejection. |
| Shapely's `LineString.is_simple` and `.length`, which both features ask of each contour (`core/src/simple_line.cpp`) | Done by GEOS's own rules, including its double-double orientation test. A contour whose answer depends on rounding inside GEOS is declined. |
| Drag-knife paths, drilling | Not yet. Declined with exit status 3. |
| Offset, pocket and fill operations | Not yet. These need a geometry library (GEOS, which the Python code reaches through Shapely). |
| Rotary output, relief, stock simulation, importers, sender, pendant, editors | Not yet. |

A request the core cannot fully reproduce is declined at the point where the Python code would enter the
missing branch. The core never writes a program for a case it does not reproduce.

## How it is checked

    python validation/native_core_parity.py --cli build/native/kerfkite-cam

compares the core with the Python reference in the same run:

* several hundred thousand numbers, including decimal ties and the values either side of them, and as many
  distances and sums;
* every `studio.cam.generate` call made by the Python test suite;
* thousands of generated projects with settings replaced by unusual values, so that rejections are compared
  as well as programs;
* thousands of ramp, helix and holding-tab jobs on contours that cross, touch, fold back, nearly close or
  sit at the edge of the work area, and a few at the limits on lines and vertices.

The reference for lengths and totals is CPython 3.12 or later: earlier versions computed `math.dist` and
`sum()` differently in the last bit, so the comparison is run with 3.12 or later.

Each case must be reproduced exactly, or declined. A different answer fails the check. The share of cases
reproduced is the measure of progress, and the list of declined features is the work that remains.

`.github/workflows/native-core.yml` runs the build, the self-test and this comparison on macOS (Apple
silicon and Intel), Windows, and Linux (x86-64 and ARM64).

## Adding a part

1. Port the Python branch into `core/src/`, keeping the order of its checks so that an invalid project fails
   with the same message.
2. Replace the `NotPorted` it used to throw.
3. Add projects that reach the branch to `generated_cases` in `validation/native_core_parity.py`.
4. Run the comparison. It must report no differences.
