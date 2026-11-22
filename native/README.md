# KerfKite native core

A C++17 library and command-line tool that produce KerfKite's machine output without Python. This is the
first stage of moving the application to C++: the core first, the editors last.

The Python package remains the reference until the core reproduces all of it. A part of the Python code is
removed only when its C++ replacement gives the same output to the byte on every case that can be compared.

Polygon geometry is the one exception, by the project owner's decision. Offsets, pockets, hatch fills and
V-carving have a single implementation, this core's own polygon kernel, which Studio's Python calls through
its compiled extension. They are shown to be right by geometry (invariants, and a comparison with Shapely
by tolerance), not by matching another library's vertices.

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
`normalise`, `compose`, `offset`, `hull`, `circle`, `transform`, `rotate`, `pocket`, `clip-lines`, `artwork`,
`inspect`, `locate`, `delaunay`, `voronoi`, `distance`), with rings and points in millimetres. It tests the kernel from outside, and it is the command
Studio's CAD scripts draw with; see [The polygon kernel](#the-polygon-kernel).

    kerfkite-cam operation request.json

answers the request Studio's Python makes for an offset, a pocket or a fill once it has checked the
settings (`core/include/kerfkite/operation_command.hpp`): closed contours in, tool paths out. Its `helix`
operation answers where a helix may descend into a pocket: contours and the first point of each pass in, a
centre and a start angle out. Its `vcarve` operation carves the contours with a V-bit: contours, sampling
interval, bit angle, depth and tool radius in, paths and the depth at each of their points out. `generate`
runs the same code for a whole project.

    kerfkite-cam rotary request.json
    kerfkite-cam rotary-wrap request.json

write a job for a rotary axis, as Studio's rotary export does (`core/src/rotary.cpp`, the port of
`studio/rotary.py`). `rotary` takes `{"project": ..., "options": ...}`: the project and the rotary setup
(postprocessor, wrap axis, diameter, drive axis, angle offset, direction, feed reference and fixture) that
the editor sends. `rotary-wrap` takes `{"job": ..., "options": ...}`: a planar program given as G-code
lines. The answer is the rotary job: the program with angles in place of the wrapped axis and inverse-time
feeds, the planar preview segments, totals, warnings and the setup as worked out (`"rotary"`).

    kerfkite-cam relief request.json
    kerfkite-cam stock request.json

prepare a relief and a stock preview, as Studio's relief finishing and `/api/simulate` do
(`core/src/relief.cpp` and `core/src/stock.cpp`, the ports of `studio/relief.py` and
`studio/simulation.py`). `relief` takes the relief settings the editor sends, with the height field as rows
of numbers (`heightmap`) or as RGB bytes in base64 (`heightmap_rgb`, with `heightmap_size` as columns and
rows), and answers with the raster paths, their depths and the surface record. `stock` takes the stock
settings and a job's preview segments and answers with the height map the cutter leaves in the stock.

    kerfkite-cam tool-changes project.json

plans a router project for manual tool changes, as Studio's **Manual tool changes** does
(`core/src/tool_changes.cpp`, the port of `studio/tool_changes.py`): each run of operations with the same
tool (number, diameter, shape, V angle and drill point angle) becomes a stage, a whole job of its own with
the tool, the operations' numbers, a file name and what to do before running it.

    kerfkite-cam stl-heightmap request.json

turns an STL mesh (`content_base64`, binary or ASCII) into the height field of an STL relief at a resolution
of 2 to 128 points a side, as Studio's STL relief does (`core/src/stl_heightmap.cpp`, the port of
`studio/mesh_import.py`), with the mesh's bounds.

    kerfkite-cam solid-cad request.json

