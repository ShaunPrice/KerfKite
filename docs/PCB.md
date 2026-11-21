# Circuit boards: Gerber and Excellon import

KerfKite reads the two files a board design program exports for manufacture: **Gerber** layers (RS-274X and X2) and **Excellon** drill files. This is the second part of circuit board engraving, after [probe levelling](PROBE-LEVELING.md).

**Isolation toolpaths are not generated yet.** A copper layer is read into the list of objects it draws, in millimetres, and stops there. Turning that list into copper outlines and offsetting a cutter around them needs polygon union and offsetting, which is the next step. What works today from an imported board:

- a **board outline** layer becomes closed paths for a cut-out line job;
- a **drill file** becomes drilling operations, one group for each drill diameter;
- any layer can be inspected as data: apertures, objects, bounds and warnings.

There is no panel for this in Studio yet. The importers are reached from the `kerfkite-cam` tool and from Python (`studio.pcb_import`). Importing only reads a file: nothing here writes to a controller or starts a job.

The importers are written once, in the native core (`native/core/src/gerber.cpp`, `excellon.cpp`), and have no Python version. From Python they need the compiled KerfKite extension.

## Commands

    kerfkite-cam gerber board-F_Cu.gbr                       the layer as JSON
    kerfkite-cam gerber board-F_Cu.gbr --arc-tolerance 0.01
    kerfkite-cam gerber-outline board-Edge_Cuts.gbr --offset [-95,115] --width 300 --height 200
    kerfkite-cam excellon board.drl
    kerfkite-cam excellon board.drl --units inch --integer 2 --decimal 4 --omitted-zeros leading
    kerfkite-cam excellon-drills board.drl --offset [-95,115]

The file is the input (`-` reads standard input) and the answer is JSON on standard output. Each `--option value` pair is a field of the request described below. A file that is refused exits with status 2 and the reason on standard error; status 1 is a file that cannot be opened or a usage error.

From Python:

    from studio import pcb_import
    from studio.cam import generate

    outline = pcb_import.outline_paths(pcb_import.read('board-Edge_Cuts.gbr'))
    offset = pcb_import.placement([outline['bounds']], margin=5)      # one offset for every layer
    outline = pcb_import.outline_paths(pcb_import.read('board-Edge_Cuts.gbr'), offset=offset)
    drills = pcb_import.drill_operations(pcb_import.read('board.drl'), offset=offset)
    for operation in drills['operations']:
        job = generate({**settings, **operation})                     # one job for each drill diameter
    copper = pcb_import.gerber(pcb_import.read('board-F_Cu.gbr'))     # objects only, for now

Every length in an answer is in millimetres, rounded to the nanometre, whatever the units of the file.

## Placing a board

Design programs export a board where it sits on their page, often with negative Y. KerfKite's work area starts at zero. The two conversions take an `offset` of `[X, Y]` that is added to every coordinate, and `placement` gives the offset that brings the lowest corner of one or more `bounds` to a margin. Use the same offset for every layer of a board, so that the holes stay registered with the outline.

Given `width` and `height`, a conversion checks that the result fits the work area and refuses it otherwise, with the extent it would have.

Mirroring a layer for the underside of a board is not provided yet.

## Gerber layers

### What is read

