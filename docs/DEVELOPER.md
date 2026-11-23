# Developer handoff

KerfKite Studio combines a browser-based design workspace, native C++ programs for CAD, CAM and machine control, and a separate machine sender. The workstation server `kerfkite-studio` serves the editor and every design and machine route; the HTTPS editor `kerfkite-web` serves the same editor to accounts without a controller; the MCP assistant server `kerfkite-mcp` drives it for an assistant. The Raspberry Pi runs the same programs as the desktop. The native Android companion is a smaller editor and client with its own build. No Python is part of the programs or the packages: Python remains only in the development tools (the validation harnesses, the help and template builders) and uses the standard library unless a harness says otherwise.

## Set up a simulator checkout

Install CMake 3.16 or newer, a C++17 compiler and Go 1.24 for the phone pendant's TLS relay. For STEP exchange install Open CASCADE 7.6 or later (on Debian and Ubuntu `libocct-data-exchange-dev` and `libocct-modeling-algorithms-dev`); CMake then builds the add-on `kerfkite-step` as well. Install Node.js 22 for the JavaScript checks and Python 3.10 or later for the harnesses. Build and run from the repository root:

```sh
cmake -S native -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
build/native/kerfkite-studio --static studio/static
```

Open the printed loopback address. Keep the server's default simulator setting: controller ports stay closed unless it is started with `--enable-hardware`. [The native programs](../native/README.md) describes each program and its options. For the filesystem-only editor open `studio/static/index.html` directly. Its supported CAM subset runs in JavaScript.

## How a design becomes a cut

| Responsibility | Entry points | Change contract |
| --- | --- | --- |
| Project versioning, named documents, recovery | `studio/static/app.js`, `projects.js`, `design-layers.js` | A portable `pi-cnc-studio/1` file contains editable objects, settings, metadata and extras. Design layer visibility and output are independent. CAM uses millimetres, feed in mm/min, Z zero at the stock surface and negative cutting depths. |
| Solid features, sketches, surfaces and assemblies | `native/core/src/solid_cad.cpp`, `surface.cpp`, `assembly.cpp`, `scad.cpp`; the solid kernel in `native/core/src/solid/` and the polygon kernel in `native/core/src/polygon/`; their browser modules | Keep input parameters and source geometry in project extras. Meshes and solved poses are derived. STEP files go through the Open CASCADE add-on (`native/step/`); the editable feature history stays in the Studio project. The kernels are KerfKite's own: see [the solid kernel](SOLID-KERNEL.md) and [the polygon kernel](POLYGON-KERNEL.md). |
| Import and migration | `studio/static/interchange.js`, `lightburn-import.js`, `native/core/src/program_import.cpp`, `dxf.cpp`, `pdf_import.cpp`, `bundle_import.cpp` | Stage a report with objects, layers, devices, presets, warnings and head mode. Put each object's source layer key in `sourceLayer`. Preview first, apply in one edit and keep new layer output disabled for operator review. |
| CAM and simulation | `native/core/src/cam.cpp`, `operations.cpp`, `relief.cpp`, `fixture_collision.cpp`, `stock.cpp`, `rotary.cpp`, `studio/static/offline-cam.js` | Derive preview and postprocessed output from the same document. Test the posted instructions as well as the displayed paths. Validate footprint, units, motion dialect and work frame explicitly. |
| HTTP services and machine transport | `native/server/studio.cpp`, `design_routes.cpp`, `design_extra.cpp`, `machine_routes.cpp`, `machine.cpp`, `serial_port.cpp`, `protocols.cpp`; `web_app.cpp` and `web_store.cpp` for the hosted editor | Register a design route once for both the local and the hosted editor. A design route has no machine access. Keep preview, phone arming, transport and direct machine commands in their existing boundaries. |
| Other clients | `native/server/mcp_tools.cpp`, `workshop.cpp`, `remote_tools.cpp`, `android/`, `studio/static/monitor.js` | Test the client that actually consumes a change. Desktop solid modelling is distinct from the Android companion's supported operations. |

Browser scripts are loaded as classic scripts from `studio/static/index.html`. Several editor modules extend common functions in load order. Features that reference the tab panels mount after `tool-tabs.js`. When a project opens or resets, listen for `cnc-project-loaded` to refresh controls. Notify project persistence through `cnc-project-changed` for edits stored in extras.

Large stock previews POST to the authenticated `/api/simulate` design route. A session advertises
`stock_wire_v1` when it accepts the little-endian `application/vnd.kerfkite.stock-v1` layout that
`studio/static/stock-wire.js` writes and the server reads. The header and its segment count bound
the request before the grid calculation. JSON clients use the same parameter validation and result
shape. The file editor runs its existing JavaScript simulator locally.

## Run the checks

```sh
python scripts/simulate_studio.py --all --require-step --json simulation.json
python validation/simulate_solid_workspace.py
python validation/check_pinned_results.py
python scripts/build_help.py
bash scripts/ci-local.sh --list
```

