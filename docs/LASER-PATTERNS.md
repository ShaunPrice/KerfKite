# Perforation, holding bridges and curved leads

Open **CAM**, choose **Laser** and choose **Line** for a centreline cut. Server-based CAM also supports patterns on inside and outside profiles. For those profiles, all pattern distances follow the kerf-compensated path.

| Cut pattern | Settings | Result |
| --- | --- | --- |
| Continuous cut | No pattern dimensions | Follow the whole vector at commanded power, including any programmed power ramps. |
| Perforation | Cut length and beam-off gap, in millimetres | Start each contour with the cut length. Alternate cutting and beam-off travel. The pattern continues around vertices. Its final interval ends at the contour endpoint. |
| Holding bridges | Beam-off gap in millimetres and number per closed contour | Divide each contour into equal lengths and centre one beam-off gap in each division. Each hole in the operation receives its own set of bridges. |

## Try a perforated rectangle

1. In **Design**, create a rectangle at least 10 mm from the bed edges. Open **CAM → Laser** and set **Cut pattern** to **Perforation**.
2. Enter a cut length of 3 mm and a gap of 1 mm. Generate the preview and inspect the breaks in the powered path. The G-code traverses each gap at feed rate with **S0**. Playback includes the beam-off moves. Each subsequent pass repeats the same gaps.
3. In **Layers**, choose **Use current CAM for layer** to give the selected layer this recipe. To give one object its own recipe, select it, change CAM settings, then choose **Assign current settings to selected**. Save the project to retain both recipes.
4. To retain a cutout, choose **Holding bridges**, enter the desired gap width and bridge count, and regenerate the preview. Check that a neighbouring path or an outer lead does not cut through a bridge. The programmed gap measures distance along the laser centreline. Actual retained width depends on kerf and material.

## Enter a contour with a curve

Under **Lead-in / lead-out**, choose **Straight tangent**, **Quarter arc · left of travel** or **Quarter arc · right of travel**. The selected side refers to the direction of motion at each end. A 3 mm curved lead has 3 mm of nominal arc length. Studio represents the quarter circle with short linear moves, with chord error bounded to 0.01 mm before G-code rounding.

Leads remain continuous outside the pattern. Start/end power ramps still follow the full path including the leads. During a programmed gap, power stays at zero even inside a ramp. Check the lead's location in the preview for each contour. Open a saved job to adjust the lead length or side when its stock layout changes.

Raster image powers and hatch-fill operations have their own power and spacing controls. For perforation and bridges use vector contour operations.
