# Tiling an oversized job

A design larger than the machine can reach is cut one tile at a time. **CAM → Tile an oversized job** divides the previewed job into a grid of tiles from the origin, and gives each tile a program of its own in which the tile's front-left corner is X0 Y0. You cut a tile, slide the sheet along by exactly one tile, and cut the next.

Router, knife and laser jobs can be tiled. Rotary jobs are not, and a tiled job does not follow a probed surface.

## Divide the job

1. Set the **work area** to the size of the whole sheet, up to 2,000 mm each way, and place the design in it.
2. Choose the mode (router, knife or laser), set the operations and generate a preview as usual.
3. Under **Tile an oversized job**, enter the **Tile width** and **Tile height**: the travel of the machine, or a little less.
4. Choose **Divide the previewed job into tiles**. The panel lists each tile that has cutting in it, with the corner of the design it starts from and the length it cuts. Tiles with nothing to cut are left out.

Editing the design or its settings makes the tiles stale: divide the job again.

## Cut the tiles

For each tile, in order:

1. Fix the sheet so that the tile lies inside the machine's travel, and set work zero at the tile's front-left corner. For the first tile that is the origin of the design. For each later tile the sheet moves by exactly one tile width or height: a fence along one edge and a pin or a stop a tile apart keep it true. Any error in that move shows as a step where cuts cross from one tile to the next.
2. Either choose **Export** and run the file `tile-column1-row1.nc` with any sender, or, at the workstation, tick the usual check on the Control tab and choose **Run this tile**. The run uses the ordinary confirmation and controller checks, with the tile's size, not the whole sheet's, compared with the machine's travel.

## What a tile's program does

A cut is kept in a tile only where it lies inside the tile. Where a cut leaves the tile, the tool retracts to the job's clearance height. Where a cut enters, the tool travels to the entry point at the clearance height and comes down to the cut's depth at the job's plunge feed. A cut along the shared edge of two tiles is made in both. Everything else in the job (units, spindle on, dwell, spindle off) is in every tile's program, so each is complete on its own. A tile that holds the whole job is the job, line for line.

**A knife job** is divided as a router job is: the blade lifts where a cut leaves a tile and comes down where one enters. A drag knife that comes down in the middle of a cut has not been turned to the cut's direction by a lead-in, so allow for the blade's offset where cuts cross a tile's edge, or place tile edges away from fine detail.

**A laser job** has no heights. In a tile the beam is put on (the job's own `M3` or `M4` with its power) only at the start of a cut inside the tile, with the head already there, and is put out (`M5`) wherever a cut leaves the tile and before every travel. Each burning move carries its feed and power. Engraving passes that repeat are divided pass by pass.

The divider reads the job's own program, as the leveller does: absolute millimetre moves in straight lines. A job with a rapid across the work below its clearance height is refused, since the tool could not be lifted out of a tile safely.

## Command line and route

    kerfkite-cam tile request.json        {"lines": [...], "tile_width": 300, "tile_height": 200}

The answer has `columns`, `rows`, `bounds`, `kind` (`cutter` or `laser`, told by the power on the cutting moves) and `tiles`, each with `column`, `row`, `origin`, `lines` and `cut_length`. `plunge` sets the feed for coming back down into a cut; by default it is the feed of the job's own plunges. A request the tool rejects exits with status 2 and the reason on standard error.

`POST /api/tile` takes `{"project", "tile_width", "tile_height"}`, generates the project's job and answers the same. It makes files only. `POST /api/run` accepts `"tile": {"width", "height", "column", "row"}` to run one tile of the project.

## What has been checked

`tests/test_tiling.py` follows every tile's program with an interpreter written for the test. Moved back by the tile's origin, the cuts of all tiles must lie on the job's cuts at the job's depths, and their length must add up to the job's: nothing cut twice, nothing left out. Within a tile the tool never crosses the work below the clearance, never leaves the tile and never starts a cut from somewhere it is not. It checks several tile sizes, ramped cuts, each refusal, and a tile run on the simulator. A laser job's tiles are followed the same way with the beam in mind: it is on only during burning moves, never during travel and never at the end of a program, at the job's power, and the tiles burn the job's length exactly. A knife job's tiles are checked as a router's are. `validation/tiling_workflow.py` drives the panel in a browser.

No tiled job has been cut or burnt on a machine. The accuracy of the result depends on how truly the sheet is moved between tiles.
