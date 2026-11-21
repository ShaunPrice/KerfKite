# Machine setup from the controller

When a controller is connected for the first time, Studio asks it what machine it is and sets the workspace to match. Nothing is written to the controller: its settings are read and left as they are.

This is the same on Windows, macOS, Linux and the Raspberry Pi, and through the MCP workshop, which drives the same editor. The hosted web editor has no controller connection, and the simulator has no machine settings, so neither changes the workspace.

## What is asked

| Firmware | Requests | What is read |
| --- | --- | --- |
| Grbl 1.1, grblHAL, FluidNC, xPRO V5 | `$$`, `$I` and the start-up line | Travel (`$130`–`$132`), maximum rates (`$110`–`$112`), maximum spindle speed or laser power (`$30`, or `Laser/FullPower` on an xPRO V5 in laser mode), laser mode (`$32`), soft and hard limits (`$20`, `$21`), homing (`$22`), reporting units (`$13`), firmware name and version |
| Marlin | `M115`, `M211`, `M503` | Firmware name and machine type, soft endstop limits, maximum feed rates (`M203`) |

A Marlin firmware that does not answer one of these reports still connects: the report is skipped after three seconds and the item is listed as not reported.

## What is set

| Workspace setting | From |
| --- | --- |
| Work area width and height | X and Y travel |
| Z travel of the machine profile | Z travel |
| Largest feed and plunge feed the fields accept | The slower of the X and Y maximum rates, and the Z maximum rate, up to Studio's own limit of 10,000 mm/min |
| Largest spindle speed or laser power the field accepts | `$30` |
| Router or laser head | `$32`, when it differs from the head selected |

A value already above a new limit is lowered to it, and the message says so.

The machine is saved under **My machine profiles** with a name made from its firmware and work area, for example `Grbl 1.1h · 300 × 180 mm`. On a later connection the same machine is recognised by its firmware and travel. A project already set up for that machine is left as it is, so a work area narrowed for one job is not undone; a project set up for a different machine is switched to the connected one.

**Set up from controller**, under **My machine profiles**, repeats the reading at any time while a controller is connected.

## What is not guessed

- A setting the controller does not report is listed as not reported and left for you to enter.
- Travel beyond 2,000 mm, the largest work area Studio accepts, is reported and not applied.
- If the firmware identifies itself as a different dialect from the one chosen for the connection, the message says so. The workspace keeps the dialect of the connection; reconnect with the matching dialect.
- If artwork already lies outside the reported work area, the work area is not changed and the message says why.
- Firmware travel settings are often left at their defaults. Check the reported travel against the machine before relying on it.

## Not included

The phone pendant's status deliberately carries no machine configuration, so KerfKite Pocket does not receive these settings. Programs can read the description from `GET /api/machine` on the workstation.

The readback has been tested with synthetic controllers that answer as each firmware does. It has not yet been run against physical controllers.
