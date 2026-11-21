# KerfKite native core

A C++17 library and command-line tool that produce KerfKite's machine output without Python. This is the
first stage of moving the application to C++: the core first, the editors last.

The Python package remains the reference until the core reproduces all of it. A part of the Python code is
removed only when its C++ replacement gives the same output to the byte on every case that can be compared.

Polygon geometry is the one exception, by the project owner's decision. Offsets, pockets and hatch fills
have a single implementation, this core's own polygon kernel, which Studio's Python calls through its
compiled extension. They are shown to be right by geometry (invariants, and a comparison with Shapely by
tolerance), not by matching another library's vertices.

## Build

    cmake -S native -B build/native -DCMAKE_BUILD_TYPE=Release
    cmake --build build/native --config Release
    ctest --test-dir build/native -C Release

A C++17 compiler and CMake 3.16 or later are the only requirements. There is nothing to install and
nothing is downloaded: the core contains no third-party code. (Earlier revisions fetched and built GEOS for
the polygon operations; the core's own polygon kernel has replaced it, and the `KERFKITE_WITH_GEOS`,
`KERFKITE_SYSTEM_GEOS`, `KERFKITE_GEOS_SHARED` and `KERFKITE_GEOS_URL` options no longer exist.) On Linux
x86-64 with two processor cores the whole build takes under a minute.

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
    kerfkite-cam gerber-isolation COPPER.gbr --tool-width MM [--passes N] [--mirror-x X]

read circuit board files: a Gerber layer as the objects it draws, a board outline as closed paths, a drill
file as tools and holes, its holes as drilling operations, and a copper layer as isolation contours. The board file itself is the input, and each
`--option value` pair is a field of the request. See `docs/PCB.md`.

    kerfkite-cam polygon request.json

answers a request to the core's own polygon kernel (`"op"`: `union`, `intersection`, `difference`, `xor`,
`normalise`, `compose`, `offset`, `pocket`, `clip-lines`, `artwork`, `inspect`, `locate`), with rings in
millimetres. It is there to test the kernel from outside; see [The polygon kernel](#the-polygon-kernel).

    kerfkite-cam operation request.json

answers the request Studio's Python makes for an offset, a pocket or a fill once it has checked the
settings (`core/include/kerfkite/operation_command.hpp`): closed contours in, tool paths out. `generate`
runs the same code for a whole project.

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
| Circuit board import: Gerber layers, Excellon drill files, board outline paths, drilling operations (`core/src/gerber.cpp`, `core/src/excellon.cpp`) | Done. New in the core: there is no Python version. |
| Isolation toolpaths from a copper layer, with the places the cutter cannot reach (`core/src/isolation.cpp`) | Done. New in the core, on the polygon kernel: there is no Python version. |
| Drilling at point markers and circular hole outlines, in one plunge or in pecks (`core/src/drilling.cpp`) | Done. The hole list and tool shape that Studio adds for its stock simulation are not in the core's job yet. |
| Drag-knife paths: the alignment lead-in and the swivel at each corner (`core/src/knife.cpp`) | Done. Points within 0.1 nm of the last point kept are dropped, so every move has a direction, and a path that is then a single point is refused, as in the reference. |
| Ramp entry on router line jobs (`core/src/router_entry.cpp`) | Done, with or without holding tabs. A helix entry is accepted only on a pocket, so on a line job it is rejected with the reference's message; on a pocket it waits for pockets and is declined (`helical entry into pockets`). |
| Holding tabs on router line jobs (`core/src/tabs.cpp`) | Done: tab count or explicit positions, tabs across the seam, and every rejection. |
| Shapely's `LineString.is_simple` and `.length`, which both features ask of each contour (`core/src/simple_line.cpp`) | Done by GEOS's own rules, including its double-double orientation test. A contour whose answer depends on rounding inside GEOS is declined. |
| Inside and outside offsets (router: tool radius; laser: half the kerf), concentric pockets, hatch and cross-hatch fills (`core/src/operations.cpp`) | Done, on the core's own polygon kernel. This is the only implementation: Studio calls it through its extension (`studio/operations.py`), and keeps Shapely only as the fallback where the extension is not installed. |
| The core's own polygon kernel (`core/src/polygon/`, `core/include/kerfkite/polygon.hpp`) | Done. See [The polygon kernel](#the-polygon-kernel). |
| 3D solid modelling (`core/include/kerfkite/solid.hpp`, `core/src/solid/`) | Done as a kernel and a `solid` command. New in the core: Studio's Python still uses a third-party library and is not switched over. See `docs/SOLID-KERNEL.md`. |
| V-carving | Not yet. Declined with exit status 3. |
| Rotary output, relief, stock simulation, importers, sender, pendant, editors | Not yet. |

A request the core cannot fully reproduce is declined at the point where the Python code would enter the
missing branch. The core never writes a program for a case it does not reproduce.

## Solid kernel

`core/include/kerfkite/solid.hpp` and `core/src/solid/` are KerfKite's own 3D solid modelling kernel: closed
triangle meshes on a 1 nm grid, primitives, extrusion and revolution, affine maps, union, difference and
intersection with exact arithmetic, sections on any plane, ray casting and height maps for machining, and
STL. It uses the C++ standard library only.

    kerfkite-cam solid REQUEST.json
    kerfkite-cam solid-batch REQUESTS.jsonl

build a solid from a tree of shapes and operations and answer with its measures and, on request, its mesh,
sections, projection, height map, ray hits or STL. `ctest` runs `kerfkite_solid_selftest`, which checks the
kernel by invariants that need no reference;

    python validation/solid_kernel_check.py --tool build/native/kerfkite-cam

compares it with the third-party library Studio uses today, which is needed for that comparison only.
Studio's Python does not call the kernel yet. See `docs/SOLID-KERNEL.md` for the representation, the rounding
rule, the limits, the measurements and what is ready for 3D engraving and multi-axis work.

## The polygon kernel

`core/src/polygon/` is KerfKite's own polygon library, which took the place of GEOS in the core: regions
with holes, Boolean operations, round offsets, line clipping, and the union of very many shapes with dark
and clear polarity, which printed-circuit isolation needs. It uses the C++17 standard library and nothing
else. `docs/POLYGON-KERNEL.md` is its full description; in short:

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

`core/src/operations.cpp` builds the four operations on it. The contours of a job are placed on the grid
and combined by the even-odd rule; an offset is `offset()`; a pocket is `offset()` again and again until
nothing is left; a fill clips rows to the region with `LineClipper`. A grid coordinate `n` leaves the
kernel as the double `n / 1e6`, one correctly rounded division, so the doubles the generator formats are
the same on every platform; the program writes three decimals, a thousand grid steps. Hatch rows are turned
by a sine and cosine computed from a fixed series, not by the C library, and a piece of a row shorter than
0.001 mm is dropped, because a stroke shorter than the program's last decimal cannot be cut (the Shapely
code kept pieces down to 0.000001 mm).