| Part of the format | Handling |
| --- | --- |
| Format statement `%FS`, units `%MO` | Required before the first coordinate or aperture. Leading-zero omission is the norm; trailing-zero omission and incremental notation are read, with a warning that they are deprecated. |
| Standard apertures `C`, `R`, `O`, `P` | With an optional round hole. A hole has to fit inside the aperture. |
| Aperture macros `%AM` | Primitives 1 (circle), 20 and 2 (vector line), 21 (centre line), 22 (lower-left line), 4 (outline), 5 (polygon) and 7 (thermal), each with rotation about the macro origin. Variables, `+ - x /`, unary signs and brackets; a variable that is never set is zero. |
| `D01`, `D02`, `D03`, `G01`, `G02`, `G03` | Lines, moves, flashes and arcs. |
| `G75` multi-quadrant and `G74` single-quadrant arcs | In multi-quadrant mode an arc that ends where it starts is a full circle. In single-quadrant mode the centre offsets carry no sign: the centre is the candidate that turns the right way through no more than a quarter turn and fits both ends best, and an arc that ends where it starts has no length. |
| Polarity `%LPD`, `%LPC` | Kept on every object. |
| Regions `G36` … `G37` | One object for each contour, arcs flattened. A contour has to close. |
| Step-and-repeat `%SR` | Expanded into copies, along Y first and then X as the specification orders them. A block is closed by `%SR*%`, by another `%SR`, or by the end of the file. |
| Attributes `%TF`, `%TA`, `%TO`, `%TD` | File attributes are returned; `.FileFunction` is also given as `file_function`. Aperture and object attributes are skipped. Attributes written as comments (`G04 #@! TF…`) are read too. |
| Deprecated but common | `G54`, `G55`, `G70`, `G71`, `G90`, `G91`, `M00`, `M01`, several commands between one pair of `%`, coordinates without `D01` after a `D01`, lines drawn with a rectangular aperture, and `%IN`, `%LN`, `%IPPOS`, `%ASAXBY`, `%IR0`, `%MIA0B0`, `%OFA0B0`, `%SFA1B1`. |

Where an older habit has only one reading, it is read and a warning says so: a line drawn before `G01`, a line drawn before any position is set (it starts from the origin), coordinates without an operation code.

### What is declined

A file is refused, with the line and the reason, in preference to being read in part. The messages begin `Line N:`.

| Not supported | Message |
| --- | --- |
| Block apertures | `block apertures (%AB) are not supported` |
| Aperture transformations | `aperture mirroring (%LM) is not supported`, likewise `rotation (%LR)` and `scaling (%LS)` |
| Image transformations | `a negative image (%IPNEG) is not supported`, `swapped axes (%AS)…`, `image rotation (%IR)…`, `image mirroring (%MI)…`, `an image offset (%OF)…`, `an image scale factor (%SF)…` |
| Moiré macro primitive | `aperture macro NAME uses the moire primitive (6), which is not supported` |
| Rectangular holes in standard apertures | `aperture D10 has a rectangular hole, which is not supported` |
| Lines drawn with an obround, polygon or macro aperture, or any aperture with a hole; arcs drawn with anything but a circle | `a line is drawn with aperture D12 (obround); only a circle or a rectangle may be used` |
| Different digits for X and Y | `the format statement gives X and Y different digits, which is not supported` |
| Any other extended command or code | `the extended command %KO is not supported`, `the code G12 is not supported` |

A file is also refused when it leaves out what it has to state, because the alternative is a guess: no `%FS` or `%MO` before coordinates, an arc before `G74` or `G75`, a flash or line before an aperture is selected, an aperture or macro that is not defined, an aperture defined twice, a region contour that does not close, coordinates without an operation code other than after `D01`, an arc whose centre is not the same distance from its two ends (by more than a quarter of the radius), and a file with no `M02`, which is taken to be truncated.

### The answer

    {
      "format": "gerber",
      "units": "mm",                       the units of the file; the answer is always millimetres
      "coordinate_format": {"integer": 4, "decimal": 6, "omitted_zeros": "leading", "notation": "absolute"},
      "file_function": "Copper,L1,Top",    or null
      "attributes": {".FileFunction": "Copper,L1,Top", ".FilePolarity": "Positive"},
      "arc_tolerance": 0.005,
      "apertures": [ ... ],
      "objects": [ ... ],                  in drawing order
      "counts": {"flash": 5, "line": 3, "arc": 1, "region": 1},
      "bounds": [2.0, 4.2, 21.0, 25.0],    or null when nothing dark is drawn
      "warnings": [ ... ]
    }

Request fields: `text` (the file) and optionally `arc_tolerance` (0.005 mm; 0.0005 to 0.1).

**Apertures**, in order of their D code:

    {"code": 10, "shape": "circle",    "diameter": 0.25, "hole": 0.0}
    {"code": 12, "shape": "rectangle", "width": 1.5, "height": 1.0, "hole": 0.0}
    {"code": 13, "shape": "obround",   "width": 2.0, "height": 1.0, "hole": 0.0}
    {"code": 14, "shape": "polygon",   "diameter": 2.0, "vertices": 6, "rotation": 30.0, "hole": 0.5}
    {"code": 15, "shape": "macro",     "macro": "RoundRect", "parts": [ ... ]}

