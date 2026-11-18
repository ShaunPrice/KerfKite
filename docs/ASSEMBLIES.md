# Assemblies, mates and interference

Open **3D CAD → Assembly mates & interference** in the running Studio app or hosted editor. You can fit a shaft to a bore, place a fixture plate or check the fit of several modelled components. Dimensions use millimetres and rotations use degrees.

## Try a bearing fit

1. Choose **Try bearing tutorial**. The demo opens a 40 × 30 × 8 mm plate and an 8 mm diameter shaft in a 10 mm bore. The shaft extends through the plate. The report should read **No interference detected**.
2. With **Shaft** selected, change **Parent origin XYZ** from `20, 15, 0` to `24, 15, 0` and choose **Solve & check**. Both parts turn red. The report identifies the interfering pair and gives their common volume in mm³.
3. Restore the parent origin to `20, 15, 0`. Set **Shift in datum XYZ** to `0, 0, 3` and solve. The shaft slides along the bore. Drag the view or focus it and use the arrow keys to orbit.
4. Choose **Save assembly to project**, then save or download the project from **Projects**. Choose **Close** to discard the working copy instead.

## Assemble your own parts

Build a part in **3D CAD**. Open the assembly and choose **Insert current solid** to capture its parametric feature history in the project. Give it a name and click **Rename part**. Repeat for additional parts. To feed a revised solid into an existing instance, select it and click **Replace selected geometry**. The part keeps its local datum and mates.

Independent parts have a world origin and XYZ rotation. To constrain a part, select it, choose its parent under **Mate to**, and enter datum origins and orientations in each part’s own coordinates. Each datum's Z axis defines its normal or joint axis. Rotations apply about X, then Y, then Z. Opposing face normals can be matched by rotating one reference frame by 180° about its local X or Y axis.

| Mate | Allowed motion |
| --- | --- |
| Fixed frames | Coincident origins and matching orientations. |
| Coincident planes | Translation along parent datum X and Y and rotation about Z. |
| Concentric axes | Translation along the common Z axis and rotation about Z. |
| Coincident points | Rotation about all three axes at the shared point. |

Moving a parent moves its descendants when you solve. The solver supports directed trees with one driving mate per child. It rejects cycles, duplicate assignments and input on locked axes. Independent roots give you multiple subassemblies in the same document. To release a mate, choose **Independent placement** and enter the desired world pose. Release child mates before removing their parent.

## Interpret an interference report

Studio uses bounding boxes to shortlist pairs, intersects the transformed manifold solids and measures any positive overlap over 0.000000001 mm³. Contact within 0.000001 mm is reported separately. Contact and overlap are computed on the solid kernel's tessellated geometry. Curves are approximated by facets. Fit tolerances should account for that approximation and the manufacturing process.

This check describes the assembled pose. Moving through intermediate positions, evaluating machine travel and toolholder clearance requires a separate motion analysis. The solver accepts up to 24 part instances and 150,000 triangles across the assembly. Project files store parametric inputs and mates; reopening the assembly rebuilds the preview.
