# Developer handoff

KerfKite Studio combines a browser-based design workspace, a Python CAD/CAM service and a separate machine sender. The Raspberry Pi runs the same geometry and CAM code as the desktop service. The native Android companion is a smaller editor and client with its own build.

## Set up a simulator checkout

Use 64-bit Python 3.12 and a C++ compiler for the optional accelerated relief extension. Install Node.js 22 for the JavaScript checks. Create a virtual environment, activate it, then run these commands from the repository root. STEP exchange needs a platform with a compatible Open CASCADE wheel.

```sh
python -m pip install -e ".[web,step,mcp]"
python -m pip install markdown-it-py playwright
python -m playwright install chromium
python -m studio.server --port 8765
```

Open the printed loopback address. Keep the server's default simulator setting. For the filesystem-only editor open `studio/static/index.html` directly. Its supported CAM subset runs in JavaScript. STEP, assembly solving, fixture checks, bundle inspection and DXF parsing use the Python service.

## How a design becomes a cut

| Responsibility | Entry points | Change contract |
| --- | --- | --- |
| Project versioning, named documents, recovery | `studio/static/app.js`, `projects.js`, `design-layers.js` | A portable `pi-cnc-studio/1` file contains editable objects, settings, metadata and extras. Design layer visibility and output are independent. CAM uses millimetres, feed in mm/min, Z zero at the stock surface and negative cutting depths. |
| Solid features, sketches, surfaces and assemblies | `studio/solid_cad.py`, `cad_exchange.py`, `assembly.py`; their browser modules | Keep input parameters and source geometry in project extras. Meshes and solved poses are derived. STEP files use Open CASCADE; the editable feature history stays in the Studio project. |
| Import and migration | `studio/static/interchange.js`, `lightburn-import.js`, `studio/program_import.py`, `import_bundle.py` | Stage a report with objects, layers, devices, presets, warnings and head mode. Put each object's source layer key in `sourceLayer`. Preview first, apply in one edit and keep new layer output disabled for operator review. |
| CAM and simulation | `studio/cam.py`, `operations.py`, `relief.py`, `fixture_collision.py`, `simulation.py`, `studio/static/offline-cam.js` | Derive preview and postprocessed output from the same document. Test the posted instructions as well as the displayed paths. Validate footprint, units, motion dialect and work frame explicitly. |
| HTTP services and machine transport | `studio/design_api.py`, `server.py`, `web_server.py`, `device.py`, `protocols.py` | Register a design route once for both authenticated hosted and local services. A design route has no machine access. Keep preview, phone arming, transport and direct machine commands in their existing boundaries. |
| Other clients | `studio/mcp_server.py`, `android/`, `studio/static/monitor.js` | Test the client that actually consumes a change. Desktop solid modelling is distinct from the Android companion's supported operations. |

Browser scripts are loaded as classic scripts from `studio/static/index.html`. Several editor modules extend common functions in load order. Features that reference the tab panels mount after `tool-tabs.js`. When a project opens or resets, listen for `cnc-project-loaded` to refresh controls. Notify project persistence through `cnc-project-changed` for edits stored in extras.

## Run the checks

```sh
python scripts/simulate_studio.py --all --require-step --json simulation.json
python validation/simulate_solid_workspace.py
python scripts/build_help.py
bash scripts/ci-local.sh --list
```

Use `--browser PATH` to select an installed Chromium executable. The browser harness starts its own simulator-only service and temporary browser profiles. The JSON report includes each scenario's measurements and skipped checks. `--require-step` makes a missing STEP runtime a failure. The repository also has OS-specific local checks in `scripts/ci-local.sh` and Android build instructions in [Android](ANDROID.md). Write diagnostic reports outside the source tree when they contain local paths.

## Example change

To extend DXF import, add a small, independently authored fixture to `tests/test_program_import.py`. Decide how unsupported entities, XY projection and unit conversion should appear in the review. Implement the reader in `studio/program_import.py`. Verify its numerical result, handle a malformed variant, and test review → enable layer → CAM → project reopen in `validation/import_fixture_workflow.py`. Add the supported entity and limits to [Program import](PROGRAM-IMPORT.md). Importing artwork never grants permission to send a program to a device.

For future work, use the acceptance matrix and the specific workflow guides. Physical cutting, lens distortion calibration, general constrained sketch and assembly solving, additional native vendor formats, and platform-specific binary redistribution each require their own validation.

The Android and MCP clients retain their legacy 680 × 240 mm example canvas for project compatibility. These dimensions describe an editing canvas. Configure each machine using its own measured travel and origin.

The paired phone listener is managed by `studio/phone_monitor_service.py`. Its authenticated API is in `studio/monitor.py`. The native client and activity are in the Android Pocket package. `tests/test_phone_pendant.py` exercises action bounds and revocation. `validation/android_pendant_workflow.py` drives an emulator against the simulator.

For sketch constraints, start with `studio/sketch_constraints.py` and `studio/sketch_profile.py`. Supplementary assembly mates are solved in `studio/assembly_constraints.py`. The browser tracks linked CAD geometry in `studio/static/solid-links.js`. The browser harness exercises crossing lines, linked contours and reliefs, and parameter-driven sketches.

## Installer releases

The [desktop packaging guide](INSTALLER-BUILDS.md) covers the release builds, runtime locks, Android signing and clean-install checks.
