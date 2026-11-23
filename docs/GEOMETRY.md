# Geometry, text and controller capabilities

Current development behavior; see [STATUS.md](STATUS.md) for acceptance limits and [USER-GUIDE.md](USER-GUIDE.md) for operation instructions.

## Involute spur gears

New gears use metric zero-shift external involute teeth. Controls: tooth count, module, pressure angle (14.5–25 degrees), pitch-circle tooth thinning, centre-hole diameter and page rotation. Outside diameter is derived as module × (teeth + 2). A mating pair needs matching module/pressure angle; standard centre distance is module × (teeth A + teeth B) / 2. Thinning is per gear, so the pair's thinning adds to backlash.

Conservatively reject tooth counts below ceil(2 / sin(pressure angle)^2), oversized bores and pointed/overlapping teeth. New gear objects cannot be stretched with resize handles; regenerate using module. Existing decorative gears in old projects are not silently converted. The root uses a radial transition and circular root arc, not a generated hob trochoid. No load rating, root stress, interference sweep or physical mesh test has been completed. Use the internal outside/inside profile operation for cutter-radius or laser-kerf compensation, or export to suitable external CAM. Compensation does not establish physical gear accuracy. SVG export preserves geometry and hole-first path order. Numerical tests verify pitch thickness, tooth count and dimensions.

Reference: https://khkgears.net/gear-knowledge/gear-technical-reference/calculation-gear-dimensions/

Polygons support 3–128 sides, rotation and circular centre holes checked against actual edge clearance.

## Text

Bundled Lora, Montserrat and Cormorant Garamond fonts (SIL Open Font License) and imported TTF/OTF fonts convert to closed contours. The parser is opentype.js 2.0.0 (MIT); licenses are bundled in static/vendor. Local files work without fetch or a CDN. Text retains editable settings in the project, plus portable outlines. Custom font files must be reimported to regenerate text in another session.

Layouts: straight, vertical text down, cubic Bezier baseline with adjustable rise, letter spacing, page rotation and Y-axis rotation. Choose symmetric rise or independent custom cubic control points. Custom control X positions are percentages of text width and Y positions are millimetres; numeric fields and draggable orange handles edit the same values. Endpoints remain fixed at the baseline start/end. Settings survive text load/replace and portable projects; cusp/stationary-tangent cases are rejected. Tight curves may still overlap letters. Latin kerning and composite glyphs are supported; complex-script shaping is rejected rather than misrepresented. No automatic ligatures are applied.

Depth is separate from font style. Text creates per-vertex negative-Z contours, with shallowest depth and optional Y-axis tilt. Router CAM uses step-down limits and clearance retracts, with conservative plunge-feed speed for 3D contour segments. XYZ paths can also be exported. A rotatable 3D path view and sampled stock-removal preview are available. Separate operations provide pockets, sampled V-carving and raised/recessed text/artwork relief; solid STL extrusion preserves holes. The sampled preview is an approximation, not a collision/gouge certificate. Font style itself does not define cutter geometry or engraving depth.

## Artwork

1,768 original built-in vector templates across 132 categories, including botanical sets, ornamental borders, mandalas, seasonal designs, nautical motifs, music, space, workshop, food, travel, garden, celebrations, animals, coasters, bookmarks, jewellery and hanging ornaments. The library displays 36 previews per page, with category/search/favourites filters. The MCP snapshot contains the same artwork. Source reference sheets have not been traced or redistributed. Existing simple motifs remain available. Foreground leaf/berry masking trims underlying vector segments, so exports do not retain hidden stem lines. Natural aspect ratios are retained for built-in previews and insertion. These are engraving contours; automatic stencil bridges, automatic one-piece cutout construction and production minimum-feature validation are not provided; manual boolean editing and CAM warnings do not replace these checks. Artwork quality still needs user review at intended production size.

## Protocols

The protocol catalogue (`native/core/src/protocols.cpp`, with the controller transports in `native/server/machine.cpp`) separates supported export dialects from controller transport. GRBL / GRBL-ESP32 retains its output. Marlin router export omits GRBL work-coordinate/modal setup and uses millisecond dwell; the controller must have CNC spindle support configured. Both GRBL and experimental Marlin live adapters have software tests. Physical ports are disabled by default and require `--enable-hardware`; no physical machine was commissioned. Marlin pause stops host scheduling and may not stop already buffered motion. Marlin dynamic-laser output remains unsupported.

Use the [rotary workspace](ROTARY.md) to check the seam, preview cylindrical engraving, export a program or stream to a supported independent rotary controller.

## Area CAM, relief and preview limits

