# Sketching a driven profile

Use the **3D CAD** tab in the running Studio application. An extrusion uses XY coordinates in millimetres. A revolution uses radius and Z, and revolves around the local Z axis. The feature’s **Position and rotation** settings place it in the part.

## Draw a plate with a driven corner

1. In **Parameters**, enter `peak` under **New parameter** and click **Add parameter**. Enter `20` as its value.
2. Choose **Sketch extrusion** as the feature type and click **Add feature**. The editor creates a three-point profile. Set its height to the plate thickness.
3. Click **Edit sketch on plane**. Choose a grid step in millimetres. With **Snap to grid** selected, new or dragged coordinates round to multiples of that step from the local origin. At small scales the displayed grid uses larger intervals for readability.
4. Select a vertex in the drawing or in **Vertex**. Click empty space to insert a point immediately after it. The loop closes automatically from the last point to the first. Drag a point to reshape it.
5. Select the corner you want to drive. Choose **Y** under **Coordinate**, choose **peak** under **Driving parameter**, then click **Link axis**. The selected point moves to Y = 20 mm. Drag it sideways. Its X changes while Y remains linked.
6. Click **Apply sketch**. Studio validates the profile and rebuilds the feature. Change the `peak` parameter to `28` and check the new corner position. Give two vertices the same Y parameter to maintain a horizontal edge, or the same X parameter to maintain a vertical edge.
7. To use a formula, enter an expression such as `peak / 2` in a sketch point’s numeric field in the feature inspector. The sketch plane preserves that expression while you drag the other axis. Choose **Release axis** to replace the expression with its current numeric result.

**Fit sketch** recentres the drawing. **Remove vertex** removes the selected point from the draft. **Cancel sketch** discards edits made in the open dialog. After applying, use **Undo CAD edit** to revert the change. Save the project to retain the points, expressions and named parameters.

## Join and trim crossing sketch edges

Open **Edit sketch on plane** on an extrusion or revolution. A self-crossing draft can still be opened for repair. Choose **Trim crossing**, then select the two nonadjacent edges on the drawing or choose their point numbers. A crossing must lie inside both straight segments. **Retained loop** previews either portion of the outline in green. Choose **Keep highlighted loop** to cut away the other portion and join the retained edges at their intersection. **Apply sketch** validates the new closed profile. **Cancel trim** leaves the original edges in place.

Try a four-point bow tie at (0, 0), (30, 20), (0, 20) and (30, 0). Trim edges 1–2 and 3–4. Either choice makes a triangle with a join at (15, 10). For a dimension-driven trial, create a parameter named `span` with value 30 and use it for both 30 mm coordinates. Apply the trim, set `span` to 40 and reopen the sketch. The join moves to (20, 10). Its two coordinate expressions retain the original line definitions. If the reference segments cease to intersect, the rebuild reports the conflict.

A single sketch feature keeps one closed loop. Apply additional trims to repair other crossings in a complex outline. Remove any fillets or geometric constraints on the four source endpoints before trimming those edges. Fillets and constraints on surviving unaffected geometry are retained. A change involving a constrained edge or removed reference prompts you to edit that relation first. In the drawing canvas, use the separate trim-and-join tool for two open paths.

## Constrain a profile

Open **Geometric constraints**. Point numbers start at one. Edge 1 runs from point 1 to point 2. The last edge runs back to the first point.

| Relation | What it controls |
| --- | --- |
| Fixed point | Both coordinates of one point, optionally using named dimensions |
| Point distance | The straight-line distance between any two distinct points |
| Parallel edges | Two edge directions, allowing either orientation |
| Perpendicular edges | A right angle between the edges |
| Equal edge lengths | Both edges have the same length |
| Directed edge angle | The angle in degrees from the first edge direction to the second, counterclockwise in the sketch plane |

