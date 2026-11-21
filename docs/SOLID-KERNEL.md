# The solid kernel

KerfKite has its own 3D solid modelling kernel in the native core: `native/core/include/kerfkite/solid.hpp`, implemented in `native/core/src/solid/`. It depends on the C++17 standard library and nothing else. It was written to replace the third-party library behind Studio's 3D CAD (see [Third-party components](SUPPLY-CHAIN.md)) and to carry 3D engraving and, later, multi-axis machining.

**Studio does not use it yet.** Studio's Python modules still call the third-party library; switching them over is a separate step, and the section [Moving Studio over](#moving-studio-over) lists what each of their calls becomes. Until then the kernel is reached from C++ and through the `kerfkite-cam solid` command.

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

1. **Intersect.** A bounding-box tree finds the pairs of triangles that may meet. Each pair is intersected exactly: where two triangles cross, the segment they share is recorded on both, its ends being grid vertices or exact rational points. Triangles that lie in one plane and overlap are grouped, to be subdivided together.
2. **Subdivide.** Each triangle with records is triangulated again, so that every recorded point is a vertex and every recorded segment an edge (an incremental triangulation with edge recovery by flips, then Delaunay flips for shape). A group of coplanar triangles is triangulated as one, so that where they overlap they are cut into the *same* pieces. After this step, triangles meet only in whole edges and vertices.
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

`section(solid, z)` gives the material at the horizontal plane as closed rings, counter-clockwise around material and clockwise around holes. A vertex exactly in the plane counts as below it, so a face lying in the plane is included when the material is above it and excluded when it is below ("lower faces inclusive, upper faces exclusive"). Each triangle with vertices on both sides gives one directed segment, and the segments are joined through the triangles' shared edges, not by comparing coordinates, so the rings are closed for any input. Rings carry one point for each mesh edge crossed, so a straight side cut through several triangles has points along it.

`section(solid, plane)` does the same for any plane, and returns the rings both in the plane's own (u, v) coordinates and in space. This is the section needed for tilted tool axes.

`projection(solid)` is the outline seen along Z. It takes the edges where the upward-facing part of the surface ends and fills them by the positive winding rule; a through hole shows, a blind pocket does not.

Behind `extrude`, `revolve` and `projection` is a small exact 2D arrangement (`planar.cpp`, `profile.cpp`): segments on the grid are split where they cross, triangulated, and filled by the even-odd, non-zero or positive rule. `resolve_rings` exposes it. Rings may cross themselves or each other; crossings are resolved before rounding.

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

The answer always has `volume`, `area`, `bounds`, `centroid`, `vertices`, `triangles`, `bodies`, `empty` and the counts of the last operation. On request it adds: `"mesh": true` and `"grid_mesh": true` (the arrays), `"check": true` (the exact self-intersection count and the complete check), `"sections": [z, …]`, `"plane_sections": [{"origin", "normal"}, …]`, `"projection": true`, `"height_map": {"x0", "y0", "dx", "dy", "columns", "rows", "empty"}`, `"rays": [{"origin", "direction"}, …]` and `"stl": "ascii"`. `solid-batch` answers each line with `{"ok": …}` or `{"error": message}`.

JSON appears only in this command (`native/core/src/solid_command.cpp`); the kernel's own interface has none.

## Testing

Correctness is shown in two independent ways.

**Invariants that need no oracle** (`native/tests/solid_selftest.cpp`, run by CTest as `solid`):

- exact expected results on small cases: two unit cubes offset by half give volumes 1.875, 0.875 and 0.125 exactly with 20, 14 and 8 vertices; cubes sharing a face become one box of 8 vertices; cubes sharing an edge or a corner stay two bodies; a cavity is a second shell;
- on generated cases of six kinds (placed primitives; solids sharing faces and then turned together; a solid and a copy a few nanometres away; chains of operations; batches against the same operations one by one; idempotence and absorption): the volume identities to the tolerance above, commutativity, A∪A = A and A∩A = A to the last bit, A−A empty;
- every result checked by `count_self_intersections` (exactly no pair of triangles meeting wrongly) and by `verify_exact` (the arrangement of the result with itself: every cell wound exactly once or not at all);
- section areas integrating to the volume, a height map agreeing with rays at every node, an ASCII STL reading back to the identical solid.

`--stress N SEED` runs N cases of each kind with full verification; `--digest N SEED` prints one hash of every resulting mesh, for comparing compilers.

**Comparison with the third-party library as an oracle** (`validation/solid_kernel_check.py`). It builds every solid twice, in the oracle and as a request to the kernel, from the same leaves, and compares volume, bounding box, number of bodies and section areas at three heights. Tolerances follow from the grid: a layer of 4 nm for each operation on the area of the leaves. It also runs Studio's own tests for solids, scripts, assemblies and lofts on a stand-in for the library, so that those tests run on the kernel's primitives and every solid they make is compared.