rebuilds a parametric solid history (`{"model": ...}`) as Studio's 3D CAD does (`core/src/solid_cad.cpp`,
the port of `studio/solid_cad.py`): parameters and their expressions, boxes, cylinders, spheres, sketched
extrusions and revolutions, lofts and scripted features, patterns and runs of Boolean operations on the
solid kernel, with the mesh, measures and silhouette. `solid-cad-section` (`{"model", "z"}`) gives the
material at a height, and `sketch-solve` (`{"feature", "parameters"}`) a sketch's vertices with its
constraints solved. STEP features are still Python's.

    kerfkite-cam assembly-solve request.json

solves an assembly (`{"assembly", "mesh"}`) as Studio's **Assemblies** does (`core/src/assembly.cpp`, the
port of `studio/assembly.py`): the parts placed by their mates, with the contacts and interferences between
them.

    kerfkite-cam cad-script request.json

previews a CAD script (`{"source", "parameters", "overrides"}`) as Studio's script editor does
(`core/src/scad.cpp`, the port of `studio/scad_language.py` and `studio/scad_geometry.py`): the drawing's
outlines, bounds and area, or the solid's mesh, bounds and volume, or the reference's message with the line
and column.

    kerfkite-cam program-import request.json

reads posted G-code, an HP-GL plot, a LinuxCNC INI file or an ASCII DXF drawing (`{"name", "text", "unit"}`)
as Studio's **Import** does (`core/src/program_import.cpp`, the port of `studio/program_import.py`, and the
core's own DXF reader, `core/src/dxf.cpp`): the artwork, layers, presets, devices and warnings, or the
reference's message. Program text that is not ASCII, and DXF splines given only by fit points, are declined.

    kerfkite-cam artwork-relief request.json

turns closed artwork contours (`paths`) into a raised or recessed relief, as Studio's **Artwork relief**
does (`core/src/artwork_relief.cpp`): a grid of 16 to 128 samples, each set where its cell meets the
artwork (raised) or is not covered by it (recessed), decided exactly on the polygon kernel, and then the
relief's paths. `artwork-raster` answers with the grid of 0s and 1s alone.

    kerfkite-cam fixture-collision request.json

