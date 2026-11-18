# Import SVG drawings

Open **Images → SVG sizing**, then choose **Import SVG**. You can also drop an SVG file on the drawing bed. The installed Studio editor and the portable file editor use the same importer.

**Fit artwork to shape box** stretches the visible geometry to the Design panel's Width and Height and places its lower-left corner at X and Y. This is useful when a drawing has no reliable scale. To preserve its proportions in this mode, enter matching width and height ratios.

**Original page scale at X/Y** places the SVG page's lower-left corner at Design X and Y. It keeps the page margins, drawing scale and aspect-ratio alignment. In SVG, Y increases down the page. In Studio, Y increases up the bed. An SVG with a two-inch-wide page should produce a 50.8 mm page width. The importer understands mm, cm, Q, inches, points, picas and CSS pixels at 96 px/in. When only a viewBox supplies the size, each unit is one CSS pixel. Use absolute page dimensions for original-size import. Geometry that lands outside the bed needs a different placement or work area.

The importer follows moves, lines, horizontal and vertical edges, cubic and quadratic Béziers (including shorthand), elliptical arcs and close-path commands. Each moveto starts its own path. For example, a gear outline and centre bore stay separate in CAM. Corners and segment endpoints are retained. Curves are sampled at a target spacing of 0.25 mm or finer in the drawing. Rectangles with rounded corners, circles, ellipses, polylines, polygons and lines are supported.

Group translation, rotation, scaling, skew and matrix transforms are supported, including a local `use` reference to a named shape or group. Definitions are drawn only when referenced. Elements hidden with display, visibility or zero opacity are omitted. The imported result is planar outline geometry. Stroke width, colours and paint order do not create machining operations. Use Studio's layers and CAM settings to choose the operation.

Convert text, dashed strokes, masks, clipping, markers and visual filter effects to plain paths before import. Nested SVG viewports, symbol viewport references, percentage geometry, raster images and stylesheets require conversion. Scripts, external references and cyclic references are rejected. Each file may contain up to 3,000 imported paths and 100,000 sampled vertices.

## Try a compound path

Save this drawing as an SVG file. In **Design**, set X and Y to zero. In **Images**, choose **Original page scale at X/Y** and import the file. It contains a 30 × 10 mm rectangle and a second, separate 10 mm line. Generate a preview and inspect the travel between the shapes.

```xml
<svg xmlns="http://www.w3.org/2000/svg" width="60mm" height="30mm" viewBox="0 0 60 30">
  <path d="M5 5 h30 v10 h-30 z m35 0 h10" fill="none" stroke="black"/>
</svg>
```
