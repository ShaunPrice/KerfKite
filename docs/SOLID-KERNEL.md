# The solid kernel

KerfKite has its own 3D solid modelling kernel in the native core: `native/core/include/kerfkite/solid.hpp`, implemented in `native/core/src/solid/`. It depends on the C++17 standard library and nothing else. It was written to replace the third-party library behind Studio's 3D CAD (see [Third-party components](SUPPLY-CHAIN.md)) and to carry 3D engraving and, later, multi-axis machining.

**Studio uses it.** Wherever the compiled extension is installed, Studio's 3D CAD (parametric parts, CAD scripts, lofts, STEP tessellations, assemblies) is modelled by this kernel, and the 2D drawings of the script engine by the [polygon kernel](POLYGON-KERNEL.md). The third-party library remains in two roles only: the fallback for an installation without the extension, and the oracle the kernel is tested against. The section [How Studio uses the kernel](#how-studio-uses-the-kernel) describes the switch, what each of Studio's calls became, and what Studio does when the kernel refuses a result. The kernel is also reached from C++ and through the `kerfkite-cam solid` command.

## What a solid is

A `Solid` is a closed triangle mesh whose vertices lie on a grid of **1 nm** (`kUnitsPerMm` = 10^6 grid units to the millimetre). Coordinates are limited to ±100 000 mm. Every solid the kernel returns satisfies all of the following, checked exactly:

- every edge is used by exactly two triangles, in opposite directions (the surface is closed and consistently oriented);
- no triangle has zero area;
- the volume is positive (the triangles are counter-clockwise seen from outside).

The empty solid is the one exception: it has no triangles.

A solid may have several shells: separate bodies, and cavities inside bodies. Two bodies that only touch, at a point or along an edge, stay separate: each has its own copy of the vertices where they touch, so the "two triangles at every edge" rule still holds. `num_bodies()` counts connected shells, so a cavity counts as one.

Vertices are stored in ascending (x, y, z) order. Each triangle starts at its lowest vertex, and the triangles are in ascending order of their three indices. This order is part of the result: the same input gives the same arrays on every platform.

A `Solid` never changes after it is made, and copies share one mesh. Reading a solid, and running operations on the same solid, from several threads is safe: the one thing an operation records on a shared mesh, that it has been found free of self-intersection, is an atomic flag.

## Exact arithmetic

Every decision about topology (which side of a plane a point is on, whether two triangles meet, the order of triangles around an edge) is made with integer arithmetic and gives the exact answer. Floating point is used for three other things only: to skip exact work when an interval of doubles already proves the sign; to choose between results that are all valid (which diagonal a triangulation uses, which sub-nanometre features are dropped); and for measurements such as areas, ray hits and height maps.

