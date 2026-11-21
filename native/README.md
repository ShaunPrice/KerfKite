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
parser is vendored in `third_party/` (JSON for Modern C++ 3.11.3, MIT), and the first configuration
downloads the GEOS 3.14.1 release, checks its SHA-256 and builds it with the core (see [GEOS](#geos)).
`-DKERFKITE_WITH_GEOS=OFF` builds without it and without a network; the polygon operations are then
declined.

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
| Drag-knife paths, ramp and helix entry, holding tabs, drilling | Not yet. Declined with exit status 3. |
| Inside and outside offsets (router: tool radius; laser: half the kerf), concentric pockets, hatch and cross-hatch fills (`core/src/operations.cpp`) | Done, with GEOS 3.14.1, the release inside the Shapely wheels the reference uses. Declined in a build without GEOS. |
| V-carving | Not yet. Declined with exit status 3. |
| Rotary output, relief, stock simulation, importers, sender, pendant, editors | Not yet. |

A request the core cannot fully reproduce is declined at the point where the Python code would enter the
missing branch. The core never writes a program for a case it does not reproduce.

## GEOS

The Python code does its polygon work through Shapely, which is a wrapper around the GEOS library. The
core calls GEOS directly, through the small layer in `core/src/geometry.hpp`, and makes the same calls
with the same arguments. For the results to be the same bytes the library must be the same release:
Shapely 2.2.0, which the desktop installer pins, carries **GEOS 3.14.1**, so that is the release the core
builds. `shapely.geos_version_string` shows what a Python environment has; the comparison is only
meaningful when it says 3.14.1.

`cmake/geos.cmake` fetches the upstream release tarball (from the GitHub release, then from
download.osgeo.org; both are the same file), checks its SHA-256 and adds it to the build. GEOS is compiled
by its own build files as an optimised build, which is how the Shapely wheels are made, so it gets the
flags its authors set: `-O3 -ffp-contract=off` on GCC and Clang, `/O2 /fp:precise` on MSVC. Its tests,
benchmarks, tools and documentation are not built. The source is not copied into this repository.

| Option | Default | Effect |
| --- | --- | --- |
| `KERFKITE_WITH_GEOS` | `ON` | `OFF` leaves GEOS out. Offsets, pockets and fills are declined, as V-carving is. |
| `KERFKITE_SYSTEM_GEOS` | `OFF` | Link an installed GEOS found with `CMAKE_PREFIX_PATH`. It must be exactly 3.14.1; another version stops the configuration. |
| `KERFKITE_GEOS_SHARED` | `OFF` | Build GEOS as shared libraries placed beside `kerfkite-cam` instead of linking it into the program. |
| `KERFKITE_GEOS_URL` | empty | A local copy or mirror of `geos-3.14.1.tar.bz2`, used instead of downloading. It must have the pinned SHA-256. |

GEOS is LGPL 2.1; `docs/LICENSING.md` says what that asks of anyone who distributes a program containing
it, and `third_party/geos/` holds the licence text.

Measured on Linux x86-64 with two processor cores: building GEOS adds about three and a half minutes to
the first build (about 20 seconds without it), nothing to later builds, and takes `kerfkite-cam` from
0.5 MB to 4.6 MB (0.4 MB to 3.6 MB with symbols stripped). Built as shared libraries, GEOS is 5.5 MB
beside a 0.6 MB program.

What has been shown and what has not. On Linux x86-64 the core's GEOS, compiled here with GCC 13 and again
with Clang 18, gave the same vertices as the wheel's GEOS, compiled by its maintainers with GCC 14, on
every case compared: beyond the 4,000 contour jobs the comparison runs each time, about 70,000 more with
the GCC build and 11,000 with the Clang build.
That is evidence that the compiler does not matter on that platform, not proof for the others. On
macOS, Windows and Linux ARM64 the wheel's GEOS and the core's are again built by different compilers and
possibly different C++ standard libraries. Both use the system's mathematics library, and both are built
without floating-point contraction, so the same results are expected, but only the comparison run on each
platform can show it. The GEOS functions whose results the output depends on are:

* `GEOSBufferWithParams_r` (offsets and pockets; uses sine and cosine for the rounded corners),
* `GEOSSymDifference_r` (the even-odd region) and `GEOSIntersection_r` (hatch rows),
* `GEOSisValid_r`, `GEOSArea_r`, `GEOSLength_r`, `GEOSisEmpty_r` and `GEOSGeom_getExtent_r`,
* and the order in which GEOS returns polygons, holes and ring vertices, which the core keeps.

If a platform differs, the comparison reports the first differing case; the fix is then to decline the
operation on that platform, not to accept a different program.

## How it is checked

    python validation/native_core_parity.py --cli build/native/kerfkite-cam

compares the core with the Python reference in the same run:

* several hundred thousand numbers, including decimal ties and the values either side of them, and as many
  distances and sums;
* every `studio.cam.generate` call made by the Python test suite;
* thousands of generated projects with settings replaced by unusual values, so that rejections are compared
  as well as programs, and thousands of closed contours (alone, nested, overlapping, nearly closed) put
  through the offset, pocket and fill operations.

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
