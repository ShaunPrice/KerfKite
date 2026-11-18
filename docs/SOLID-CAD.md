# From a dimensioned part to a machining preview

Open **3D CAD** in the Studio workstation or HTTPS editor. Dimensions are in millimetres. The server builds a triangulated solid from the feature list, in order. The canvas shows the last successful rebuild and the status line identifies a failing feature. Save the project from **Projects** to preserve its parameters and feature history. Export STL when you need the finished mesh in another application.

## Tutorial 1. Make a mounting plate

1. Choose **Guided plate tutorial**. It opens a practice model with a 60 × 32 × 6 mm plate and a bore of radius 4 mm. Change the named parameter `width` to 80. The bore centre follows `width / 2` and moves to X = 40 mm.
2. Select **bore** and choose **Suppress**. The plate becomes solid and the displayed volume increases by approximately 302 mm³. Choose **Enable** to restore the through hole. Circles are tessellated, so measured volumes approach the analytic value.
3. Choose **Keep lesson model**. Drag the viewport to orbit, scroll to zoom, or choose **Top view**. A focused viewport also accepts arrow keys and the plus and minus keys.
4. Open **Projects** and save or download the project. Reopen it and change the width again to verify the bore remains centred. Exit the tutorial before keeping the model if you want to return to your original part.

## Tutorial 2. Model a raised boss or a pocket

Start with the saved plate. Add a cylinder of radius 5 and height 3 with its origin at X = 15, Y = 15, Z = 6. Select **Join** to form a raised boss. To make a pocket instead, place the cylinder at Z = 3, set its height to 4 and select **Cut**. Inspect the resulting volume and top face. Feature order matters because each operation uses the material accumulated by the preceding features.

Use a rectangular pattern to repeat a feature by a fixed XYZ displacement or use a polar pattern around a centre and axis Z. Counts include the initial instance. Try a pair of bores with a 20 mm X step. Confirm that both stay inside the stock as the plate dimensions change.

## Feature and parameter reference

| Control | Meaning |
|---|---|
| Box | XYZ lengths from its minimum corner |
| Cylinder or cone | Radius and axial height, with optional second radius for taper |
| Sphere | Radius about its local centre |
| Sketch extrusion | Simple closed XY polygon extruded along local Z |
| Sketch revolution | Profile in the radial/Z plane swept about local Z |
| Join, Cut, Intersect | Add material, subtract a tool solid, or keep the overlap |
| Suppress | Skip a feature during rebuild while retaining its definition |
| Origin and rotation | Apply local rotation in degrees before translation |
| Named parameter | Drive multiple dimensions with numbers, parentheses, other names and arithmetic |
| Undo and redo | Traverse the current CAD editing history |

Begin with an enabled additive feature. If a rebuild fails, inspect its name in the error, check parameter spelling and remove circular references. For sketches, check for crossed edges and coincident adjacent points. An empty intersection returns a rebuild error.

See the [graphical sketch tutorial](SKETCH.md) to insert, constrain and drag vertices.

## Tutorial 3. Choose a machining representation

| Intended result | Action in 3D CAD |
|---|---|
| Laser or knife outline | Transfer the whole part silhouette. It includes holes open through the complete thickness. |
| Inspect a blind pocket | Select a horizontal section between its floor and the top. The section includes both the outer material boundary and internal boundaries at that height. |
| Router contour | Transfer the outline, check its placement in the bed, select the required profile and generate a preview in CAM. |
| Router top relief | Transfer the part into 3D relief, review physical width, height, depth, cutter and stepover, then generate the relief toolpaths. |

Placement X and Y give the lower-left of the part in the work area. Keep **Keep contours linked to the CAD part** selected to update the existing machining object when the part rebuilds. Its placement, layer and operation settings stay with the object. If a section disappears or the rebuilt part no longer fits, Studio blocks generation for that linked output. Correct the part or select the object in **Properties** and choose **Detach CAD link and keep current drawing**. Clear the link option before a transfer when you want an independent snapshot. A section describes the material at one height. Assigning a pocket operation to all its contours can clear the surrounding plate. Review the removal preview before choosing the stock region to machine. Top relief covers surfaces accessible from above.

## Tutorial 4. Read the toolpath

Open **Help → Machining lessons**. The lessons use the same offline contour generator as the portable editor. Each offers a dimension to change, toolpath playback and the resulting G-code.

- For the router, set a 1 mm step-down and observe the three Z levels in a 3 mm groove.
- For the laser, set a 5 mm entry ramp. Compare the S values and the colour along the start of the path.
- For the drag knife, set a 0.5 mm blade offset. Compare the generated corner arcs with the dashed nominal shape.

These exercises run independently of the workstation project. The command-line acceptance fixture exercises the server generator, including tool compensation, the stock model, and the simulated controller. See [Simulation](SIMULATION.md).

## Scope

Studio models one ordered solid history, which may contain disconnected bodies. The [assembly editor](ASSEMBLIES.md) fits parts using datum mates and checks interference. The [surface and STEP workspace](SURFACES-STEP.md) handles editable patches and exact geometry exchange. Five-axis machining and continuous full-machine collision detection remain outside this release. The solid modeller requires a Studio server. The downloadable project carries the editable model between workstations.

### Linked top relief

For a relief that follows later part edits, select **Router top relief** and leave **Keep machining geometry linked to the CAD part** checked. Choose **Prepare in CAM**. Review the placement, output width, height, depth, cutter geometry, stepover, stock allowance and sampling resolution. **Generate** records that setup with this relief. For example, a 36 mm part prepared as an 18 mm relief becomes a 24 mm relief if you change the CAD dimension to 48 mm. Each axis retains its own scale. The layer, object machining settings and XY placement stay with the relief. Rebuilds recompute both the raster paths and the sampled Z values. Generate a fresh CAM preview after rebuilding.

If the CAD part changes between preparing its STL and generating relief, return to **3D CAD** and prepare the current part again. Clear the link checkbox before preparing to make an independent snapshot. Choosing a separate STL or heightmap in the relief panel also creates an independent object. To keep an existing linked relief at its present dimensions, select it and choose **Detach CAD link** in Properties.
