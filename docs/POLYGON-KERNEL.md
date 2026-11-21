# The polygon kernel

KerfKite's own two-dimensional polygon library: regions with holes, Boolean operations, round offsets, line
clipping and the union of very many shapes. It is part of the native core (`native/core/src/polygon/`,
interface in `native/core/include/kerfkite/polygon.hpp`) and depends on nothing but the C++17 standard
library. It was written for KerfKite from the published methods named under [Sources](#sources), not taken
or translated from another library.

**It is what the application uses** for inside and outside offsets, pockets and hatch fills.
`native/core/src/operations.cpp` builds those operations on the kernel; `kerfkite-cam` runs that code, and
Studio's Python calls the same code through its compiled extension (`studio/operations.py`), so the two cut
the same paths. GEOS is no longer part of the native build, which now contains no third-party code and
downloads nothing. Shapely, which carries GEOS, remains in Studio for three things: as the fallback for
these operations where the extension is not installed, in the modules that have not been moved yet, and as
the oracle the kernel is tested against. [The operations](#the-operations) describes how they are built
and how the paths differ from the ones GEOS gave.

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
V-carving is not part of this: it needs a Voronoi diagram, stays on Shapely in Studio, and is declined by
`kerfkite-cam`.

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

### What still uses Shapely in Studio

These were not changed, and each could move onto the kernel:

| Module | Use of Shapely | What moving it needs |
| --- | --- | --- |
| `studio/operations.py`, V-carving | `voronoi_diagram` of points sampled along the outline, clipped to the region; distances to the boundary | A Voronoi diagram or medial axis of a polygon, which the kernel does not have; then clipping (the kernel's `LineClipper`) and point-to-boundary distance. The largest piece. |
| `studio/operations.py`, the fallback | The Shapely code of offsets, pockets and fills | Nothing: it goes when the portable wheel carries the extension or is retired. |
| `studio/router_entry.py` | The region of the contours and its polygons, the inset by the cutter radius for helix and ramp clearance, nearest points, `LineString` lengths and simplicity | The `operation` command already gives the region and its insets; it also needs the distance from a point to a region's boundary and the nearest boundary point, which are small additions to `measure.cpp`. The core's own `router_entry.cpp` and `simple_line.cpp` already hold the line parts. |
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
* the crossing-number test for a point in a polygon, and symbolic perturbation for a vertex on a line.
