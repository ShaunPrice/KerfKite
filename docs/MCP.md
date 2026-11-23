# Use Claude or ChatGPT with the KerfKite workshop

The KerfKite MCP server gives an assistant its own Studio editor, CAD/CAM calculations, template library access and file transfer. Start in the simulated workshop to make designs, test macros and export files. The assistant skill supplies the workflow. The MCP connection supplies the running tools. Set up both for interactive editing.

## Install the local server

The MCP server, `kerfkite-mcp`, is installed with KerfKite Studio beside the workstation server. It needs no Python.

| System | Program |
| --- | --- |
| Windows | `%LOCALAPPDATA%\Programs\KerfKite Studio\kerfkite-mcp.exe` |
| macOS | `/Applications/KerfKite Studio.app/Contents/Resources/kerfkite-mcp` |
| Linux | `/opt/kerfkite/kerfkite-mcp` from the Debian package, or `kerfkite/kerfkite-mcp` in the extracted archive |

Run it with `--help` to check the installation. The template and calculation tools run without a browser. The interactive workshop opens its editor in an installed Google Chrome, Chromium or Microsoft Edge. Select another Chromium build with `--chromium` or the `KERFKITE_MCP_CHROMIUM` environment variable and its executable path. The browser keeps its sandbox, so on Linux run the server under an ordinary account, not as root. STEP operations use the STEP add-on when it was installed with Studio. To build the programs from source, follow `native/README.md` in the source tree.

Create a private working directory for each assistant workspace. The MCP process stores generated files and the workshop's browser storage in the directory chosen with `--output-dir`. Use one running server per output directory. Each user's normal Studio session keeps its own documents and browser storage. Download a project from Studio to hand it to the assistant. Export the edited result and reopen it in Studio to continue there. Add `--show-workshop` when launching the local MCP server if you want to see its separate editor window.

## Install the skill and connect the client

