# The polygon kernel

KerfKite's own two-dimensional polygon library: regions with holes, Boolean operations, round offsets, line
clipping and the union of very many shapes. It is part of the native core (`native/core/src/polygon/`,
interface in `native/core/include/kerfkite/polygon.hpp`) and depends on nothing but the C++17 standard
library. It was written for KerfKite from the published methods named under [Sources](#sources), not taken
or translated from another library.

**It is not yet what the application uses.** Inside and outside offsets, pockets and hatch fills still go
through GEOS (`native/core/src/operations.cpp`) and, in Studio, through Shapely. The kernel is built and
tested beside them, with or without GEOS in the build, and can be tried through `kerfkite-cam polygon`.
[Switching the operations over](#switching-the-operations-over) lists what replaces what.

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

Not provided: Voronoi diagrams (V-carving still needs them), curves other than circular arcs drawn as straight
edges, and anything in three dimensions.

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
vertex. It gives outer rings clockwise and holes counter-clockwise, the directions GEOS has given until now,
so the direction of cut does not change.

## Limits

| Limit | Value |
| --- | --- |
| Coordinates | 2^37 units either side of the origin, about 137 m. The application's work area is within 10 m. |
| Offset distances and shape sizes | up to 2^35 units, about 34 m; shapes at least 2 units across |
| Edges in one operation, counting those made where edges cross | 8,000,000 |
| Shapes in an `Artwork` | 4,000,000; they are united as they arrive in steps of at most 2,000,000 edges |
| Arc segments | 1 to 1,024 a quarter turn |
| Rounds of resolving | 200 |

Input beyond a limit, or not finite, throws `kerfkite::ValueError`. Nothing recurses in proportion to the
size of the input.

## Results are the same everywhere

Integer arithmetic is exact. The floating-point arithmetic that places offset and arc vertices uses only
addition, subtraction, multiplication, division and square root, which IEEE 754 rounds correctly, and its
results are rounded to the grid. The bucket grid uses floating point too, but only to decide which pairs of
edges are examined, which cannot change a result.

Two things could still make a platform differ, and the self-test's recorded digest exists to catch them:

* **Fused multiply-add.** A compiler allowed to contract `a * b + c` rounds once where the source rounds
  twice. The build passes `-ffp-contract=off` or `/fp:precise`, as the rest of the core requires, and the
  kernel repeats that with a pragma where a compiler honours one.
* **Extended precision.** A compiler that evaluates `double` in 80-bit registers (32-bit x86 without SSE2)
  is not supported.

Measured: GCC 13 and Clang 18 on x86-64 give byte-identical answers to the 133,602 requests of the full
comparison with GEOS, and the same digest with either 128-bit integer, at `-O2` and at `-O3 -march=native`.
A build with fused multiply-add deliberately allowed (`-march=native -ffp-contract=fast`) also gave the
same digest, which shows that the test's cases do not turn on it, not that none can. ARM64 and MSVC have not
been compiled or run: the kernel is written to the standard and the portable integer is exercised here, but
the first build on each is the test. The continuous build (`.github/workflows/native-core.yml`) runs CTest
on them, and a different result would fail the digest there.

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
* a digest of every generated result, compared with the value recorded in the test.

`tests/test_polygon_kernel.py` checks the same properties through `kerfkite-cam polygon` when the tool has
been built (`KERFKITE_CAM`, or `build/native`).

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
| Application: outside and inside offsets (regions, then rings in order and direction) | 900 and 914 | 153 and 188 | 87 and 156 |
| Application: pockets, pass by pass | 896 pockets, 4,456 passes | 526 passes, 9 pockets | 186 |
| Application: hatch fills | 890 | 291 | 100 |

`Thin` cases are accepted and counted, not hidden. For Boolean results they are slivers no wider than the
tolerance that one library keeps and the other loses (30% of the generated pairs are a set of rings and the
same set moved by a few nanometres, which is nothing but slivers). For offsets and pocket passes the kernel
lay between the two swept offsets in every case; `thin` there means it was further from GEOS's polygon
buffer than the tolerance, at a sharp corner or where that buffer is itself off. For hatch fills it means
that some row ran along an edge or grazed a corner, so that the two did not cut the same parts, while the
kernel's rows lay in the region and covered what its width allows. For application pockets, 9 are pockets
the reference refuses for exceeding 100,000 vertices and the kernel completes within the limit.

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
* The limit of 100,000 vertices on a pocket in `studio/operations.py` was set for GEOS, which draws every
  arc again at every pass. The kernel's passes have far fewer vertices (4,057 against 156,384 for one
  generated pocket of 29 passes), so some pockets the reference refuses will be accepted.
* Only x86-64 with GCC and Clang has been run. See [above](#results-are-the-same-everywhere).
* `is_valid` does not check that the interior of a polygon is connected. (Results of the kernel are; the
  comparison with GEOS, whose validity test does check it, found none that was not.)

## Switching the operations over

Each call `studio/operations.py` makes to Shapely, and `native/core/src/operations.cpp` makes to the GEOS
layer, and what replaces it:

| Reference (Shapely) | Core today (`geometry::`) | Kernel (`polygon::`) |
| --- | --- | --- |
| `Polygon(path)` | `context.polygon(path)` | `ring_from_mm(path)` |
| `p.is_valid` | `polygon.valid()` | `is_simple(ring)` |
| `p.area < 1e-9` | `polygon.area() < 1e-9` | `std::fabs(signed_area(ring)) < 1e-9` |
| `GeometryCollection()`, then `result.symmetric_difference(p)` for each contour | `empty_collection()`, `symmetric_difference` | `compose(rings)` once, for all contours |
| `result.is_empty` | `result.empty()` | `region.empty()` |
| `g.buffer(d, quad_segs=32)` | `context.buffer(g, d, style)` | `offset(region, d)` |
| `inset.buffer(-step, quad_segs=32)` until empty | the same | `offset(inset, -step)` until `empty()` |
| `outlines(result)` | `geometry::outlines(result)` | `outlines(region)` |
| `affinity.rotate(g, -angle).bounds` | `rotate`, `bounds()` | turn the vertices of the region (`ring_to_mm`) by `-angle` and take their extremes; the kernel does not turn regions |
| `rotated.intersection(LineString(row))`, `lines(...)`, `affinity.rotate(line, angle)` | `intersection`, `lines`, `rotate` | turn the two ends of the row by `+angle`, `to_grid` them, and `LineClipper(region).clip(a, b)`: the parts are already in the region's frame, in order along the row |
| `line.length > 1e-6` | `line.length()` | the length of the part |
| `shapely.errors.GEOSException` | `geometry::Error`, declined | does not arise; `ValueError` only for input beyond the limits |

`kerfkite-cam polygon` with `"op": "compose"` is `studio.operations.region` with its three messages, and the
`cam` group of the validation script makes inside, outside, pocket and fill jobs from kernel calls in exactly
this way and compares them with `studio.operations`.

Where the kernel's tool paths will differ from GEOS's, and why that is acceptable:

* **Coordinates** are whole nanometres. The G-code is written to at most a few decimal places.
* **Where a ring starts**: at its smallest vertex, where GEOS starts wherever its algorithm happens to.
  Direction is unchanged (outer rings clockwise, holes counter-clockwise from `outlines`).
* **Arc vertices**: the same number for a square corner (32 a quarter turn), in different places, just
  outside the true arc instead of just inside. The tool path is the documented allowance further from the
  work at round corners, never nearer.
* **The order of the holes of one polygon** and of polygons of exactly equal area: the kernel's defined
  order, where GEOS's is an accident of its algorithm.
* **Vertices on straight runs** are dropped.
* **Pockets**: the same passes. The last pass, when it is thinner than the tolerance, can exist in one and
  not in the other. The kernel's passes have far fewer vertices, since it does not draw an arc again at every
  pass, so the reference's limit of 100,000 vertices is reached much later.
* **Hatch rows**: the same rows to within 3 nm, except rows that run along an edge or graze a corner, which
  the two cut differently by nature. A row that only touches a corner is nothing to GEOS; moved to the grid
  it can cut one to four nanometres off the corner, which passes the reference's test of `> 1e-6` mm. A
  port may prefer to drop parts shorter than a few grid units.
* **Contours refused**: a contour that touches itself or has no area is refused with the reference's
  message. The two can disagree only on a contour whose self-contact or emptiness is a matter of less than
  the grid.

Because the output is no longer GEOS's to the byte, `validation/native_core_parity.py` cannot compare these
four operations byte for byte with the Python reference once the core uses the kernel. Either the comparison
for them becomes the tolerance comparison of `validation/polygon_kernel_check.py`, or Studio calls the
kernel through the extension so that there is again one implementation.

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
* the crossing-number test for a point in a polygon, and symbolic perturbation for a vertex on a line.