`hole` is the diameter of a round hole at the centre, or 0. A polygon's `diameter` is that of the circle through its vertices; with no rotation one vertex lies on the positive X axis, and `rotation` is in degrees anticlockwise.

A macro aperture is given as the shapes its definition produces for this aperture's parameters, already evaluated, rotated and in millimetres about the flash point:

    {"shape": "circle",  "exposure": true,  "diameter": 0.6, "x": 0.5, "y": 0.3}
    {"shape": "polygon", "exposure": false, "points": [[x, y], ...]}        closed: the last point is the first

To build the aperture's image, take the parts in order: a part with `exposure` true is added to the image, and one with `exposure` false is cut from what the earlier parts have built. A thermal is given as a disc, a cut disc and two cut bars.

**Objects**:

    {"type": "flash",  "polarity": "dark", "aperture": 11, "at": [5.0, 5.0]}
    {"type": "line",   "polarity": "dark", "aperture": 10, "start": [5.0, 5.0], "end": [15.0, 5.0]}
    {"type": "arc",    "polarity": "dark", "aperture": 10, "start": [15.0, 10.0], "end": [10.0, 15.0],
                       "centre": [10.0, 10.0], "clockwise": false, "points": [[15.0, 10.0], ..., [10.0, 15.0]]}
    {"type": "region", "polarity": "dark", "points": [[2.0, 18.0], ..., [2.0, 18.0]]}

An arc is given both ways: as the arc itself (`start`, `end`, `centre`, `clockwise`) and flattened into `points`, which run from the start to the end. No chord of the flattened arc lies further than `arc_tolerance` from the true arc, and there are never fewer than eight chords to a full turn. A region's `points` are its contour, closed, with any arcs flattened in the same way. A line that starts and ends at one point is a dot of its aperture.

When an arc's centre is slightly off, so that its two ends are at different distances from it, the flattened radius runs evenly from one to the other and a warning gives the largest difference found (differences under 0.01 mm are not reported).

`bounds` covers the dark objects with their apertures. A clear object cannot extend the image and is left out of it.

**Polarity.** The copper image is built by taking the objects in order: a dark object adds its shape, and a clear object removes its shape from everything drawn before it. The shape of a flash is its aperture at `at`; of a line or arc, its aperture swept along the path; of a region, the inside of its contour.

### Limits

| Limit | Value |
| --- | --- |
| File size | 16 MiB |
| Objects, after step-and-repeat | 100,000 |
| Points of flattened arcs and region contours together | 500,000 |
| Apertures | 10,000 |
| Aperture macros; characters in one macro | 1,000; 65,536 |
| Shapes and polygon points in one macro aperture | 2,000; 20,000 |
| Shapes and polygon points in all macro apertures | 50,000; 200,000 |
| Nested brackets in a macro expression | 32 |
| Step-and-repeat copies along one axis | 1,000 |
| Coordinates | −10,000 to 10,000 mm |
| Any aperture dimension | 0 to 1,000 mm |
| Characters in one command | 4,096 |
| Warnings listed | 50 |

A step-and-repeat is checked against the object and point limits before any copy is made.

## Board outline

`gerber-outline` (`pcb_import.outline_paths`) reads a profile layer, such as KiCad's `Edge.Cuts`, and joins its lines and arcs into closed paths.

Request fields: `text`, and optionally `tolerance` (0.01 mm; 0.000001 to 1), `arc_tolerance`, `offset`, `width` and `height`.

    {
      "paths": [[[8.0, 5.0], [42.0, 5.0], ..., [8.0, 5.0]], ...],   each closed, the largest first
      "line_width": 0.1,                    the widest line of the outline; the paths are its centre line
      "bounds": [5.0, 5.0, 45.0, 35.0],
      "warnings": [ ... ]
    }