sweeps a posted program's linear moves against fixture boxes, as Studio's fixture check does
(`core/src/fixture_collision.cpp`, the port of `studio/fixture_collision.py`): the cutter and its holder are
upright cylinders, and the answer lists the first contacts of each move (up to 100) with the line, the motion,
the part that touches and where, or refuses the first line it cannot read.

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
| Drilling at point markers and circular hole outlines, in one plunge or in pecks (`core/src/drilling.cpp`) | Done, with the hole list and the drill that Studio's stock preview draws. |
| Drag-knife paths: the alignment lead-in and the swivel at each corner (`core/src/knife.cpp`) | Done. Points within 0.1 nm of the last point kept are dropped, so every move has a direction, and a path that is then a single point is refused, as in the reference. |
| Ramp entry on router line jobs and on offset contours (`core/src/router_entry.cpp`) | Done, with or without holding tabs. A helix entry is accepted only on a pocket, so on a line job it is rejected with the reference's message. |
| Helical entry into pockets (`core/src/router_entry.cpp`, `core/src/pocket_entry.cpp`) | Done. Where each helix goes (clear of the pocket's walls and islands by the cutter's radius, with a clear feed to the pass) is decided on the core's own polygon kernel. This is the only implementation: Studio calls it through its extension (`studio/router_entry.py`), and keeps Shapely only as the fallback where the extension is not installed. |
| Holding tabs on router line jobs (`core/src/tabs.cpp`) | Done: tab count or explicit positions, tabs across the seam, and every rejection. |
| Shapely's `LineString.is_simple` and `.length`, which both features ask of each contour (`core/src/simple_line.cpp`) | Done by GEOS's own rules, including its double-double orientation test. A contour whose answer depends on rounding inside GEOS is declined. The work is bounded: a tree over the segments leaves out, a branch at a time, the pairs GEOS's test is certain to pass, so a contour of 100,000 long segments side by side (a comb, a fan, a spiral) takes a tenth of a second where testing every pair took a minute; and past a fixed count of work, about a second, a contour is declined (`a contour whose self-crossing test needs more comparisons than the core allows`). |
| Inside and outside offsets (router: tool radius; laser: half the kerf), concentric pockets, hatch and cross-hatch fills (`core/src/operations.cpp`) | Done, on the core's own polygon kernel. This is the only implementation: Studio calls it through its extension (`studio/operations.py`), and keeps Shapely only as the fallback where the extension is not installed. |
| The core's own polygon kernel (`core/src/polygon/`, `core/include/kerfkite/polygon.hpp`) | Done. See [The polygon kernel](#the-polygon-kernel). |
| 3D solid modelling (`core/include/kerfkite/solid.hpp`, `core/src/solid/`) | Done as a kernel and a `solid` command. New in the core, and the only implementation of KerfKite's own: Studio's 3D CAD calls it through its extension (`studio/solid_kernel.py`), and keeps a third-party library only as the fallback where the extension is not installed. See `docs/SOLID-KERNEL.md`. |
| The Delaunay triangulation and Voronoi diagram of points, and the distance from a point to a region's boundary (`core/src/polygon/voronoi.cpp`, `distance.cpp`) | Done, in the polygon kernel: exact tests in 128 and 256 bits, O(n log n), points on one line and on one circle as ordinary input. |
| V-carving (`core/src/vcarve.cpp`) | Done, on the kernel's Voronoi diagram: the sampled medial axis of the reference, with its allowance of one sampling interval. This is the only implementation: Studio calls it through its extension (`studio/operations.py`), and keeps Shapely only as the fallback where the extension is not installed. No function of the C library but `sqrt` is behind a carve, so it is the same on every platform. |
| Rotary output for a fourth axis (`core/src/rotary.cpp`): a planar job or a given program wrapped onto a cylinder for LinuxCNC, grblHAL and xPRO V5, on A, B or C, with inverse-time feeds at the surface or cut radius and the fixture's checks | Done, for generated jobs and given G-code. A given program with text that is not ASCII is declined. The cut length uses CPython's `hypot` (`pymath::vector_norm`) and the C library's `asinh`, which CPython calls, so it is Python's on each platform. |
| Studio's CAM on the core | Done. Studio's `generate`, `generate_rotary` and `wrap_job` call the core through the extension (`core_cam` in `studio/_core_native.cpp`) and keep their Python as the reference and the fallback for what the core declines; the job carries everything the Python job has, the paths and depths cut, the holes and drill of a drilling job and the operation count of an ordered one included. See `docs/NATIVE.md`. |
| Relief finishing (`core/src/relief.cpp`): the settings, height fields as numbers or as RGB image bytes, the flat and ball cutter contact on threads, a GPU or both at once, as a calibration at start decides, the raster paths and their depths | Done, to the bit. Studio's `prepare_relief` calls it through the extension and keeps its Python as the reference and the fallback. STL and artwork height fields are still rasterised in Python. |
| Stock preview (`core/src/stock.cpp`): the height map a job's segments leave in the stock, for flat, ball, V and drill cutters | Done, to the bit: distances are CPython's `hypot` and minima Python's `min`. Studio's stock preview calls it through the extension; a segment row with text that is not a number is declined. |
| Manual tool-change plans (`core/src/tool_changes.cpp`): stages for runs of operations with one tool, each a whole job | Done, to the byte, with every refusal. Studio's `plan_tool_changes` and `/api/tool-changes` call it through the extension and keep the Python as the reference and the fallback. A stage of fewer than two program lines, where the reference fails with an IndexError, is declined. |
| Fixture clearance (`core/src/fixture_collision.cpp`): a posted program's moves swept against fixture boxes, continuously | Done, to the byte: CPython's `hypot`, Python's `min` and `max`, and `round(x, 5)` for the positions. Studio's `check_fixtures` and `/api/fixture-collision` call it through the extension and keep the Python as the reference and the fallback. A program line that is not ASCII, which the reference reads with Unicode's rules, is declined. |
| Artwork reliefs (`core/src/artwork_relief.cpp`): closed contours rasterised raised or recessed on the polygon kernel, then the relief | Done, as the one implementation: the kernel decides each cell exactly on its grid, so a cell whose edge lies within a millionth of a millimetre of the outline may differ from the Shapely reference, which answers without the extension. Refusals are the reference's. |
| STL height fields for reliefs (`core/src/stl_heightmap.cpp`): binary and ASCII STL read, normalised and rasterised from above | Done, to the bit, with every refusal. Studio's `stl_heightmap` calls it through the extension and keeps its Python as the reference and the fallback. Base64 text of whole groups followed by nothing but `=`, which some Python releases read and others refuse, is declined. |
| Importers, sender, pendant, editors | Not yet. |

