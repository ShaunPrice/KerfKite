---
name: kerfkite-workshop
description: Use KerfKite Studio to design editable CNC, laser and drag-knife projects, build reusable macros or artwork, simulate toolpaths, and deliver project files through the KerfKite MCP tools. Also applies to explicit requests for status or control of an operator-paired pendant.
---

# KerfKite workshop

Start with the user's deliverable. An editable Studio project, a template library, a macro library, a triangle mesh and posted G-code serve different purposes. Keep the editable project alongside an exchange file when the user wants further design changes. Use actual tool results for dimensions, warnings and filenames.

## Choose a working surface

- For a printable or machinable part with a visual scene, use the assistant workshop. Read `workshop_state()` first. This is a persistent assistant-owned editor session running in simulation. To revise an existing operator project, request its exported `.cncstudio.json` or use the file the user supplied. Transfer the edited copy back to the operator at the end.
- For specific drawing, CAD, CAM or exchange calculations, use `design_actions()` for available operations and `design_call` with the selected operation’s inputs. A calculation can produce geometry or an artifact independently of the open project. Check the project state if an edit was requested.
- For direct template geometry and reusable artwork, use `list_templates`, `get_template`, `create_artwork` and `save_template`. Use the workshop library to manage browser-saved templates and favorites.
- Discover the server's advertised tools. If live tools are unavailable, offer a portable `.scad` design or macro-library JSON that the user can import. Identify any geometry or toolpath checks that still require Studio.

## Build and inspect a part

For “make a 70 × 40 × 5 mm plate with four 4 mm mounting holes,” call `workshop_macro(name="Mounting plate", parameters={"width":70,"length":40,"thickness":5,"margin":7,"bore":4})`. The bore centres should be at X 7 and 63, Y 7 and 33. A new solid joins the current part, so inspect the starting project before adding it. Use `workshop_command(line="view top")` to inspect the bores. Verify the actual dimensions in the solid panel using the controls API. Export the project and, if requested, the solid mesh. Retain the source and parameter values for later edits.

Use a one-line statement such as `cube([30,20,4]);` for a quick scripted feature. For multiline geometry, open the **CAD script** editor. Find its source and preview controls, enter the program, preview, inspect the dimensions and apply. Script errors identify a line or operation. Correct the source and check the current project before running a second time. Use sketches or supported exact features for STEP output. Export tessellated scripted geometry as STL.

## Drive an editor workflow

Use `workshop_controls` to search by task, label or panel and page the matches with `offset` and `limit`. Controls expose references, visibility and enabled state. Open the relevant tab or dialog, then use `workshop_action` with the returned reference. Inspect select options to obtain their values. Search again after a panel rebuild. For a canvas gesture, inspect its bounds and supply local pixel coordinates. The drawing plane uses millimetres with the origin at the lower left. A camera pan uses pixels and a solid orbit uses degrees.

`workshop_state()` reports the selection, open dialog, project, recent downloads and feedback. Use `workshop_snapshot()` for visual checks such as hole placement, motif overlap or toolpath direction. For project replacement or a destructive dialog, preserve the current work and pass an explicit confirmation only when the user has requested that change. Use the typed command or named macro where available to avoid dependence on viewport size.

## Create a reusable macro

Use `workshop_macros()` to find existing names and defaults. Save a new recipe with `workshop_save_macro(name="Frame", kind="commands", source="rect {width} {height}\nfit drawing", parameters={"width":60,"height":30})`. Use `replace=true` only to update a definition the user wants to replace. For a portable recipe exchange, build a version 1 library for import. A complete entry is shown below.

```json
{"format":"kerfkite-cad-macros","version":1,"macros":[{"name":"Frame","kind":"commands","source":"rect {width} {height}\nfit drawing","parameters":{"width":60,"height":30}}]}
```

Upload the JSON, choose the library’s **Import JSON…** control, then run `workshop_macro(name="Frame", parameters={"width":80})`. Verify an 80 × 30 mm rectangle. Repeat with another width only if a second instance is wanted. To build a parametric profile or solid, use `kind: "scad"` with bare parameter names, e.g. `cube([width,20,4]);`. Macro export preserves definitions and defaults. Project export preserves the instances and scripted geometry actually created. Use the two files for different handoffs.

## Compose artwork and review CAM

Search for an appropriate motif, obtain its paths at the desired XY location and dimensions, and place each instance within the selected bed. Keep disconnected contours separate. To close a polyline repeat its first point. Order holes before outer contours where stock retention matters. Use `save_template` for a reusable motif and `create_artwork` for a placed drawing project and SVG. For joins or visible foreground leaves, inspect the actual paths for unwanted crossing tool lines.

Select laser, router or drag knife as appropriate. Apply the user's measured stock, origin, cutter and material settings. Feed is in mm/min, negative Z cuts below the stock surface, and safe Z is above it. Confirm envelope, compatible tool, depth, passes, kerf or cutter compensation and the dialect in the posted output. `preview_job` accepts the CAM settings and ordered paths. Review its bounds and warnings before `export_gcode`. Simulation provides design evidence. Report which settings still need commissioning on the user's machine.

## Hand files back

Use `workshop_upload` with a base filename and base64 contents. Select the uploaded artifact through the intended import, file or project control. Read the import report before applying. Use `workshop_export(kind="project")`, `svg`, `stl` or `gcode` for the requested file, or obtain the download from `workshop_state()`. Keep its generated artifact identifier. For remote clients use `workshop_read_file` in bounded byte ranges, decode each `content_base64` segment and append bytes in offset order. Continue from `next` until it equals `size`. Check the reconstructed size against the export record. Supply a downloadable file in the user's client if supported, or the actual output directory and filename for a local operator. A path on the server alone does not deliver a remote download.

When a hosted file library is connected, list files in the configured account, download a requested file by its returned `file_id`, then use the downloaded artifact for editor import. For a requested cloud save use `hosted_upload_file` on the exported artifact. For an explicitly requested Pi delivery, discover its name with `hosted_destinations` and send the chosen artifact or stored file ID through `hosted_send_file`. Verify the transfer receipt. A file in the receiver inbox still needs the operator's job setup and run decision. `hosted_delete_file` removes a stored file with operator confirmation.

## Operator-paired machine

`pendant_status` and the named pendant action tools are available only when the operator has paired the server. Read the current state, arm mode, controller epoch and request identifier. A design request authorises work in the simulated editor. Use an actual machine action only for a specific operator request, with the confirmed target, amount and units. Start, resume, jog, home and zero require the client's interactive operator confirmation plus the workstation's own gates. Keep one request ID for the same physical action. If the connection drops, read the current status before any further action. Treat an accepted request and an observed machine outcome as separate evidence.
