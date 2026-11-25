# Machine setup tools

**Control → Machine setup tools** holds five tools for setting a machine up: finding work zero on a touch plate, reading and changing the controller's settings, calibrating an axis, surfacing the spoilboard and running short macros of your own. The panel appears in the workstation editor, not in the hosted editor or the editor opened from a file.

Every tool that moves the machine or writes to its controller needs the controller connected and idle, asks before it acts, and is refused while a job runs. They have been exercised on the simulator only. The first use of each on a real machine should be supervised, with a hand near the stop.

## Zero on a touch plate

A touch plate is a metal block of known thickness wired to the controller's probe input, with a clip for the cutter. The tool finds the plate by probing and sets work zero from it. It works with the Grbl family (Grbl 1.1, grblHAL, FluidNC, xPRO V5) and with Marlin, which all probe with `G38.2`. A Marlin firmware has that move only when it is built with `G38_PROBE_TARGET`: see [Marlin](#marlin) below.

Test the circuit first: touch the clip to the plate and confirm that the controller's status report shows the probe pin (`Pn:P`). A probe that does not trigger drives the cutter into the plate until the probe travel is used up.

**Z only.** Lay the plate flat on the stock, place the cutter over it within 15 mm, enter the **Plate thickness**, tick the setup check and choose **Probe and set work zero**. The cutter touches the plate quickly, backs off 1 mm, touches again slowly, sets work Z so that the stock surface is zero, and retracts 5 mm.

**X, Y and Z.** A corner plate hangs over the front-left corner of the stock, with faces to the left and in front that stand a known distance outside the stock's edges. Enter that distance as the **Plate wall**, the **Cutter diameter**, and how near the cutter is to the plate's left and front faces (it must be over the plate, **within** the distance given: when unsure, give a larger figure). After Z, the cutter moves out past the left face, goes 4 mm below the plate's top, probes in against the face twice and sets work X; then the same in front for Y. Work zero is then the corner of the stock at its surface, and the cutter ends 10 mm inside the stock on both axes, above the plate. There must be clear space to the left of and in front of the plate for the cutter to go down.

Each probing move has to finish within 10 seconds, because the controller acknowledges it only when the motion ends: a slow feed with a long distance is refused. If a probe ends without contact the controller raises `ALARM:5`, the tool says that zero was not set completely, and the controller needs **Unlock alarm**.

Setting work zero forgets a probed surface ([probe levelling](PROBE-LEVELING.md)) and cancels a preview armed for the phone. Remove the plate and the clip before cutting.

The program, for a corner plate (distances relative, in millimetres):

    G21
    G91
    G38.2 Z-15.000 F150.0     down onto the plate
    G0 Z1.000
    G38.2 Z-2.000 F40.0       again, slowly
    G10 L20 P1 Z15.000        the plate's top is its thickness above the stock
    G0 Z5.000
    G0 X-21.588               out past the left face
    G0 Z-9.000                down beside the plate
    G38.2 X25.000 F150.0      in against the face
    G0 X-1.000
    G38.2 X2.000 F40.0
    G10 L20 P1 X-11.588       the cutter's centre is the wall and its radius outside the stock
    G0 X-2.000
    G0 Z9.000
    G0 X23.588                to 10 mm inside the stock
    ...                       the same for Y
    G90

### Marlin

On Marlin the zeroing program is the same, except that work zero is set by renaming the present position (`G92 Z15.000`) where the Grbl family sets a work offset (`G10 L20 P1 Z15.000`). Marlin has no status report of the probe pin: test the circuit with `M119` from a macro, which lists the probe's state in the log.

Two things differ in what happens when it goes wrong:

- **A firmware without the probing move.** Marlin answers an unknown command and carries on. A zeroing program that carried on would set work zero wherever the cutter happened to be, so the program is stopped at the first line the firmware says it does not know, and nothing is zeroed.
- **A probe that finds nothing.** Marlin reports `Failed to reach target`. The driver treats any error as a fault: the program stops before zero is set and the controller has to be reconnected.

Marlin does not report where a probe touched, so [probe levelling](PROBE-LEVELING.md) remains for the Grbl family.

## Controller settings