Every operation of a job (lines, drilling, knife paths, offsets, pockets, fills, V-carving, entries and
tabs) is now ported, so no job is declined for its operation. A request the core cannot fully reproduce is
still declined, never approximated: a number written in non-ASCII digits or beyond the range of a double; a
contour whose self-crossing test turns on rounding inside GEOS, or needs more work than the core allows;
and a few degenerate contours under tabs and ramps (a segment of no length, a tab that starts before the
seam). The core never writes a program for a case it does not reproduce.

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

compares it with the third-party library Studio used before, which the core needs for that comparison only.

    build/native/kerfkite_solid_bench [REPEAT] [FILTER]

times twelve models on the kernel and prints a hash of each result, which must not change when the kernel
is made faster. Configured with `-DKERFKITE_SOLID_PROFILE=ON` it also prints the time of each phase; that
option compiles clocks into the kernel and is for measuring only. `validation/benchmark_solid.py` times the
same kind of model through Studio, on the kernel and on the third-party library. See the Performance
section of `docs/SOLID-KERNEL.md`.

Studio's 3D CAD is modelled by this kernel wherever its compiled extension is installed: `setup.py` compiles
the kernel and `solid_command.cpp` into the extension, and `studio/solid_kernel.py` calls them. The 2D
drawings of Studio's CAD scripts run on the polygon kernel, through the same command `kerfkite-cam polygon`
answers, which gained the corners a drawing asks for (inscribed arcs, miters, bevels), a convex hull, a
circle and affine maps. The third-party library remains Studio's fallback without the extension. See `docs/SOLID-KERNEL.md` for
the representation, the rounding rule, the limits, the measurements, how Studio uses the kernel and what is
ready for 3D engraving and multi-axis work.

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
* `delaunay` and `voronoi_edges` give the Delaunay triangulation of grid points and the edges of its dual
  Voronoi diagram: divide and conquer on a quad-edge structure in O(n log n), with the side-of-line test
  in 128 bits and the in-circle test in 256, both exact, so points on one line and four or more on one
  circle need no special care. A Voronoi vertex is worked out as an exact fraction and rounded to the
  nearest grid point, halves upwards. 12,000 points, the most a V-carve samples, take 0.03 s.

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

`core/src/vcarve.cpp` builds V-carving on the kernel. The outlines of the region are sampled every
sampling interval; the Voronoi edges of the samples are cut to the region with `LineClipper`; each piece is
divided into steps, and at each point the bit goes as deep as a circle of radius `min(tool radius,
distance to the wall - interval)` allows, the distance coming from `BoundaryDistance` and the tangent of
the bit's half angle from the kernel's series. Pieces that share an end are joined into longer paths, and
the paths are ordered so that each starts near where the last ended. At every point the cut is an interval
short of the nearest wall, and in the middle of every move at least half an interval, which the self-test
measures with a distance function of its own and `tests/test_vcarve.py` with Shapely.

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
* tens of thousands of lines, random and built to sit on the edge of each rule, for which the core must say
  whether the line crosses itself exactly as Shapely's `is_simple` does, by each of its methods, and long
  lines for which the work it counts must stay within a multiple of n log n;
