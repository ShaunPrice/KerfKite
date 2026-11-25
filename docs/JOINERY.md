# Joinery, inlays and nesting

**Design → Joinery & nesting** holds three helpers: corner reliefs for a round cutter, inlays for an end mill or a V-bit, and nesting. Each changes the drawing only, in one step that **Undo** reverses. Generate a new preview afterwards.

## Dog-bone corners

A round cutter leaves every inside corner round, with the cutter's radius. A square-cornered part pushed into such a corner will not seat. A dog-bone cuts a small circle at the corner, just far enough for the corner point itself to be cleared.

1. Select a closed outline: a slot, a pocket, or a part with notches.
2. Choose where the cutter runs. **Inside the outline** suits a pocket, a slot or an inside profile: the corners of the opening are relieved. **Round the outside** suits a part cut out of the sheet: only the corners that turn into the part are relieved, such as the bottoms of its notches.
3. Leave the **Relief radius** as offered, or raise it. It starts 0.05 mm above the radius of the CAM cutter and may not be smaller than the cutter.
4. Choose **Add dog-bones to selected object**.

Each relief is a circle of that radius whose edge passes through the corner point, centred on the line that halves the corner. The message gives the number added. A corner whose edges are too short for the relief is left as it was and counted: the relief may take at most half of each edge, so that reliefs at the two ends of one edge cannot meet. Corners sharper than 10° or flatter than 170° are left alone.

The object keeps its name with **· dog-bones** added. A rectangle or gear becomes a plain outline, since its shape settings would redraw it without the reliefs.

## Inlay

An inlay is a pocket in one piece and a plug of another material that fits it. **Make pocket and plug** draws both from the selected closed shape. Choose the kind under **Inlay**.

### Straight-sided, for an end mill

- **The pocket** is the shape itself, with a pocket operation. The cutter rounds its inside corners.
- **The plug** is what the cutter can reach inside the shape, made smaller all round by the **Clearance**, with an outside profile. Its corners are therefore rounded to match the pocket. It is placed beside the shape.

Set the **Cutter diameter** (it starts as the CAM cutter) and the clearance: 0.1 mm suits a snug fit in wood, and more suits paint or glue. Then give the pocket its depth and the plug a little more, so that the plug stands proud for sanding flush. A feature of the shape narrower than the cutter cannot be cut in the pocket and is absent from the plug.

### V-carved, for a V-bit

Both halves are carved with a V-bit, so their walls slope. The plug is carved as the mirror image of the shape, turned over, glued into the pocket and its back cut or planed away. The sloping walls wedge together, and inside corners come out sharp because the bit rises into them.

Enter:

- **Clearing cutter diameter**: the end mill that clears the flat areas.
- **V-bit angle** and **V-bit diameter**.
- **Pocket depth**: how deep the pocket is carved.
- **Plug goes in by**: how far the plug enters the pocket. It must be at least 0.1 mm less than the pocket depth; the difference is the gap left under the plug for glue.
- **Plug left above the surface**: how much of the plug stands out of the work, to be cut away.

Four objects are added, each with its own operation:

| Object | Operation | Cutter |
|---|---|---|
| V inlay pocket · clear | Pocket over the pocket's flat floor | Clearing cutter |
| V inlay pocket · carve | V-carve of the shape to the pocket depth | V-bit |
| V inlay plug · clear | Pocket over the stock round the plug, inside a border | Clearing cutter |
| V inlay plug · carve | V-carve round the mirrored shape, started below the stock top | V-bit |

The pocket's clearing is left out when the shape is too narrow for the clearing cutter: the V-bit then cuts all of it. The two cutters have tool identities, so **Manual tool changes → Generate separate stage files** makes a file for each. The original shape is unchanged: turn its output off if it should not be cut.

**How the plug is sized.** The plug's outline is carved at a depth below the top of its stock (the operation setting `v_start_depth`), so that the plug is narrower than the shape at its tip and reaches the shape's own size at the depth it goes in by. V-carving stops one sampling step short of the wall it carves towards, so the pocket's wall stands that much proud; the plug's outline is carved that step ÷ tan(half the angle) deeper to let it down to where it was meant to sit. The tool sets **V detail sampling** to 0.2 mm or finer for both carves. The bit must be wide enough to cut the deeper of the two carves: the tool refuses a bit that is not.

Carve a test inlay in scrap of both materials before the real one: the fit depends on the true angle of the bit and on Z zero being set the same way for both cutters.

## Nest parts on the sheet

**Nest parts** packs every object whose output is on towards the bottom-left corner of the work area, with the **Gap** between parts and the **Margin** at the edge of the sheet. Parts are placed by their own outlines, so one part can sit in the hollow of another, largest first, each at the lowest and then leftmost place where it fits.

**Turn parts** sets how a part may be turned to fit:

- **By quarter turns** (the default).
- **Not at all**, where the grain of the wood or a printed pattern must keep its direction.
- **In steps of 45°** or **15°**, for sheet without a direction. Finer steps take longer and pack irregular parts tighter.

Of the turns allowed, each part takes the one that ends lowest on the sheet. A closed outline counts as solid: nothing is placed inside a part's holes. A relief, and any object whose paths carry their own depths or powers, is moved but never turned. A turned rectangle, polygon or gear becomes a plain outline, since its shape settings would redraw it upright. Objects with output off stay where they are.

The parts are compared on a grid of cells of 0.25 mm or 1/400 of the sheet's longer side, whichever is larger, and every cell a part touches counts as taken. So the gap between two parts is never less than the gap asked for, and can be up to two cells more.

The message gives the area of the sheet the parts now take and how many were turned. If they cannot fit, nothing moves and the message says so. Up to 200 parts are nested at once.

Objects that must stay together, such as a part and the holes inside it, should be one object or be moved together afterwards with [Align & distribute](USER-GUIDE.md).

## What has been checked

`tests/js/joinery.test.cjs` checks the dog-bone geometry (the corner point stays on the outline, a cutter of the radius fits at each relief, the reach along each edge, outlines drawn in either direction) and the nesting. Parts nested by outline are checked by their own edges: every pair is at least the gap apart, every part is on the sheet, nothing is placed in a hole, two brackets nest into each other only when one may be turned, and fixed parts keep their angle. `tests/test_v_inlay.py` follows the V-bit's point through the pocket's program and the plug's and works out the shape each leaves: across the walls of an arrow-shaped inlay the plug's wall lies on the pocket's to within 0.12 mm of depth, neither inside it nor away from it. `validation/joinery_workflow.py` drives the panel in a browser against the simulator. No part made with these tools has been cut on a machine: check the fit on scrap.
