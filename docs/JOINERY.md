# Joinery, inlays and nesting

**Design → Joinery & nesting** holds three helpers for parts cut with a round cutter. Each changes the drawing only, in one step that **Undo** reverses. Generate a new preview afterwards.

## Dog-bone corners

A round cutter leaves every inside corner round, with the cutter's radius. A square-cornered part pushed into such a corner will not seat. A dog-bone cuts a small circle at the corner, just far enough for the corner point itself to be cleared.

1. Select a closed outline: a slot, a pocket, or a part with notches.
2. Choose where the cutter runs. **Inside the outline** suits a pocket, a slot or an inside profile: the corners of the opening are relieved. **Round the outside** suits a part cut out of the sheet: only the corners that turn into the part are relieved, such as the bottoms of its notches.
3. Leave the **Relief radius** as offered, or raise it. It starts 0.05 mm above the radius of the CAM cutter and may not be smaller than the cutter.
4. Choose **Add dog-bones to selected object**.

Each relief is a circle of that radius whose edge passes through the corner point, centred on the line that halves the corner. The message gives the number added. A corner whose edges are too short for the relief is left as it was and counted: the relief may take at most half of each edge, so that reliefs at the two ends of one edge cannot meet. Corners sharper than 10° or flatter than 170° are left alone.

The object keeps its name with **· dog-bones** added. A rectangle or gear becomes a plain outline, since its shape settings would redraw it without the reliefs.

## Inlay

An inlay is a pocket in one piece and a plug of another material that fits it. **Make pocket and plug** draws both from the selected closed shape, for a straight-sided cutter:

- **The pocket** is the shape itself, with a pocket operation. The cutter rounds its inside corners.
- **The plug** is what the cutter can reach inside the shape, made smaller all round by the **Clearance**, with an outside profile. Its corners are therefore rounded to match the pocket. It is placed beside the shape.

Set the **Cutter diameter** (it starts as the CAM cutter) and the clearance: 0.1 mm suits a snug fit in wood, and more suits paint or glue. Then give the pocket its depth and the plug a little more, so that the plug stands proud for sanding flush. Both objects carry their own operation settings; the original shape is unchanged, so turn its output off if it should not be cut.

A feature of the shape narrower than the cutter cannot be cut in the pocket and is absent from the plug. V-bit inlays, whose two halves are carved at an angle, are not made by this tool.

## Nest parts on the sheet

**Nest parts** moves every object whose output is on into rows from the bottom-left corner of the work area, tallest first, with the **Gap** between parts and the **Margin** at the edge of the sheet. Each part is placed by its bounding box, at its own angle: parts are not turned and are not fitted into each other's hollows. Objects with output off stay where they are.

The message gives the area of the sheet the parts now take. If they cannot fit, nothing moves and the message says so.

Objects that must stay together, such as a part and the holes inside it, should be one object or be moved together afterwards with [Align & distribute](USER-GUIDE.md).

## What has been checked

`tests/js/joinery.test.cjs` checks the dog-bone geometry (the corner point stays on the outline, a cutter of the radius fits at each relief, the reach along each edge, outlines drawn in either direction) and the nesting (no overlap, the gap kept, the sheet's bounds). `validation/joinery_workflow.py` drives the panel in a browser against the simulator. No part made with these tools has been cut on a machine: check the fit on scrap.
