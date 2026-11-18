# Artwork MCP interface

Local stdio server built with FastMCP 3. Optional dependency; the normal UI does not require it. This server creates artwork and files, not machine motion. It has not been registered in Claude Desktop, Codex or a hosted ChatGPT connector.

## Install and launch

From the project directory:

```sh
.venv/bin/python -m pip install '.[mcp]'
.venv/bin/python -m studio.mcp_server --output-dir ./artifacts
```

On Windows use `.venv\Scripts\python.exe` instead. Use Python 3.12 for the currently tested dependency set. A local MCP client launches the process and communicates over stdin/stdout; the command is not a web server. Logs go to stderr. No remote HTTP endpoint, OAuth service or MCPB bundle is provided yet.

Example client configuration, after installation (replace all paths for your machine):

```json
{
  "mcpServers": {
    "cnc-studio-artwork": {
      "command": "/absolute/path/cnc-studio/.venv/bin/python",
      "args": ["-m", "studio.mcp_server", "--output-dir", "/absolute/path/cnc-studio/artifacts"]
    }
  }
}
```

Windows uses an absolute path ending in `.venv\\Scripts\\python.exe`. Client configuration syntax varies; the essential contract is that executable, arguments and stdio transport. No account configuration was changed by this implementation.

## Tools

| Tool | Purpose | Writes |
| --- | --- | --- |
| `list_templates` | Search the 79 original built-ins by query/category | No |
| `get_template` | Obtain scaled, positioned XY geometry for a template ID | No |
| `create_artwork` | Create SVG and `.cncstudio.json` project files from ordered polylines | Unique local files |
| `save_template` | Create `.templates.json` for import into the browser library | Unique local file |
| `preview_job` | Validate using the same CAM generator as the UI, returning bounds/warnings/sample G-code | No |
| `export_gcode` | Generate an `.nc` file using current CAM | Unique local file |

Resource: `cnc-studio://artwork-guide`.

## Example requests to an assistant

- “Find six-petal flower templates and arrange three flowers in a 180 × 60 mm engraving.”
- “Create a vine border, save it as a reusable template and give me the SVG.”
- “Create an eight-sided polygon from closed XY points, then save a project for review.”
- “Check this laser project’s bounds and lead-ins before exporting G-code.”

The assistant supplies geometry or composes retrieved templates. This is not an integrated generative-image model and does not automatically vectorise bitmap images. The browser's polygon/gear generators are currently UI tools; MCP can accept their exported polylines but does not expose dedicated parametric gear generation yet.

## Coordinate and review rules

Coordinates are millimetres, positive XY from the bottom-left of the work area. Each path is a distinct polyline; repeat the first point at the end to close it. Put centre holes before outer outlines. Generated project files preserve object geometry; importing the SVG separately uses the UI's current SVG scaling rules instead.

Open `.cncstudio.json` using **Open project**, or import `.templates.json` using **Template library → Import library**. Browser library storage and MCP artifact files are separate. Template files contain geometry, not material cutting settings.

Default speed/power values are placeholders. The shared CAM supports compensated router profiles, pockets, sampled V-carving, knife offset paths, laser hatch and supplied per-vertex router depths/powers. Send the operation and tool parameters as part of the project; ordered operations must retain a compatible head, protocol and cutter. There is no dedicated MCP tool for importing heightmaps/STL, calculating relief contact, generating parametric gears, camera tracing or rotary export. Use the relevant browser/server workflow first and supply supported prepared paths when appropriate. `create_artwork` itself accepts XY polylines and supplies placeholder laser settings, not a complete material-specific CAM plan. MCP does not bypass physical commissioning. It never opens serial ports or sends machine commands.

All outputs have generated unique filenames beneath the output directory chosen at server startup. Tool callers cannot specify arbitrary file paths or overwrite existing documents. Large geometry is bounded to the same limits as the UI.

## Validation

`PYTHONPATH=. .venv/bin/python validation/mcp_smoke.py` starts a separate real stdio MCP process and uses the MCP SDK to initialize, list tools and exercise all six tools. It creates files in a temporary directory. The successful local result is in `validation/mcp-smoke.json`. This verifies local protocol operation on Mac; it is not proof of installation in a specific assistant client or on Windows/Linux/Pi.