The fixture and the browser harness start `kerfkite-studio` from `build/native` (or `--server PATH`, or the `KERFKITE_STUDIO` environment variable) on a free loopback port in simulation, with temporary browser profiles. Use `--browser PATH` to select an installed Chromium executable. The JSON report includes each scenario's measurements and skipped checks. `--require-step` makes a missing STEP add-on a failure. `check_pinned_results.py` compares the core's results on this platform with the values pinned for every platform it is built for. The repository also has OS-specific local checks in `scripts/ci-local.sh` and Android build instructions in [Android](ANDROID.md). Write diagnostic reports outside the source tree when they contain local paths.

### The Python reference

Until Studio 0.7 the programs were ported from a Python implementation, which stayed in the tree as their reference. It was removed in 0.7, with its tests and the harnesses that compared the two. They are kept at the last commit that had them, which `.github/workflows/native-core.yml` names as `REFERENCE`. Continuous integration checks that commit out beside the change and runs its comparisons against the programs built from the change: the core's CAM, imports and kernels, the workstation server's and the hosted editor's routes, the assistant server's tools over both transports, and STEP exchange. A change that alters behaviour the programs share with the reference therefore shows up as a difference. When the change is intended, say so in the pull request. To run a comparison yourself:

```sh
git worktree add ../kerfkite-reference REFERENCE
python -m pip install -e "../kerfkite-reference[dev,step]"
python ../kerfkite-reference/validation/server_route_parity.py --server build/native/kerfkite-studio
```

Replace `REFERENCE` with the commit the workflow names. [The native core](NATIVE.md) describes each comparison and the rules by which the programs differ from the reference on purpose.

## Example change

To extend DXF import, add a small, independently authored fixture to the core's self-test (`native/tests/selftest.cpp`). Decide how unsupported entities, XY projection and unit conversion should appear in the review. Implement the reader in `native/core/src/dxf.cpp`. Verify its numerical result, handle a malformed variant, and test review → enable layer → CAM → project reopen in `validation/import_fixture_workflow.py`. Add the supported entity and limits to [Program import](PROGRAM-IMPORT.md). G-code, HP-GL and LinuxCNC INI files are read by `native/core/src/program_import.cpp`, PDF pages by `pdf_import.cpp` and LightBurn bundles by `bundle_import.cpp`. Importing artwork never grants permission to send a program to a device.

For future work, use the acceptance matrix and the specific workflow guides. Physical cutting, measured camera lens accuracy on the stock plane, arbitrary curve constraints and general assembly solving, additional native vendor formats, and platform-specific binary redistribution each require their own validation.

The Android and MCP clients retain their legacy 680 × 240 mm example canvas for project compatibility. These dimensions describe an editing canvas. Configure each machine using its own measured travel and origin.

The paired phone listener is `native/server/phone_monitor.cpp`, with the arming and Start rules in `phone_jobs.cpp`; on the LAN its TLS is the Go relay in `packaging/pendant-tls`. The native client and activity are in the Android Pocket package. The server's self-test exercises action bounds and revocation. `validation/android_pendant_workflow.py` drives an emulator against the simulator, and `validation/phone_arm_workflow.py` the browser monitor and the workstation's confirmation.

For sketch constraints, start with `solve_sketch` in `native/core/src/solid_cad.cpp`; assembly mates are solved in `native/core/src/assembly.cpp`. The browser tracks linked CAD geometry in `studio/static/solid-links.js`. The browser harness exercises crossing lines, linked contours and reliefs, and parameter-driven sketches.

## Workshop extensions

| Workflow | Source | End-to-end check |
| --- | --- | --- |
| Curved parametric sketches and two-section lofts | `native/core/src/solid_cad.cpp`, `surface.cpp`, the solid kernel | `validation/sketch_join_workflow.py` and `validation/loft_workflow.py` |
| Vector SVG and PDF input | `studio/static/svg-import.js`, `native/core/src/pdf_import.cpp` | `validation/svg_import_workflow.py` and `validation/pdf_import_workflow.py` |
| Circuit boards: Gerber and Excellon import, isolation toolpaths, the board panel | `native/core/src/gerber.cpp`, `excellon.cpp`, `isolation.cpp`, `pcb.cpp`, `studio/static/pcb-board.js`, `pcb-import.js` | `validation/pcb_workflow.py` |
| Router entry, drilling and tab placement | `native/core/src/router_entry.cpp`, `pocket_entry.cpp`, `drilling.cpp`, `tabs.cpp`, `studio/static/tab-placement.js` | `validation/router_entry_workflow.py`, `validation/drilling_workflow.py`, `validation/placement_workflow.py` |
| Labels, grouping, machine profiles and calibrated quotes | `studio/static/production-labels.js`, `placement.js`, `machine-library.js`, `cycle-time.js` | `validation/production_label_workflow.py` |
| Native Android project storage and interchange | `DraftLibrary.java`, `ProjectFile.java` in `android/app/src/main/java/au/cncstudio/pocket/` | `validation/android_projects_workflow.py` |

