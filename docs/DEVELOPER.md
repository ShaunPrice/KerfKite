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

Open the printed loopback address. Keep the server's default simulator setting. The C++ workstation server serves the same editor and routes without Python: build the native core (`native/README.md`) and run `build/native/kerfkite-studio --static studio/static`; it has no STEP exchange yet. For the filesystem-only editor open `studio/static/index.html` directly. Its supported CAM subset runs in JavaScript. STEP, assembly solving, fixture checks, bundle inspection and DXF parsing use the Python service.

## How a design becomes a cut

| Responsibility | Entry points | Change contract |
| --- | --- | --- |
| Project versioning, named documents, recovery | `studio/static/app.js`, `projects.js`, `design-layers.js` | A portable `pi-cnc-studio/1` file contains editable objects, settings, metadata and extras. Design layer visibility and output are independent. CAM uses millimetres, feed in mm/min, Z zero at the stock surface and negative cutting depths. |
| Solid features, sketches, surfaces and assemblies | `studio/solid_cad.py`, `cad_exchange.py`, `assembly.py`; `studio/solid_kernel.py` for the kernel they model with; their browser modules | Keep input parameters and source geometry in project extras. Meshes and solved poses are derived. STEP files use Open CASCADE; the editable feature history stays in the Studio project. Solids and script drawings are modelled by KerfKite's own kernels through the compiled extension, and by manifold3d without it; `tests/test_solid_backends.py` compares the two, so run it after a change to any of these modules. Surface meshes come from the core's port of `surface_mesh` (`native/core/src/surface.cpp`); change both and run `validation/surface_mesh_check.py`. |
| Import and migration | `studio/static/interchange.js`, `lightburn-import.js`, `studio/program_import.py`, `import_bundle.py` | Stage a report with objects, layers, devices, presets, warnings and head mode. Put each object's source layer key in `sourceLayer`. Preview first, apply in one edit and keep new layer output disabled for operator review. |
| CAM and simulation | `studio/cam.py`, `operations.py`, `relief.py`, `fixture_collision.py`, `simulation.py`, `studio/static/offline-cam.js` | Derive preview and postprocessed output from the same document. Test the posted instructions as well as the displayed paths. Validate footprint, units, motion dialect and work frame explicitly. |
| HTTP services and machine transport | `studio/design_api.py`, `server.py`, `web_server.py`, `device.py`, `protocols.py` | Register a design route once for both authenticated hosted and local services. A design route has no machine access. Keep preview, phone arming, transport and direct machine commands in their existing boundaries. |
| Other clients | `studio/mcp_server.py`, `android/`, `studio/static/monitor.js` | Test the client that actually consumes a change. Desktop solid modelling is distinct from the Android companion's supported operations. |

Browser scripts are loaded as classic scripts from `studio/static/index.html`. Several editor modules extend common functions in load order. Features that reference the tab panels mount after `tool-tabs.js`. When a project opens or resets, listen for `cnc-project-loaded` to refresh controls. Notify project persistence through `cnc-project-changed` for edits stored in extras.


Large stock previews POST to the authenticated `/api/simulate` design route. A session advertises
`stock_wire_v1` when it accepts the little-endian `application/vnd.kerfkite.stock-v1` layout in
`studio/stock_wire.py` and `studio/static/stock-wire.js`. The header and its segment count bound
the request before the grid calculation. JSON clients use the same parameter validation and result
shape. The file editor runs its existing JavaScript simulator locally.

## Run the checks

```sh
python scripts/simulate_studio.py --all --require-step --json simulation.json
python validation/simulate_solid_workspace.py
python scripts/build_help.py
bash scripts/ci-local.sh --list
```

Use `--browser PATH` to select an installed Chromium executable. The browser harness starts its own simulator-only service and temporary browser profiles. The JSON report includes each scenario's measurements and skipped checks. `--require-step` makes a missing STEP runtime a failure. The repository also has OS-specific local checks in `scripts/ci-local.sh` and Android build instructions in [Android](ANDROID.md). Write diagnostic reports outside the source tree when they contain local paths.

## Example change