Compared with the paths GEOS gave, the same regions are cut, with vertices in other places: rings start at
their left-most vertex, the corners of an arc lie just outside the true arc where GEOS's lay just
inside, vertices on straight runs are dropped, and a pocket has far fewer vertices because an arc is not
drawn again at every pass. `docs/POLYGON-KERNEL.md` gives the measured differences.

    ctest --test-dir build/native -R polygon
    python validation/polygon_kernel_check.py --cli build/native/kerfkite-cam

The first runs the kernel's self-test, with the compiler's 128-bit integer and with the kernel's own. The
second compares the kernel with Shapely by tolerance over generated cases; its `cam` group runs
`studio.operations.prepare` on the kernel and on its Shapely fallback for each generated job and compares
the tool paths, and checks that this tool and the extension answer alike to the last bit.

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

For offsets, pockets and fills the reference calls this core's kernel through the compiled extension, so
the geometry of the operation is shared and cannot differ. Those jobs still compare everything round it:
the checks of the settings and their order, the order of cutting, leads, tabs, ramp entries on the offset
contours, laser power, post-processing and the totals. The comparison says which reference it is using.
Without the extension the reference would fall back to Shapely, whose vertices differ, so the comparison
stops; `--without-polygon-operations` runs it with those jobs left out, and says so in its result.
`validation/polygon_kernel_check.py` is the check of the geometry itself.

`.github/workflows/native-core.yml` runs the build, the self-test and this comparison on macOS (Apple
silicon and Intel), Windows, and Linux (x86-64 and ARM64).

## Adding a part

1. Port the Python branch into `core/src/`, keeping the order of its checks so that an invalid project fails
   with the same message.
2. Replace the `NotPorted` it used to throw.
3. Add projects that reach the branch to `generated_cases` in `validation/native_core_parity.py`.
4. Run the comparison. It must report no differences.
