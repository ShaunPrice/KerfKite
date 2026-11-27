# Drawing with a pen

A spring-loaded pen in place of the cutter or the laser turns the machine into a plotter. It draws shapes, patterns and lettering, fills shapes with hatching, and can imitate a broad calligraphy nib with a fine pen. Choose **CAM → Machine head → Pen · drawing**.

Nothing in this guide has been drawn on a machine. The programs were checked against the built-in simulator only; draw on scrap paper first.

## Two ways to lift the pen

Choose **Pen is lifted by** to match how the pen is mounted.

| | The Z axis | A servo or solenoid |
|---|---|---|
| Mounting | A spring-loaded pen holder in the router's collet or clamp | A pen holder with its own lift, wired to the spindle or laser output |
| Pen down | Z moves a little below Z zero; the spring takes up the difference | `M3` with the **Pen-down S value** |
| Pen up | Z moves up to the **Pen lift** height | `M5`, or `M3` with the **Pen-up S value** |
| Z axis | Moves for every stroke | Never moves: set the holder's height by hand |
| Spindle | Never started | The output is the holder's signal, so no spindle or laser may be connected to it |

Either way, **take the cutter or the laser off first**. With a Z pen the program never starts the spindle, but a router left in the machine with the pen beside it is still a router.

### A pen on the Z axis

1. Mount the holder and jog down until the pen just touches the paper. Set **Z zero** there.
2. Choose **Use typical pen settings**: the pen is pressed 1 mm below Z zero, lifted 3 mm, and drawn at 2000 mm/min. Change them to suit:
   - **Pen pressed below Z zero** is how far the holder's spring is compressed. It must be less than the spring's travel. More gives a firmer line; a brush pen draws wider.
   - **Pen lift** is the height the pen travels at. A few millimetres is enough on flat paper and keeps the job quick.
   - **Plunge** is the speed the pen comes down at.
3. Preview. The program has no `M3`.

The **Step down** of a cutter means nothing to a pen: the pen always goes to the one height.

### A pen on a servo or solenoid

The holder is worked from the controller's spindle or laser output. The program lowers the pen with `M3 S` and the **Pen-down S value** (the **Spindle RPM** field, renamed), raises it with `M5`, and waits the **Pause for the holder** after each so that the pen has arrived before the machine moves. Z is not in the program at all.

- If the holder is up when the output is off, leave the **Pen-up S value** at 0.
- If it needs a signal to stay up, enter that value. The program then raises the pen with `M3 S` and that value and does not switch the output off between strokes. When the program ends the controller switches the output off, and the holder goes wherever it rests with no signal: make sure that is up.
- Find the two S values before putting a pen in: send `M3 S…` from a macro and watch the holder.
- A Grbl-family controller must be in spindle mode (`$32=0`), which **Apply mounted head** sets for a pen. In laser mode the controller would switch the output off during travel by itself and scale it with speed.
- The pause is written in the controller's own units: seconds for the Grbl family and LinuxCNC, milliseconds for Marlin, and the configured dwell units for Mach3 and UCCNC.

## What a pen can draw

- **Line** follows every path once: outlines, patterns, imported drawings, traced images.
- **Hatch fill** fills closed shapes with parallel lines at the **interval** and **angle** set, with an optional crosshatch. Set the interval to the width of the pen's line for a solid fill, or wider for shading.
- **Times over each line** draws everything again, for a pen that leaves a thin line.

Cutting operations (profiles, pockets, V-carving, drilling) and holding tabs are refused in pen mode.

### Lettering

In the **Text** tool choose the font **Workshop Mono · single line**. Its letters are single strokes, which a pen writes as a hand does. An outline font is drawn as hollow outlines, or filled with the hatch. See [Engraved lettering](LABELS.md).

### A broad nib, for calligraphy

A calligraphy pen has a wide, flat nib held at an angle: strokes across the nib are thick and strokes along it are thin. **Broad nib** imitates one with an ordinary fine pen by drawing every stroke several times, side by side along the nib's edge.

- **Nib width** is the width of the imitated nib. 0 draws a plain line.
- **Nib angle** is the angle of the nib's edge, anticlockwise from the X axis. 30° to 45° is usual for italic and gothic hands.
- **Pen line width** is the width of the line the real pen draws. It sets how many strokes fill the nib: a 1.2 mm nib with a 0.4 mm pen is four strokes.