- The pieces may be drawn in any order and in either direction. Two ends join when they lie within `tolerance` of each other.
- An outline with a loose end is refused: `The board outline is open at X100.004 Y-83.000; the nearest other loose end is 0.004 mm away.` Close it in the design, or raise the tolerance if the gap is a drawing error.
- A place where more than two lines meet is refused: `The board outline branches at X10.000 Y10.000: 3 lines meet there`.
- An edge drawn twice counts once. Flashes, filled regions, clear lines and lines with no length are left out, and a warning says how many.
- More than one closed loop is accepted, with a warning: the largest comes first, and the others may be cut-outs inside it.

The paths are the outline itself. Use them as `paths` of a router job; to cut the board out at its true size, choose the outside contour operation so that the cutter runs outside the line.

## Excellon drill files

### What is read

| Part of the format | Handling |
| --- | --- |
| Header `M48` … `%` or `M95` | A `%` before `M48` is accepted. A file with no header is read as a body. |
| Units | `METRIC`, `INCH`, `M71`, `M72`. |
| Zero suppression | `LZ` (leading zeros present, trailing omitted) and `TZ` (the reverse). Coordinates with a decimal point need neither. |
| Digits | `METRIC,LZ,000.000`; comments `;FILE_FORMAT=3:3` (Altium, EasyEDA) and `; FORMAT={3:3/ absolute / metric / suppress leading zeros}` (KiCad). |
| Tools | `T1C0.8`, with feed and speed words skipped (`T01F00S00C0.3`). In the header, or at first use in the body. `T0` unloads. |
| Holes | `X…Y…`; a missing axis keeps its last value. |
| Repeats | `R5X1.0`: the last hole again, five more times, each a step further on. |
| Slots | `X…Y…G85X…Y…`, and straight routed slots: `G00X…Y…`, `M15`, `G01X…Y…`, `M16`, `G05`. |
| Plating | `;TYPE=PLATED`, `;TYPE=NON_PLATED`, and KiCad's `; #@! TF.FileFunction,Plated,…` comment. |
| Skipped | `FMAT,2`, `ICI,OFF`, `VER`, `DETECT`, `ATC`, `G90`, `G40`, `M47` messages, `M06`, `M09`, feed and speed lines. An unknown line in the header is skipped with a warning. |

### How the numbers are read

Many drill files leave out part of their number format. Each part is taken from the first of these that gives it:

1. the request (`units`, `integer` and `decimal`, `omitted_zeros`);
2. the header;
3. a format comment;
4. for digits and zero suppression only, the default of the format description: 2.4 digits for inches, 3.3 for millimetres, trailing zeros omitted.

**Units are never assumed.** A file that states none is refused: `The drill file does not state its units (METRIC, INCH, M71 or M72); give "units" in the request`.

When a default was used and it mattered, the answer carries a warning that names the request field to set, and `interpretation` always says how the file was read:

    "interpretation": {
      "units": "inch", "units_from": "header",
      "integer": 2, "decimal": 4, "digits_from": "default",
      "omitted_zeros": "trailing", "zeros_from": "default"
    }

`digits_from` and `zeros_from` are `request`, `header`, `comment`, `default`, `decimal points` (every coordinate carried a decimal point, so no format was needed) or `unused` (no coordinates). `omitted_zeros` names the zeros that are left out, which is the opposite of the header word: `LZ` is `"trailing"`.

The zero-suppression warning is given only when some coordinate has fewer digits than the format, since otherwise both readings agree. Check the hole positions against the board before drilling whenever a warning is present: a wrong reading moves holes by a factor of ten or more, and the bounds make that easy to see.

A request that contradicts the header is obeyed, with a warning. A header and a comment that disagree use the header, with a warning. A format that changes after coordinates have been read is refused.

### What is declined

The messages begin `Line N`.

| Not supported | Message |
| --- | --- |
| Incremental coordinates | `selects incremental coordinates (G91), which are not supported`, and `(ICI)` |
| Format 1 | `selects a format other than FMAT,2, which is not supported` |
| Routed arcs | `routes an arc (G02, G03), which is not supported` |
| Cutter compensation | `uses cutter compensation (G41, G42), which is not supported` |
| Canned cycles | `uses a canned cycle (G81 to G84), which is not supported` |
| Zero offsets | `sets a zero offset (G93), which is not supported` |
| Patterns and step-and-repeat | `uses pattern and step-and-repeat codes (M25, M01, M02, M08), which are not supported`; `repeats a pattern (% in the body), which is not supported` |
| Drilled text | `drills text (M97, M98), which is not supported` |
| Anything else in the body | `Line 7 is not understood: …` |

