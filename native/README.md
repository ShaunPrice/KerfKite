# KerfKite native core

A C++17 library and command-line tool that produce KerfKite's machine output without Python. This is the
first stage of moving the application to C++: the core first, the editors last.

The Python package remains the reference until the core reproduces all of it. A part of the Python code is
removed only when its C++ replacement gives the same output to the byte on every case that can be compared.

## Build

    cmake -S native -B build/native -DCMAKE_BUILD_TYPE=Release
    cmake --build build/native --config Release
    ctest --test-dir build/native -C Release

A C++17 compiler and CMake 3.16 or later are the only requirements. There is nothing to install: the
first configuration downloads the GEOS 3.14.1 release, checks its SHA-256 and builds it with the core (see
[GEOS](#geos)). `-DKERFKITE_WITH_GEOS=OFF` builds without it and without a network; the polygon operations
are then declined. Apart from GEOS, which is being replaced separately, the core has no third-party
source.

JSON is read and written by the core's own code (`core/include/kerfkite/json.hpp`, `core/src/json.cpp`):
all of RFC 8259 on input, numbers that stay whole numbers or floats as they were written, object names in
byte order, and one-line output. Floats are read as the nearest double and written as the shortest text
that reads back as the same double, in the form Python writes, by exact integer arithmetic
(`core/include/kerfkite/decimal.hpp`) and not by the C library. Arrays and objects nest at most 200 deep
in text that is read, text must be UTF-8, and a number beyond the range of a double is refused.
`tests/json_selftest.cpp` checks all of this under CTest; `docs/NATIVE.md` gives the comparisons made with
Python and with the library this code replaced.

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

    kerfkite-cam gerber LAYER.gbr
    kerfkite-cam gerber-outline EDGE.gbr [--offset [X,Y]]
    kerfkite-cam excellon HOLES.drl [--units mm|inch]
    kerfkite-cam excellon-drills HOLES.drl [--offset [X,Y]]

read circuit board files: a Gerber layer as the objects it draws, a board outline as closed paths, a drill
file as tools and holes, and its holes as drilling operations. The board file itself is the input, and each
`--option value` pair is a field of the request. See `docs/PCB.md`.

    kerfkite-cam polygon request.json

answers a request to the core's own polygon kernel (`"op"`: `union`, `intersection`, `difference`, `xor`,
`normalise`, `compose`, `offset`, `pocket`, `clip-lines`, `artwork`, `inspect`, `locate`), with rings in
millimetres. It is there to test the kernel from outside; see [The polygon kernel](#the-polygon-kernel).

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
| Circuit board import: Gerber layers, Excellon drill files, board outline paths, drilling operations (`core/src/gerber.cpp`, `core/src/excellon.cpp`) | Done. New in the core: there is no Python version. Isolation toolpaths are not generated yet. |
| Drilling at point markers and circular hole outlines, in one plunge or in pecks (`core/src/drilling.cpp`) | Done. The hole list and tool shape that Studio adds for its stock simulation are not in the core's job yet. |
| Drag-knife paths: the alignment lead-in and the swivel at each corner (`core/src/knife.cpp`) | Done, except a path that returns to a point through steps too short to keep: the reference fails on it, and the core declines it. |
| Ramp entry on router line jobs (`core/src/router_entry.cpp`) | Done, with or without holding tabs. A helix entry is accepted only on a pocket, so on a line job it is rejected with the reference's message; on a pocket it waits for pockets and is declined (`helical entry into pockets`). |
| Holding tabs on router line jobs (`core/src/tabs.cpp`) | Done: tab count or explicit positions, tabs across the seam, and every rejection. |
| Shapely's `LineString.is_simple` and `.length`, which both features ask of each contour (`core/src/simple_line.cpp`) | Done by GEOS's own rules, including its double-double orientation test. A contour whose answer depends on rounding inside GEOS is declined. |
| Inside and outside offsets (router: tool radius; laser: half the kerf), concentric pockets, hatch and cross-hatch fills (`core/src/operations.cpp`) | Done, with GEOS 3.14.1, the release inside the Shapely wheels the reference uses. Declined in a build without GEOS. |
| The core's own polygon kernel (`core/src/polygon/`, `core/include/kerfkite/polygon.hpp`) | Done and tested beside GEOS; **not yet what the operations above use**. See [The polygon kernel](#the-polygon-kernel). |
| 3D solid modelling (`core/include/kerfkite/solid.hpp`, `core/src/solid/`) | Done as a kernel and a `solid` command. New in the core: Studio's Python still uses a third-party library and is not switched over. See `docs/SOLID-KERNEL.md`. |
| V-carving | Not yet. Declined with exit status 3. |
| Rotary output, relief, stock simulation, importers, sender, pendant, editors | Not yet. |

A request the core cannot fully reproduce is declined at the point where the Python code would enter the
missing branch. The core never writes a program for a case it does not reproduce.

## Solid kernel

`core/include/kerfkite/solid.hpp` and `core/src/solid/` are KerfKite's own 3D solid modelling kernel: closed
triangle meshes on a 1 nm grid, primitives, extrusion and revolution, affine maps, union, difference and
intersection with exact arithmetic, sections on any plane, ray casting and height maps for machining, and
STL. It uses the C++ standard library only, and is built with or without GEOS.

    kerfkite-cam solid REQUEST.json
    kerfkite-cam solid-batch REQUESTS.jsonl

build a solid from a tree of shapes and operations and answer with its measures and, on request, its mesh,
sections, projection, height map, ray hits or STL. `ctest` runs `kerfkite_solid_selftest`, which checks the
kernel by invariants that need no reference;

    python validation/solid_kernel_check.py --tool build/native/kerfkite-cam

compares it with the third-party library Studio uses today, which is needed for that comparison only.
Studio's Python does not call the kernel yet. See `docs/SOLID-KERNEL.md` for the representation, the rounding
rule, the limits, the measurements and what is ready for 3D engraving and multi-axis work.

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

## The polygon kernel

`core/src/polygon/` is KerfKite's own polygon library, written to take the place of GEOS in the core:
regions with holes, Boolean operations, round offsets, line clipping, and the union of very many shapes
with dark and clear polarity, which printed-circuit isolation needs. It uses the C++17 standard library and
nothing else, and is built whether GEOS is in the build or not. `docs/POLYGON-KERNEL.md` is its full
description; in short:

* Vertices are whole nanometres. Every decision about the shape of a result is exact integer arithmetic in
  128 bits (the compiler's type, or the kernel's own where there is none). A crossing is rounded to the
  nearest grid point and the edges near it are bent through that point, repeatedly, until the edges meet
  only at shared ends. Degenerate input (shared edges, overlaps, slivers, rings that touch themselves) is
  ordinary input.
* An offset is the exact union of the region with polygons built on the grid round each edge and corner, so
  it is always valid and nothing smaller than the tool can turn inside out. Round corners are drawn just
  outside the true arc (by at most 0.00034 of the radius at 32 segments a quarter turn), so a tool path is
  never nearer the work than the tool's radius.
* No trigonometric function of the platform's library is called, so results are the same on every
  platform. The self-test compares a digest of its results with a recorded value.
* A result has a defined order: outer rings counter-clockwise, holes clockwise, each ring from its smallest
  vertex, polygons and holes by their first vertices.

The operations of this core still call GEOS. Replacing those calls is the next step, and the document lists
each call with its replacement and the ways the tool paths will differ (where a ring starts, where the
vertices of an arc fall, the order of the holes of one polygon). The output will then no longer be GEOS's to
the byte, so the comparison below will have to treat offsets, pockets and fills by tolerance, as
`validation/polygon_kernel_check.py` does.

    ctest --test-dir build/native -R polygon
    python validation/polygon_kernel_check.py --cli build/native/kerfkite-cam

The first runs the kernel's self-test, with the compiler's 128-bit integer and with the kernel's own. The
second compares the kernel with Shapely by tolerance over generated cases, including the four operations
made from kernel calls against `studio.operations`.

## How it is checked

    python validation/native_core_parity.py --cli build/native/kerfkite-cam

compares the core with the Python reference in the same run:

* several hundred thousand numbers, including decimal ties and the values either side of them, and as many
  distances and sums;
* every `studio.cam.generate` call made by the Python test suite;
* thousands of generated projects with settings replaced by unusual values, so that rejections are compared
  as well as programs, and thousands of closed contours (alone, nested, overlapping, nearly closed) put
  through the offset, pocket and fill operations;
* thousands of ramp, helix and holding-tab jobs on contours that cross, touch, fold back, nearly close or
  sit at the edge of the work area, and a few at the limits on lines and vertices.

The reference for lengths and totals is CPython 3.12 or later: earlier versions computed `math.dist` and
`sum()` differently in the last bit, so the comparison is run with 3.12 or later.

Knife swivels and laser arc leads take their sines, cosines and arc tangents from the platform's maths
library, as Python does. The core therefore matches the Python on the same machine, while the last bit of
those points may differ from one platform to another.

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
