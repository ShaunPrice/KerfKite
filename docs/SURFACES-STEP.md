# Surface modelling and STEP exchange

In **3D CAD**, open **Surfaces and STEP** below the solid viewport. The Studio server supplies the modelling kernel. A filesystem project retains its geometry when moved to a server. All Studio coordinates are millimetres.

## Shape a practice surface

1. Choose **Editable surface patches**, then **Practice shaping a patch**. Practice opens a flat 60 × 36 mm control net while preserving your project.
2. Select row 2, column 2. Set **Z** to **12** and choose **Apply point**. The pole moves 12 mm. The smooth surface approaches the pole while the corner points stay fixed.
3. Drag in the preview or use its arrow keys to orbit. Click a point or use the selector to inspect and edit its X, Y and Z. The X/Y footprint and the height can both change.
4. Choose **Export STEP**. Once the download finishes, choose **Keep practice surface** to save it with the project, or **Exit practice** to restore the previous net. Outside practice, applied edits are part of the project.
5. To machine the top envelope, choose **Prepare router relief**. Review model dimensions, stock zero, maximum depth, cutter and stepover before generating the relief preview. The imported model carries geometry only. Vertical, folded and overhanging areas need a suitable setup beyond top-access relief.

The editor starts with a bicubic tensor-product Bézier patch. To create a multispan surface, choose **Clamped B-spline / NURBS**. Set the polynomial degree of each direction, then choose **Apply point**. Each degree must be smaller than the number of poles in that direction. A lower degree gives each pole more local influence. Increase **Point weight** above 1 to pull the surface toward that pole, then apply. Weights apply in either basis. **Extend U** and **Extend V** extrapolate a new row or column in the working net. Edit its positions to continue the surface. Each direction supports 2–8 poles. Studio uses uniformly spaced interior knots and clamped ends. The preview samples each visible patch in 24 intervals per direction. STEP export uses its mathematical surface and trim curves.

## Join, trim and machine a surface

1. In **Surfaces and STEP**, select **Editable surface patches**. Set **Join direction** to **Along U** or **Along V**, then choose **Join a new patch**. The new control net starts at the last patch’s end and extrapolates its shape. Use **Active patch** to edit any net in the strip. Existing single-patch projects open directly.
2. Choose **Position (C0)** for a joined edge that can form a crease. Choose **Match tangent (C1)** to match the first derivatives on both sides. Studio drives the first line of child control points and weights from its parent for C0, and also drives the second line for C1. The highlighted message identifies driven points. Move the corresponding parent point to change the seam. Joined patches share the same transverse pole count and degree. Incompatible degree or weight changes show an error and preserve the previous geometry.
3. Open **Trim surface bounds**. Enter 0.1 and 0.9 as the U start and U end, then **Apply trim**. U and V run from 0 to 1 over the complete strip. These limits crop the surface while preserving all control nets. **Reset bounds** restores the full rectangular range.
4. Choose **Draw trim outline or holes**. Click points around the outer edge, or choose **Use unit square**. Choose **Add hole** and click three vertices around the opening. Enter exact coordinates in the text box when needed. Each loop closes automatically. The dashed lines mark seams. A hole or outer boundary may cross a seam. Choose **Apply outline**. Invalid or intersecting loops leave the last successful trim in place. **Clear outline and holes** removes these loops while retaining the rectangular bounds.
5. **Export STEP** creates trimmed analytic faces sewn into one connected shell. **Prepare router relief** transfers the trimmed mesh. Review the stock-top envelope and cutter sampling in **3D relief** before generating a toolpath. Save the Studio project to retain the patch nets, continuity settings, crop and drawn loops.

A strip can contain 16 patches along one axis, with one polygonal UV outer loop and up to eight polygonal holes, each with 3–64 distinct vertices. A straight segment in UV follows the underlying 3D surface. The combined crop and loops must leave a connected surface. Branching patch networks and freeform spline trim curves remain outside this editor.

## Install STEP exchange

From the Studio checkout, activate its existing virtual environment and run:

```sh
python -m pip install ".[step]"
```

Restart the Studio server. The optional extra installs CadQuery's no-VTK bindings for Open CASCADE 7.9. Editing an original patch and preparing its router relief use the base install. STEP operations require the extra and a compatible wheel or build for the installed Python and OS. The browser, including a phone browser, uses the server's kernel.

## Exchange a part

1. Rebuild a solid in **3D CAD**. Open **Surfaces and STEP** and choose **Current solid part**.
2. Choose **Export STEP**. The export builds analytical boxes, cylinders, cones, spheres, extrusions and revolutions in feature order, including transforms, patterns and booleans. The regular solid viewport and CAM use a tessellation. Small numerical differences are expected on curved geometry.
3. Use **Import STEP** and choose a local `.step` or `.stp` file. Inspect the millimetre dimensions and counts of faces and solids. The translator interprets the file's declared length units. An imported reference retains its source inside the project and can be re-exported. This is geometry exchange. Feature history, PMI, materials, colours and product hierarchy are outside its scope.
4. For a file containing closed solids, choose **Add STEP to part**. The new step feature supports join/cut/intersect, position, rotation, patterns and suppression. Standard part sections and silhouettes then feed router, laser or knife CAM. The feature keeps the original STEP for later exact STEP export. Files containing open faces stay in the exchange workspace and can supply top relief.
5. Save the Studio project to retain its native history and exchange geometry. Import accepts up to 1.5 MB, 256 roots and 2,000 faces, with a 60,000-triangle preview limit. The portable project limit applies to the full project including encoded STEP files.

Run `python -m unittest discover -s tests -p test_cad_exchange.py` in the environment with the step extra to exercise source-unit conversion, transforms, patterns, curved-surface round trips, solid-feature booleans and malformed inputs.
