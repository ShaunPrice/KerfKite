# Studio 0.6 workshop acceptance

These checks used simulated controllers. The xPRO capability tests replayed vendor identification, settings and status responses. No serial CNC machine, cut material or rotary fixture was connected.

| Path | What was exercised | How to repeat |
| --- | --- | --- |
| Keyboard CAD and macros | Command entry, viewport navigation, first-party language and geometry, documents and STL round trips, all seven starter macros, overrides, import/export, editing a saved recipe and rejection of an invalid command sequence | Run the browser harness below |
| Rotary router | Continuous angles through 360°, variable radial depth, inverse-time feed, axis mapping, simulator streaming, invalidation and rejection when an incoming or outgoing depth exceeds stock | Run the browser and backend harnesses below |
| Rotary laser | Per-segment power, unpowered travel, cylindrical playback and reviewed job submission. The separate depth-map fixture measured four powered exposures at the centre of a four-slice sample. | Run both harnesses below |
| Desktop clients | Native menu actions, command shortcut and source editing in AppKit and GTK. WinForms app and smoke executable cross-compiled. | Run the desktop installer acceptance workflow for the release tag |
| Assistant tools | Real stdio client discovered controls, created a macro instance, retrieved its source-bearing project and binary STL, read an uploaded artifact, captured a screenshot and exported a PDF. HTTP tests checked OAuth metadata, the resource challenge and token validation. | Run the MCP checks below |
| Optional remote services | Temporary HTTPS workshop accounts, named receiver file transfer, verification of content digest, simulator pendant and local workstation arm challenge | Run the MCP test modules below |

From a checkout with the relevant extras installed and Chromium available:

```sh
python scripts/simulate_studio.py --all --require-step
python validation/simulate_solid_workspace.py --browser /absolute/path/to/chromium
python -m unittest discover -s tests -p 'test_mcp*.py'
python -m validation.mcp_smoke
```

The full desktop browser acceptance run reported 54 passing checks. Eleven simulation groups passed in the kernel harness, including Python and JavaScript regression discovery and STEP exchange. The [MCP protocol receipt](mcp-smoke.json) records the tools exercised over stdio. Specific tool registration in a personal Claude or ChatGPT account, laser depth per exposure, machine calibration and physical rotary performance require tests on the intended deployment.