| Value | Size | Type |
| --- | --- | --- |
| Grid coordinate | below 2^37 | 64-bit integer |
| Orientation of four grid points | below 2^117 | 128-bit integer (`I128`: the compiler's type where there is one, a portable two-word version otherwise; both give the same integers) |
| Plane of a grid triangle: normal, offset | below 2^78, 2^117 | `I128` |
| Where an edge meets a plane: numerators, denominator | below 2^155, 2^118 | `BigInt`, a fixed-capacity integer of 1536 bits |
| Where three planes meet: numerators, denominator | below 2^273, 2^234 | `BigInt` |
| Orientation of four such points | below 2^1058 | `BigInt` |

A constructed point is kept as an exact rational (x, y, z) / w. Beside it the kernel keeps an interval of doubles that is guaranteed to contain each coordinate. A predicate is evaluated on the intervals first; if the interval result excludes zero, that is the sign. Otherwise the predicate is evaluated again with the integers. Either way the sign returned is the exact one, so the intervals affect speed only. A coordinate that happens to be a whole number is kept exactly, so that points in an axis-aligned plane give exact zeros without the integers.

A point where an edge crosses a plane starts with the interval alone: its rational is computed when a predicate cannot decide from the interval, or when the point becomes part of a triangle. Most such points never are. [Performance](#performance) describes this and the other ways the kernel avoids exact work whose answer is already certain, with the error bound of each test in doubles.

Reaching the capacity of `BigInt` raises an internal error; it does not wrap around. The bounds above hold for every input within the coordinate limit.

## Curved surfaces

Circles are faceted by a segment count, as Studio asks for them today.

- `cylinder`: a regular polygon of `segments` sides inscribed in the circle, with a vertex on the +X axis. 4n − 4 triangles for a cylinder or frustum.
- `sphere`: `segments` around the equator (rounded up to a multiple of 4) and half as many bands from pole to pole, with all vertices on the sphere. 2n(n/2 − 1) triangles. The third-party library subdivides an octahedron instead, so its spheres have other triangles and a slightly different volume; both are inscribed.
- `revolve`: `segments` equal steps over the angle turned, whatever the angle, starting on the +X axis.
- `extrude`: `slices` layers; the top is turned by `twist_degrees` (counter-clockwise seen from +Z) and then scaled.

Sines and cosines do not come from the C library, whose last digit differs between platforms. `sincos_degrees` reduces the angle exactly to at most 45°, converts to radians with one multiplication, and evaluates the two Taylor series to x^17 and x^18 in a fixed order of additions, multiplications and divisions. Each of those operations is correctly rounded under IEEE 754, so every platform computes the same doubles. The terms left out are below 9·10^-20 and 4·10^-21, and the result is within 4·10^-16 of the true value (the self-test compares 20 000 angles with the C library). Multiples of 90°, and 30° and 45°, are exact. `sincos_turn(k, n)` gives the angle 360·k/n with exactly mirrored results around the circle, so a polygon with a multiple of 4 sides touches its bounding box exactly. Each coordinate is then rounded to the grid.

## Affine maps

A translation is rounded to the grid and is then exact. A map whose matrix is whole numbers (quarter turns, mirrors in the axis planes, whole-number scales) is exact too, and keeps the mesh as it is. Any other map rounds every vertex to the nearest grid point. If that rounding leaves a triangle without area or folds the surface, the mesh is repaired by the engine described next; if nothing is left, the map is refused.

## Boolean operations

The kernel computes the **arrangement** of the operands' triangles and reads the result from it. The method is that of mesh arrangements with winding numbers (Zhou, Grinspun, Zorin and Jacobson, *Mesh arrangements for solid geometry*, 2016), written from the published description.

1. **Intersect.** A bounding-box tree over each operand finds the pairs of triangles that may meet. Each pair is intersected exactly: where two triangles cross, the segment they share is recorded on both, its ends being grid vertices or exact rational points. Triangles that lie in one plane and overlap are grouped, to be subdivided together.
2. **Subdivide.** Each triangle with records is triangulated again, so that every recorded point is a vertex and every recorded segment an edge (an incremental Delaunay triangulation: points inserted in the order of a Hilbert curve, flips decided by an in-circle test on interval enclosures, then edge recovery by flips; see [Stage two](#stage-two-the-same-solids-divided-differently)). A group of coplanar triangles is triangulated as one, so that where they overlap they are cut into the *same* pieces. After this step, triangles meet only in whole edges and vertices.
3. **Cells.** Identical triangles are merged into faces. Around every edge the faces are put in exact angular order, which joins their sides into the regions of space (cells) that the faces bound.
4. **Winding numbers.** Crossing a face changes, by a known amount, the number of times each operand's surface winds around a point. One exact ray count for each connected piece fixes the winding numbers of every cell.
5. **Extract.** A cell is solid or empty by the rule of the operation (union: inside either; intersection: inside both; difference: inside the first and not the second). The faces between a solid cell and an empty one are the result. They are paired at each edge across the solid side, so that the mesh is closed and oriented by construction and bodies that only touch stay separate.

Coplanar faces, shared edges and vertices, identical solids, one solid inside another, and solids that touch without overlapping are not special cases of this method: they are ordinary inputs to exact predicates.

A batch union joins neighbours in the list pair by pair until one solid is left, so each operand takes part in about log2(n) operations. A batch difference is the first solid minus the union of the rest. A batch intersection folds from the left.

An operand that has not been checked for self-intersections (a primitive just made, a solid after a rounded map) is intersected with itself as part of the same arrangement, and the answer is remembered. A self-intersecting operand is therefore resolved, not trusted: its solid part is what its surface winds around a positive number of times.

## Rounding, and why the result stays valid

Before rounding, the result of step 5 is exact: a valid solid whose new vertices are rational points. Vertices the operands already had are grid points and do not move. Each new vertex is rounded to the nearest grid point, less than 0.87 nm away.

For features much larger than the grid, rounding by under a nanometre cannot change the topology. For features about as fine as the grid it can flatten a triangle, push one triangle through another, or turn a sliver inside out. Such features arise in ordinary use: two faces that should coincide, on solids turned by the same angle, differ by the rounding of their vertices and cross at a hair's angle. The kernel deals with this in three stages.

**Before rounding, sub-resolution features are removed where that is exact.**

- *Thin cells.* A cell whose volume is less than one grid unit times its surface area (a slab thinner than about 2 nm) takes the state, solid or empty, of what surrounds most of it. This is exact in the sense that matters: the boundary between solid and empty cells is a closed, oriented surface without self-intersections whatever state each cell is given.
- *Ribbons.* A crack between two nearly coincident faces is open at its rim, so it is part of the space around it and not a cell. Where two crossing triangles lie within 8 nm of each other's planes, the kernel adds thin separating triangles (16 nm either side of each edge, belonging to no solid) that close the rim. The crack then is a cell, and the rule above applies.
- *Unneeded vertices.* A vertex whose triangles lie in one plane, or in two planes meeting in a straight edge through it, is taken out and the hole filled with fewer triangles. The surface as a set of points does not change. This removes the traces of subdivision on faces that end up whole, and leaves fewer points to round.

**After rounding, what is still below the resolution is merged.** Rounded vertices joined by an edge of at most 2 nm become one vertex (triangles that close up are dropped and the rest re-paired). A triangle left with a corner within 2 nm of its opposite edge has that edge replaced by the other diagonal.

**Then the result is checked, exactly.**

- No triangle with a moved vertex has zero area.
- No triangle with a moved vertex meets any other triangle except in a common vertex or a whole common edge.
- For every shell, whether or not it moved, the space in front of one of its triangles is empty (an exact ray count), so no shell has turned inside out and none lies on the wrong side of another.

Triangles without a moved vertex are faces of the exact arrangement and cannot meet wrongly. So after the second check the shells are closed surfaces that do not cross, and the third then fixes what is on each side of every shell: together the three checks are the definition of a valid solid, not a sample of it. If a check fails, the rounded mesh is resolved against itself with the same engine and rounded again, up to four times. If it still fails, the whole operation is tried again with the sizes above doubled, at most three times, doubling each time (so features up to about 16 nm may then be removed). If that fails too, the operation is refused with the error "the result has features too fine for the 1 nm grid". **An invalid solid is never returned.**

Consequences to know:

- The volume of a result can differ from the exact volume by a layer of a few nanometres on the surfaces involved. The identities V(A∪B) + V(A∩B) = V(A) + V(B) and V(A−B) = V(A) − V(A∩B) hold to that tolerance, not exactly.
- Material or gaps thinner than about 2 nm are not kept.
- The refusal is rare. In the generated tests it occurred for solids whose faces nearly coincide after an arbitrary turn (solids built to share faces and then turned together; an intersection joined back to its turned operand), at about one case in a thousand of those kinds, and for 2 of 25 000 random trees; the measured counts are in [Testing](#testing).

## Sections and projection

`section(solid, z)` gives the material at the horizontal plane as closed rings, counter-clockwise around material and clockwise around holes. A vertex exactly in the plane counts as below it, so a face lying in the plane is included when the material is above it and excluded when it is below ("lower faces inclusive, upper faces exclusive"). Each triangle with vertices on both sides gives one directed segment, and the segments are joined through the triangles' shared edges, not by comparing coordinates, so the rings are closed for any input. A ring has a point where it passes from one triangle to the next, except where the two triangles are parts of one flat face (an exact test): the section of a box is four points however its faces are triangulated. A face that has been turned by an arbitrary angle is no longer exactly flat on the grid, and a section keeps a point where it crosses the crease between its triangles; Studio drops such points (within 2 nm of the line between their neighbours) before it shows or cuts an outline.

`section(solid, plane)` does the same for any plane, and returns the rings both in the plane's own (u, v) coordinates and in space. This is the section needed for tilted tool axes.

`projection(solid)` is the outline seen along Z. It takes the edges where the upward-facing part of the surface ends and fills them by the positive winding rule; a through hole shows, a blind pocket does not.

Behind `extrude`, `revolve` and `projection` is a small exact 2D arrangement (`planar.cpp`, `profile.cpp`): segments on the grid are split where they cross, triangulated, and filled by the even-odd, non-zero or positive rule. `resolve_rings` exposes it, and `region_triangles` gives the triangles of the region where the rings wind at least a given number of times, with lines that split triangles without bounding anything, grouped into pieces joined edge to edge with their boundary loops; the surface mesher cuts drawn outlines with it. Rings may cross themselves or each other; crossings are resolved before rounding.

## Queries for machining

These are measurements in doubles on the exact grid coordinates.

| Call | Gives |
| --- | --- |
| `Index(solid)` | A bounding-box tree over the solid, kept for many queries. |
| `Index::raycast(origin, direction, max_distance)` | The nearest hit: distance, point, the triangle and its unit outward normal. |
| `Index::occluded(from, direction, distance, clearance)` | Whether anything lies along a direction from a point: the test for whether a tool axis reaches a surface point. |
| `Index::contains(point)` | Whether a point is inside the solid. |
| `height_map(solid, x0, y0, dx, dy, columns, rows, empty)` | The highest surface Z above each node of a regular XY grid: the input for 3D relief engraving. |
| `height_map(solid, frame, …)` | The same along any tool axis: heights along `frame.w` over a grid in `frame.u`, `frame.v`. |
| `frame_for_axis(origin, axis)` | An orthonormal frame for a tool axis. |
| `sample_surface(solid, spacing)` | Points spread over the surface with their normals, in a defined order. |
| `triangle_normals()`, `vertex_normals()` | Unit normals for each triangle, and area-weighted for each vertex. |
| `section(solid, plane)` | Sections on tilted planes. |
| `min_gap(a, b, search_length)` | The smallest distance between two surfaces, for contact tests in assemblies. |

A ray or a height-map node exactly on an edge or a vertex of the mesh hits one of the triangles there: the two triangles at an edge evaluate the same products, so a ray cannot pass between them. The self-test checks that a height map agrees with rays cast from above at every node.

**What is ready for 3D engraving:** the height map of a solid at any resolution and along any axis, in the form `studio/relief.py` consumes (a grid of heights; the caller normalises to 0..1 and chooses the depth).

**What is ready for multi-axis work:** surface points with normals, sections on any plane, visibility along any direction, and height maps along any tool axis. **What is not written:** toolpaths, tool and holder shapes, gouge and collision checking for a moving tool, and machine kinematics. `height_map` samples at the nodes; it does not give the conservative cell maximum that `studio/mesh_import.py` computes for unknown meshes.

## STL

`read_stl` reads binary and ASCII files (a binary file is recognised by its length matching its triangle count). `from_stl` welds the triangles on the grid into a solid, and accepts solids that touch along edges or at points. Malformed files, files over 256 MiB, and triangle counts over the caller's limit are refused. `to_stl_binary` writes the header Studio writes and computes each normal as Studio's Python writer does. `to_stl_ascii` writes coordinates as exact decimals of millimetres, so a file reads back to the identical solid.

Studio's existing STL code (`studio/solid_cad.py` for export, `studio/mesh_import.py` for the relief height field) is unchanged.

## Limits

| Limit | Value | When exceeded |
| --- | --- | --- |
| Coordinate | ±100 000 mm | refused |
| Triangles in one solid, or in the operands of one operation | 2 000 000 | refused |
| Triangle pairs one operation examines | 60 000 000 | refused ("simplify them or combine fewer at once") |
| Points in the rings of one extrusion, revolution or projection | 200 000 | refused |
| Segments of a circle | 3 to 65 536 | refused |
| Height-map nodes | 16 000 000 | refused |
| STL file | 256 MiB | refused |

Refusals are `kerfkite::ValueError` with a message that names the cause. There is no recursion on the size of the input: trees are walked with explicit stacks.

## The `solid` command

    kerfkite-cam solid REQUEST.json
    kerfkite-cam solid-batch REQUESTS.jsonl

The request is `{"shape": NODE, ...}`. A node is one of:

| Node | Meaning |
| --- | --- |
| `{"box": [x, y, z], "center": false}` | A box. |
| `{"cylinder": {"height", "radius" or "radius_low" and "radius_high", "segments", "center"}}` | A cylinder, frustum or cone. |
| `{"sphere": {"radius", "segments"}}` | A sphere. |
| `{"extrude": {"rings", "height", "slices", "twist", "scale": [sx, sy], "fill"}}` | A prism over rings of [x, y] points. `fill` is `"evenodd"`, `"nonzero"` or `"positive"`. |
| `{"revolve": {"rings", "segments", "degrees", "fill"}}` | A solid of revolution; ring x is the radius, y is Z. |
| `{"mesh": {"vertices", "triangles", "weld"}}`, `{"grid_mesh": …}` | The caller's triangles, in millimetres or in nanometres. |
| `{"hull": [NODE, …]}` | The convex hull. |
| `{"union": […]}`, `{"difference": […]}`, `{"intersection": […]}` | Boolean operations on a list. |
| `{"translate": [x, y, z], "of": NODE}`, and likewise `rotate`, `scale`, `mirror`, `rotate_about` (`{"axis", "degrees"}`), `matrix` (three rows of four) | Maps. |
| `{"kept": i}` | A solid the caller kept from an earlier request (see below). Not available from the command line. |

The answer always has `volume`, `area`, `bounds`, `centroid`, `vertices`, `triangles`, `bodies`, `empty` and the counts of the last operation. On request it adds: `"mesh": true` and `"grid_mesh": true` (the arrays), `"check": true` (the exact self-intersection count and the complete check), `"sections": [z, …]`, `"plane_sections": [{"origin", "normal"}, …]`, `"projection": true`, `"height_map": {"x0", "y0", "dx", "dy", "columns", "rows", "empty"}`, `"rays": [{"origin", "direction"}, …]`, `"min_gap": {"to": NODE, "search_length"}` (the distance to another shape) and `"stl": "ascii"`. `solid-batch` answers each line with `{"ok": …}` or `{"error": message}`.

JSON appears only in this command (`native/core/src/solid_command.cpp`); the kernel's own interface has none.

**Kept solids.** Studio builds a model one operation at a time and asks about each intermediate solid (its triangle count, its bounds). So that a mesh does not cross as text at every step, the compiled extension has a second entry beside `core_call("solid", request)`: `solid_call(request, kept)` answers with the same JSON and with the solid itself, as an opaque object that Python holds and hands back in the `kept` list of a later request, where the node `{"kept": i}` stands for it. A kept solid shares its mesh, never changes, and remembers what the kernel has established about it (that it has no self-intersection), which a mesh sent back as text would lose. The object is released when Python drops it. A third entry, `solid_mesh(solid)`, gives the mesh of a kept solid as two byte strings (64-bit floats x, y, z in millimetres for each vertex, 32-bit numbers a, b, c for each triangle): the numbers of the `mesh` of an answer, without writing thousands of them as text for numpy to read back.

## Testing

Correctness is shown in two independent ways.

**Invariants that need no oracle** (`native/tests/solid_selftest.cpp`, run by CTest as `solid`):

- exact expected results on small cases: two unit cubes offset by half give volumes 1.875, 0.875 and 0.125 exactly with 20, 14 and 8 vertices; cubes sharing a face become one box of 8 vertices; cubes sharing an edge or a corner stay two bodies; a cavity is a second shell;
- on generated cases of six kinds (placed primitives; solids sharing faces and then turned together; a solid and a copy a few nanometres away; chains of operations; batches against the same operations one by one; idempotence and absorption): the volume identities to the tolerance above, commutativity, A∪A = A and A∩A = A to the last bit, A−A empty;
- every result checked by `count_self_intersections` (exactly no pair of triangles meeting wrongly) and by `verify_exact` (the arrangement of the result with itself: every cell wound exactly once or not at all);
- section areas integrating to the volume, a height map agreeing with rays at every node, an ASCII STL reading back to the identical solid.

`--stress N SEED` runs N cases of each kind with full verification; `--digest N SEED` prints one hash of every resulting mesh, for comparing compilers.

**Two builds as solids** (`kerfkite_solid_shapes`, `native/tools/solid_shapes.cpp`). For a change that may divide a solid into other triangles but must leave it the same solid: one build records every Boolean operation of generated models with operands and results (`record CASES SEED FILE`, `models FILE`), the other replays them (`replay FILE OUT`), and either compares the two files (`compare FILE_A FILE_B`), record by record, as the same mesh, the same set of points or within the grid. [Stage two](#stage-two-the-same-solids-divided-differently) gives the results for the change it was written for.

**The shorter ways against the full computation** (the same self-test). Each way the kernel has of avoiding exact work (see [Performance](#performance)) is checked against the computation it stands in for: the 256-bit product and the three-word sum against the big integer; the flat tables against the standard ones; a tree over part of the boxes against a scan; a pool that leaves crossings pending against one that computes each at once, over requests with crossings on the grid, repeats and points reached two ways (the same ids, the same intervals once computed, the same answer from every predicate); the turn and the plane side decided in doubles against the exact ones, on triples in one line and points on and beside the plane; a probing arrangement against a counting one on triangles that touch. And whole operations are computed twice, with every shorter way taken and with none (`detail::plain_paths()`), and must give the same vertices, triangles and counts. `--plain N SEED` does that for N generated pairs of solids.

**Comparison with the third-party library as an oracle** (`validation/solid_kernel_check.py`). It builds every solid twice, in the oracle and as a request to the kernel, from the same leaves, and compares volume, bounding box, number of bodies and section areas at three heights. Tolerances follow from the grid: a layer of 4 nm for each operation on the area of the leaves. It also runs Studio's own tests for solids, scripts, assemblies and lofts on a stand-in for the library, so that those tests run on the kernel's primitives and every solid they make is compared.

The oracle is not always right, and the check does not take its word where the two differ in volume. It follows the difference down to the operation where it starts and tests that operation by a rule that needs neither side: the union or intersection of a solid with itself is that solid, and a hull contains every point of its operands. A case is set aside only when the rule shows the oracle wrong and the kernel right.

Results measured on this code (GCC 13.3 and Clang 18.1 on x86-64 Linux):

| Check | Result |
| --- | --- |
| Self-test, as CTest runs it | Passes. |
| Operations with every shorter way and with none (`--plain 6000 77`) | 18 000 operations: the same vertices, triangles and counts in every one. |
| Generated cases with every result verified exactly (`--stress 1000 77`) | 6000 cases, 0 failures. 2 refused as too fine, both among the 1000 cases of solids sharing faces and then turned together. |
| The same under AddressSanitizer and UndefinedBehaviorSanitizer (`--stress 350 23`) | 2100 cases, 0 failures, no report from either sanitizer. |
| GCC `-O3` against Clang `-O3` (`--digest 700 2026`) | 4200 cases: the hash of every vertex and triangle of every result is equal. 3 refusals in each (2 of the 700 turned-together cases, 1 of the 700 absorption cases). |
| The compiler's 128-bit integers against the portable ones (`-DKERFKITE_SOLID_PORTABLE_ARITHMETIC`) | The same hash on the same 4200 cases. |
| Warnings under `-Wall -Wextra` | None with either compiler. |
| Primitives against the oracle's own | 42 compared, all within tolerance. |
| Random trees against the oracle, seeds 1 to 5, 5000 trees each | 24 956 solids evaluated by both. **0 failures.** Largest difference as a share of its tolerance: volume 0.78, bounding box 0.21, section area 0.47. |
| Refused by the kernel as too fine, of those 25 000 trees | 2. The oracle refused none. |
| Not comparable: a hull over a flake | 30. The oracle keeps flakes without volume (a few triangles at one point, where two solids touch) as bodies; the kernel has none, and the hull of a flake is a solid of full size. |
| Set aside because the oracle is wrong by a rule | 4: in two, the oracle's union or intersection of a solid with itself has another volume than the solid (by 0.08 and 0.27 mm³); in two, the oracle's hull leaves its own operands' points more than 7 mm outside. The kernel satisfies the rule in each. |
| Bodies and bounds that differ only by oracle flakes | 16 solids where the oracle counts flakes as bodies, 34 where its bounds include them; equal when they are left out. |
| Bounds that differ with equal volumes | 21. In every one the oracle's bounds extend beyond the kernel's (by 0.17 to 3.3 mm) and the kernel's never beyond the oracle's by more than 0.6 nm, while the volumes agree within 6·10^-6 mm³: the oracle has a fin without thickness on a body. |
| Different number of bodies with equal volumes | 83. In the cases examined, either bodies touch along an edge or at a point (the kernel keeps them apart, the oracle may join them), or faces lie closer than the grid (they coincide in the kernel and it has one body; the oracle has two). Not all 83 were examined one by one. |
| Studio's tests on the kernel's solids | 30 tests in four modules, all passing. 370 solids made, 148 distinct ones compared, all within tolerance. |

The commands, from the repository root with the core built in `build/native`:

    build/native/kerfkite_solid_selftest --stress 1000 77
    build/native/kerfkite_solid_selftest --digest 700 2026      # the last field must be equal on every compiler and platform
    build/native/kerfkite_solid_selftest --plain 6000 77        # every shorter way against none: "0 differ"
    build/native/kerfkite_solid_bench 5                         # times, and a hash of each model that must not change
    python validation/benchmark_solid.py                        # the same through Studio, on the kernel and the library
    python validation/solid_kernel_check.py --tool build/native/kerfkite-cam --trees 5000 --seed 1

What these runs do not show: the hash comparison was not made with MSVC, nor on ARM64 or macOS; the differential trees are at most three operations deep, on primitives a few millimetres across; and the sanitizer run covers the generated cases, not the JSON command. The tests in doubles described under [Performance](#performance) were likewise run with GCC and Clang on x86-64 only. They cannot change a result on any platform, because each falls back to the integers when it is not certain; what remains to confirm elsewhere is the digest, as before.

## Performance

The kernel was made faster in two stages. The first changed no result: every digest recorded for it stayed what it was, and so did every answer of the `solid` command on the models measured below, to the last character (their measures, meshes, outlines and the counts of each operation). The second, [below](#stage-two-the-same-solids-divided-differently), changes how some faces are divided into triangles and so the recorded digests, once and deliberately, while every solid stays the same set of points; a tool written for it checks that. This section says what was measured, where the time went, what was changed and why, and what is left.

### What was measured

Two tools, both in the repository:

- `kerfkite_solid_bench [REPEAT] [FILTER]` (`native/tools/solid_bench.cpp`) builds twelve models on the kernel alone and prints the best time of each with a hash of every vertex and triangle of the result. Built with `-DKERFKITE_SOLID_PROFILE=ON` it also prints the time of each phase (clocks in `core/src/solid/timing.hpp`, compiled into no other build).
- `validation/benchmark_solid.py` rebuilds the same kind of model through Studio's entry points (`studio.solid_cad.rebuild`, `studio.scad_geometry.build_script`) on the kernel and on the third-party library, each in a fresh interpreter. With `--answers` it hashes every answer the kernel gives on the way instead of timing.

All times are from the two-core sandbox this was developed in, GCC 13.3 at `-O3`, one thread. The machine was shared with other builds, and a run could take half as long again from one minute to the next; each figure is the best of several runs (3 to 7 of a model, 18 to 30 of a model under 50 ms).

The kernel alone:

| Model | Before | After | Triangles out |
| --- | --- | --- | --- |
| A plate with one bore | 7.0 ms | 2.9 ms | 400 |
| The pinned part without its scripted post: five tapered bores, a sphere, a turned rib, a revolved groove, a trim | 857 ms | 172 ms | 12 014 |
| The pinned part | 992 ms | 202 ms | 13 222 |
| A plate less 64 bores in a row, each overlapping the next, as one pattern | 5.02 s | 1.34 s | 6448 |
| A plate less 64 separate bores, as one pattern | 1.35 s | 0.61 s | 24 844 |
| The same plate cut by one bore after another, 64 operations | 15.1 s | 2.03 s | 24 844 |
| Two spheres, 48 segments | 21.8 ms | 11.6 ms | 3808 |
| Two spheres, 96 segments | 78.1 ms | 47.3 ms | 14 544 |
| A union of 50 turned boxes | 85.9 ms | 41.9 ms | 3406 |
| A block less 8 slots, 16 bores and a sphere, 21 operations | 491 ms | 83.8 ms | 5746 |
| A revolved profile with two cuts | 30.4 ms | 12.1 ms | 1672 |
| A twisted extrusion, bored and trimmed | 29.2 ms | 14.1 ms | 1496 |

The same through Studio, with the third-party library beside it, is under [What differs for a user](#what-differs-for-a-user).

The larger operations that `kerfkite_solid_selftest --bench` times:

| Task | Before | After |
| --- | --- | --- |
| Union, difference, intersection of two spheres of 20 448 triangles each, one turned and offset | 0.24 s, 0.13 s, 0.12 s | 0.12 s, 0.05 s, 0.03 s |
| The same with 39 600 triangles each | 0.39 s, 0.32 s, 0.20 s | 0.24 s, 0.08 s, 0.06 s |
| The same with 101 760 triangles each | 1.02 s, 0.81 s, 0.60 s | 0.65 s, 0.24 s, 0.18 s |
| Union of a sphere of 20 448 triangles with itself turned about its centre: every triangle crosses several others (78 284 crossing pairs, 197 468 triangles out) | 3.6 s | 1.7 s |
| Batch union of 200 overlapping cylinders (37 600 triangles in, 1532 out) | 3.9 s | 1.6 s |
| Height map of 500 × 500 nodes over a solid of 86 560 triangles | 0.008 s | 0.008 s |
| One horizontal section of the same solid | 0.0003 s | 0.0003 s |
| Projection of the same solid | 0.025 s | 0.026 s |
| Index of the same solid and 200 000 rays | 0.19 s | 0.20 s |
| Exact self-intersection check of the same solid | 0.17 s | 0.17 s |

The last five rows are code this work did not touch; they differ by the noise of the machine. An operation still costs little for the triangles that stay whole and most for those that are cut, so the two slow rows remain the ones where nearly every triangle is cut. Every row is within the aim the kernel was written to: a few seconds for the Boolean rows, about a second for a height map over 50 000 triangles.

### Where the time went

Shares of the time of one build, from the profiled build. "Checking" is the exact check of the rounded result, with everything it runs.

| Phase | Plate with one bore | The pinned part | 64 overlapping bores | 64 bores one by one |
| --- | --- | --- | --- | --- |
| Before: finding candidate pairs | 7% | 8% | 2% | 28% |
| Before: intersecting pairs | 34% | 14% | 11% | 57% |
| Before: subdividing cut triangles | 22% | 5% | 45% | 1% |
| Before: removing unneeded vertices | 19% | 13% | 37% | 7% |
| Before: checking the rounded result | 0% | 49% | 1% | 0% |
| Before: everything else | 18% | 11% | 4% | 7% |
| After: finding candidate pairs | 1% | 7% | 1% | 4% |
| After: intersecting pairs | 14% | 17% | 7% | 50% |
| After: subdividing cut triangles | 49% | 15% | 65% | 7% |
| After: removing unneeded vertices | 10% | 19% | 21% | 23% |
| After: checking the rounded result | 0% | 25% | 1% | 0% |
| After: everything else | 26% | 17% | 5% | 16% |

Before, four things took most of the time, none of them the geometry a user asked for:

- **Points computed to be thrown away.** A wall of a bore passes through the top face of a plate. That face is many triangles, and for every one of them whose box overlaps the wall's, the kernel worked out where the wall's edges cross the face's plane and where the face triangle's edges cross the wall's plane, with big integers, looked each point up among the points it had, and then found, by putting the points in order along a line, that the two triangles do not meet. The last of 64 cuts of one bore after another made 57 074 such points; the cut itself adds about two hundred vertices to the plate.
- **Triangles looked up against their own solid.** Every triangle was looked up in one tree over all triangles, and those of its own solid thrown away afterwards. A plate with many holes has long thin triangles on its faces whose boxes overlap thousands of others.
- **Big integers for answers known in advance.** Whether four points lie in one plane was computed from the points, when the triangles they belong to were already known to come from one plane; whether three points lie in one line, when all three had just been placed on one segment. Those are the cases where the intervals cannot decide, so each went to a determinant of numbers hundreds of bits long.
- **The check of the rounded result done as a full intersection.** It needs one answer, whether any pair meets, and made and kept every point as if a result were to be built from them.

### What was changed, and why no result can change

The contract is that the kernel gives the same integers as before: the same points with the same ids in the same order, the same triangles, the same counts. Each change below is either a way of finding the same thing in less time, or a test whose answer is known to be the exact one whenever it gives one.

**Crossings remembered.** Where an edge between two grid points crosses a plane depends only on the ratio of the plane's values at the two ends. The first time, the point is made by the pool as before; after that an equal ratio on the same edge returns the same point. Ratios are compared exactly, as 256-bit products. The pool held each value once already, so it would have returned that id itself, after computing the point again. A triangle also remembers the points it has on the last two planes it met, and planes are compared exactly (three corners of one triangle in the plane of the other).

**Crossings left pending.** A crossing takes its id and its place among the points at once, with an enclosure in doubles, and its rational is computed only when a predicate cannot decide from the enclosure or the point becomes part of a triangle. The pool's two rules are kept exactly: a value is held once, and a value on the grid is a grid point. A point is left pending only when its enclosure proves it is off the grid (some coordinate's enclosure holds no whole number) and differs from every point the pool has (no point filed under the same unit cell has intervals meeting it in all three coordinates); otherwise it is computed at once. So every point has the id it had, and the count of points made is what it was. *The enclosure:* the point is a + (b − a)·L with L = s / (s + t), where s and t are the sizes of the plane's values at a and b. Each value is converted to a double within a relative 2^-51 (two rounded conversions and a rounded sum), so the rounded sum and quotient give L within 2^-49. With D = b − a exact, the rounded product and sum are within |D|·(2^-49 + 2^-53) + |x|·2^-53 of the true coordinate x. The enclosure is x ± (|D|·2^-47 + |x|·2^-51), four times that. For coordinates up to 100 m it is at most 0.002 nm wide either side.

**The check only probes.** The arrangement that checks a rounded result is told that only one answer is wanted. Its crossings are made for one pair, without being looked up or filed, and dropped after it. This is exact because the answer for a pair depends on its points through their values alone: it compares them along a line by value, and it asks whether a point is a corner of a triangle by id, where a crossing is never a corner of the triangle whose edge it lies inside. Triangles that only touch the other's plane at one corner are decided from that corner without making the other triangle's crossings. The self-test compares a probing arrangement with a counting one on 59 634 pairs of lattice triangles, 11 747 of which meet.

**One tree for each operand, built when it pays.** Triangles of a solid known to be free of self-intersection are not looked up against one another, and a tree is built only after a dozen questions have been asked of it (before that, every box is looked at). The pairs found are the same set, examined in the same order.

**Turns and sides in doubles first.** Two tests in doubles stand in front of exact predicates. Each computes the value in doubles together with a bound on its error, and answers only when the value is beyond the bound; otherwise the exact computation runs as before.

- *The turn of three points in a plane* (`Planar::filtered_turn`), the sign of A·B − C·D with A = b_u − a_u, B = c_v − a_v, C = b_v − a_v, D = c_u − a_u. Each coordinate is a double m with a radius r (zero for a whole coordinate; half the interval's width plus the rounding of the midpoint otherwise). With u = 2^-53: the true A is within dA = |A|·u + r_a + r_b of the computed one, the true A·B within |A|·dB + |B|·dA + dA·dB of the product of the computed ones, which the rounded product P is within |P|·u of, and the rounded difference within |P − Q|·u of P − Q. The bound is the sum of those, computed with 2u for u and then enlarged by a millionth, which covers the rounding of the bound itself (about twenty operations, each within 2^-53).
- *The side of a plane a grid point is on* (`Arrangement::side_in_doubles`), the sign of n·p − d. The four numbers of the plane are within 2^-51 of their integers and p is exact, so the computed value is within M·(2^-51 + 4·2^-53) of the true one, M being |n_x p_x| + |n_y p_y| + |n_z p_z| + |d|. The bound is M·2^-49.

Both use only additions, subtractions, multiplications and comparisons of doubles, which IEEE 754 rounds the same way everywhere, and the build forbids contraction into fused operations (`-ffp-contract=off`, `/fp:precise`); no function of the C library is involved. So the same cases are decided in doubles on every platform. That is not needed for the results to agree, because the answer is the exact sign either way; it only means the work done is the same too. The self-test compares each test with the exact predicate on generated points: 900 000 turns, 103 951 of them of three points in one line, and 209 123 sides, 73 445 of them of points on the plane and as many again one grid unit beside it, with coordinates up to the 100 m limit. No decided sign was wrong, and no point on the plane or in the line was decided (110 004 turns and 73 449 sides were left to the exact computation).

**Planes and lines known from where they came.** Every triangle of a result lies in the plane of one loaded triangle, and the kernel now carries that through. Whether two neighbouring triangles are flat across their edge, in removing unneeded vertices and in ordering the faces round an edge, is whether two grid triangles lie in one plane, which takes three small products in place of a determinant of big integers. (If the two planes are one, the fourth point is in the plane of the other three. If they are two, they meet in the line of the shared edge, which the fourth point is not on.) Triangles found to share a plane are joined into sets so that the question is computed once. In the triangulation, a point placed on a segment is recorded as lying on its line, and three points of one segment's line do not turn. Frames for 2D tests within a plane are taken along the largest component of the plane's normal, which is never edge-on; within one plane a turn has the same sign relative to the triangle's own in every frame that is not edge-on, so the choice changes no answer.

**Shapes examined once.** A primitive is not known to be free of self-intersection, so its first operation examines every pair of its own triangles whose boxes overlap: thousands for a cylinder, whose cap triangles all share a corner. For a box, a straight cylinder or a prism nothing comes of it: no pair meets, and no edge passes through the plane of another triangle, so no point is asked for. Such a shape is remembered (the triangles and the vertices relative to the first, compared in full, in a list of at most 48 shapes for each thread), with the number of pairs examined. An operand of a remembered shape is not examined again; the number is added to the count of pairs, so the counts are what they were. Everything the examination decides is exact and unchanged by moving a solid a whole number of grid units. Spheres and tapered cylinders are not remembered: their nearly flat facets are bent a nanometre one way or the other by the grid, and some pairs do ask for a point.

**Less work for the same integers.** 128-bit products are computed on the two's complement bits without separating the signs. Faces, edge uses and the triangles of a solid are put in order by a counting pass on the first key and small sorts within it (the order `std::sort` gives). The node-based hash tables are flat ones; none is ever walked in its own order. Coplanar triangles separated by an edge of one of them are told apart after three turns, and the turns of two triangles are computed once each where the tests asked for them up to three times. One triangulation object serves every triangle of an operation. The centroid is summed in three machine words. A mesh crosses from the kernel to numpy as bytes (`solid_mesh`), with the numbers the text had.

### Stage two: the same solids, divided differently

The work above kept every integer the kernel gives. The second stage takes the gains that change how a solid is divided into triangles but not the solid, with the recorded digests re-recorded once (the owner's decision, 22 November 2026).

- **Runs of features in Studio.** Studio applied each feature as it came, so a plate with 60 bores was 60 differences, each arranging the whole plate again. Consecutive features with one operation are now applied as one batch (`studio/solid_cad.py`): cuts as the solid less the union of their tools, additions as one union, intersections as one intersection. That is most of what makes lazily evaluated solid libraries fast. A run can only shrink the solid (cuts, intersections) or only grow it (additions), so the solid after a run is empty only if one after a feature would have been. When the batch fails, or its result does not pass Studio's checks, the run is done again feature by feature, so that the error names the feature that causes it, and errors are reported in the order of the features. The triangle limit applies to the solid after each run.
- **Faces triangulated by Delaunay insertion** (`planar.cpp`). The points of a face are inserted in the order of a Hilbert curve over their bounding box (by the middles of their intervals), so each is found by a short walk from the one before. After each insertion the edges around the new point are flipped while the point across one of them is proved strictly inside the circle through the triangle on this side (Lawson's flips), then each required edge is recovered by flips as before (Sloan), and the edges that are not required are flipped by the same rule until none can be. The in-circle test is evaluated on the interval enclosures of the four points with outward rounding; where the intervals cannot tell, which includes every point on the circle, the edge stays. Every flip is then a true Delaunay flip, so the flipping ends, and the triangulation is a fixed function of the points, the required edges and the order of insertion, which is fixed: the same on every platform. Before, the triangulation had no Delaunay step during insertion and inserted points in order of their coordinates, so the walks were long where triangles were long and thin, the first two items that were left under [What is left](#what-is-left).
- **Pairs apart on the line of their planes** (`Arrangement::apart_on_line`). Two triangles that each cross the other's plane, with no corner in it, have a point in common exactly when their segments on the line where the two planes meet overlap. Four orientations of their corners, exact in 128 bits, decide that before any point is made for the pair. It changes no result.
- **Edge owners found by index.** The faces of a cluster that own each edge are found through a sorted index instead of a scan of every face for every piece of edge. It changes no result.
- **Unions composed.** A batch union groups its solids by whether their bounding boxes meet (overlap or touch), joins each group pair by pair, and puts the groups side by side as one mesh: solids of different groups are a whole grid unit apart in some coordinate, so no triangle of one meets one of another. A difference leaves out the tools whose box is apart from the solid's.

**How the solids were shown to be the same.** `kerfkite_solid_shapes` (`native/tools/solid_shapes.cpp`) records every Boolean operation that generated models make on one build of the kernel, with its operands and result, replays the same operations on the same operands on another build, and compares the results: as meshes, as sets of points (`same_shape()`, the exact arrangement of the two meshes, in which no region may be inside one alone), or, failing that, as within what the grid holds (each difference of the two is empty to the kernel, whose Boolean operations leave out what is thinner than the grid). Recorded on the kernel before this stage and replayed on it after:

| Of 7532 operations of 1500 generated models (10 kinds), 958 of them empty | |
| --- | --- |
| The same mesh | 6463 |
| The same set of points, exactly | 7480 |
| Within the grid: each difference empty to the kernel | 52; thickest layer 0.15 nm, furthest vertex 34 nm from the other surface |
| Different | **0** |
| Refused as too fine | 1, in both |

The 52 are where a different division of a face leads the rounding to the grid to drop a different sliver; the furthest vertex lies on such a sliver, which is far thinner than it is long. Through Studio, every model of `validation/benchmark_solid.py` built in runs was compared with the same model built feature by feature: in every one, both differences of the two are empty to the kernel. Only the block with 21 cuts is divided differently, into 14 614 triangles instead of 12 834 (the cavity's faces), with a volume 2.3·10⁻⁶ mm³ apart. The self-test still passes, `--plain 200 7` finds no difference between the shorter ways and none, and `--stress 300 77` and the oracle comparison (`solid_kernel_check.py --trees 2000 --seed 1`, with Studio's 32 solid tests on the stand-in) find no failure. The new triangulation gives the same hashes on every platform it was built on: MSVC 2022 on Windows x64, and GCC 13.3 on x86-64 Linux and on ARM64 Linux, print `4b462149515efb4b` for `--digest 700 2026`, `30aa2a95a9dd185e` for `--digest 500 11` and `f91cdc30edb8e0c2` for `--stress 1000 77` (0 failed in each), and the CTest digest passes there; `--stress 350 23` under AddressSanitizer and UndefinedBehaviorSanitizer has no failure and no report.

**What was re-recorded.** The generated-case digest of the self-test (`--digest 150 2026`, run by CTest) is now `e150789607e7145b` (it was `d606943f815a575d`), and the hash of the model "64 separate bores, one pattern" of `kerfkite_solid_bench` changed. The part pinned by the installed-wheel check (`6c12e0b4dd788bbb`) and every other benchmark model kept their hashes.

**Times.** The kernel alone, best of several runs on the two-core sandbox:

| Model | Before stage two | After |
| --- | --- | --- |
| A plate less 64 separate bores, as one pattern | 523 ms | 266 ms |
| The same plate cut by one bore after another, 64 operations | 1.57 s | 1.04 s |
| A plate less 64 bores in a row, each overlapping the next | 1.30 s | 1.13 s |
| The pinned part | 204 ms | 196 ms |
| A block less 8 slots, 16 bores and a sphere, 21 operations | 84 ms | 82 ms |

The small models of the benchmark are within the noise of the machine, except the revolved and extruded parts, about 8 per cent slower: on faces of few points the flips cost more than the shorter walks save. Through Studio the runs of features give most of the gain; see the table under [What differs for a user](#what-differs-for-a-user).

### What is left

Where the kernel still spends its time, and what could be done about it:

- **Many surfaces in one plane.** The 64 overlapping bores still spend more than half their time subdividing the caps, which all lie in two planes and are cut by one another, after stage two shortened the walks (627 ms of 1.08 s in the profiled build, against 873 ms of 1.36 s before).
- **Checking the rounded result** is a quarter of the pinned part. Triangles of one flat face are still compared pair by pair; a check of each flat patch as a whole (every triangle keeps its turn and the boundary does not cross itself) would be linear, and needs a proof before it replaces the pairwise one.
- **The examination of a sphere or a tapered cylinder against itself** (about a third of two spheres) cannot be skipped as an inert shape's can. Marking primitives as free of self-intersection when they are made would save it, and would change the recorded count of pairs examined and, in rare ties, the order of points.
- **Threads.** The kernel uses one, and none were added. Operations that do not depend on one another (the groups and the pairs at one level of a batch union, the features of a pattern) could run on several threads with the same result whatever the scheduling, since no operation's result depends on another's state. Since Studio now applies runs of features as batches, that is where its time goes, so threads are the next step; the counts of the last operation are kept for each thread and would have to be gathered in a fixed order. Within one operation the points are numbered in the order the pairs are examined, so the examination itself cannot be split without a second pass that restores the order.

## How Studio uses the kernel

### Which code runs

`studio.solid_kernel.solid_backend()` answers `kernel` when the compiled extension is installed and `manifold` when it is not (the portable wheel, or a source checkout that was never built). `KERFKITE_SOLID_BACKEND=manifold` selects the third-party library where the extension is installed; `=kernel` refuses to run without the extension; `auto` is the default. With neither the extension nor the library, modelling fails with a message that names both.

```python
from studio import solid_kernel
solid_kernel.solid_backend()   # 'kernel' with the extension, 'manifold' without it
```

`studio/solid_kernel.py` gives the two kernels the part of the library's interface that Studio calls, so the modelling code (`solid_cad.py`, `scad_geometry.py`, `solid_loft.py`, the STEP tessellation in `cad_exchange.py`, `assembly.py`) is the same on both. Nothing in Studio's modelling stays on the library when the kernel is in use, and the library is not imported then. Reading STEP itself is still the optional Open Cascade add-on; the kernel takes its triangles.

| Studio calls | Now runs on | Notes |
| --- | --- | --- |
| `Manifold.cube`, `.cylinder`, `.sphere` | `box`, `cylinder`, `sphere` | The sphere is faceted differently; see below. |
| `Manifold(Mesh64(vertices, triangles))` (lofts, polyhedra) | `from_mesh` | A mesh that is not a closed solid gives a status the callers test, as the library does. |
| `mesh64.merge()` then `Manifold(mesh64)` (STEP tessellations) | `from_mesh` with `weld` | Welds on the 1 nm grid. |
| `.rotate`, `.translate`, `.scale`, `.mirror`, `.transform` | `rotated`, `translated`, `scaled`, `mirrored`, `transformed` | Rounded to 1 nm; quarter turns, axis mirrors and whole-number scales are exact. |
| `a + b`, `a - b`, `a ^ b`, `Manifold.batch_boolean` | `batch_boolean` | |
| `Manifold.batch_hull` | `convex_hull` | Points all in one plane are refused. |
| `.volume()`, `.surface_area()`, `.bounding_box()`, `.num_tri()`, `.num_vert()`, `.is_empty()`, `.status()` | the measures in every answer | |
| `.to_mesh64()` | the `mesh` of the answer | Vertex and triangle order are the kernel's. |
| `.slice(z)` | `section(solid, z)` | Then cleared of points on straight sides, as described under Sections. |
| `.project()` | `projection(solid)` | |
| `.min_gap(other, length)` (assembly contact) | `min_gap` | |
| `triangulate(polygon)` (polyhedron faces) | a small routine in `solid_kernel.py` | Cuts off one corner at a time and uses every vertex of the face. |
| `CrossSection`: polygons by a fill rule, `square`, `circle`, Boolean operations, `hull`, `offset`, transforms, `bounds`, `area` | the polygon kernel, through its `polygon` command | A circle has exactly the vertices of the solid kernel's cylinder. Offsets are described in [The polygon kernel](POLYGON-KERNEL.md#offsets). |
| `CrossSection.extrude`, `.revolve` | `extrude`, `revolve` | Same twist sense and order (twist, then scale); same steps and start. |

### When the kernel refuses

The kernel refuses a result it cannot represent validly on its grid ("the result has features too fine for the 1 nm grid"; see [Rounding](#rounding-and-why-the-result-stays-valid)). Studio then reports an error that names the feature or the script line and says what to do: two faces almost coincide there, usually after a turn, and moving, turning or resizing one shape by a small amount removes the cause.

**Studio never hands the model to the other library**, whether the backend was chosen explicitly or by `auto`. The two facet differently, so a part that silently changed kernel between two rebuilds would change shape, triangle count and section outlines without the user having changed anything. A user who needs the model as it stood can set `KERFKITE_SOLID_BACKEND=manifold` for the whole installation. The refusal was not met in any model of Studio's tests or documents, nor in 5000 random scripts run on both backends (below).

### What differs for a user

- **Spheres.** Both kernels put `$fn` vertices round the equator. The kernel's sphere is bands of latitude and has about twice the triangles of the library's (9024 against 4608 at 96 segments), so its volume is a little nearer the true sphere and models with many spheres reach Studio's triangle limits at about half the number of spheres. The script engine's limit on overlapping triangle pairs is four times as large on the kernel, so that two overlapping spheres within the limit before are within it now.
- **Round offsets** (`offset(r=…)`). Both draw each round corner with vertices on the true arc. The kernel divides a corner into equal parts of at most 1.0625 steps of 360° / `$fn`; the library takes whole steps from one end. The outlines differ by slivers far inside the facet error either has against the true arc.
- **Twisted extrusions.** The side of a twisted extrusion is made of quadrilaterals that are not flat, and the two kernels may split them along different diagonals. The outlines at each layer are the same; the volume between layers is not, and with few slices the difference is large (a 4.5 by 5.7 mm outline turned 30° and tapered in a single slice: 15% above the true volume on the kernel, 24% below it on the library). More slices bring both to the true shape.
- **Coordinates** are whole nanometres. A value such as `14.649999998509884` is now `14.65`.
- **Section outlines** have a point at each corner and none along straight sides. The library's sections carry points along straight sides too: a turned box cut through five of its faces gave 10 points there and gives 5 here.
- **Speed.** The exact checks cost time: the kernel is slower than the library, by a factor of 2 to 4 on plates with bores, boxes, revolved and extruded parts, 4 to 8 where spheres or tapered and turned tools are cut (every vertex made by such a cut has to be rounded and the result checked), and 9 on a pattern of bores that overlap one another, whose caps are many surfaces in one plane. Before the work described under [Performance](#performance) the factors were about 6 to 45.
- **Runs of features.** Consecutive features with one operation are applied together (see [Stage two](#stage-two-the-same-solids-divided-differently)), on either backend. The solid is the same; where tools overlap one another, its faces can be divided differently from the feature-by-feature way, and the triangle limit applies after each run of features rather than after each feature.

Time for one rebuild through Studio, on the two-core sandbox this was developed in. The machine was shared with other builds; each figure is the best of 5 rebuilds (18 to 30 of a model under 50 ms). The models are those of `validation/benchmark_solid.py`, which prints this table. Both backends run the same Studio code, so the library's times now include the runs of features as well; "before" is the kernel of the first stage with features applied one by one.

| Model | Library | Kernel before | Kernel now | Now, against the library |
| --- | --- | --- | --- | --- |
| A plate with one bore | 2 ms | 4 ms | 5 ms | 2.5 |
| The pinned bracket of the installed check: seven features, a sphere, tapered bores, a turned rib, a revolved groove, a scripted post | 61 ms | 259 ms | 259 ms | 4.3 |
| A plate with 64 overlapping bores in one pattern | 135 ms | 1.47 s | 1.22 s | 9.0 |
| A plate with 64 separate bores in eight patterns | 144 ms | 1.45 s | 390 ms | 2.7 |
| A plate with 24 separate bores, one feature each | 44 ms | 385 ms | 143 ms | 3.3 |
| A plate with 60 separate bores, one feature each | 101 ms | 1.94 s | 357 ms | 3.5 |
| Two spheres, `$fn = 48` | 3 ms | 12 ms | 12 ms | 3.8 |
| Two spheres, `$fn = 96` | 10 ms | 48 ms | 47 ms | 4.6 |
| A union of 50 turned boxes | 17 ms | 44 ms | 45 ms | 2.6 |
| A block with 21 cuts: slots, tapered bores, cross bores, a sphere | 37 ms | 293 ms | 286 ms | 7.7 |
| A revolved profile with two cuts | 6 ms | 12 ms | 13 ms | 2.1 |
| A twisted extrusion, bored and trimmed | 5 ms | 13 ms | 13 ms | 2.8 |

### How the switch is tested

- `tests/test_solid_backends.py` builds every model of Studio's tests and every program of the scripting guide on both backends, through the same Studio code, and compares them: 22 drawings and 33 solids by volume, surface area, bounds, three sections and the projection; 6 solids with spheres, a twisted extrusion and 2 assemblies by what their faceting leaves comparable. The tolerances are stated at the head of that file and follow from the grid: a 1 nm shift of each surface for each of the two results, so a volume within 2 nm times the surface area. The largest difference found is 0.12 of its tolerance (volume 5.6·10^-5 mm³, surface area 2.9·10^-5 mm², section area 2.5·10^-5 mm², bounds within 1 nm).
- The same file tests the choice of backend, modelling with the library absent, modelling with the extension absent, and that a refusal is an error and never another backend.
- Three tests in `tests/test_scad_geometry.py` compared an area or a volume with its exact value to six or seven decimal places. They now allow the rounding of a circle's vertices to the grid: the perimeter of the circles times half a grid diagonal (2.7·10^-5 mm² for the drawing, 4.0·10^-5 and 3.6·10^-5 mm³ for the two solids).
- `validation/check_installed_cam.py` models a part and a drawing through Studio's entry points with the installed extension and compares every vertex, triangle and outline with a recorded digest (`6c12e0b4dd788bbb`). That is the proof, on every platform a wheel or installer is built for, that the package carries the kernel and that the kernel gives the same bytes there. The digest is the same from GCC 13.3, from Clang 18.1, and with the portable 128-bit integers that a compiler without a 128-bit type (MSVC) uses, all on x86-64 Linux.
- `validation/solid_kernel_check.py` runs Studio's four solid test modules on a stand-in that builds every solid in both: 30 tests, all passing, 148 distinct solids compared, 0 failures.
- `validation/simulate_solid_workspace.py`, the browser exercise of the solid workspace, passes on the kernel.
- `validation/solid_backend_check.py` builds random CAD scripts through Studio on both backends: drawings (polygons with holes, Boolean operations, offsets with each kind of corner) and solids (every primitive, extrusions, revolutions, polyhedra, transforms, Boolean operations and hulls, nested up to three deep). It is run by hand, not by the test suite. Measured with seeds 1 to 5, 600 drawings and 400 solids each:

| | Drawings | Solids |
| --- | --- | --- |
| Built | 3000 | 2000 |
| Agree within the grid's tolerance | 2954 (largest difference 0.35 of its tolerance) | 1421 (largest 0.083) |
| Refused by both (an empty result, a budget) | 46 | 187 |
| Not compared: a twisted extrusion in the script | | 390 |
| Refused by the kernel as too fine | 0 | 0 |
| The oracle wrong by a rule | 0 | 2 |
| Differences | **0** | **0** |

The two cases set aside are of the kinds already listed under [Testing](#testing). In one the library's result has a fin without thickness: the volumes agree, and the library's surface area and bounds are the larger. In the other the library's hull is not convex and leaves the vertices of its own operands 6.5 mm outside it, while the kernel's hull holds them. One further thing the runs showed is a property of the grid and not a fault: a Boolean operation can leave an edge shorter than a micrometre, rounding its ends to the grid turns it slightly, and a mitred offset of 2.8 mm from that edge then lay 24 nm from the library's. The check allows an offset that much (1.5 nm over the edge's length, times the distance).

    python validation/solid_backend_check.py --drawings 600 --solids 400 --seed 1

## Known limitations

- **The refusal for features too fine for the grid** described above. It is an error, not a wrong result, but the third-party library returns a solid in those cases.
- **The validity of a result rests on the code of the exact checks** listed under Rounding, and on the exact predicates under them. The tests found no invalid result, with every result re-checked by `count_self_intersections` and `verify_exact`; a test cannot prove there is none.
- **Which sub-resolution features are removed is decided with doubles** (thin cells, ribbons, merged vertices, flipped caps). Those decisions choose between valid results, so they cannot make a result invalid, and they are deterministic; but they are thresholds, and a feature a few nanometres thick is kept or dropped by them.
- **`from_mesh` checks that a mesh is closed, oriented and without zero-area triangles; it does not look for self-intersections.** They are found, and resolved, the first time the solid takes part in a Boolean operation; `count_self_intersections` reports them on request.
- **A section of a face that is not exactly flat keeps a point at each crease.** Points inside an exactly flat face are left out; a face turned by an arbitrary angle is flat only to the grid's rounding, and the kernel does not remove points by a tolerance. Studio does, within 2 nm, for the outlines it shows and cuts.
- **The sphere has about twice the triangles of the third-party library's** for the same segment count, with thin triangles at the poles. A sphere with triangles of even size would be cheaper for the same accuracy.
- **Speed against the third-party library**: slower by a factor of 2 to 6 through Studio on most parts, and 14 on a pattern of overlapping bores; see [What differs for a user](#what-differs-for-a-user) and, for what is left to gain, [Performance](#what-is-left).
- **No simplification beyond exact removal of unneeded vertices.** Triangles are not merged or re-meshed for quality; long thin triangles remain.
- **Results near the resolution are valid but may be untidy**: where faces nearly coincide, the result can carry small steps and extra vertices a few nanometres in size.
- **Convex hull** tests every point against every face: adequate for the sizes Studio allows, quadratic in the worst case.
- **Batch union time** grows with the amount of overlap; see Performance.
- **The shapes remembered as examined** (see [Performance](#what-was-changed-and-why-no-result-can-change)) are held for each thread that models, eight megabytes at most, until the thread ends.
- **Platforms.** The same results on every platform follow from the design: integers decide topology, and the doubles that choose between valid results use only correctly rounded operations (`+ − × ÷`, `sqrt`) in a fixed order, with contraction into fused multiply-adds switched off by the build (`-ffp-contract=off`, `/fp:precise`). GCC and Clang on x86-64 Linux were compared hash for hash, with the compiler's 128-bit type and with the portable one. The comparison with MSVC, and on ARM64 and macOS, is **not** among the results recorded here; `--digest` is the command to make it with. On a platform that evaluates `double` in extended precision (32-bit x87) the results could differ; that is not a supported target.