To extend DXF import, add a small, independently authored fixture to `tests/test_program_import.py`. Decide how unsupported entities, XY projection and unit conversion should appear in the review. Implement the reader in `studio/program_import.py`. Verify its numerical result, handle a malformed variant, and test review → enable layer → CAM → project reopen in `validation/import_fixture_workflow.py`. Add the supported entity and limits to [Program import](PROGRAM-IMPORT.md). G-code, HP-GL and LinuxCNC INI files are answered by the native core's port of the reader (`native/core/src/program_import.cpp`), with the Python as the reference: a change to those readers is made in both, and `validation/native_core_parity.py --program-import` compares them. DXF is read by the core's own reader (`native/core/src/dxf.cpp`); extend it there and in `_dxf`, and run `validation/dxf_import_check.py`, which compares the two to the tolerance. PDF pages are read by the core's port of `studio/pdf_import.py` (`native/core/src/pdf_import.cpp`): a change to one is made in both, and `validation/native_core_parity.py --pdf-import` compares them. LightBurn bundles are read by the core's port of `studio/import_bundle.py` (`native/core/src/bundle_import.cpp`), compared by `--bundle-import`. Importing artwork never grants permission to send a program to a device.

For future work, use the acceptance matrix and the specific workflow guides. Physical cutting, measured camera lens accuracy on the stock plane, arbitrary curve constraints and general assembly solving, additional native vendor formats, and platform-specific binary redistribution each require their own validation.

The Android and MCP clients retain their legacy 680 × 240 mm example canvas for project compatibility. These dimensions describe an editing canvas. Configure each machine using its own measured travel and origin.

The paired phone listener is managed by `studio/phone_monitor_service.py`. Its authenticated API is in `studio/monitor.py`. The native client and activity are in the Android Pocket package. `tests/test_phone_pendant.py` exercises action bounds and revocation. `validation/android_pendant_workflow.py` drives an emulator against the simulator.

For sketch constraints, start with `studio/sketch_constraints.py` and `studio/sketch_profile.py`. With the compiled extension the constraints are solved by the native core's port of the solver (`solve_sketch` in `native/core/src/solid_cad.cpp`), so a change to the solver is made in both and compared with `validation/sketch_constraint_check.py`. Supplementary assembly mates are solved in `studio/assembly_constraints.py`; with the compiled extension assemblies are solved by the native core's port (`native/core/src/assembly.cpp`), so a change is made in both and compared with `validation/assembly_check.py`. The browser tracks linked CAD geometry in `studio/static/solid-links.js`. The browser harness exercises crossing lines, linked contours and reliefs, and parameter-driven sketches.

## Workshop extensions

| Workflow | Source | End-to-end check |
| --- | --- | --- |
| Curved parametric sketches and two-section lofts | `studio/sketch_curves.py`, `studio/solid_loft.py`, `studio/solid_cad.py`, `studio/cad_exchange.py` | `validation/sketch_join_workflow.py` and `validation/loft_workflow.py` |
| Vector SVG and PDF input | `studio/static/svg-import.js`, `studio/pdf_import.py` | `validation/svg_import_workflow.py` and `validation/pdf_import_workflow.py` |
| Circuit boards: Gerber and Excellon import, isolation toolpaths, the board panel | `native/core/src/gerber.cpp`, `excellon.cpp`, `isolation.cpp`, `studio/pcb_import.py`, `studio/pcb_api.py`, `studio/static/pcb-board.js`, `pcb-import.js` | `validation/pcb_workflow.py` |
| Router entry, drilling and tab placement | `studio/router_entry.py`, `studio/drilling.py`, `studio/static/tab-placement.js` | `validation/router_entry_workflow.py`, `validation/drilling_workflow.py`, `validation/placement_workflow.py` |
| Labels, grouping, machine profiles and calibrated quotes | `studio/static/production-labels.js`, `placement.js`, `machine-library.js`, `cycle-time.js` | `validation/production_label_workflow.py` |
| Native Android project storage and interchange | `DraftLibrary.java`, `ProjectFile.java` in `android/app/src/main/java/au/cncstudio/pocket/` | `validation/android_projects_workflow.py` |

The desktop and hosted editors share the Python CAM service. When adding CAM algorithms, exercise the offline JavaScript implementation on the same inputs or report the server requirement before generating a file. Comparing emitted tool positions, power, depths and stock removal catches regressions that a successful HTTP response would miss.

## Installer releases

The [desktop packaging guide](INSTALLER-BUILDS.md) covers the release builds, runtime locks, Android signing and clean-install checks.


## Native desktop workbench

