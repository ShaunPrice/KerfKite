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

## Profile requirements

Each feature accepts one closed polygon of 3–256 distinct vertices. Edges must enclose a positive area and must not cross each other. Revolution profiles require nonnegative radii. Use a second subtractive feature for a hole. The editor supports straight edges, grid snapping and parameter-driven coordinates. Tangency, dimensional solving for free curves, and sketch fillets are outside this editor’s current scope.