To try a fully constrained rectangle, start with four points near (0, 0), (20, 0), (19, 11) and (−1, 9), in that order. Fix point 1 at (0, 0) and point 2 at (20, 0). Set distance 2–3 to 10. Make edges 1 and 2 perpendicular, edges 1 and 3 parallel, and edges 1 and 3 equal in length. The sketch reports **Fully constrained**. Enter a model parameter for the 10 mm distance and change it after applying the sketch to drive the extrusion.

**Remaining degrees of freedom** counts independent local coordinate changes at the current solution, including any free translation or rotation. Redundant relations are reported separately. A conflicting addition leaves the current relation set intact. Select a saved relation to inspect its references, replace it or remove it. Starting geometry selects the nearby solution where several shapes satisfy the same dimensions. On an existing model with a dimension conflict, reopen the sketch to correct the dimension or release a relation.

For a horizontal or vertical edge, select its first vertex and choose **Make next edge horizontal** or **Make next edge vertical**. This links the two relevant coordinates to the same expression. If the first coordinate uses a literal value, Studio creates a named driving parameter that you can edit in **Parameters**.

## Round a corner with a tangent arc

Select a vertex and enter a positive **Corner fillet radius** or a named radius expression. Choose **Set fillet**. The sketch shows a circular arc tangent to both neighbouring edges. The numbered point is the intersection of the original straight edges. Choose **Remove fillet** to restore that sharp corner. Adjacent fillets may meet at a common tangent to produce a rounded slot end. Overlapping fillets and reversed or straight-through corners are rejected. The solver places the straight construction edges first, then fits the tangent arcs to them.

**Apply sketch** preserves constraints, radius expressions and the original construction vertices. Solid and CAM previews sample the arcs. STEP export with the optional Open CASCADE runtime writes exact circular arcs. Extrusion and revolution both use this profile.

## Draw a dimension-driven Bézier edge

1. Create an extruded rectangle with points (0,0), (20,0), (20,10) and (0,10). Add a model parameter named `rise` with value 4.
2. Open **Edit sketch on plane**, select **Point 3** and expand **Cubic Bézier edge**. This edits the top edge from point 3 to point 4. For its start control enter X = `40/3`, Y = `10+rise`. For its end control enter X = `20/3`, Y = `10+rise`. Choose **Set cubic edge** and **Apply sketch**.
3. The arch reaches Y = 13 mm halfway across the top. Change `rise` to 8. The arch now reaches Y = 16 mm. A horizontal section through the solid and its linked machining outline follow the rebuilt curve.
4. To sculpt by hand, enter numeric coordinates for a control, press **Set cubic edge**, then drag its square handle. The grid snap applies when enabled. A coordinate containing a parameter expression stays driven when you drag. **Make edge straight** restores the chord between the construction vertices.
5. For a smooth join, check **Tangent at start** or **Tangent at end** and choose **Set cubic edge**. The selected handle follows the direction of the adjoining straight or cubic edge. Its distance from the shared vertex comes from the entered control coordinates. At a cubic-to-cubic join, choose which of the two handles follows the other. A change to the driving tangent updates the follower.

Multiple cubic edges form a piecewise Bézier profile. Tangency is directional (G1); handle lengths on the two sides may differ. Point constraints reference the construction vertices. Relations between edges measure their endpoint chords. The constraint count in the sketch refers to construction vertices. To change the number of vertices on a cubic, straighten that edge first. Sharp or curved joints can be mixed with circular fillets elsewhere on the outline.

STL, stock previews and section paths use an adaptive polygonal approximation with a maximum cubic chord deviation of 0.01 mm. STEP writes the four Bézier poles as an exact curve before extruding or revolving the face.

## Profile requirements

Each feature accepts one closed loop with 3–256 distinct construction vertices and up to 128 geometric relations. Revolutions require nonnegative radius coordinates. A second subtractive feature can produce a hole. Supported sketch edges are straight segments, cubic Béziers and tangent circular corner fillets. A Bézier endpoint can follow a tangent at its shared vertex. Interior contact between unrelated curves, curvature-continuity constraints and general NURBS sketch edges remain open.