**Show settings** lists the numbered settings the controller reported, with the names of the common Grbl ones. Change a value and choose **Write**: after you confirm, the setting is written (`$100=80.125`) and every setting is read back, so the list shows what the controller now holds. **Export settings…** saves them all as a JSON file. Export before changing anything: a wrong value can make the machine move the wrong distance or the wrong way.

Only numbered settings can be written, one at a time. The simulator reports `$30` and `$32` and keeps whatever is written to it.

### Marlin

Marlin keeps these settings under M codes, not numbers. **Show settings** lists what the firmware's `M503` report gave for each of X, Y and Z:

| Setting | Meaning |
|---|---|
| `M92` | Steps per millimetre |
| `M203` | Maximum feed, in millimetres a second |
| `M201` | Maximum acceleration, in millimetres a second squared |

**Write** sends the one value (`M92 X80.125`) and then `M503`, so the list shows what the firmware now holds. Marlin keeps a change only until it is switched off. **Store in the controller · M500** writes the present settings to its memory, after you confirm; nothing is stored until you choose it, so a setting can be tried and abandoned by cycling the power. Other Marlin settings are not offered here.

## Calibrate an axis

1. Mark where the axis is, against a rule or with a dial gauge.
2. Move it a known distance: with the jog buttons, or with a macro such as `G91` / `G0 X100` / `G90` for a longer move.
3. Measure how far it went, choose the **Axis**, and enter the **Commanded** and **Measured** distances. The tool reads the axis's steps per millimetre from the controller's settings and shows the corrected value: steps × commanded ÷ measured.
4. **Write the corrected steps per mm** writes it as a setting: `$100`, `$101` or `$102` on the Grbl family, `M92` on Marlin (where it must then be stored to last). Repeat the move to confirm.

A correction of more than 20% is refused: that is a wrong pulley, microstep setting or measurement, not a calibration.

## Surface the spoilboard

**Add surfacing job to the design** draws passes back and forth over an area, along its longer side, then once round its edge, as one object with a line operation at the depth given. The cutter's centre stays its radius inside the area, so nothing is cut outside it; the corners keep the cutter's radius. The passes are spaced evenly at no more than the **Stepover**, which may not exceed the cutter diameter.

The job uses router mode and the CAM feed, plunge and spindle speed: the [tool library](TOOL-LIBRARY.md) gives starting values for a surfacing bit. Set work zero at the front-left corner of the area and Z zero on its highest point, preview the job, and run it like any other.

## Macros

A macro is a short G-code program of your own, sent to the idle controller when you choose **Run macro** and confirm. Four are provided to start with. Write the G-code one line to a row; a note may follow a semicolon. **Save macro** keeps it under its name, and **Export macros…** and **Import macros…** move the library (`kerfkite-macros/1`, up to 100 macros) between browsers.

A macro holds plain G-code words only: letters, numbers, spaces and `. + -`, at most 100 lines of 78 characters. Controller commands that begin with `$`, such as settings and homing, are refused: use **Controller settings** and the **Home** and **Unlock alarm** buttons. Running a macro forgets a probed surface and cancels a preview armed for the phone, since a macro can move work zero.

## What has been checked

`tests/test_machine_setup.py` runs the routes against the simulator. The simulator's touch plate is at a place the zeroing program is not told; the test follows the program's own moves and the controller's probe reports, works out where work zero ended up, and compares it with the plate's faces, wall and top for several walls and cutters. It also checks a probe that finds nothing, each refusal, a setting written and read back, and what a macro may hold. `tests/js/setup-tools.test.cjs` checks the calibration arithmetic, that a surfacing pass covers its whole area and stays inside it, and the macro rules. `validation/setup_tools_workflow.py` drives the panel in a browser.

The Marlin tests in the same file use a simulated Marlin with the plate in the same place. Marlin reports no contacts, so they go by the position the simulator holds: after a corner plate the cutter is 10 mm inside the stock on both axes and 15 mm above it in the new coordinates, for several walls and cutters; a probe that finds nothing stops the program before any `G92`; a setting is written, read back from `M503` and stored only when asked; and the status the editor reads is unchanged. `validation/setup_tools_workflow.py` drives both controllers in a browser.

None of these tools has been run on a physical controller. Marlin builds differ in what they include and in how they word their reports: the first use on one is the real test of this driver.
