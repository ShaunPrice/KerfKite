# The polygon kernel

KerfKite's own two-dimensional polygon library: regions with holes, Boolean operations, round offsets, line
clipping, the union of very many shapes, and the Delaunay triangulation and Voronoi diagram of points. It is
part of the native core (`native/core/src/polygon/`,
interface in `native/core/include/kerfkite/polygon.hpp`) and depends on nothing but the C++17 standard
library. It was written for KerfKite from the published methods named under [Sources](#sources), not taken
or translated from another library.

**It is what the application uses** for inside and outside offsets, pockets, hatch fills and V-carving.
`native/core/src/operations.cpp` and `native/core/src/vcarve.cpp` build those operations on the kernel;
`kerfkite-cam` runs that code, and Studio's Python calls the same code through its compiled extension
(`studio/operations.py`), so the two cut the same paths. GEOS is no longer part of the native build, which
now contains no third-party code and downloads nothing. Shapely, which carries GEOS, remains in Studio for
three things: as the fallback for these operations where the extension is not installed, in the modules
that have not been moved yet, and as the oracle the kernel is tested against.
[The operations](#the-operations) describes how they are built and how the paths differ from the ones GEOS
gave, and [V-carving](#v-carving) does the same for the carve.

## What it does

| Need | Function |
| --- | --- |
| The region of closed contours under even-odd fill, as `studio.operations.region` builds it | `compose` |
| The region of any rings (crossing, overlapping, repeated) under even-odd, non-zero or positive fill | `normalise` |
| Union, intersection, difference, symmetric difference of regions with holes | `combine`, `unite`, `intersect`, `subtract`, `exclusive_or` |
| The union of many regions at once | `unite_all` |
| A region grown or shrunk by a distance, with round joins | `offset` |
| A disc, a rectangle, an obround, a polyline drawn with a round pen | `disc`, `rectangle`, `obround`, `stroke` |
| Dark and clear shapes in drawing order, as a photoplotter file draws them | `Artwork` |
| Straight segments cut to a region (hatch rows) | `LineClipper` |
| Area, bounds, orientation, point location, whether a ring is simple, whether a region is valid | `area`, `signed_area`, `bounds`, `orientation`, `locate`, `is_simple`, `is_valid` |
| The rings of a region in the order and directions the application cuts them | `outlines` |
| The Delaunay triangulation of points | `delaunay` |
| The edges of the Voronoi diagram of points, cut to a box | `voronoi_edges` |
| The distance from a point to the boundary of a region | `BoundaryDistance` |

Not provided: the Voronoi diagram of segments (the exact medial axis of a polygon), triangulation of a
polygon with holes, curves other than circular arcs drawn as straight edges, and anything in three
dimensions.

## The grid

Every vertex is a pair of 64-bit integers counting nanometres (1e-6 mm). A millimetre value is multiplied by
1e6 and rounded to the nearest whole number on the way in, halves upwards, and a result is divided by 1e6
on the way out, so every coordinate of a result is a whole number of nanometres.

Whether a point is left or right of a line, whether two edges cross, which ring contains which: every such
decision is made with exact integer arithmetic in 128 bits. No tolerance is compared anywhere to decide the
shape of a result. The compiler's 128-bit integer is used where there is one; where there is not (MSVC) the
same arithmetic is done from 64-bit pieces (`wide.hpp`). The two give the same results, and the self-test is
built both ways.

### Rounding

A point that has to be constructed, where two edges cross, is not on the grid in general. It is rounded to
the nearest grid point, and both edges are bent through that point; so is every other edge that passes
within half a unit of it. A vertex that lies exactly on an edge, and the ends of a length two edges share,
split the edge where they are, and nothing moves. Bending an edge can make it meet another that it missed
before, so the pieces are looked at again, round after round, until a round finds nothing: the edges then
meet only at shared end points, as exact arithmetic confirms and not as a rule of thumb assumes. This is
snap rounding, repeated until it is stable.

An edge is never bent towards a vertex that was already there. An edge and a vertex may therefore end
closer than a unit and still exactly apart, each bend moves an edge by at most 0.71 nm, and a result put
through the kernel again comes back vertex for vertex.

What this means for a result:

* It is always a valid region. Shared edges, collinear overlaps, touching vertices, slivers, rings that
  touch themselves, repeated points and features of a few nanometres are ordinary input.
* An edge of a result lies within about a nanometre of where exact arithmetic would put it. In the
  comparison with GEOS the limit used is 3 nm; in a sample of 9,830 Boolean results the kernel's boundary
  was within 0.71 nm of GEOS's in 99% and within 1 nm in 99.8%, the rest being slivers, as follows.
* A sliver thinner than a nanometre may vanish, and where two edges run nearly along each other the sliver
  between them may be kept or lost for as far as they run.
* A simple case takes one round or two, the second finding nothing. The most seen in any test is
  4. The limit is 200 rounds, after which the kernel refuses the input rather than run on; no
  input has come near it, but that none can is not proved.

## Regions and Boolean operations

All of them are one procedure (`arrangement.cpp`):

1. Every ring gives edges, each recording by how much the winding number of its operand rises across it.
2. The edges are resolved as described under [Rounding](#rounding). Pairs of edges near each other are
   found with a grid of buckets, with a finer grid inside any crowded bucket.
3. A sweep from left to right gives the winding number of each operand on both sides of every edge.
4. An edge with the inside on one side only is boundary. What "inside" means is the only difference
   between the operations and the fill rules.
5. Boundary edges are followed into closed walks, always turning so as to stay on one face of the inside. A
   walk that passes a vertex twice is cut there into simple rings. Counter-clockwise rings are outer rings
   and clockwise rings are holes; a second sweep finds the outer ring of each hole.

## Offsets

Growing a region by `d` is the union of the region with the strip swept by each edge and the sector round
each corner that turns away from the outside. The kernel builds those pieces on the grid as fans of
triangles round the vertices of the region, and checks each triangle, exactly, to be counter-clockwise or
flat. The boundaries of the region and of all the pieces add up to one closed curve for each ring, and
because no piece winds negatively the winding number of that curve at a point is the number of pieces over
the point, plus one inside the region. The grown region is therefore exactly where the winding number is
positive. No loop of the curve has to be recognised and cut out, and a feature smaller than the tool cannot
turn inside out. Shrinking is growing the outside.

An offset is therefore the exact union (or difference) of polygons that are themselves on the grid, and is
always a valid region. Its accuracy is that of those polygons:

* It is never short of the distance by more than the rounding (a few nanometres).
* Round joins are drawn **outside** the true arc: a run of tangents to the circle, each turning from the
  last by at most 1.0625 steps, a step being a quarter turn divided by `segments_per_quarter`. The corners
  of the run lie outside the true arc by at most `d * (1 / cos(0.53125 * step) - 1)`, which at the default
  of 32 segments is 0.00034 `d`: half a micrometre for a 3.175 mm cutter. GEOS draws chords inside the arc,
  short by up to 0.00030 `d`. For tool paths the kernel's choice is the safe one: the tool is never nearer
  the work than its radius.
* `ArcOptions::tolerance` sets the allowance in millimetres instead, and the number of segments then
  follows from each radius.
* A corner that turns by less than a step is a single mitred point. Offsetting a result again therefore adds
  no vertices to its arcs, and the passes of a pocket do not drift: pass 40 is the inset of the contour by
  the whole depth, to the same allowance as pass 1.
* An offset smaller than one grid unit returns the region unchanged.

Directions for arcs come from rotating by a fixed step, from halving an angle with square roots, and from
the series for one sine and cosine. No trigonometric function of the platform's library is called.

Discs, obrounds and the ends and joins of strokes are drawn the same way, so a disc is never smaller than
its diameter.

### The corners of a drawing

A drawing asks for other corners than a tool path does. `offset(region, distance, JoinOptions)` gives
them, for the `offset()` of Studio's CAD scripts:

* **Round**: an arc whose vertices lie **on** the true arc, as a drawing program draws it. The arc is
  divided into equal parts of at most 1.0625 steps, a step being a whole turn divided by `segments`, so it
  is short of the distance between its vertices by at most `d * (1 - cos(0.53125 * step))`.
* **Miter**: the two offset edges carried on until they meet. A corner that would reach further from the
  vertex than `miter_limit` times the distance is cut square instead, by the line across the corner at the
  distance from the vertex.
* **Bevel**: the ends of the two offset edges joined by one straight edge.

Only the points of the corner differ from the round offset above. They are joined to the vertex in the same
fan of exactly checked triangles, so the result is the same kind of exact union or difference and is
always a valid region, and the directions of an arc come from the same series and square roots. The
self-test checks each join on small cases with known answers and, on generated shapes grown and shrunk,
that every result is a valid region, that a bevel adds least, a miter most and an arc lies between, and that
growing contains the region and shrinking is contained in it.

`convex_hull(points)` is the hull as one counter-clockwise ring, exact on the grid, and empty when the
points lie on one line.

Studio's script engine keeps a 2D drawing as the rings of a region and asks for each step through the
kernel's `polygon` command (`kerfkite/polygon_command.hpp`): `normalise`, `union`, `intersection`,
`difference`, `offset` with a `join`, `hull`, `circle` (the cross-section of the solid kernel's cylinder,
vertex for vertex), `transform` and `rotate`. [The solid kernel](SOLID-KERNEL.md#how-studio-uses-the-kernel)
describes that use and how it is compared with the third-party library it replaced.

## Line clipping

`LineClipper::clip(a, b)` gives the parts of the segment from `a` to `b` that lie in the region or along its
boundary, in order from `a`. The boundary edges that cross the line are found exactly and ordered along it by
comparing their positions as exact fractions (256-bit products). A vertex lying exactly on the line has no
side, so crossings are counted twice, with such vertices taken as left of the line and as right of it, and
the parts found either way are joined: a line along an edge of the region is a part, and a line that only
touches a corner gives nothing. Each end of a part is then rounded to the grid.

The ends of the segment itself are grid points, so a hatch row asked for at an arbitrary height or angle is
moved by up to half a nanometre. Where such a row meets an edge at a shallow angle its end slides along the
edge by correspondingly more; it stays on the row and at the boundary.

## Voronoi diagrams of points

`delaunay(points)` gives the Delaunay triangulation of grid points and `voronoi_edges(triangulation, low,
high)` the edges of the Voronoi diagram that is its dual, cut to a box (`voronoi.cpp`). V-carving follows
this diagram.

### What comes back

A `Triangulation` holds the distinct points in order of x and then y; every triangle counter-clockwise from
its smallest point, the triangles in order; and every edge once, from its smaller point, with the triangle
on its left and the triangle on its right, or none where the edge is on the convex hull. A `VoronoiEdge` is
a straight segment between two grid points, the smaller first, with the two points (sites) it lies between;
the edges come in order of their ends. Nothing else is stored: there are no faces or cells, because the
application needs only the edges.

### The method

The triangulation is built by divide and conquer on the quad-edge structure, as Guibas and Stolfi describe
it: the points are sorted, halved until two or three remain, and the halves are merged from their lower
common tangent upwards, each new cross edge being the one whose circle through the ends of the last is
empty. Sorting and merging take **O(n log n)** time whatever the arrangement of the points; the structure
holds eight 32-bit numbers for each edge, about 100 bytes a point in all.

### The two tests

* **Which side of a line a point is on** is the sign of a product of coordinate differences, exact in 128
  bits, as everywhere in the kernel.
* **Whether a point is inside the circle through three others** is the sign of a determinant of the fourth
  degree. With differences below 2^38 its terms reach 2^154, so it is taken as three products of 128-bit
  values added in 256 bits (`Int256` in `wide.hpp`: four 64-bit pieces, from the same 64-bit multiplication
  the portable 128-bit integer uses, so it is one code for every compiler).

Neither test has a tolerance or a floating-point short cut, so there is nothing for a platform to round
differently, and no perturbation.

### Points on one line and on one circle

Both are ordinary here: the samples along a straight wall lie on one line, and the samples of a rectangle or
of anything symmetric lie four or more to a circle.

* **Repeated points** count once.
* **Points all on one line** are joined in a chain. There are no triangles; each edge's bisector is a
  Voronoi edge without ends, cut to the box.
* **Points on one line among others** make no flat triangle: the in-circle test is strict, and three points
  on a line have no circle.
* **Four or more points on one circle** can be triangulated in more than one way, and which diagonals are
  chosen follows from the order of the points alone, so it is the same everywhere. It does not matter to
  the diagram: the triangles either side of such a diagonal have the same centre, the Voronoi edge between
  them has no length, and edges of no length are left out. The Voronoi edges that remain are the same
  whichever diagonals were chosen.

### Voronoi vertices and the rounding rule

A Voronoi vertex is the centre of the circle through the corners of a triangle, and is not a grid point in
general. An edge lies on the perpendicular bisector of its two sites `a` and `b`; with `m` the middle of
`ab` and `d` the direction of `ab` turned a quarter turn to the left, the centre for a third point `c` is
`m + t d` with

    t = (c - a) . (c - b) / (2 (b - a) x (c - a))

an exact fraction of two 128-bit whole numbers. The edge runs from the `t` of the triangle on the right of
`ab` to the `t` of the triangle on its left, and has no end on a side where there is no triangle. That range
is cut to the box by comparing fractions exactly (256-bit products), **and only then** are the two ends
worked out, each coordinate as one exact quotient **rounded to the nearest grid point, halves upwards**.
So:

* an end is within 0.5 nm of the exact vertex in each coordinate (0.71 nm in all);
* the three edges that meet at a vertex round the same exact point, so they end at the same grid point;
* the centre of a thin triangle, which may be kilometres away, is never represented: only where its edges
  cross the box;
* an edge whose two ends round to one grid point is left out.

### Limits

Coordinates up to 2^37 units (137 m) either side of the origin, for the points and for the box; up to
2,000,000 points. Beyond either, `ValueError`. Nothing recurses in proportion to the number of points: the
depth of the halving is its logarithm.

### The distance to a boundary

`BoundaryDistance(region).to(point)` is the distance in grid units from a grid point to the nearest edge of
the region. For each edge the foot of the perpendicular is placed exactly (before the edge, on it, past
it); the distance is then the square root of a whole number, or a whole number divided by such a square
root, each whole number converted to the nearest double first. Square root and division are rounded
correctly by IEEE 754, so the distance to an edge is the same double everywhere, within 4 parts in 10^16 of
the true one, and the answer is the least over all edges. The edges are kept in buckets, which only limit
how many are measured.

## The order of a result

* Outer rings run counter-clockwise, holes clockwise.
* A ring does not repeat its first vertex and starts at its smallest vertex, comparing x and then y.
* A vertex on the straight line between its neighbours is dropped, unless another ring touches there, in
  which case both rings keep it.
* The holes of a polygon are in the order of their first vertices, and the polygons of a region in the order
  of the first vertices of their outer rings; rings with the same first vertex are ordered by their following
  vertices.
* Two rings may touch at single points. They never cross or share a length of edge, no ring touches itself,
  and the interior of each polygon is connected.

`outlines` re-orders a region for the application, as `studio.operations.outlines` does: polygons by area,
smallest first, and for each its holes and then its outer ring, every ring closed by repeating its first
vertex. It gives outer rings clockwise and holes counter-clockwise, the directions the Shapely code has
always given, so the direction of cut does not change.

## Limits

| Limit | Value |
| --- | --- |
| Coordinates | 2^37 units either side of the origin, about 137 m. The application's work area is within 10 m. |
| Offset distances and shape sizes | up to 2^35 units, about 34 m; shapes at least 2 units across |
| Edges in one operation, counting those made where edges cross | 8,000,000 |
| Shapes in an `Artwork` | 4,000,000; they are united as they arrive in steps of at most 2,000,000 edges |
| Arc segments | 1 to 1,024 a quarter turn |
| Rounds of resolving | 200 |
| Points in a triangulation | 2,000,000, each within 2^37 units of the origin |

Input beyond a limit, or not finite, throws `kerfkite::ValueError`. Nothing recurses in proportion to the
size of the input.

## Results are the same everywhere

Integer arithmetic is exact. The floating-point arithmetic that places offset and arc vertices uses only
addition, subtraction, multiplication, division and square root, which IEEE 754 rounds correctly, and its
results are rounded to the grid. The bucket grid uses floating point too, but only to decide which pairs of
edges are examined, which cannot change a result.

A triangulation and its Voronoi edges are whole-number arithmetic throughout, and the distance to a boundary
adds only a square root and a division.

Two things could still make a platform differ, and the self-test's recorded digests exist to catch them
(one for regions, offsets and hatch rows, and one for triangulations, Voronoi edges and distances):

* **Fused multiply-add.** A compiler allowed to contract `a * b + c` rounds once where the source rounds
  twice. The build passes `-ffp-contract=off` or `/fp:precise`, as the rest of the core requires, and the
  kernel repeats that with a pragma where a compiler honours one.
* **Extended precision.** A compiler that evaluates `double` in 80-bit registers (32-bit x86 without SSE2)
  is not supported.

Measured: GCC 13 and Clang 18 on x86-64 give byte-identical answers to the 133,602 requests of the full
comparison with GEOS, and the same digest with either 128-bit integer, at `-O2` and at `-O3 -march=native`.
The digest of triangulations, Voronoi edges and distances is likewise the same with GCC 13 and Clang 18 and
with either integer, and 1,114 generated jobs, 810 of them V-carves, gave the same bytes from
`kerfkite-cam` built with GCC, with Clang, with the kernel's own 128-bit integer throughout, and under the
address and undefined-behaviour sanitizers.
A build with fused multiply-add deliberately allowed (`-march=native -ffp-contract=fast`) also gave the
same digest, which shows that the test's cases do not turn on it, not that none can. ARM64 and MSVC have not
been compiled or run: the kernel is written to the standard and the portable integer is exercised here, but
the first build on each is the test. The continuous build (`.github/workflows/native-core.yml`) runs CTest
on them, and a different result would fail the digest there.

The operations are pinned in the same way. `validation/check_installed_cam.py`, which every wheel build
runs on the installed wheel, generates an inside offset, an outside offset, a pocket and a cross-hatch fill
at 33.3 degrees through the extension and compares the four programs with a recorded digest. `kerfkite-cam`
built with GCC 13 and with Clang 18 writes the same four programs as the extension does.

## How it is tested

`native/tests/polygon_selftest.cpp`, run by CTest twice (`polygon`, and `polygon_portable_int128` with the
kernel's own 128-bit integer), needs no Python and no other library:

* exact answers for small cases of every operation, for the arithmetic, and at the limits of the range;
* for 400 generated pairs of regions, from a few nanometres to tens of millimetres across: every result is
  valid; `|A or B| + |A and B| = |A| + |B|`, `|A xor B| = |A or B| - |A and B|` and
  `|A - B| = |A| - |A and B|`; a region united or intersected with itself is itself; the operations give the
  same answer with their operands exchanged; every vertex of an offset is at the offset distance from the
  source boundary, never nearer by more than 3 units and further only by the arc allowance; an offset and
  its source are nested; hatch rows lie in the region, end at its boundary, and their lengths times the
  spacing give its area;
* a knot of slivers through one point, artwork of 3,000 dark and clear shapes with three contours round
  it, and a pocket followed until nothing is left;
* a digest of every generated result, compared with the value recorded in the test;
* for triangulations: the in-circle test against the same determinant taken another way in 128 bits on
  20,000 sets of four points, and again with the points moved to the edge of the range and made 65,536 times
  larger, where the sums pass 2^128; small cases with known answers (a square, points on a line, a centre
  on a half grid step, a centre 137 m outside the box); and 260 generated sets of up to 900 points
  (scattered, lattices with repeats, on one line, on one circle with and without its centre, the samples of
  rectangles and of walls, points a few units apart, clusters, points at the edge of the range), for each of
  which, exactly: the points are the distinct points given; every triangle turns left; no point is inside
  any triangle's circle; every edge has its triangles on the proper sides; every hull edge has all points on
  one side; `points - edges + triangles + 1 = 2` (Euler's formula); with `h` points on the hull there are
  `2n - 2 - h` triangles and `3n - 3 - h` edges, so the hull is complete; every corner of `convex_hull` is
  on a hull edge; points on one line form a chain; and every Voronoi edge lies in the box with each end
  equally far from its two sites and no third site nearer, to the rounding of the end. The triangulation
  must also be the same for the points given in reverse and for the points moved 68 m. A second digest
  covers these results and the distances.

`tests/test_polygon_kernel.py` checks the same properties through `kerfkite-cam polygon` when the tool has
been built (`KERFKITE_CAM`, or `build/native`). For triangulations it repeats the exact checks in Python's
whole numbers, compares the Voronoi edges of scattered points with Shapely's `voronoi_diagram` (the same
total length to 0.0001 mm, and every end of either diagram within 0.00001 mm of an edge of the other), and
the distance to a boundary with Shapely's.

`validation/polygon_kernel_check.py` compares the kernel with Shapely 2.2.0 (GEOS 3.14.1):

    python validation/polygon_kernel_check.py --cli build/native/kerfkite-cam --cases 12000

It generates cases on the grid and gives GEOS the grid coordinates themselves, so both see the same input:
random polygons with holes, nested and overlapping sets, rectangles on a lattice with shared edges and
exact fits, polygons a few units across, rings that cross themselves, and constructions made to be awkward
(slivers, bars through one point, combs, repeated rings, rings that touch themselves, coordinates at the
edge of the work area), from tens of nanometres to metres. Results are compared by the area of their
symmetric difference and by the Hausdorff distance between their boundaries, against tolerances that follow
from the grid and the arc allowance. A difference that is nowhere wider than the tolerance is accepted and
counted separately as `thin`. For offsets the deciding test is containment between two offsets made the
slow, sure way (the region with a disc swept along every edge added or taken away), because GEOS's own
polygon buffer is not exact: its distance from the true offset is measured in the same run, and was up to
681 times what the chords of its arcs explain (about a fifth of the offset distance; the case examined was
a straight edge drawn across a notch where two arcs meet). GEOS's overlay is not exact either: one case in
a further 2,000 was a sliver two units wide that GEOS's own symmetric difference misreported, which the
script settles with exact rational arithmetic on the input.

The run of 21 November 2026 (GCC 13, seed 20261121; 133,602 requests to the kernel) had no failures:

| Check | Cases | Thin | Refused alike |
| --- | --- | --- | --- |
| Union, intersection, difference, symmetric difference against GEOS | 12,000 each | 945, 638, 2,391, 2,660 | |
| The area identities of those four | 12,000 | | |
| The region of arbitrary rings under each fill rule | 12,000 | 434 | |
| A result united with itself is itself, vertex for vertex | 12,000 | | |
| `compose` against `studio.operations.region` | 6,000 | 45 | 2,283 |
| Lines cut to a region | 37,817 lines, 6,000 regions | 5,493 | |
| Offsets outwards and inwards, between swept offsets | 3,033 and 2,924 | 221 and 241 | |
| The offset distance measured from the result, with no oracle | 4,386 | | |
| Offset and back brackets the source | 4,386 | | |
| Pocket passes against the inset of the source by the whole depth | 6,940 passes, 800 pockets | 2,319 | |
| Artwork of 1 to 25 dark and clear shapes; two contours round each | 300; 596 | 49; 150 | |

`Thin` cases are accepted and counted, not hidden. For Boolean results they are slivers no wider than the
tolerance that one library keeps and the other loses (30% of the generated pairs are a set of rings and the
same set moved by a few nanometres, which is nothing but slivers). For offsets and pocket passes the kernel
lay between the two swept offsets in every case; `thin` there means it was further from GEOS's polygon
buffer than the tolerance, at a sharp corner or where that buffer is itself off. For hatch fills it means
that some row ran along an edge or grazed a corner, so that the two did not cut the same parts, while the
kernel's rows lay in the region and covered what its width allows. (That run also made the four
operations from kernel calls and compared them with `studio.operations`, which was then Shapely alone; the
comparison of the operations as they now are is under [The operations](#the-operations).)

Measured without any oracle in that run: no point of an offset boundary was nearer the source than the
distance by more than 1.3 nm, and none further than the arc allowance and 4 nm. Resolving took at most 4
rounds.

The script also replays a recorded run through another build and compares the answers byte for byte
(`--replay`). The Clang 18 build gave the same bytes for all 133,602 requests. A build with
AddressSanitizer and UndefinedBehaviorSanitizer (GCC 13) ran the self-test and 52,100 of the requests, from
every group but pockets, with no report and the same bytes.

## Measured speed

On the development sandbox (2 shared cores, x86-64, GCC 13 `-O2`), `kerfkite_polygon_selftest --bench`:

| Case | Time |
| --- | --- |
| A pocket: a contour of 2,000 vertices shrunk by 1.5 mm and then 50 times by 0.8 mm (27,482 vertices of tool path) | 0.07 s |
| The union of 50,000 discs, rectangles, obrounds and strokes that overlap their neighbours (1.2 million edges in, 10 polygons of 220,956 vertices out) | 3.2 s |
| That image grown by 0.1 mm for an isolation contour | 0.7 s |
| A hatch of the contour: 2,001 rows, 117,679 parts | 0.15 s |
| The triangulation and Voronoi edges of 1,000 scattered points (1,984 triangles, 2,893 edges) | 0.002 s |
| The same for 12,000 points, the most a V-carve samples (23,974 triangles, 35,605 edges) | 0.03 s |
| The same for 100,000 points | 0.30 s |
| The same for 11,960 samples of the walls of a comb, every half millimetre: points on lines and on circles throughout | 0.02 s |

These are the best of three runs. The machine was shared while they were taken and the other runs were up
to 45% slower (the union took 3.2 s, 3.6 s and 4.6 s).

## Known weaknesses

* Termination of the resolving rounds is observed, not proved; the limit of 200 rounds turns a failure to
  settle into a refusal.
* Time grows with the number of edges and crossings. The worst case met is one where every edge crosses
  every other: a polygon of 343 sides shrunk by exactly its radius, so that all its offset edges pass
  through the centre (59,000 crossings), took 0.25 s.
* A shape with a neck or sliver thinner than a nanometre can come apart or lose the sliver, and an offset of
  it then lacks what the sliver would have added.
* The limit of 100,000 vertices on a pocket was set for GEOS, which draws every arc again at every pass.
  The limit and its message are kept, but the kernel's passes have far fewer vertices (4,057 against
  156,384 for one generated pocket of 29 passes), so it now takes a very large pocket to reach it, and
  some pockets the Shapely code refuses are cut.
* Only x86-64 with GCC and Clang has been run. See [above](#results-are-the-same-everywhere).
* The Voronoi diagram is of points, not of the region's edges, so a V-carve is the reference's sampled
  approximation of the medial axis and no more: it is safe by its allowance of one sampling interval, which
  also makes it shallower than the bit could go, and round a corner that turns inwards its paths stand
  further apart than the interval. An exact medial axis needs the Voronoi diagram of segments, which the
  kernel does not have.
* A V-carve still lifts the tool once for each spoke, about once for every sample of the outline. Joining
  halves the number of paths the reference wrote; cutting each spoke out and back would remove most of the
  rest at the cost of cutting it twice, and was not done.
* `is_valid` does not check that the interior of a polygon is connected. (Results of the kernel are; the
  comparison with GEOS, whose validity test does check it, found none that was not.)

## The operations

`native/core/src/operations.cpp` is the one implementation of inside and outside offsets, pockets and hatch
fills. `kerfkite-cam generate` reaches it directly. Studio's `studio/operations.py` checks the settings of
a job in Python, with the messages and in the order they always had, and then asks the compiled extension
for the geometry (the core's `operation` command, `kerfkite/operation_command.hpp`); the extension is built
from the same source files. What each step of the Shapely code became:

| Shapely code (now the fallback) | Kernel (`polygon::`) |
| --- | --- |
| `Polygon(path)` | `ring_from_mm(path)` without the path's last point |
| `p.is_valid` | `is_simple(ring)` |
| `p.area < 1e-9` | `std::fabs(signed_area(ring)) < 1e-9` |
| `GeometryCollection()`, then `result.symmetric_difference(p)` for each contour | `compose(rings)` once, for all contours |
| `result.is_empty` | `region.empty()` |
| `g.buffer(d, quad_segs=32)` | `offset(region, d)` |
| `inset.buffer(-step, quad_segs=32)` until empty | `offset(inset, -step)` until `empty()` |
| `outlines(result)` | `outlines(region)` |
| `affinity.rotate(g, -angle).bounds` | the extremes of the region's vertices turned by `-angle`; the region itself is not turned |
| `rotated.intersection(LineString(row))`, `lines(...)`, `affinity.rotate(line, angle)` | the two ends of the row turned by `+angle` and put on the grid, then `LineClipper(region).clip(a, b)`: the parts are already in the region's frame, in order along the row |
| `line.length > 1e-6` | the part is at least 0.001 mm long |
| `shapely.errors.GEOSException` | does not arise; `ValueError` only for input beyond the kernel's limits |

Four things are fixed in that file and are worth knowing:

* **From the grid to doubles.** A grid coordinate `n` leaves the kernel as the double `n / 1e6`. That is one
  division, which IEEE 754 rounds correctly, so every platform gets the same double; the core's JSON writes
  it as the shortest text that reads back as the same double, and Python reads that text back exactly, so
  Studio's generator and `kerfkite-cam`'s format the same numbers. The program is written to three decimals,
  a thousand grid steps, so the conversion never decides a digit.
* **Turning hatch rows.** The sine and cosine of the hatch angle come from a fixed series of additions and
  multiplications after the angle has been folded exactly into the first eighth of a turn, not from the C
  library, whose last bit differs between platforms. A whole number of quarter turns gives exactly 0 and 1.
* **Short pieces of a hatch row.** A piece shorter than 0.001 mm is dropped. The program's coordinates have
  three decimals, so a shorter stroke cannot be cut. The Shapely code kept pieces longer than 0.000001 mm,
  which a row grazing a corner can produce.
* **The closing point.** A contour counts as closed when its last point is within 0.000001 mm of its first.
  That last point is then taken to be the first, not a further vertex. The Shapely code appends the first
  point again after it, and then usually refuses the contour as self-crossing because of the spike that
  makes.

The checks of a job's settings, their messages and their order are unchanged, as are the warnings, the
check of the compensated path against the work area, and the limit of 100,000 vertices on a pocket.
V-carving is built on the kernel as well, in its own file: see [V-carving](#v-carving).

### Which code runs

`studio.operations.polygon_backend()` answers `kernel` when the compiled extension is installed and
`shapely` when it is not (the portable wheel, or a source checkout that was never built). Setting
`KERFKITE_POLYGON_BACKEND=shapely` selects the Shapely code where the extension is installed; `=kernel`
refuses to run without it. The two cut the same regions, but **the fallback's vertices are not the
kernel's**, so a program written by an installation without the extension differs from one written with it
in the ways listed next. `kerfkite-cam` always uses the kernel.

### How the tool paths differ from the ones GEOS gave

* **Coordinates** are whole nanometres. The G-code is written to three decimals.
* **Where a ring starts**: at its left-most vertex, where GEOS started wherever its algorithm happened to.
  Leads, ramp entries and tabs on an offset contour are placed from that start, so they move with it.
  Direction is unchanged: outer rings clockwise, holes counter-clockwise.
* **Arc vertices**: the same number for a square corner (32 a quarter turn), in different places, just
  outside the true arc where GEOS's were just inside. At round corners the tool path is up to 0.034% of the
  offset distance further from the work than the exact offset, and never nearer; GEOS's was up to 0.03%
  nearer. For a 3.175 mm cutter that is half a micrometre either way.
* **The order of the holes of one polygon**, and of polygons of exactly equal area: the kernel's defined
  order, where GEOS's was an accident of its algorithm. Polygons are still cut smallest first, each one's
  holes before its outline.
* **Vertices on straight runs** are dropped.
* **Pockets**: the same passes, with far fewer vertices, since an arc is not drawn again at every pass. A
  last pass thinner than the tolerance can exist in one and not in the other: where the cutter's centre
  could reach exactly one point, GEOS's arcs, drawn from inside, leave a speck that becomes a pass, and the
  kernel's leave nothing.
* **Hatch rows**: the same rows. A row that runs along an edge of the region now runs in the row's direction
  like its neighbours; GEOS gave it the direction of the edge. Pieces under 0.001 mm are gone.
* **Contours accepted**: a contour that closes to within the tolerance but not exactly is now cut; a contour
  that touches itself or has no area is refused as before. The two can otherwise disagree only on a contour
  whose self-contact or emptiness is a matter of less than the grid.
* **Which message, for a path that leaves the work area**: the first offending point of a ring is reported,
  X before Y, and the ring starts elsewhere, so a job refused for `Compensated Y` can now be refused for
  `Compensated X`, or the other way round.

### How the operations are checked

`tests/test_polygon_operations.py` runs `prepare()` on the kernel and on the Shapely fallback against
expectations worked out from the geometry: areas of offsets of squares, the distance of every vertex from
the source contour, the number and size of pocket passes, the rows of a fill and their total length, the
order and direction of rings, and every rejection in its order.

The `cam` group of `validation/polygon_kernel_check.py` generates jobs (stars with holes, overlapping
contours, rectangles on a lattice with shared edges; tools from a fifth of a millimetre up, hatch angles
that are round and that are not) and runs `prepare()` both ways for each:

    python validation/polygon_kernel_check.py --cli build/native/kerfkite-cam --only cam --cases 5000

| Check (run of 21 November 2026, GCC 13, seed 20261121; no failures) | Jobs | Refused by both | Thin |
| --- | --- | --- | --- |
| The extension's tool paths and `kerfkite-cam`'s for the same request are the same doubles | 3,000 | | |
| Outside offsets: the region between swept offsets of the source, then rings in number, order and direction | 740 | 69 | 128 |
| Inside offsets, likewise | 755 | 122 | 162 regions, 1 job by its rings |
| Pockets, pass by pass against the inset of the source by the whole depth | 746 pockets, 3,672 passes | 156 | 446 passes, 7 pockets |
| Hatch and cross-hatch fills, row by row | 759 | 79 | 89 |

`Refused by both` are jobs the Shapely code refuses (a tool that does not fit, contours that cancel or
cross); the kernel must refuse them with the same message, unless the matter turns on less than the grid.
`Thin` has the meaning it has above: the kernel lay between the two swept offsets, and was further from
GEOS's buffer than the tolerance at a sharp corner or where that buffer is itself off (GEOS's own distance
from the true offset was up to 20 times what its chords explain in this run); for pockets, a last pass
that only one of the two finds, thinner than the tolerance, or a pocket the Shapely code refuses for its
100,000 vertices; for fills, a row along an edge or grazing a corner. Of the 591 fills that were neither,
every row matched the reference's row: both ends within 3 nm (worst 2.97 nm), or, where the row meets an
edge at a shallow angle, on the reference's line to within 3 nm with its ends on the region's boundary
(the furthest such end was 104 nm along the row from the reference's).

A separate count of outcomes, over the 3,949 generated jobs of `validation/native_core_parity.py` that ask
for one of these operations: 3,905 end the same way on the kernel and on the Shapely fallback (a program,
or the same rejection). The other 44 are 36 contours the Shapely code refuses as self-crossing and the
kernel accepts (35 close only to within the tolerance; one has two vertices two millionths of a micrometre
apart); 5 jobs refused for leaving the work area on the other axis first; 2 fills at an angle of a
billionth of a degree whose only rows run along an edge; and 1 outside offset whose ramp entry, placed from
the new start of the ring, no longer fits the work area.

`validation/native_core_parity.py` then compares whole jobs between `kerfkite-cam` and the Python generator
with the extension. The geometry of an offset, pocket or fill is shared there by construction, so what that
comparison shows for these jobs is everything round the operation: the settings and their rejections, the
order of cutting, leads, tabs, ramp entries on the offset contours, laser power, post-processing and the
totals. It names the reference it is using, and without the extension it stops instead of comparing
Shapely's vertices with the kernel's (`--without-polygon-operations` leaves those jobs out and says so).

Measured on Linux x86-64 (GCC 13, Python 3.13) with the default sizes, before and after the change. No
case differed in either run.

| | Suite calls | Generated cases | Ramp, helix and tab cases |
| --- | --- | --- | --- |
| Before: core on GEOS, reference on Shapely | 210: 116 programs, 90 rejections, 4 declined | 20,000: 9,678 programs, 10,255 rejections, 67 declined | 6,005: 3,255 programs, 2,750 rejections |
| After: core and reference on the kernel | 213: 118 programs, 91 rejections, 4 declined | 20,000: 9,701 programs, 10,231 rejections, 68 declined | 6,005: 3,255 programs, 2,750 rejections |

The suite has three more calls because of the new tests. Among the generated cases, the 23 further
programs and the one further declined job (a helical entry into a pocket, which is not ported) come from
the differences in outcome counted above, chiefly contours that close only to within the tolerance, which
both sides used to refuse.

## V-carving

`native/core/src/vcarve.cpp` is the one implementation of V-carving. `kerfkite-cam generate` reaches it
directly; Studio checks the settings in Python, in the order and with the messages they always had, and
asks the extension for the paths and depths (`"op": "vcarve"` of the `operation` command,
`studio.operations.kernel_vcarve`). The Shapely code in `studio/operations.py` remains as the fallback for
an installation without the extension.

### The method

It is the reference's sampled medial axis, unchanged in what it cuts:

1. Every ring of the region is sampled at equal steps of at most the sampling interval (`v_resolution`), at
   most 12,000 samples in all.
2. The Voronoi diagram of the samples is cut to the region. What remains approximates the medial axis, with
   a spoke from the axis to the wall between each two neighbouring samples.
3. Each piece is divided into steps of at most the interval, and at each point the bit goes as deep as lets
   it cut a circle of radius `r = min(tool radius, distance to the wall - interval)`, no deeper than the
   depth of cut: `z = -min(depth, r / tan(angle / 2))`.

The interval taken off the distance is what makes the carve safe between points as well as at them. The
distance to the wall cannot change faster than the cutter moves, so between two points one step apart it
is never less than at the nearer of them less half a step; a straight move between two points whose cuts
are each an interval short of the wall therefore stays at least half an interval short of it in its middle.

### What the kernel fixes

| Shapely code (now the fallback) | Kernel |
| --- | --- |
| `outlines(g)`, each ring from wherever GEOS starts it | every ring of the region from its smallest vertex |
| `line.interpolate(length * i / n)` | the same positions, lengths being square roots of whole numbers, each sample rounded to the grid |
| `voronoi_diagram(MultiPoint(samples), edges=True)` | `voronoi_edges(delaunay(samples), box of the region)` |
| `.intersection(g)` | `LineClipper(g).clip` for each edge |
| a piece shorter than a tenth of the interval is dropped | the same |
| `edge.interpolate(length * i / n)` | the same positions along the piece, each rounded to the grid |
| `pt.distance(g.boundary)` | `BoundaryDistance(g).to(point)` |
| `math.tan(math.radians(angle / 2))` | sine over cosine from the kernel's series, the same on every platform |
| a piece that nowhere cuts deeper than 0.0001 mm is dropped | the same |
| one path for each piece, in the order GEOS lists them | pieces joined into longer paths, in an order with little travel |

Every coordinate of a path is a whole number of nanometres divided by 1e6, and every depth comes from one
square root, at most two divisions and the series. **No function of the C library is called but `sqrt`**,
so a V-carve is the same on every platform, and `validation/check_installed_cam.py` pins one.

### Joining and order

The reference wrote one path for each piece of the diagram and lifted the tool between every two. The
kernel cuts every piece once, as before, but in fewer paths and with less travel. Both rules use whole
numbers, or doubles got from them by correctly rounded operations, so the result is the same everywhere.

* **Joining.** Pieces that share an end are joined end to end. From each end where an odd number of pieces
  meet, taken in order of x and then y, a path sets out along the piece whose far end is smallest, and at
  each junction follows the unused piece that continues most nearly straight (of two equally straight, the
  one whose far end is smaller), until none is left. What then remains is closed loops, which are followed
  the same way from their smallest point. The centre line of a stroke becomes one path and each spoke
  another.
* **Order.** The first path is the one with the smallest end of all, started from that end. Each next path
  is the one with an end nearest to where the last finished (of two equally near, the one whose end is the
  smaller point, then the earlier path), entered at that end, so a path may run in either direction.

Over the shapes of the check below the kernel wrote 46% as many paths as the fallback and 16% of its travel
between paths.

### Limits and messages

`V-carve outline too complex; increase sampling interval`, `V-carving requires router mode` and `Operation
produced no toolpaths` are raised as before, in the same order relative to the other checks. One limit is
applied earlier: a job may hold 200,000 points, and a carve whose pieces already pass that is refused in
the core with the generator's own message, `Job exceeds 200,000 cutting segments`, without being built. A
flat area far wider than the sampling interval reaches it (every spoke runs to the middle), and the
Shapely code measured every point of such a carve, one at a time, before the generator refused it. Where
such a job is wrong in another way as well (a ramp entry, say), it is the limit that is now reported.

### How the carve differs from the one Shapely gave

* **What is cut** is the same sampled medial axis. Given the same samples, the kernel's pieces and
  Shapely's coincide to nanometres (measured below).
* **Where the samples are.** The kernel samples each ring from its smallest vertex, where the Shapely code
  starts wherever GEOS starts the ring; and where a ring's length is a whole number of intervals, rounding
  could give Shapely one sample more. The spokes therefore stand elsewhere along the walls, by up to half
  an interval, and near a corner the pieces differ by about an interval. A piece that barely cuts in one
  carve may not cut at all in the other.
* **Round a corner that turns inwards** the method leaves gaps wider than the interval in either carve: the
  two samples beside such a corner own wedges that widen with the distance from it. The two carves put the
  edges of those wedges in different places, up to a few intervals apart. This is the reference's
  behaviour, and what its warning about wide areas refers to.
* **Coordinates** are whole nanometres; the program is written to three decimals.
* **Paths**: fewer and longer, in another order, some run the other way.

### How it is checked

The safety rule is: at every point of every path the circle the bit cuts at that depth, of radius
`|z| tan(angle / 2)`, reaches no wall; the point is in the region; `|z|` is within the depth of cut and the
cut radius within the tool's.

* `native/tests/selftest.cpp` carves 100 generated shapes and measures every point and the middle of every
  move with a plain floating-point distance of its own, compares a digest of every path and depth with a
  recorded value, and checks the program of a carve and each refusal.
* `tests/test_vcarve.py` measures the same rule with Shapely on the kernel and on the fallback, compares
  the kernel's carve with Shapely's carve of the same samples and with the fallback as shipped, and checks
  the joining and the order against their stated rules, the refusals on either code, and that Studio uses
  the core's answer unchanged.
* `validation/vcarve_check.py` runs those measurements on the shapes of the parity comparison and prints the
  worst figures.

    python validation/vcarve_check.py --cases 4000

The run of 22 November 2026 (GCC 13, Shapely 2.2.0 with GEOS 3.14.1, seed 93) carved 2,287 of the generated
shapes; the other 757 had nothing to carve or passed a limit. No shape failed.

| Measure | Result |
| --- | --- |
| The room between the cut and the nearest wall, at any point that cuts | at least 1.0000 sampling intervals |
| The same in the middle of any move that cuts | at least 0.5082 sampling intervals |
| Against Shapely's carve of the same samples, 2,283 shapes: the furthest any point of either carve is from the other | 0.0000022 mm (2.2 nm; 0.000015 of the sampling interval) |
| Total length of the two | within 2.1 parts in a million |
| Deepest cut of the two | within 0.0000062 mm |
| Against the fallback as shipped, the 958 shapes with a cut at least one interval in radius: the furthest such a cut is from a path of the other carve | 0.20 intervals in the middle shape, 1.99 in 95% of shapes, 5.44 in the worst |
| The same for the 295 of them with no corner turning inwards | 0.50 intervals in the worst |
| Total length against the fallback, 2,281 shapes | 335,842 mm against 335,225 mm in all (0.18%); by shape, 0.05% in the middle one and 8.7% in 95% |
| Paths | 184,039 against 401,030 |
| Travel between paths | 95.8 m against 611.1 m |

The first two rows are the safety rule with room to spare: an interval at the points, as the allowance
provides, and half an interval between them, as the argument above requires. The next three show that the
kernel's diagram and GEOS's are the same diagram: given the same samples the two carves agree to
nanometres. The rows against the fallback as shipped measure how far apart two carves of the same shape
are when their samples start in different places: half an interval where the walls are sampled evenly as
seen from every path, and a few intervals in the wedges round corners that turn inwards. The length of a
single shape can differ much more than the total where one carve barely cuts and the other does not: in
the worst, the kernel cut eleven times the length the fallback did.

`validation/native_core_parity.py` compares whole V-carve jobs between `kerfkite-cam` and the Python
generator with the extension: words of block letters, shapes with holes, thin strokes, flat areas, round
shapes and stars, with sampling intervals, bit angles, depths, step-downs and tool diameters over their
whole ranges and beyond them. The paths and depths are shared by construction; what is compared is the
checks and their order, the depth levels, the program and the totals.

### What still uses Shapely in Studio

These were not changed, and each could move onto the kernel:

| Module | Use of Shapely | What moving it needs |
| --- | --- | --- |
| `studio/operations.py`, the fallback | The Shapely code of offsets, pockets, fills and V-carving (`voronoi_diagram` of points sampled along the outline, clipped to the region; distances to the boundary) | Nothing: it goes when the portable wheel carries the extension or is retired. V-carving moved onto the kernel's own Voronoi diagram; see [V-carving](#v-carving). |
| `studio/router_entry.py` | `LineString` lengths and simplicity of each contour; the placing of a helix only as the fallback | The placing of a helix in a pocket has moved: `native/core/src/pocket_entry.cpp` does it on the kernel (the region, its insets, the nearest point of an inset, the distance to the walls and the line clipper), and Studio calls it through the `operation` command's `helix`. The Shapely code stays for an installation without the extension. The core's own `router_entry.cpp` and `simple_line.cpp` already hold the line parts. |
| `studio/cam.py` | `LineString(path).is_simple` before tabs and entries | The core's `simple_line.cpp` answers this by GEOS's rules; the kernel's `is_simple` (exact, on the grid) could replace both once the rejections are allowed to follow the grid. |
| `studio/design_api.py` | `region`, `outlines`, `unary_union` and `box` for Boolean operations between shapes in the editor | `combine()` and `unite_all()` exist; it needs a command that takes two sets of contours and returns outlines. Small. |
| `studio/solid_text.py`, `studio/cad_exchange.py` | Regions of glyph and profile outlines, orientation, constrained Delaunay triangulation for the faces of solids, clipping to boxes | Regions and orientation exist. Triangulating a polygon with holes is new work (the kernel's sweep could be extended to produce monotone pieces and triangles). |
| `studio/sketch_profile.py`, `studio/scad_geometry.py` | `Polygon` validity and area, `LinearRing` orientation | `is_simple`, `signed_area`, `orientation`: all exist. Small. |
| `studio/pdf_import.py`, `studio/program_import.py` | `STRtree` to find nearby paths, `MultiPoint` hulls, `Polygon` and `LineString` construction, containment | A spatial index (the kernel's bucket grid is internal) and a convex hull; containment is `locate()`. Small to medium. |

## Sources

The kernel was written from the general, published descriptions of these methods and from first
principles, without reading the source of GEOS, JTS, Clipper, Boost.Polygon, CGAL or any other geometry
library while writing it:

* winding numbers and fill rules for regions bounded by arbitrary closed curves;
* plane sweep over non-crossing segments (Bentley and Ottmann's ordering of segments along a sweep line);
* snap rounding (Greene and Yao; Hobby; Guibas and Marimont) and iterated snap rounding (Halperin and
  Packer): rounding crossings to a grid and bending every segment that passes through a rounded pixel;
* the Minkowski sum of a polygon and a disc as a union of swept edges and corner sectors, and the winding
  number of the curve bounding them (the convolution view of Guibas, Ramshaw and Stolfi);
* the crossing-number test for a point in a polygon, and symbolic perturbation for a vertex on a line;
* the Delaunay triangulation by divide and conquer on the quad-edge structure (Guibas and Stolfi,
  "Primitives for the manipulation of general subdivisions and the computation of Voronoi diagrams", ACM
  Transactions on Graphics 4(2), 1985), the in-circle determinant, and the Voronoi diagram as the dual of
  the triangulation, each vertex the centre of a triangle's circumscribed circle.

The triangulation and the Voronoi diagram were written from that paper's description and from the
geometry, without reading or translating the source of GEOS, Boost, CGAL, Qhull, Triangle or any
implementation of Fortune's algorithm.
