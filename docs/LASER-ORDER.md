# Plan the laser cut order

In **Layers → Laser cut order**, enable **Plan compatible line cuts within each layer**. The preview, generated G-code and downloaded job use the planned sequence. Object geometry stays editable in its original design order. Save the project to retain the planning switches.

- **Minimise travel** chooses the nearest eligible contour start at each step. It preserves each contour’s start vertex and direction. The report compares nominal XY travel between design contours, starting at work zero. Physical time also depends on acceleration, speeds, positioning, lead moves and the controller.
- **Inside before outside** orders strictly enclosed cuts before their enclosing outline, including open engravings within a closed contour. Paths that cross or touch are independent. This setting can increase travel when the original sequence crossed a holding contour too soon.
- **Remove exact duplicate paths** removes matching open paths in either direction and matching closed loops with a different starting vertex or winding. Keep this option off for intentional repeated cuts. Offset or resampled copies count as different geometry.

### Try nested frames

1. Start an empty laser project with **Line** operation, one pass and zero lead/ramp distances. Draw a large rectangle, then a smaller one entirely inside it. Duplicate the smaller one at exactly the same coordinates.
2. Open **Layers**, enable planning and turn on **Inside before outside** and **Remove exact duplicate paths**.
3. Generate a preview. The inner rectangle runs once before the larger one. The report identifies one duplicate. Disable planning to compare the original operation order.

Planning processes contiguous single-pass line operations whose complete settings agree within a single output layer. An override with a different feed, power or other setting starts a new group. Raster and per-vertex power, fill, leads, ramps, holding tabs, multipass jobs and incompatible operations retain their order. Studio joins compatible contours into a single planned operation. Layer order continues to determine when each layer runs.

A group supports 200 contours, 15,000 vertices and 500,000 edge comparisons. A group exceeding a bound keeps its source sequence and appears in the report. Check the preview when nesting spans operation boundaries or a group reaches one of these limits.