Also refused: a tool that is selected but not defined, a hole before any tool is selected, a tool defined twice with different diameters, a tool with no diameter or one above 100 mm, a routed move without `M15`, and a file that ends with the routing tool down. A file with no `M30` is read, with a warning that it may be truncated.

### The answer

    {
      "format": "excellon",
      "units": "mm",
      "interpretation": { ... },
      "plated": true,                       or false, or null when the file does not say
      "file_function": "Plated,1,2,PTH",    or null
      "tools": [{"number": 1, "diameter": 0.4, "hits": [[105.0, -85.0], [110.5, -85.25]]}, ...],
      "slots": [{"tool": 3, "diameter": 1.0, "points": [[130.0, -100.0], [134.0, -100.0]]}],
      "counts": {"hits": 6, "slots": 2},
      "bounds": [103.6, -106.4, 134.5, -84.8],   of the hole and slot edges, or null
      "warnings": [ ... ]
    }

Tools are in the order the file defines them, and each tool's `hits` are in the order the file drills them. A slot's `points` are its centre line: two points for a `G85` slot, two or more for a routed one.

### Limits

| Limit | Value |
| --- | --- |
| File size | 8 MiB |
| Holes, after repeats | 100,000 |
| Slots; routed points | 20,000; 100,000 |
| Tools | 1,000 |
| Repeat count | 10,000 |
| Coordinates | −10,000 to 10,000 mm |
| Tool diameter | above 0, at most 100 mm |
| Characters in one line | 1,024 |

## Drilling operations

`excellon-drills` (`pcb_import.drill_operations`) turns the holes into operations for the ordinary [drilling](DRILLING.md) generator. Request fields: those of `excellon`, and optionally `offset`, `width` and `height`.

    {
      "operations": [
        {"operation": "drill", "tool_shape": "drill", "tool_diameter": 0.4,
         "paths": [[[5.0, 25.0], [5.0, 25.0]], [[10.5, 24.75], [10.5, 24.75]]]},
        {"operation": "drill", "tool_shape": "drill", "tool_diameter": 0.8, "paths": [ ... ]}
      ],
      "groups": [{"tool_diameter": 0.4, "holes": 2, "tools": [1]}, {"tool_diameter": 0.8, "holes": 4, "tools": [2, 4]}],
      "slots": [ ... ],                     as above, moved by the offset; not drilled
      "bounds": [3.6, 3.6, 25.48, 25.2],    of the edges of the holes these operations drill
      "interpretation": { ... },
      "warnings": [ ... ]
    }

Each operation is a project fragment: merge it over the job's settings (head, controller, work area, spindle speed, depth, plunge feed, peck depth) and generate it. A hole is a point marker, a path that stays where it starts.

- Holes are grouped by diameter, smallest first. Two tools of the same diameter drill together.
- **Each diameter is a separate job.** An ordered job has one tool, so the generator refuses operations with different drills in one job. Change the drill and set work Z again between jobs.
- A group of more than 3,000 holes is split into several operations of the same diameter, which may share one ordered job.
- Slots are not drilled. They are listed with their centre line and width, to be cut as lines with a cutter of that width or smaller.
- Drills finer than 0.01 mm are refused, as the generator would refuse them.

## How it is checked

`tests/test_pcb_import.py` reads small Gerber and Excellon files written by hand for the purpose (`tests/pcb/`), and compares the answers with coordinates, arcs, macro shapes and areas worked out separately in the test. It covers each feature above, each refusal, the limits, and several hundred randomly damaged copies of the fixtures, every one of which has to be read or refused with a reason. `native/tests/selftest.cpp` repeats a short form of the checks without Python.

The importers have been run on the project's own fixtures and on files written to follow the formats that KiCad, Altium, EasyEDA and Eagle export. They have not yet been run on a large collection of boards from those programs. Check an imported board's bounds, hole counts and warnings against the design before cutting.