The oracle is not always right, and the check does not take its word where the two differ in volume. It follows the difference down to the operation where it starts and tests that operation by a rule that needs neither side: the union or intersection of a solid with itself is that solid, and a hull contains every point of its operands. A case is set aside only when the rule shows the oracle wrong and the kernel right.

Results measured on this code (GCC 13.3 and Clang 18.1 on x86-64 Linux):

| Check | Result |
| --- | --- |
| Self-test, as CTest runs it | Passes. |
| Generated cases with every result verified exactly (`--stress 1000 77`) | 6000 cases, 0 failures. 2 refused as too fine, both among the 1000 cases of solids sharing faces and then turned together. |
| The same under AddressSanitizer and UndefinedBehaviorSanitizer (`--stress 350 23`) | 2100 cases, 0 failures, no report from either sanitizer. |
| GCC `-O3` against Clang `-O2` (`--digest 700 2026`) | 4200 cases: the hash of every vertex and triangle of every result is equal. 3 refusals in each (2 of the 700 turned-together cases, 1 of the 700 absorption cases). |
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
| Studio's tests on the kernel's solids | 30 tests in four modules; 28 pass; 2 compare a volume to seven decimal places (see [Moving Studio over](#moving-studio-over)). 368 solids made, 146 distinct ones compared, all within tolerance. |

The commands, from the repository root with the core built in `build/native`:

    build/native/kerfkite_solid_selftest --stress 1000 77
    build/native/kerfkite_solid_selftest --digest 700 2026      # the last field must be equal on every compiler and platform
    python validation/solid_kernel_check.py --tool build/native/kerfkite-cam --trees 5000 --seed 1

What these runs do not show: the hash comparison was not made with MSVC, nor on ARM64 or macOS; the differential trees are at most three operations deep, on primitives a few millimetres across; and the sanitizer run covers the generated cases, not the JSON command.

## Performance

Measured with `kerfkite_solid_selftest --bench` (GCC 13.3, `-O3`, one thread) on the two-core sandbox this was developed in. The machine was shared with other builds. The figures are from a quiet moment; under load every row took about twice as long (the batch union 7 to 8 s).

| Task | Time | Aim |
| --- | --- | --- |
| Union, difference, intersection of two spheres of 20 448 triangles each, one turned and offset | 0.20 s, 0.15 s, 0.12 s | a few seconds |
| The same with 39 600 triangles each | 0.38 s, 0.28 s, 0.21 s | |
| The same with 101 760 triangles each | 1.08 s, 0.96 s, 0.62 s | |
| Union of a sphere of 20 448 triangles with itself turned about its centre: every triangle crosses several others (78 284 crossing pairs, 197 468 triangles out) | 3.9 s | a few seconds |
| Batch union of 200 overlapping cylinders (37 600 triangles in, 1532 out) | 4.0 s | a few seconds |
| Height map of 500 × 500 nodes over a solid of 86 560 triangles | 0.008 s | about a second for 50 000 triangles |
| One horizontal section of the same solid | 0.0002 s | well under a second |
| Projection of the same solid | 0.025 s | |
| Index of the same solid and 200 000 rays | 0.19 s | |
| Exact self-intersection check of the same solid | 0.18 s | |

Time is spent where surfaces cross: an operation costs little for the triangles that stay whole and most for those that are cut. The two slow rows are the ones where nearly every triangle is cut. The batch union is slower than its size suggests because every cylinder overlaps its neighbours and the intermediate results are cut again at each level.

## Moving Studio over

What each call Studio makes on the third-party library becomes. Lengths are millimetres on both sides.

| Studio calls | Kernel | Differences to expect |
| --- | --- | --- |
| `Manifold.cube(size, center)` | `box(size, center)` | None beyond the grid. |
| `Manifold.cylinder(h, r_low, r_high, segments, center)` | `cylinder(h, r_low, r_high, segments, center)` | Same polygon; vertices rounded to 1 nm. |
| `Manifold.sphere(r, segments)` | `sphere(r, segments)` | Other faceting (bands, not a subdivided octahedron): other triangle count (9024 for 96 segments against 4608), volume a little different. |
| `Manifold(Mesh64(vertices, triangles))` | `from_mesh(vertices, triangles)` | Refused with a reason (`ValueError`) where the library returns a status. |
| `mesh64.merge()` then `Manifold(mesh64)` (STEP tessellations) | `from_mesh(vertices, triangles, true)` | Welds on the 1 nm grid; accepts solids touching along edges. Reading STEP itself stays with the optional Open Cascade add-on; the kernel takes its triangles. |
| `CrossSection(contours, fill)` then `.extrude(height, n_divisions, twist, scale)` | `extrude(rings, height, {slices = n_divisions + 1, twist_degrees = twist, scale_x, scale_y, fill})` | Same twist sense and order (twist, then scale). |
| `.revolve(segments, degrees)` | `revolve(rings, segments, degrees, fill)` | Same steps and start. |
| `.rotate([x, y, z])`, `.translate(v)`, `.scale(v)`, `.mirror(n)`, `.transform(3×4)` | `rotated(x, y, z)`, `translated(v)`, `scaled(v)`, `mirrored(n)`, `transformed(matrix)` | Results rounded to 1 nm; a 2-vector for a 3D shape must be padded by the caller as now. |
| `a + b`, `a - b`, `a ^ b` | the same operators, or `boolean(a, b, op)` | See [Rounding](#rounding-and-why-the-result-stays-valid). |
| `Manifold.batch_boolean(list, op)` | `batch_boolean(list, op)` | |
| `Manifold.batch_hull(list)` | `convex_hull(list)` | The hull of empty solids is empty. Points that all lie in one plane are refused. |
| `.status() != NoError`, `.is_empty()` | a `ValueError` at the point of failure; `.empty()` | Every `Solid` that exists is valid; there is no status to test. |
| `.volume()`, `.bounding_box()`, `.num_tri()`, `.num_vert()` | `.volume()`, `.bounds()`, `.num_triangles()`, `.num_vertices()` | Bounds are a `Box3`, not a tuple of six. |
| `.to_mesh64()`: `vert_properties`, `tri_verts` | `.vertices()`, `.triangles()` | Vertex and triangle order differ; see the order above. |
| `.slice(z)` then `.to_polygons()` | `section(solid, z)` | Same convention at faces in the plane. More points on straight sides. |
| `.project()` then `CrossSection(…, Positive).to_polygons()` | `projection(solid)` | Already resolved by the positive rule. |
| `.min_gap(other, length)` | `min_gap(a, b, length)` | |
| `kernel.triangulate(polygons)` | not offered | Used by Studio only to build polyhedron faces; `from_mesh` with any triangulation of each face will do. |
| 2D `CrossSection` operations: `square`, `circle`, `offset`, Boolean operations, `hull`, transforms, `bounds`, `num_vert`, `num_contour` | not in this kernel | These belong to the 2D polygon kernel (`polygon.hpp`). This kernel takes rings and gives rings. |

Two of Studio's tests compare a volume with its exact value to seven decimal places (`test_frustum_and_sphere`, `test_parametric_module_array_can_be_drilled_and_extruded` in `tests/test_scad_geometry.py`). On the 1 nm grid those volumes differ from the exact ones by 4·10^-6 and 1.3·10^-5 mm³, so the two tests need a tolerance when Studio moves over. The other 28 tests in the four modules pass on the kernel's solids as they are.

## Known limitations

- **The refusal for features too fine for the grid** described above. It is an error, not a wrong result, but the third-party library returns a solid in those cases.
- **The validity of a result rests on the code of the exact checks** listed under Rounding, and on the exact predicates under them. The tests found no invalid result, with every result re-checked by `count_self_intersections` and `verify_exact`; a test cannot prove there is none.
- **Which sub-resolution features are removed is decided with doubles** (thin cells, ribbons, merged vertices, flipped caps). Those decisions choose between valid results, so they cannot make a result invalid, and they are deterministic; but they are thresholds, and a feature a few nanometres thick is kept or dropped by them.
- **`from_mesh` checks that a mesh is closed, oriented and without zero-area triangles; it does not look for self-intersections.** They are found, and resolved, the first time the solid takes part in a Boolean operation; `count_self_intersections` reports them on request.
- **Sections keep one point for each mesh edge crossed**, so straight sides carry collinear points. They are not removed.
- **No simplification beyond exact removal of unneeded vertices.** Triangles are not merged or re-meshed for quality; long thin triangles remain.
- **Results near the resolution are valid but may be untidy**: where faces nearly coincide, the result can carry small steps and extra vertices a few nanometres in size.
- **Convex hull** tests every point against every face: adequate for the sizes Studio allows, quadratic in the worst case.
- **Batch union time** grows with the amount of overlap; see Performance.
- **Platforms.** The same results on every platform follow from the design: integers decide topology, and the doubles that choose between valid results use only correctly rounded operations (`+ − × ÷`, `sqrt`) in a fixed order, with contraction into fused multiply-adds switched off by the build (`-ffp-contract=off`, `/fp:precise`). GCC and Clang on x86-64 Linux were compared hash for hash, with the compiler's 128-bit type and with the portable one. The comparison with MSVC, and on ARM64 and macOS, is **not** among the results recorded here; `--digest` is the command to make it with. On a platform that evaluates `double` in extended precision (32-bit x87) the results could differ; that is not a supported target.
