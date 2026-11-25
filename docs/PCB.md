# Circuit boards

KerfKite makes a printed circuit board from the files a board design program exports for manufacture: it engraves round the copper so that each track and pad is cut free of the rest of the blank (isolation routing), drills the holes and cuts the board out. It reads **Gerber** layers (RS-274X and X2) and **Excellon** drill files, and follows the surface of the board with [probe levelling](PROBE-LEVELING.md), which thin engraving needs.

**None of this has yet been run on a physical machine.** It has been run on the simulator. The board files it has been tested with are fixtures written by hand in the style of common design programs' output (`tests/pcb/`), not exports from those programs. Check an imported board's size, hole positions and warnings against the design, rehearse on the simulator, and cut a scrap board first.

The first part of this document is the guide to making a board in Studio. The rest is the reference: the commands, the route, what is read from each format and what is declined.

## Making a board in Studio

### What is needed

- From the design program: one Gerber file for each **copper layer**, the **board outline** (KiCad's `Edge.Cuts`, a profile layer) and the **drill file**. A board can be made from the copper alone; the outline is needed to place both sides of a two-sided board alike.
- An engraving tool. A V-bit is usual: 20° to 60° with a tip of about 0.1 mm. Its width of cut depends on its depth, which is why the board is levelled.
- Drills for each hole diameter, and an end mill to cut the board out.
- A Grbl-family controller with a probe input, for levelling. Without one the board can still be imported, previewed and exported.
- The workstation server or the hosted editor. Board import is part of the native core; the **Circuit board** panel is hidden in the editor opened from a file. The hosted editor has the panel too: it can import, preview and export, but has no machine.

### Import

1. Set the work area and the controller under **CAM** as for any job.
2. Open **Projects**, **Circuit board**. Choose the copper layer, the board outline and the drill file. Any of the three may be left out.
3. Choose the **side**. **Top** is cut as the files draw it. **Underside, mirrored** is for the bottom copper: see [Both sides](#both-sides).
4. **Board corner X** and **Y** place the board: the lowest corner of its outline goes that far from the work origin. Leave room for the cut-out cutter.
5. Set the engraving tool and choose **Review board**.

The review lists what was read from each file and every warning, and draws the copper, the outline and the holes on the canvas. Nothing has been added to the design yet. Change a setting and review again as often as needed. **Add to design** then adds the board as ordinary objects: one layer for each tool, with Output off.

Importing reads files. It writes nothing to the controller and starts no job.

### The engraving tool

| Setting | Meaning |
| --- | --- |
| Tool | **V-bit**: give the tip width and the included angle, and the width of cut follows from the engraving depth: tip + 2 × depth × tan(angle / 2). A 30° bit with a 0.1 mm tip cuts 0.154 mm wide at 0.1 mm deep. **Width of cut stated**: for an engraving end mill, or a width measured from a test cut. |
| Engraving depth | How far below the surface the tool cuts. The copper of a common board is 0.035 mm thick; 0.05 to 0.1 mm clears it when the board is levelled. |
| Passes | The number of contours cut round every piece of copper. One pass isolates. More passes widen the gap, which makes soldering easier. |
| Overlap | How much of its width each pass shares with the one before, 20% unless changed. Contour *k* (from 0) lies half the width of cut plus *k* × width × (1 − overlap) outside the copper. |
| Direction | **Climb** (the default) or **conventional**, for a spindle turning clockwise seen from above. In climb milling the copper that is kept is on the right of the tool's travel: clockwise round a pad, anticlockwise round the inside of a clearance. Conventional runs every contour the other way. |
| Arc tolerance | How far a round shape or an arc may lie from its true curve, 0.005 mm unless changed. A larger value gives fewer points. |
| Cut the isolation off at the board outline | Leaves out what lies outside the outline, such as the contour round a pour that runs to the board's edge. See [Clipping](#clipping-to-the-outline). |

When there are several passes the **outermost is cut first and the one next to the copper last**, so that the cut that forms the edge of a track removes little and is not pushed aside by a full-width slot. Within a pass, each contour is cut after the one that starts nearest to where the last ended.

The width of cut is what decides whether the board works. Too narrow a setting leaves thin copper between passes; too wide a setting moves the contours away from where the tool really cuts, and hides gaps the tool cannot pass. Measure a test cut if the tip width is not known.

### Warnings, and the places the cutter cannot reach

Every warning of every file is listed under the review. Three kinds need action.

**The cutter cannot pass between separate copper features.** Where two pieces of copper are nearer to each other than the tool is wide, no contour can run between them: the copper of the blank stays between them and they are left joined, which is a short circuit if they belong to different nets. KerfKite never leaves this unsaid. The warning gives the number of such places, the width the tool cuts and the smallest gap, for example:

    The cutter cannot pass between separate copper features in 2 places: the paths leave them joined.
    The tool cuts 0.300 mm wide and the smallest gap is 0.295 mm. A tool that cuts no wider than
    0.292 mm would separate everything.

Each place is marked with a red ring on the canvas, and stays marked after the board is added (**Forget board marks** removes the marks). The remedy is a narrower cut: a finer tip, a smaller angle or less depth. Or change the design's clearances. A gap exactly as wide as the tool counts as one the cutter cannot pass; a gap has to exceed the width of cut by 0.002 mm to be relied on.

Two pieces of copper that touch at a single point are reported in the same way, with a gap of zero.

**The drill file's number format.** See [Drilling](#drilling-by-diameter).

**Layers that do not lie on one board.** `Holes lie outside the board outline` and `The copper reaches beyond the board outline` mean that the files do not share an origin or a scale: a drill file read with the wrong units or digits, or a layer of another board.

Other warnings are for information: deprecated codes that were read, the outline's loops, slots that are not drilled, and specks. A *speck* is a sliver of copper thinner than twice the arc tolerance, which arises where a design ends a clear shape exactly on a round one (the bars of a thermal pad reach exactly to its rim). Specks are left out and counted.

### The stages of a board

**Add to design** makes one layer for each tool, in cutting order, and gives the objects of each layer a tool number and name:

| Layer | Object | Operation and tool |
| --- | --- | --- |
| PCB top · isolation | Isolation contours | Line at the engraving depth, with the V-bit or engraver |
| PCB top · drill 0.8 mm (one for each diameter) | The holes of that diameter, as circles | Drill, with a drill of that diameter, to the drill depth |
| PCB top · slots 1 mm (one for each width) | The centre lines of the slots | Line, with an end mill of the slot's width |
| PCB top · cut-out | The board outline | Outside contour, with the cut-out cutter. A loop inside the board is cut on its inner side. |

These are ordinary objects and layers. They can be moved, hidden, deleted and saved with the project, and the preview, the 3D view, holding tabs, probe levelling and the run controls treat them as any other. Feed, plunge feed, spindle speed and clearance come from **CAM** and apply to whichever stage is the job.

Every layer starts with **Output off**, as every import does, so that nothing is cut before it has been reviewed. A job has one tool: Studio refuses to preview or run operations with different tools as one job. There are two ways to work through the stages.

- **A stage at a time, with levelling.** Under **Board stages** in the panel, choose a stage: its Output is turned on and that of the other board stages off. Generate the preview, fit that stage's tool, set work Z on the board, probe if the stage is an engraving, and run. Then choose the next stage.
- **A file for each tool.** Choose **Output every stage**, then **Manual tool changes** under CAM and **Generate separate stage files**. Each stage is a separate G-code file that ends retracted with the spindle stopped. See *Manual tool changes* in the [user guide](USER-GUIDE.md). These files are not levelled.

Set work Z again after every tool change. Keep the XY origin: every stage is in the same coordinates.

### Engraving with levelling

1. Choose the isolation stage and **Generate preview**. The G-code plunges to the engraving depth and nowhere deeper.
2. Under **Control**, connect, and set work zero with the tool just above the board.
3. Open **Probe levelling**, connect the probe (one lead on the tool, the other on the copper), and **Probe surface**.
4. Run the job with **Follow the probed surface** ticked, or export the levelled G-code.

[Probe levelling](PROBE-LEVELING.md) describes each step and its limits.

### Drilling by diameter

Each drill diameter is its own stage, because each needs its own drill. Two tools of the same diameter in the file drill together. Slots are not drilled: they are added as lines to be cut with an end mill of the slot's width.

Many drill files leave out part of their number format, and a wrong reading moves every hole by a factor of ten or more. **Drill number format** shows how the file was read and where each part came from: the file's header, a format comment, a default, or what was entered. The section opens by itself when a default was used. Units are never assumed: a file that does not state them is refused until they are given. To correct a reading, state the units, the integer and decimal digits, or the zeros that are omitted, and review again. The holes are drawn on the canvas over the copper, so a wrong reading is seen at once: pads without holes, or a refusal because the holes do not fit the work area.

Drill depth is measured to the tip. Add the length of the drill's point to the thickness of the board, and protect the spoilboard.

### The cut-out

The outline is cut on its outer side, so that the board keeps its size, with the cutter, depth and step down set under **Drilling and cut-out**. Add holding tabs to the outline object (**Holding tabs** under CAM) to keep the board in place as it comes free, or hold it with tape.

### Both sides

A two-sided board is turned over to cut its underside, and the two sides have to register. KerfKite's rule is simple: **the board is turned over left to right, about a vertical line through the middle of its outline, and everything cut with the board face down is mirrored about that line.** The outline is symmetrical about its own middle only if it is a rectangle, but the *line* is the same for both sides whatever the shape, so the turned board occupies the mirror image of where it lay and the holes of one side meet the pads of the other.

1. Import the top with its outline and drill file, and cut it: engrave, drill, and leave the cut-out for last.
2. Turn the board over left to right, so that its left edge becomes its right edge, and fix it so that the middle line of the outline is where it was. Two pins through drilled holes that are mirror images of each other, or a fence along the left and right edges, do this.
3. In the panel, choose the bottom copper, choose **Underside, mirrored**, and leave **Register with the board already in this design** ticked. The underside is then placed with the same offset as the top and mirrored about the middle of the same outline, even if the outline is not chosen again.
4. Engrave the underside, then cut the board out. The outline and the holes do not need to be imported again for the underside. If they are, they are mirrored with the copper, for holes drilled or an outline cut with the board face down.

Gerber files draw every layer as seen from the top of the board, the bottom layer included, which is why the underside has to be mirrored and the top does not. Do not mirror the files in the design program as well.

What has to be treated alike: the outline, both copper layers and the drill file of one board take the **same offset**, and whatever is cut face down takes the **same mirror line**. From the command line and the route this is the caller's to do (`offset` and `mirror_x`); the panel does it.

### Clipping to the outline

With **Cut the isolation off at the board outline**, contours and parts of contours outside the outline are left out. A contour that is cut short no longer closes, and the copper beside it is then isolated only once the board is cut out along that outline: a warning says how many contours were cut short. Do not use this option for a board that will stay in a larger panel.

### What is not supported

- Running on a real machine has not been tested, as said above.
- Removing all the copper between the tracks (a rub-out). More passes widen the isolation; they do not clear a region.
- Choosing the engraving width for each gap, or a second, finer tool for the tight places.
- Solder mask, silk screen and paste layers as toolpaths. Any layer can be inspected as data.
- Negative layers (`.FilePolarity,Negative`, old plane layers): refused for isolation.
- Nets. KerfKite knows pieces of copper, not which pieces belong to one net, so it reports every place where two separate pieces are too close, including pieces that a wire or a via would join anyway.
- Automatic alignment of the second side by camera or probe. Registration is by the mirror line and the fixture.
- Tool-length measurement and automatic tool changes. Work Z is set by hand after each change.
- Holes larger than the drills to hand as milled circles: drill them, or draw the circle and use a pocket or an inside contour.
- What the importers decline, listed in the reference below.

## Reference

The importers and the isolation are written once, in the native core (`native/core/src/gerber.cpp`, `excellon.cpp`, `isolation.cpp`), and never had a Python version. The isolation is built on KerfKite's own [polygon kernel](POLYGON-KERNEL.md). Studio reaches them through one design route, `POST /api/pcb-import` (`native/server/design_routes.cpp`), which the workstation server and the hosted editor both serve; it has no access to the controller.

## Commands

    kerfkite-cam gerber board-F_Cu.gbr                       the layer as JSON
    kerfkite-cam gerber board-F_Cu.gbr --arc-tolerance 0.01
    kerfkite-cam gerber-outline board-Edge_Cuts.gbr --offset [-95,115] --width 300 --height 200
    kerfkite-cam gerber-isolation board-F_Cu.gbr --tool-width 0.2 --passes 2 --offset [-95,115]
    kerfkite-cam gerber-isolation board-B_Cu.gbr --tool-width 0.2 --offset [-95,115] --mirror-x 25
    kerfkite-cam excellon board.drl
    kerfkite-cam excellon board.drl --units inch --integer 2 --decimal 4 --omitted-zeros leading
    kerfkite-cam excellon-drills board.drl --offset [-95,115]

The file is the input (`-` reads standard input) and the answer is JSON on standard output. Each `--option value` pair is a field of the request described below. A field that is not known is refused, so that a misspelt option is not passed over. A file that is refused exits with status 2 and the reason on standard error; status 1 is a file that cannot be opened or a usage error.

Every length in an answer is in millimetres, rounded to the nanometre, whatever the units of the file.

## Placing a board

Design programs export a board where it sits on their page, often with negative Y. KerfKite's work area starts at zero. The three conversions (`gerber-outline`, `gerber-isolation`, `excellon-drills`) take an `offset` of `[X, Y]` that is added to every coordinate, and `placement` gives the offset that brings the lowest corner of one or more `bounds` to a margin. Use the same offset for every layer of a board, so that the holes stay registered with the outline.

Given `width` and `height`, a conversion checks that the result fits the work area and refuses it otherwise, with the extent it would have.

`mirror_x` turns a conversion's result over about the vertical line at that X, after the offset: X becomes 2 × `mirror_x` − X. It is for what is cut with the board face down. Give the same value, the middle of the placed outline, to every conversion of that side: see [Both sides](#both-sides).

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

Request fields: `text`, and optionally `tolerance` (0.01 mm; 0.000001 to 1), `arc_tolerance`, `offset`, `mirror_x`, `width` and `height`.

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
| Header `M48` … `%` or `M95` | A `%` before `M48` is accepted. A file with no header is read as a body. A byte order mark before the first line is passed over, in Gerber files too. |
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

`excellon-drills` (`pcb_import.drill_operations`) turns the holes into operations for the ordinary [drilling](DRILLING.md) generator. Request fields: those of `excellon`, and optionally `offset`, `mirror_x`, `width` and `height`.

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

## Isolation toolpaths

`gerber-isolation` (`pcb_import.isolation`) reads a copper layer and returns the contours a cutter follows to isolate its copper.

Request fields: `text` and `tool_width`, and optionally `passes`, `overlap`, `direction`, `order`, `offset`, `mirror_x`, `clip`, `width`, `height` and `arc_tolerance`.

| Field | Default | Range | Meaning |
| --- | --- | --- | --- |
| `tool_width` | required | 0.01 to 10 mm | The width of cut at the engraving depth. `pcb_import.v_bit_width(tip, angle, depth)` gives it for a V-bit. |
| `passes` | 1 | 1 to 50 | Contours round every piece of copper. |
| `overlap` | 0.2 | 0 to 0.9 | The part of the tool width a pass shares with the one before. The step between passes is `tool_width × (1 − overlap)`. |
| `direction` | `"climb"` | or `"conventional"` | Climb keeps the copper on the right of travel, for a clockwise spindle. |
| `order` | `"outer_first"` | or `"inner_first"` | Which pass is cut first. |
| `offset` | `[0, 0]` | ±10,000 mm | Added to every coordinate. |
| `mirror_x` | none | ±10,000 mm | Turns the placed layer over about that X. |
| `clip` | none | 1 to 3,000 closed paths, 100,000 points | Paths in placed coordinates (a board outline) outside which nothing is cut. A path inside another is a cut-out. |
| `width`, `height` | none | 1 to 2,000 mm | The work area; the contours have to fit it. |
| `arc_tolerance` | 0.005 | 0.0005 to 0.1 mm | For arcs and round shapes. |

### How the copper image is built

Every object of the layer becomes a polygon on the kernel's grid of one nanometre, and the objects are combined in file order: a dark object is added to the image and a clear object is cut from what has been drawn before it.

| Object | Shape |
| --- | --- |
| Flash of a circle, rectangle, obround or regular polygon | The aperture at the flash point, less its round hole if it has one. The hole is part of the aperture: it does not remove copper drawn earlier. |
| Flash of a macro aperture | The macro's parts in order, exposure on adding and exposure off cutting from what the earlier parts built. |
| Line with a circle aperture | The line with round ends. |
| Line with a rectangle aperture | The rectangle swept without turning: a hexagon, or a rectangle when the line runs along an axis. |
| Arc | The flattened arc with round ends and round joins. |
| Region | The inside of its contour under the non-zero rule, which reads a contour that touches itself and a cut-in that runs along itself. |
| A line with no length | A flash of its aperture. |
| Anything drawn with an aperture of no size | Nothing; a warning counts them. |

Round shapes are many-sided. A round shape is drawn just outside its true curve, by no more than the arc tolerance; an arc is drawn as chords just inside it, by no more than the arc tolerance. The image is therefore within the arc tolerance of the design everywhere, on either side. Because a round *clear* shape is also drawn just outside its curve, copper beside a clearance can be up to the arc tolerance short.

Islands that vanish when shrunk by the arc tolerance are specks, left out and counted (`specks`).

### The contours

Contour *k* is the boundary of the copper grown by `tool_width / 2 + k × step`, each grown from the copper itself so that no error builds up from pass to pass. Its straight parts lie at exactly that distance; its round corners lie outside the true curve by at most 0.001 mm. A contour is never nearer the copper image than its distance, to a few nanometres.

A contour is a closed path, the first point repeated at the end, unless `clip` cut it. An outer contour runs clockwise and a contour inside a clearance anticlockwise, which is climb milling; `"conventional"` reverses every path.

### The places the cutter cannot reach

The first contour is the boundary of the copper grown by half the tool width. Two pieces of copper are separated by it exactly when they lie in different pieces of that grown region. KerfKite finds which grown piece each island of copper lies in, with exact arithmetic, and reports every pair of islands that share one and are near enough to be the cause: no further apart than the tool width plus 0.002 mm (twice the corner allowance, and a margin). Every island that shares a contour with another is accounted for by at least one reported place.

Each place gives the two islands (their positions in `copper`), the gap between them, the nearest points of the two and the point midway. `separating_tool_width` is the widest tool, to the micrometre below, that leaves a contour in every gap of the board: the smallest gap less 0.002 mm. It is null when no tool of 0.01 mm or more would do, as when two pieces touch.

The gaps are measured on the image, whose round shapes are many-sided, so the gap between two round pads can differ from the design's by up to the arc tolerance on each side.

### The answer

    {
      "format": "isolation",
      "tool_width": 0.2, "passes": 2, "overlap": 0.2, "step": 0.16,
      "direction": "climb", "order": "outer_first",
      "offset": [-95.0, 115.0], "mirror_x": null, "arc_tolerance": 0.005,
      "layer": {"units": "mm", "file_function": "Copper,L1,Top", "counts": {...}, "bounds": [...]},
      "operations": [{"operation": "line", "paths": [[[x, y], ...], ...]}],   in cutting order
      "cut": [{"pass": 2, "distance": 0.26, "paths": 5}, {"pass": 1, "distance": 0.1, "paths": 7}],
      "path_count": 12, "point_count": 730,
      "clipped": {"cut": 0, "dropped": 0},        contours cut short by the clip, and left out whole
      "bounds": [4.74, 4.74, 45.1, 35.2],         of the contours: the tool's centre
      "copper": [{"shell": [[x, y], ...], "holes": [[[x, y], ...]]}, ...],    the image, for preview
      "islands": 6, "specks": 0,
      "copper_bounds": [5.0, 5.0, 44.9, 35.0],
      "unreachable": [{"at": [26.0, 25.0], "gap": 0.295, "between": [[25.85, 25.0], [26.145, 25.0]],
                       "islands": [3, 4]}],
      "unreachable_count": 1,                      all of them; at most 1,000 are listed
      "smallest_gap": 0.295, "separating_tool_width": 0.292,
      "warnings": [ ... ]                          the layer's own, then those of the isolation
    }

Each entry of `operations` is a project fragment for a router line job: merge it over the job's settings with the engraving depth as `depth` and `step`, and generate. The operations of one answer use one tool and may share an ordered job. Contours are divided among operations of at most 3,000 paths and 100,000 points, which is what the generator takes in one operation.

### Limits

| Limit | Value |
| --- | --- |
| File size | 16 MiB. Through Studio's route, 4 MiB for each file, and the panel sends at most 7 MiB in all. |
| Points of the copper image | 2,000,000 |
| Contours in one pass; in the answer | 20,000; 100,000 |
| Points of all contours | 1,000,000 |
| Points of one contour | 100,000 |
| Unreachable places listed | 1,000 |

A Studio project holds 3,000 paths and 100,000 points in all, which is the limit a board added to a design meets first. Fewer passes or a larger arc tolerance bring a large board within it.

### The same on every platform

The polygon kernel works in whole nanometres with exact integer arithmetic, and the isolation calls no function of the platform's mathematics library: the vertices of a regular polygon aperture come from the kernel's own series for sine and cosine. One isolation job is pinned in `validation/check_pinned_results.py` and one in the core's self-test, so that a platform giving different bytes fails its build. The Gerber importer itself uses the platform's library in two places, flattening arcs and rotating macro primitives, and rounds what it computes to the nanometre; a difference there would need a result within a few parts in 10¹⁰ of half a nanometre. The pinned jobs have no arcs and no rotated macros, so they do not depend on it.

## Studio's route

`POST /api/pcb-import` takes the files of one side of a board as text and answers with all of the above, placed. Fields: `copper`, `outline`, `drill` (any may be absent); `side` (`"top"` or `"bottom"`); `offset`, or `margin` (5; one distance or `[X, Y]`) to place the outline's lowest corner; `mirror_x` (the bottom side only; by default the middle of the placed outline); `width` and `height`; `tool_width`, or `v_bit` as `{"tip_width", "angle", "depth"}`; `passes`, `overlap`, `direction`, `order`, `arc_tolerance`; `clip` (true to clip to the outline); and `drill_format` as `{"units", "integer", "decimal", "omitted_zeros"}`. Each file may be up to 4 MiB. A field that is not known is refused.

The answer has `side`, `offset`, `mirror_x`, `bounds`, `copper` (the isolation answer), `outline` (the outline answer), `drill` (the drilling answer with `plated`, `file_function` and `counts` added) and `warnings` about the files together. The underside needs the outline, or the `mirror_x` and `offset` of the first import, and is refused without. `/api/session` has `pcb_import: true` when the route can answer.

## How it is checked

The tests ask the programs: `tests/kerfkite_core.py` hands each request to `kerfkite-cam`, and the route tests start `kerfkite-studio` and `kerfkite-web` on the loopback interface. They were ported from the Python reference's tests of the same names and run with `python -m unittest discover -s tests`, in continuous integration on every platform the programs are built for; Shapely is their oracle, and they are skipped without it. `tests/test_pcb_import.py` reads small Gerber and Excellon files written by hand for the purpose (`tests/pcb/`), and compares the answers with coordinates, arcs, macro shapes and areas worked out separately in the test. It covers each feature above, each refusal, the limits, and several hundred randomly damaged copies of the fixtures, every one of which has to be read, isolated or refused with a reason. `native/tests/selftest.cpp` repeats a short form of the checks, and the reference's route comparison sends the route these boards with their members changed.

`tests/test_pcb_isolation.py` checks the isolation against Shapely as an independent oracle. The copper image is rebuilt in the test from the importer's list of objects, shape by shape, and compared with the core's: neither may reach beyond the other by more than the arc tolerance. Every contour is then measured against the copper: every point and every mid-edge point within the corner allowance of its pass's distance, and no part of any contour nearer. Which islands the first contour separates is worked out from the contours alone and has to be exactly the grouping the reported places give; every reported gap has to be the distance Shapely measures; and every pair of islands that the oracle finds clearly nearer than the tool has to be reported. The fixtures are pads and tracks at known spacings, a ground pour with clearances, thermal reliefs by spokes and by a thermal macro, clear cut-outs, a region with a cut-in, a step-and-repeat panel and a small two-sided board, with offset, mirror, clip, direction and order each checked for what it states.

`tests/test_pcb_api.py` checks the route: placement, the underside's mirror line, the warnings of each file, the width worked out for a V-bit, every refusal as a JSON `400`, the hosted service, and that the server's controller status is the same before and after an import. `tests/js/pcb-board.test.cjs` checks the panel's own logic. `validation/pcb_workflow.py` drives the editor in a browser against the simulator: it imports the fixture board, reads the warnings and the marks, corrects the drill format, adds the board, previews the isolation at its depth, probes and runs it levelled, drills a diameter as its own stage, exports the tool stages and registers the underside.

The importers have been run on the project's own fixtures and on files written to follow the formats that KiCad, Altium, EasyEDA and Eagle export. They have not yet been run on a large collection of boards from those programs, and no board has been cut on a machine. Check an imported board's bounds, hole counts and warnings against the design before cutting.