* every `studio.cam.generate` call made by the Python test suite;
* thousands of generated projects with settings replaced by unusual values, so that rejections are compared
  as well as programs, and thousands of closed contours (alone, nested, overlapping, nearly closed) put
  through the offset, pocket and fill operations;
* thousands of ramp, helix and holding-tab jobs on contours that cross, touch, fold back, nearly close or
  sit at the edge of the work area, and a few at the limits on lines and vertices;
* thousands of pockets entered by a helix: many shapes, with and without islands, as one pocket or several,
  for a range of cutters, depths, helix radii and entry angles, and a few at the limit on lines;
* thousands of V-carves: words of block letters, shapes with holes, thin strokes, flat areas, round shapes
  and stars, for sampling intervals, bit angles, depths, step-downs and tool diameters over their ranges and
  beyond them, and a few at the limits on samples and points.

The reference for lengths and totals is CPython 3.12 or later: earlier versions computed `math.dist` and
`sum()` differently in the last bit, so the comparison is run with 3.12 or later.

Knife swivels, laser arc leads and the points of a helical entry take their sines, cosines, arc cosines and
arc tangents from the platform's maths library, as Python does. The core therefore matches the Python on
the same machine, while the last bit of those points may differ from one platform to another. The centre
of a helix does not: it is worked out with `+ - * /` alone, and the self-test compares a digest of the
centres it places with a recorded value. Nor does a V-carve: its paths and depths use `sqrt` and no other
function of the library, and the self-test compares a digest of them with a recorded value as well.

Each case must be reproduced exactly, or declined. A different answer fails the check. The share of cases
reproduced is the measure of progress, and the list of declined features is the work that remains.

For offsets, pockets and fills the reference calls this core's kernel through the compiled extension, so
the geometry of the operation is shared and cannot differ. Those jobs still compare everything round it:
the checks of the settings and their order, the order of cutting, leads, tabs, ramp entries on the offset
contours, laser power, post-processing and the totals. The place of a helix in a pocket is shared in the
same way (`core/src/pocket_entry.cpp`); the helix built there, its feed to the pass and the limits on it
are compared. That every helix keeps the cutter clear of the walls is checked on its own, by distance from
the contours as drawn: in the self-test with code of its own, and in `tests/test_router_entry.py` with
Shapely as the measure. The paths and depths of a V-carve are shared in the same way
(`core/src/vcarve.cpp`); the levels, the program and the totals built on them are compared, and that the
bit never cuts outside the outline is checked on its own, in the self-test and with Shapely in
`tests/test_vcarve.py` and `validation/vcarve_check.py`. The comparison says which reference it is using.
Without the extension the reference would fall back to Shapely, whose vertices differ, so the comparison
stops; `--without-polygon-operations` runs it with those jobs left out, and says so in its result.
`validation/polygon_kernel_check.py` is the check of the geometry itself.

`.github/workflows/native-core.yml` runs the build, the self-test and this comparison on macOS (Apple
silicon and Intel), Windows, and Linux (x86-64 and ARM64).
`.github/workflows/native-sanitizers.yml` builds the core a second time with Clang on Linux (x86-64 and
ARM64), and builds it under the address and undefined-behaviour sanitizers with GCC and Clang on Linux
(x86-64 and ARM64) and Apple Clang on both kinds of Mac, running the self-tests each time. (GCC on ARM64 runs
without the check for stack frames used after return, which takes hours there; the other five jobs keep it.) The recorded
digests are part of those tests, so every one of these compilers has to give the same results.

## Adding a part

1. Port the Python branch into `core/src/`, keeping the order of its checks so that an invalid project fails
   with the same message.
2. Replace the `NotPorted` it used to throw.
3. Add projects that reach the branch to `generated_cases` in `validation/native_core_parity.py`.
4. Run the comparison. It must report no differences.