Compensated profiles use valid closed contours and nested holes. Pockets use concentric erosion and a stepover no larger than half the cutter diameter. Sampled medial-axis V-carving is conservative and can leave material in broad regions; tool-size/inaccessible-feature warnings still need review. Heightmaps and top-access STL relief use flat/ball cutter-contact envelopes, allowances and step-downs. Undercuts, clamps, holders and out-of-stock geometry are not modeled as certified collision checks.

The relief's contact is computed by the native core on the processor's threads, or with a GPU for large jobs; see [NATIVE.md](NATIVE.md) for how it was compared with the Python reference it replaced and for measured results. No physical cut or gear mesh is established by numerical tests.

## Multi-object layout

The Design panel provides an explicit multi-object checklist, edge/centre alignment and equal-gap horizontal/vertical distribution. Use selected-object bounds or the complete work area as the reference. Distribution requires at least three objects and rejects insufficient space instead of overlapping them. A single object can align to the work area. Translations preserve depth/power arrays, text settings, tool settings and gear proportions; saved gear/surface origins translate with their paths. Changes are atomic, participate in undo and invalidate previews. Full snapping/grouping semantics remain outstanding.

## Router holding tabs

Closed router line/inside/outside contours can retain stock-bottom-relative tabs with a flat cutter. The requested cut reaches the stock bottom; tab height is less than stock thickness. For example, 10 mm stock with 2 mm tabs retains plateaus at Z −8 mm even if the main contour overcuts below −10 mm. Tab positions follow the compensated path's arc length; width includes one cutter diameter of plateau padding beyond the nominal width. Count-based placement or explicit percentages are supported, including closed-seam handling.

Placement is a geometric aid, not a retention/clamping guarantee: corners and neighboring cuts need review. Open paths, relief/per-vertex depths, unsupported tools/operations, overlapping tabs and oversized plateaus are rejected. Holding tabs currently require the Python server; the offline engine rejects them explicitly. Direct dragging of tab locations on the design is not implemented.

## Trim crossing lines into a corner

In **Design**, choose **Draw two-point line**. Click the start and end in the work area, or open **Pick by coordinate** to enter each point in millimetres. Draw a second line that crosses the first.

1. Choose **Trim & join**. Click the portion of the first line you want to keep, then the portion of the second line to keep. Pick away from the intersection so the retained sides are clear.
2. The green preview shows the entire resulting path. The circle and coordinates mark the common vertex. Use **Pick again** to change a side or **Cancel** to discard the preview.
3. Choose **Apply join** to replace the two trimmed contours with one continuous path. Generate a new CAM preview. **Undo** restores both source contours, and **Redo** reapplies the corner.

The first picked contour runs toward the corner and the second runs away from it. Open polylines retain all vertices on the chosen sides. The picked straight sections must meet within both segments. Endpoint and T junctions work, and sampled straight sections from an SVG are supported. Two open contours can belong to the same drawing object. A join keeps the first object's name, layer and CAM overrides, and other contours keep their order within each surviving object. Distinct objects need matching layer, output and CAM assignments. Sampled depth and power are interpolated at the crossing and must agree there. Closed outlines and objects carrying editable source geometry can be edited with their own tools. The design remains unchanged if a join is rejected.

Practise choosing each side in the [interactive trim lesson](../studio/static/machining-lessons.html#trim-practice). The lesson uses the same intersection and joining functions as the editor.

## Layout groups and placement snapping

In **Design → Align & distribute**, check two or more objects and choose **Group checked objects**. Grouping retains each member’s layer and machining settings. Drag a member to move the entire group. To position it precisely, select any member, set **Group left X** and **Group bottom Y**, then choose **Move selected group**. Member-specific resizing, rotation and machining properties still operate on the selected member. **Ungroup selected layout** releases the group. Group membership survives undo, redo and project exchange.

Open **Design → Placement snapping** to choose a grid pitch or nearby artwork vertices. Drag near a vertex to use it as the anchor. Resizing a handle and picking the endpoints of a new two-point line also use snapping. Visible vertices within eight screen pixels take priority over the grid. Hold Alt to place freely. Numeric entry stays exact. Each project remembers its snapping preferences.

For centreline lettering, installed typefaces, numbered batches and machine-readable codes, see [Engraved lettering and production labels](LABELS.md).

### Place router tabs on the cutter path

In **CAM → Holding tabs**, select a closed router contour and choose a line, inside or outside operation. Set the cutter diameter, then choose **Place tabs on cutter contour**. Studio calculates the compensated tool centreline for inspection. Switch contours to inspect holes. Drag a tab marker along the contour, click the contour to add one, or edit the selected percentage numerically. **Use positions** copies the result to the tab settings. Set the stock thickness, tab width, tab height and cut depth, choose **Assign tabs**, then generate a fresh preview. The same normalized positions apply to every contour in the selected object. Use **Load assigned tab settings** to revise an existing operation.