The Go launcher in `packaging/desktop/` owns the local Studio service and starts a platform window after setup. The native file and geometry controls use the document adapter in `studio/static/desktop-workspace.js` in the main editor frame. The adapter accepts no filesystem path or machine command. A host supplies a file's contents to open it and writes a returned artifact only after the operator chooses a destination. The adapter marks the offered document snapshot saved after the host confirms a successful write. An edit made during the save still counts as unsaved. The native host associates an opened file with the editor's document ID.

New commands that edit documents should reuse the editor model and its existing validation, history and preview invalidation. Extend `validation/desktop_workflow.py` for shared behavior and the smoke exercise under the affected platform's source directory for host behavior. The installer validation runs both the service and native window in simulation. [Installer build guide](INSTALLER-BUILDS.md)

## CAD command line and script geometry

`studio/static/cad-commands.js` handles typed commands, source preview and applying changes to the drawing or solid feature tree. `cad-views.js` routes viewport shortcuts. The local and hosted services share the `/api/cad-script` design route. `studio/scad_language.py` expands the bounded language into data nodes, and `studio/scad_geometry.py` builds the kernels' drawings or solids from those nodes. With the compiled extension, the route and scripted features are answered by the native core's port of both (`native/core/src/scad.cpp`), and the Python stays as the reference: a change to the language or its geometry is made in both, and `validation/native_core_parity.py --cad-script` compares them. Keep source and numeric parameter ownership intact through project exchange. Test representative scripts against the parser and geometry kernel, then exercise view and command entry through the editor. [CAD scripting tutorial and supported syntax](CAD-SCRIPTING.md)

## Assistant workshop, macros and skills

`studio/mcp_server.py` assembles the FastMCP tools. `studio/mcp_workshop.py` starts the assistant's isolated Chromium workshop over a simulator-only Flask server, registers stateless design actions and implements artifact transfer. `studio/static/mcp-controls.js` discovers the actual controls. Extend the standard editor and shared design route first when a new feature needs to work in the GUI and over MCP. Keep operator-paired status and actions in `studio/mcp_pendant.py`. `studio/mcp_transport.py` owns the authenticated remote transport policy. Use an independent output directory per assistant deployment.

The saved macro schema, import collision rules, name validation and defaults live in `studio/static/cad-macros.js`. Commands and source application run through `cad-commands.js`. Preserve the per-instance numeric overrides on project save/open. Keep the assistant tool, interactive authoring guide and bundled Help in sync when changing a macro. The browser regression `validation/cad_macro_workflow.py` builds every starter on a fresh project. It also measures parametric solids, checks that a bad command leaves artwork and camera intact, and tests library reimport. [Macro authoring guide](CAD-MACROS.md)

Assistant workflow instructions are in `skills/kerfkite-workshop/SKILL.md`. The release ZIP should contain `kerfkite-workshop/SKILL.md` inside the correspondingly named top-level folder. After changing a workflow, compare the skill examples with the server's advertised input schema, validate any changed frontmatter using the Agent Skills validator available in your development environment, and run the example against a fresh simulated editor. Check the resulting artifact content or measured geometry. Restarting the local connection and repeating a named tool call is a separate test from running the MCP SDK harness. [Tool contract and client instructions](MCP.md) · [Remote hosting](MCP-HOSTING.md)

## CAM, mesh and editor performance

Desktop installers include the native relief and stock module for their platform. A source install uses the Python implementation when its optional C++ build is unavailable. The filesystem-only editor runs its browser CAM implementation. To compare the reference and native service on identical data, run `python -m validation.benchmark_native /tmp/kerfkite-cam.json` from a checkout with the extension built. The benchmark measures each backend in a fresh process, checks the complete stock grid and hashes the planar and rotary output.

Generated fourth-axis jobs keep repeating moves in packed double-precision buffers until the postprocessor emits text. Keep the raw-text path for external callers and compare complete output when changing rounding, modal feeds, variable depth, laser power or rotary seam handling. STL export uses the kernel mesh directly and computes facet normals in batches. The interactive mesh response still supports JSON clients.

The editor keeps editable inputs in its undo history and reconstructs the toolpath after restoration. The project library indexes search metadata separately from document and revision contents. The solid viewport reuses indexed WebGL geometry during camera changes and has a Canvas renderer for systems without suitable acceleration. Run the browser harness to exercise undo, library migration, drag cancellation and mesh rendering. Use `python validation/benchmark_viewport.py --browser PATH` to compare frame times for the same part and camera motions on the local GPU.