The desktop and hosted editors share the native CAM. When adding CAM algorithms, exercise the offline JavaScript implementation on the same inputs or report the server requirement before generating a file. Comparing emitted tool positions, power, depths and stock removal catches regressions that a successful HTTP response would miss.

## Installer releases

The [desktop packaging guide](INSTALLER-BUILDS.md) covers the release builds, Android signing and clean-install checks.

## Native desktop workbench

The Go launcher in `packaging/desktop/` starts the workstation server beside it and a platform window after setup. The native file and geometry controls use the document adapter in `studio/static/desktop-workspace.js` in the main editor frame. The adapter accepts no filesystem path or machine command. A host supplies a file's contents to open it and writes a returned artifact only after the operator chooses a destination. The adapter marks the offered document snapshot saved after the host confirms a successful write. An edit made during the save still counts as unsaved. The native host associates an opened file with the editor's document ID.

New commands that edit documents should reuse the editor model and its existing validation, history and preview invalidation. Extend `validation/desktop_workflow.py` for shared behavior and the smoke exercise under the affected platform's source directory for host behavior. The installer validation runs both the server and native window in simulation. [Installer build guide](INSTALLER-BUILDS.md)

## CAD command line and script geometry

`studio/static/cad-commands.js` handles typed commands, source preview and applying changes to the drawing or solid feature tree. `cad-views.js` routes viewport shortcuts. The local and hosted editors share the `/api/cad-script` design route, which `native/core/src/scad.cpp` answers: it expands the bounded language into nodes and builds the kernels' drawings or solids from them, and scripted features in a part are built the same way. Keep source and numeric parameter ownership intact through project exchange. Test representative scripts against the parser and geometry kernel, then exercise view and command entry through the editor. [CAD scripting tutorial and supported syntax](CAD-SCRIPTING.md)

## Assistant workshop, macros and skills

`native/server/mcp_tools.cpp` declares the assistant's tools and `mcp.cpp` and `mcp_http.cpp` carry them over stdio and over HTTP with OAuth tokens (`oauth.cpp`). `native/server/workshop.cpp` starts the assistant's isolated Chromium workshop over a simulator-only workstation server (the browser is found by `browser.cpp`), registers stateless design actions and implements artifact transfer. `studio/static/mcp-controls.js` discovers the actual controls. Extend the standard editor and shared design route first when a new feature needs to work in the GUI and over MCP. Keep operator-paired status and actions in `native/server/remote_tools.cpp`. Use an independent output directory per assistant deployment.

The saved macro schema, import collision rules, name validation and defaults live in `studio/static/cad-macros.js`. Commands and source application run through `cad-commands.js`. Preserve the per-instance numeric overrides on project save/open. Keep the assistant tool, interactive authoring guide and bundled Help in sync when changing a macro. The browser regression `validation/cad_macro_workflow.py` builds every starter on a fresh project. It also measures parametric solids, checks that a bad command leaves artwork and camera intact, and tests library reimport. [Macro authoring guide](CAD-MACROS.md)

Assistant workflow instructions are in `skills/kerfkite-workshop/SKILL.md`. The release ZIP should contain `kerfkite-workshop/SKILL.md` inside the correspondingly named top-level folder. After changing a workflow, compare the skill examples with the server's advertised input schema, validate any changed frontmatter using the Agent Skills validator available in your development environment, and run the example against a fresh simulated editor. Check the resulting artifact content or measured geometry. `validation/mcp_smoke.py` exercises the tools through the MCP SDK over stdio; restarting the local connection and repeating a named tool call is a separate test. [Tool contract and client instructions](MCP.md) · [Remote hosting](MCP-HOSTING.md)

## CAM, mesh and editor performance

The relief finishing contact runs on the processor's threads and, where a calibration at start finds it worthwhile, on a GPU as well (`kerfkite-cam calibrate` reports the choice). The filesystem-only editor runs its browser CAM implementation. [Performance](PERFORMANCE.md) records the measurements.

Generated fourth-axis jobs keep repeating moves in packed double-precision buffers until the postprocessor emits text. Keep the raw-text path for external callers and compare complete output when changing rounding, modal feeds, variable depth, laser power or rotary seam handling. STL export uses the kernel mesh directly and computes facet normals in batches. The interactive mesh response still supports JSON clients.

The editor keeps editable inputs in its undo history and reconstructs the toolpath after restoration. The project library indexes search metadata separately from document and revision contents. The solid viewport reuses indexed WebGL geometry during camera changes and has a Canvas renderer for systems without suitable acceleration. Run the browser harness to exercise undo, library migration, drag cancellation and mesh rendering. Use `python validation/benchmark_viewport.py --browser PATH` to compare frame times for the same part and camera motions on the local GPU.