Alternate strokes are drawn in opposite directions to save travel. The drawing grows by half the nib's width, so keep it that far inside the work area. It works on any path, including the single-line lettering.

### Pressure along a stroke

A path that carries its own depths (`path_depths`) presses the pen by a different amount at each point, which a brush pen draws as a line of changing width. This needs the Z axis and is available to programs made through the API; the editor does not draw such strokes yet.

## Different pens for different objects

Assign the current settings to a selected object to give it its own operation, nib or number of passes. Objects are drawn in order. To change the pen for another colour, turn the output of the other objects off and draw one colour at a time.

## Round a cylinder

A pen can draw on a cylinder turned by a rotary axis: a tumbler, a rolling pin, a tube. Set up the drawing as for flat paper, then use the **Rotary** tab as for a router job: enter the cylinder's diameter, choose which design axis wraps round it, and preview. See [Rotary machining](ROTARY.md) for the setup, the seam and the checks before running.

- **A pen on the Z axis** is wrapped as a router job is. Work zero on the radial axis is the cylinder's surface, where the pen just touches; the pen is pressed below it and lifted above it, and the spindle is never started.
- **A pen on a servo** draws at the surface and the radial axis is never moved, so set the holder's height over the cylinder by hand. The pauses for the holder are kept.
- A holder that needs a signal to stay up ends the program raised (`M3` with its up value) and not switched off. Every other rotary program must still end with the output off.
- The nib, the hatch and repeated passes are wrapped with the drawing. A nib's strokes lie side by side on the surface.
- Output is for grblHAL and the xPRO V5 to run, and for LinuxCNC as a file, as for a router.
- A fixture's clearance envelope is checked for a pen as for a cutter.

A cylinder that is not round or not centred changes how hard the pen presses as it turns. The spring takes up a little of that; a servo holder with a fixed height does not.

## Larger than the machine

A drawing made with a Z pen can be [tiled](TILING.md) like a router job. A servo pen's program cannot be tiled yet.

## Command line and API

A project with `"mode": "pen"` is a pen job:

| Field | Meaning | Default |
|---|---|---|
| `pen_lift` | `"z"` or `"servo"` | `"z"` |
| `depth` | Z pen: how far below Z zero the pen is pressed | 1 |
| `safe` | Z pen: the height the pen travels at | 5 |
| `power` | Servo pen: the S value that lowers the pen | required |
| `pen_up_power` | Servo pen: the S value that raises it; 0 raises it with `M5` | 0 |
| `pen_dwell` | Servo pen: seconds to wait after each lift and lowering, 0 to 5 | 0.2 |
| `passes` | Times over each line | 1 |
| `pen_nib`, `pen_nib_angle`, `pen_tip` | The broad nib: its width, its angle and the pen's own line width | 0, 45, 0.4 |
| `operation` | `"line"` or `"fill"` | `"line"` |

Any controller that can run a router job can draw. The editor's own generator, used when there is no server, writes the same program for line drawing.

## What has been checked

`tests/test_pen.py` follows the programs with an interpreter of the test's own: the strokes drawn are the design's, the pen is up for every travel, a Z pen's program never starts the spindle and a servo pen's never moves Z. It checks the nib's strokes by their geometry (a stroke along the nib's edge is thin, one across it is as wide as the nib), the hatch, repeated passes, the pause in each controller's units, a holder that is held up by a value, and each refusal. `tests/js/pen.test.cjs` holds the offline generator to the same programs line for line. `validation/pen_workflow.py` draws a rectangle and single-line lettering in a browser, runs the job on the simulator, and repeats it with a servo, a nib and hatching.

For a cylinder, the same file follows the rotary programs: the strokes are the design's with X turned into degrees, for both lifts, each output axis, both directions and a start angle; a servo pen never moves the radial axis; a held-up holder ends raised; and given lines that end with the pen down are refused. `tests/js/pen.test.cjs` holds the editor's own wrapper to the core's programs line for line, and the browser workflow draws round a cylinder on the simulated rotary controller.

No pen has been put in a machine. The two things only a machine can show are whether the spring's travel suits the depth chosen, and which S values work a particular holder.