The release asset is **`kerfkite-workshop-skill.zip`**. Its top-level directory is `kerfkite-workshop/`, containing `SKILL.md`. The same files are in the repository under `skills/kerfkite-workshop/`. [Release downloads](https://github.com/ShaunPrice/KerfKite/releases/latest)

### Claude Code on the workshop computer

1. Copy the extracted `kerfkite-workshop` folder into `.claude/skills/` in your project, or into `~/.claude/skills/` for personal use across projects. The resulting file should be `…/.claude/skills/kerfkite-workshop/SKILL.md`. [Claude Code skills](https://code.claude.com/docs/en/skills)
2. From that project directory, register the server using absolute paths. In the example below replace the program and output paths with your own. The table above lists the program's path for each system. [Claude Code MCP setup](https://code.claude.com/docs/en/mcp)

```sh
claude mcp add --transport stdio --scope local kerfkite -- "/opt/kerfkite/kerfkite-mcp" --output-dir "/absolute/path/to/workshop-files"
```

3. In Claude Code open `/mcp`, check `kerfkite`, then invoke `/kerfkite-workshop`. Ask it to call `workshop_state` and list the starter macros. Verify that actual tool calls return a project and macro names. A registered server name alone only confirms the configuration entry.

Other stdio clients can launch the same server using this configuration shape. Merge the entry into the client's existing servers and use absolute paths. A Windows JSON path uses doubled backslashes. [Local MCP client configuration](https://py.sdk.modelcontextprotocol.io/get-started/real-host/)

```json
{
  "mcpServers": {
    "kerfkite": {
      "command": "/opt/kerfkite/kerfkite-mcp",
      "args": ["--output-dir", "/absolute/path/to/workshop-files"]
    }
  }
}
```

### Claude chat with a hosted workshop

In **Customize → Skills** (or **Plugins → Skills**), create or upload a skill, select the release ZIP and enable it. Follow the hosting guide to deploy the protected endpoint. In **Customize → Connectors**, add a custom web connector using your full HTTPS URL ending in `/mcp`. Authenticate as the configured operator. In managed workspaces an administrator may need to add or enable the connector. Open a conversation with the connector enabled and ask Claude to call `workshop_state`. [Upload a Claude skill](https://support.claude.com/en/articles/12512180-use-skills-in-claude) · [Claude remote connectors](https://support.claude.com/en/articles/11175166-get-started-with-custom-connectors-using-remote-mcp) · [KerfKite hosting](MCP-HOSTING.md)

### ChatGPT

1. Where your workspace supports custom skills, open **Plugins → Skills → Create → Upload from your computer**, select the ZIP and enable the skill after review. Skill availability and upload permissions vary with the workspace. [Skills in ChatGPT](https://help.openai.com/en/articles/20001066-skills-in-chatgpt)
2. For a hosted server, follow the hosting guide to configure OAuth and a TLS reverse proxy for the loopback MCP listener. In ChatGPT **Plugins**, use **+ → Add custom MCP server**, supply the public HTTPS `/mcp` endpoint, configure its authentication and add the plugin. The Studio editor's web address and the machine's monitor address serve different APIs. [Connect a server to ChatGPT](https://developers.openai.com/plugins/deploy/connect-chatgpt) · [KerfKite hosting](MCP-HOSTING.md)
3. An operator with a supported OpenAI workspace can alternatively use **Secure MCP Tunnel** for a private server. Configure the tunnel client on the workshop host with the local stdio launch command above. Associate the tunnel with the intended workspace, configure its restricted runtime credential privately and check the tunnel client health. Choose **Tunnel** when adding the custom MCP server. [Secure MCP Tunnel setup and permissions](https://developers.openai.com/api/docs/guides/secure-mcp-tunnels)
4. Install the plugin, open a new conversation and select it using `@`. Ask, “Use the KerfKite workshop skill and call `workshop_state`. What project is open?” A tool response verifies that this chat reaches your server. Installation in one workspace or client is separate from authorisation in another. [OpenAI connection test](https://developers.openai.com/plugins/deploy/connect-chatgpt)

## First part and file handoff

Ask the assistant, “Make a 70 × 40 × 5 mm mounting plate with four 4 mm holes, 7 mm from the edges. Show the top view and give me an editable project and STL.” On an empty workspace these tool inputs produce that part.

| Tool | Arguments | What to check |
| --- | --- | --- |
| `workshop_state` | `{}` | Confirm the open project. A new script joins the current part. |
| `workshop_macros` | `{}` | Locate **Mounting plate** and inspect its saved inputs. |
| `workshop_macro` | `{"name":"Mounting plate","parameters":{"width":70,"length":40,"thickness":5,"bore":4,"margin":7}}` | One scripted solid feature is added. |
| `workshop_command` | `{"line":"view top"}` | The camera shows the XY face. |
| `workshop_snapshot` | `{}` | Check four holes at X 7 and 63, Y 7 and 33. |
| `workshop_export` | `{"kind":"project"}` and, in a second call, `{"kind":"stl"}` | Retain the individual `artifact`, `name` and `size` from each result. |
| `workshop_read_file` | `{"artifact":"ID_FROM_EXPORT","offset":0,"length":24576}` | Replace the example ID. Decode `content_base64`, append bytes and continue from `next` until it equals `size`. |

Open the `.cncstudio.json` in Studio to edit the source and features. The STL is a mesh for exchange. Uploading a file stages its bytes in the assistant's output directory. To open it, find the corresponding Projects, import, macro or CAD file input with `workshop_controls` and use `workshop_action` with `action="upload"`, the input `ref` and `value` set to the uploaded artifact identifier. Project import can replace the current drawing. Export it first if it should be retained.

## Recipes for artwork, macros and toolpaths

- **Reusable border.** Ask for a command macro named `Frame` with `width` and `height`. Use `workshop_save_macro` with `kind="commands"`, source `rect {width} {height}\nfit drawing` and defaults `{"width":60,"height":30}`. Run it at `width=80` and check the 80 × 30 mm outline. Each run adds a design instance. Use the library's **Export library…** for the portable definitions. [Macro programming guide](CAD-MACROS.md)
- **Custom parametric geometry.** Call `design_actions` with `{"query":"cad-script"}`, then `design_call` with `{"action":"cad-script","payload":{"source":"linear_extrude(height=3) difference(){ square([20,12]); translate([6,6]) circle(d=4,$fn=48); }"}}`. Inspect the 20 × 12 × 3 mm result. To add it to the open scene, apply the same source through the editor or a named macro. [CAD language and examples](CAD-SCRIPTING.md)
- **Flower engraving.** Search `list_templates` for `flower`, choose an ID and pass it to `get_template` with the desired X, Y, width and height. Compose the returned paths and submit them to `create_artwork` for an editable project and SVG. `save_template` produces a `.templates.json` bundle for the library manager. Repeat the first point to close a polyline and keep disconnected contours separate. Coordinates are millimetres from the lower-left work origin.
- **CAM review.** Load a Studio project with the desired router, laser or knife operation. For a direct CAM call, pass the project’s CAM settings and ordered paths to `preview_job`. Inspect the bounds, warning list, time estimate and sample posted lines before requesting `export_gcode`. `create_artwork` generates a geometric project with example laser settings. Feed, power, tool geometry, stock and compensation need a material-specific setup. A simulated success verifies the program in the simulator.

## Tool reference

| Tools | Behaviour |
| --- | --- |
| `workshop_state`, `workshop_snapshot` | Read the assistant editor's project, selection, feature IDs, recent dialogs/downloads, feedback or current PNG. |
| `workshop_controls(query, offset, limit, visible_only)` | Find actual inputs, menus, panels and canvas controls. Limits are 1–100 per page. Inspect `total` and advance `offset` for more results. |
| `workshop_action(action, ref, value, points, confirm)` | Click, double-click, inspect, fill, select, check, press a key, upload, point, drag, wheel or switch open pages. For check use the string `true` or `false`. Coordinates are CSS pixels within the discovered control. For `page`, supply the page index as `value`. Rediscover references when panels change. Dialogs requiring consent default to dismissal. |
| `workshop_command(line)` | Run a measured drawing command, view instruction or script statement in the current document. |
| `workshop_macros`, `workshop_save_macro`, `workshop_macro` | List full definitions, save a new definition, or execute a named macro with numeric overrides. Existing definitions require `replace=true` to overwrite. The library lives in this assistant workspace. |
| `design_actions(query)`, `design_call(action, payload)` | Discover and run the Studio service’s registered design calculations, including solid/section, import, assembly, fixture and CAM simulation operations. Small responses arrive under `result`. Large JSON and binary results use artifact records. STEP requires the optional runtime. |
| `workshop_upload`, `workshop_export`, `workshop_read_file` | Stage up to 8 MiB per file, export `project`, `svg`, `stl` or `gcode`, or retrieve generated artifacts in chunks. Read lengths are 1–131072 decoded bytes. Identifiers are generated by the server. |
| `list_templates`, `get_template`, `create_artwork`, `save_template` | Search 1,768 built-in vector templates, scale and position a selected item, save SVG/project geometry, or create an importable library bundle. Local output paths point into the selected server directory. |
| `preview_job`, `export_gcode` | Validate prepared CAM inputs and write a reviewed program to a uniquely named `.nc` file. |

Use the current tool list in your client to discover enabled capabilities. The resource `cnc-studio://artwork-guide` provides an artwork workflow. The editor uses the application's own controls and a simulated controller. The server does not attach to an existing personal browser tab. Its separate opt-in machine interface uses the paired pendant service below.

## Use a hosted file library and Pi receiver

Configure the optional hosted-file connection on the operator's MCP server to let the assistant transfer files. Set `KERFKITE_FILES_URL` to the operator's HTTPS Studio file-server origin and `KERFKITE_FILES_USERNAME` to the account that owns the files. Supply exactly one of `KERFKITE_FILES_PASSWORD_FILE` (path to a private password file) or `KERFKITE_FILES_PASSWORD`. Set `KERFKITE_FILES_CA_FILE` if this installation uses a private certificate authority. An operator registers named receiver destinations on the hosted server. [Hosted editor, accounts, accepted formats and receiver setup](WEB.md)

1. Use `workshop_export` to create a project or reviewed job artifact. Use `hosted_upload_file` with its `artifact` and a descriptive `name` to save a copy in the configured account. Use `hosted_list_files` to inspect stored file IDs.
2. To edit a hosted project, use `hosted_download_file(file_id)` to obtain a workshop artifact. Load it through the assistant editor's Projects file input. Save the revised project as a new artifact.
3. To send a requested file to the Pi inbox, call `hosted_destinations` for a configured name. Use `hosted_send_file` with that destination and either the stored `file_id` or the current `artifact` plus filename. Review the operator confirmation and verify the receiver's returned size and hash. If a response is lost, check the inbox before resending. The receiving operator opens and reviews the job separately.
4. `hosted_delete_file(file_id)` requests confirmation for removal of that stored file. The file account, named receiver and paired pendant are separately configured and authorised.

## Optional paired pendant

The operator pairs the Studio workstation's phone monitor and configures the MCP server process for that specific monitor. Provide `KERFKITE_PENDANT_URL` and exactly one of `KERFKITE_PENDANT_TOKEN_FILE` (a private file containing the pairing token) or `KERFKITE_PENDANT_TOKEN`. Supply `KERFKITE_PENDANT_CA_FILE` if the HTTPS monitor uses a locally installed certificate authority. Network addresses require HTTPS. HTTP is available only on loopback. Enter these values in the operator's private environment and restart the server. Keep the token out of chats, example configuration and published projects. [Phone pendant setup](PHONE-CONNECTIONS.md)

A configured connection adds `pendant_status`, `pendant_job`, `pendant_jog` and `pendant_reference`. First call `pendant_status` to inspect `simulated`, connectivity, controls, local arm state, position, `control_epoch` and a `request_id`. A physical action needs a specific operator request. Supply that request ID and the current epoch for the action. Stop needs only its action and request ID. Start needs a job armed at the workstation. Start, resume, single-axis jog, home and work-zero changes use MCP client elicitation for operator confirmation. Pause and stop go through the paired monitor's existing controls. Each jog uses one of X/Y/Z, a signed 0.001–10 mm step and 0.1–1000 mm/min feed. For a lost response, check status before retrying. Reuse the original request ID for a retry of the same action. Use the workstation or paired pendant directly when the client cannot display the confirmation.

## Connection checks and troubleshooting

| Symptom | Check |
| --- | --- |
| The skill loads but there are no tools | Connect the MCP server in the same client and workspace. Verify tool discovery, then call `workshop_state`. |
| Template search works but opening the workshop fails | Install Google Chrome, Chromium or Microsoft Edge, or set `KERFKITE_MCP_CHROMIUM` to a browser executable the server account can run. On Linux the server must not run as root. |
| A control is hidden or stale | Open its tab/dialog, search by label again and inspect the fresh reference. Use `workshop_state` to read any dismissed prompt. |
| A remote file link points to a server path | Request the generated artifact by its basename with `workshop_read_file`, or collect the file directly from the operator's output directory. |
| A hosted client gets an authentication error | Check the configured issuer, audience, operator subject, scope, TLS certificate and registered client redirect. [Hosted MCP configuration](MCP-HOSTING.md) |
| The GUI shows an empty project after reconnecting | Reopen a saved project in the assistant workshop or upload the operator's project. Keep an exported project for handoff or recovery. |
| Pendant controls are absent or rejected | Check operator pairing, the approved monitor's certificate, workstation arm state, request epoch and client elicitation support. The simulation workflow works without pairing. |

For protocol testing from a source checkout, install the MCP client SDK (`python -m pip install mcp`) and run `python validation/mcp_smoke.py /opt/kerfkite/kerfkite-mcp` with the path of your program. The smoke test starts a temporary stdio server and calls its tools using the SDK's client. The standalone editor simulation runs with `python validation/simulate_solid_workspace.py`. A passing local protocol test, a successfully authenticated cloud chat and a physical machine test are separate checks. Record each client connection and actual tool response before reporting that it works in a given deployment. [Developer handoff](DEVELOPER.md)
