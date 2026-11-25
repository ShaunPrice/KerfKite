# Probe levelling

A thin engraving needs its depth held to a few hundredths of a millimetre. Isolation routing on a circuit board is the usual case: the copper is about 0.035 mm thick, and a board clamped to a spoilboard is seldom flatter than a tenth. Probe levelling measures the stock surface on a grid with the controller's probe input, then runs the job with every cutting move following that surface.

It applies to router jobs on Grbl-family controllers (Grbl 1.1, grblHAL, FluidNC, xPRO V5), which probe with `G38.2`. Laser, drag-knife and rotary jobs are not levelled.

## What you need

- A probe circuit on the controller's probe input. For a circuit board the tool itself is the probe: one lead on the cutter or collet, the other on the copper.
- The workstation server. Levelling is part of the native core; the **Probe levelling** panel is hidden in the editor opened from a file.

Test the probe before relying on it: touch the two leads together and confirm that the controller's status report shows the probe pin (`Pn:P`). A probe that does not trigger drives the tool into the board until the probe depth is reached.

## In Studio

1. Draw the job, choose **Router**, and generate a preview.
2. Under **Control**, connect the controller and set work zero with the tool just above or on the stock, inside the area to be cut.
3. Open **Probe levelling**. Set the probe spacing (10 mm suits most boards), tick the setup check and choose **Probe surface**.
4. Studio probes a grid covering the cutting bounds plus a 2 mm margin, back and forth. Work Z zero is set where the first point touches, so the surface there is Z zero.
5. When probing ends, the panel shows the number of points and the range of heights found, and **Follow the probed surface when running** is ticked. **Run previewed job** then sends the levelled program.

**Export levelled G-code** saves the levelled program for another sender. It is valid only for this board in this position.

Studio forgets the probed surface whenever work zero or the tool length may have changed: setting zero, homing, unlocking, resetting, applying a head mode, and connecting or disconnecting. A jog does not. Editing the design keeps the surface, provided the new job still cuts inside the probed area; a job that leaves it is refused.

A levelled job starts from the workstation. Arming a phone start is refused while levelling is on, because the phone start sends the previewed program as it is.

## What the probing program does

    G21
    G90
    M5                      spindle off
    G0 Z2.000               clearance, in work coordinates
    G0 X8.000 Y8.000
    G38.2 Z-1.000 F25.0     probe down to at most 1 mm below work zero
    G10 L20 P1 Z0.000       first point only: this height is work Z zero
    G0 Z2.000
    G0 X15.250 Y8.000
    G38.2 Z-1.000 F25.0
    ...

| Setting | Default | Limits |
| --- | --- | --- |
| Probe spacing | 10 mm | 1 to 100 mm; at most 400 points |
| Margin around the cutting bounds | 2 mm | 0 to 20 mm, kept inside the work area |
| Clearance Z | 2 mm | 0.5 to 25 mm |
| Probe depth below work zero | 1 mm | 0.1 to 5 mm |
| Probe feed | 25 mm/min | 1 to 200 mm/min |

Each probe has to finish within 10 seconds, because the controller acknowledges a probe line only when its motion ends. Studio refuses a combination of clearance, depth and feed that could take longer.

If a probe ends without contact the controller raises `ALARM:5` and the job stops. Studio accepts a surface only when every point reported contact and no point differs from the first by more than 5 mm.

## How the program is levelled

Heights are kept as offsets from the first probed point. Between grid points the surface is interpolated bilinearly.

- A plunge (`G1 Z`) is corrected by the surface height at its position.
- A cutting move (`G1 X Y`) is split into pieces no longer than a quarter of the grid spacing (between 0.25 mm and 2 mm), and each piece ends at its programmed depth plus the surface height there. The programmed end point is reached exactly.
- Rapids are left as written. A rapid that is not at least 0.1 mm above the highest probed point is refused.
- A cut outside the probed area is refused.
- Only absolute, millimetre programs of straight moves are accepted. Arcs, relative moves, inch units, probing lines and laser power words are refused: the leveller declines a line it does not understand in preference to passing it through.

Every router program the test suite generates for a Grbl-family controller, including holding tabs, ramp and helix entries, peck drilling and variable-depth paths, is accepted by the leveller.

## Without Studio

The `kerfkite-cam` tool carries the same three steps, with JSON in and out:

    kerfkite-cam probe-plan request.json      {"bounds":[x0,y0,x1,y1],"width":300,"height":200,"spacing":10,"zero":true}
    kerfkite-cam height-map request.json      {"grid":...,"order":...,"reports":["[PRB:8.000,8.000,-41.213:1]",...]}
    kerfkite-cam level request.json           {"lines":[...],"height_map":{...}}

`probe-plan` answers with the program lines, the points in probing order and the grid. `height-map` takes the grid and order from that answer with either the controller's `[PRB:...]` reports or plain Z heights. `level` answers with the levelled lines. A request the tool rejects exits with status 2 and the reason on standard error.

## Simulator

The simulator has a stock surface with a gentle tilt and a slight bow, so that probing and levelling can be rehearsed without a machine. It stops a probe on that surface, writes a `[PRB:...]` report in machine coordinates, tracks `G10 L20 P1`, and raises `ALARM:5` when a probe ends above the surface.

## Not yet verified on a machine

Levelling is tested against the simulator and by calculation: `tests/test_leveling.py` asks `kerfkite-cam` for plans, height maps and levelled programs and compares them with a bilinear surface worked out separately in the test. It has not yet been run on a physical controller. The first run on a machine should be supervised, with a probe that has been tested, a low probe feed and a hand on the reset.

## Circuit boards

Probe levelling is what circuit board engraving rests on. A board's Gerber layers and Excellon drill file are imported under **Projects**, **Circuit board**, which adds the isolation contours, the holes and the cut-out to the design as stages, one for each tool; the isolation stage is then previewed, probed and run as described above. See [Circuit boards](PCB.md).
