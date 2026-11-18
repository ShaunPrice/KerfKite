"""Local stdio MCP: artwork and CAM artifacts only, no controller import."""
import argparse
from pathlib import Path
from fastmcp import FastMCP
from .artwork import ArtworkStore
from .cam import generate

READ={'readOnlyHint':True,'destructiveHint':False,'idempotentHint':True,'openWorldHint':False}
WRITE={'readOnlyHint':False,'destructiveHint':False,'idempotentHint':False,'openWorldHint':False}


def create_server(output_dir):
    store=ArtworkStore(output_dir)
    mcp=FastMCP('KerfKite Studio Artwork',instructions='Create vector artwork and reusable template files for KerfKite Studio. XY in millimetres, bottom-left origin. Paths are ordered polylines; explicitly repeat the first point to close a contour. Inner contours before outer contours. No machine control tools. Defaults are not material recommendations. The shared CAM supports compensated profiles, pockets, V-carving, knife paths and supplied per-vertex router depths. This server has no dedicated bitmap/STL-to-relief, camera, rotary or parametric gear tool. Physical commissioning remains required outside MCP.')

    @mcp.tool(annotations=READ)
    def list_templates(query: str='', category: str='', limit: int=30) -> list[dict]:
        """Search the 79 original built-in flowers, vines, leaves, symbols, tags, frames and ornaments. Returns IDs, not full geometry."""
        return store.list_templates(query,category,limit)

    @mcp.tool(annotations=READ)
    def get_template(template_id: str, x: float=0, y: float=0, width: float=100, height: float=100) -> dict:
        """Get a built-in template as editable ordered XY paths, scaled to millimetres for composition."""
        return store.template(template_id,x,y,width,height)

    @mcp.tool(annotations=WRITE)
    def create_artwork(name: str, paths: list[list[list[float]]], width: float=750, height: float=400) -> dict:
        """Create SVG and browser project files from ordered XY polylines. Coordinates must fit the work area. Close contours by repeating their first point. Writes unique files only in the configured output directory."""
        return store.create_artwork(name,paths,width,height)

    @mcp.tool(annotations=WRITE)
    def save_template(name: str, category: str, paths: list[list[list[float]]]) -> dict:
        """Create a reusable template-library JSON file from artwork. Import it in the browser library manager. Preserves path order and normalizes geometry; does not overwrite existing templates."""
        return store.save_template(name,category,paths)

    @mcp.tool(annotations=READ)
    def preview_job(project: dict) -> dict:
        """Validate a project using the same laser/router/knife CAM engine as the UI. Return bounds, cutting-time estimate, warnings and first G-code lines. Project needs mode, width, height, feed, power, paths; optional operation, tool geometry, depth, step, plunge, safe, passes, path_depths, path_powers and laser ramp/lead settings. Ordered operations must use the same head, protocol and cutter."""
        result=generate(project)
        return {k:result[k] for k in ('bounds','cut_length','cut_minutes','warnings')} | {'line_count':len(result['lines']),'first_lines':result['lines'][:25]}

    @mcp.tool(annotations=WRITE)
    def export_gcode(project: dict) -> dict:
        """Write reviewed-project G-code to a unique local file. No transmission or machine operation. Uses shared validated CAM, including supported compensation/area operations and supplied varying-depth paths. Does not import a heightmap or STL."""
        return store.export_job(project)

    @mcp.resource('cnc-studio://artwork-guide')
    def guide() -> str:
        return 'Workflow: list_templates → get_template → compose ordered paths → create_artwork/save_template → review in browser. Use preview_job before export_gcode. Separate disconnected contours; put holes first. Projects use pi-cnc-studio/1; library bundles use pi-cnc-templates/1. Use the browser involute generator for parametric gears, then supply its exported paths; this server does not generate gears from tooth parameters. No machine control is exposed.'

    return mcp


def main():
    parser=argparse.ArgumentParser(description='KerfKite Studio local artwork MCP server')
    parser.add_argument('--output-dir',default=str(Path.cwd()/'artifacts'))
    args=parser.parse_args()
    create_server(args.output_dir).run(transport='stdio')


if __name__=='__main__':
    main()
