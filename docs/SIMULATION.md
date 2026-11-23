# Simulation and acceptance

From a source checkout, build the native programs as the [installation guide](INSTALL.md) describes. Run the harness from the project root with Python 3.10 or later; the harness itself needs no Python packages. The extended run also requires Node.js with its built-in test runner and the dependencies used by the web tests.

```sh
python scripts/simulate_studio.py --all --json simulation-result.json
```

For a quick CAD and machining check, omit `--all`. The harness starts the workstation server, `kerfkite-studio`, on a free loopback port in simulation and connects its controller to the built-in GRBL simulator. It takes the server from `--server PATH`, the `KERFKITE_STUDIO` environment variable or `build/native`, in that order, and needs no machine. A nonzero exit status identifies a failed check. The JSON report preserves each case, results, errors, skipped dependencies and areas awaiting other forms of acceptance.

## What the fixture checks

1. Build a 3 mm plate with a named width, a height constrained to half the width, and a rectangular through-slot. Rebuild at 32 mm and 40 mm width. Compare both solid volumes and projected areas to the analytic plate-minus-slot values. Confirm that the top projection has an inner and outer contour and that the binary STL contains the displayed triangle count.
2. Generate an outside router profile with a 2 mm flat cutter, two depth levels and 4 mm rapid clearance. Replay the posted linear moves and compare every XY cutting segment and ending depth with the preview at the precision of the posted coordinates. Check travel bounds, retracts and the final power state. Sample stock removal and confirm that the job reaches the 3 mm programmed depth and retains stock elsewhere.
3. Use the same projected contours to generate a laser line pass. Check that every rapid has the beam off, cuts stay in the programmed range and the program ends with output off. Generate a drag-knife contour with blade offset and lead. Check work-area bounds, clearance, blade depth and absence of spindle or laser activation.
4. Submit each job through the workstation Run endpoint. Hold, observe stable progress, resume and wait for reported completion at 100% in the Idle state. The simulated driver models job scheduling. Position and process checks come from the separate replay of the emitted instructions.

## Reading extended results

`--all` additionally discovers Python tests in `tests/`, JavaScript tests in `tests/` and the business and camera tests in `validation/`. These exercise the existing controller families and postprocessors, relief and stock models, tool changes, rotary, holding tabs, phone controls, hosted web app, project editing and artwork library. The report includes counts of skipped tests, with reasons for Python skips such as the optional native kernel. A missing dependency fails the affected suite. Other independent suites still run. The exit code reflects failures. Consult nested skip counts when deciding whether the tested configuration matches an intended installation.

The motion interpreter covers the absolute millimetre linear GRBL subset emitted by this fixture. The report records GUI interaction, physical commissioning and execution by external controller software as outside this simulation. Material cutting quality, machine stopping distance, fixture clearance and real tool changes require their own acceptance records.

## Advanced CAD and file workflows

The harness also checks a mated shaft offset into its bore wall, a holder strike during a posted rapid, relocation of the clamp and a plotter-to-knife conversion. With the STEP add-on, `kerfkite-step`, beside the server, it adds a real open surface and cylinder STEP round trip; `python scripts/simulate_studio.py --all --require-step` makes a missing add-on a failure. The separate browser harness in `validation/simulate_solid_workspace.py` exercises editing, saving, reopening, image registration and file import through the actual controls. Install Playwright first. The simulated fixture positions and tools are numerical test inputs.

## Workshop and Pocket checks

Run `python validation/simulate_solid_workspace.py` with Playwright and Chromium installed. Isolated browser contexts exercise sketch and loft rebuilds, registered camera tracing, PDF and compound SVG import, grouped artwork, serialised labels, tab placement, peck drilling, router entry, laser patterns and the project round trip. The disconnected editor uses the same test inputs for the operations it supports.

The Android project and pendant checks run against an emulator with an installed APK. See [Android setup](ANDROID.md) for signing and installation. Saved work from an earlier APK and a desktop source project are separate compatibility cases. A visually successful import alone cannot establish a lossless reference-file round trip.
