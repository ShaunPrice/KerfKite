# Register a camera image to the machine bed

Under **Images**, the **Camera bed registration** panel places a frozen camera image and its traced paths in millimetres on the design bed. The mapping uses a four-point planar homography. Camera access is optional and begins only when you press **Open camera**. You may supply an image file instead.

## Try the registration card

1. Click **Try registration card**. The synthetic image contains four turquoise targets and a dark rectangle. The panel solves a perspective transformation mapping the targets to the bed corners.
2. Adjust **Bed overlay opacity** to compare the rectified image and the design grid.
3. In **Image & camera line tracing**, choose **Closed shape outlines**, set threshold to 128 with inversion off, and click **Preview trace**.
4. Return to **Camera bed registration** and select **Place previewed trace on bed**. The new rectangle outline follows the dark rectangle on the rectified bed overlay. Change the camera marker placement and solve again to see how registration affects the placement of a subsequent trace.
5. Use **Projects** to save the registered artwork. The geometric paths survive a project round trip. The camera photograph and calibration overlay are session-only.

## Register your setup

Place four targets on the same plane as the artwork, spread around the region to be traced. Measure each target's X and Y in the work coordinate system. Select the correct machine profile and bed size. In **Image & camera line tracing**, load a still image or open the camera and capture a frame.

Select marker **1**, click the corresponding target in the frozen registration image, and enter its measured **Work X** and **Work Y**. Repeat for markers **2–4**. Image pixel coordinates can also be typed. Use **Solve registration**. To check accuracy, select **Independent check point**, click a fifth landmark, enter its measured work coordinates and choose **Measure check error**. The reported error in millimetres is specific to that checkpoint.

**Save for this profile** stores landmark coordinates and bed dimensions in this browser for the active account and selected machine profile. For subsequent frames from the same fixed camera, use **Recall profile calibration**. Recall checks the work area and image aspect ratio. Recalibrate when the camera position, zoom, crop, work origin or stock surface height changes. Images of the same aspect ratio can still have different optics or framing.

The overlay covers the quadrilateral enclosed by the measured targets. Registered tracing rejects contours extending outside it. **Add preview to project** in the basic tracing panel uses numeric width, height and shape placement. **Place previewed trace on bed** uses the measured camera mapping.

The trace input is downsampled to at most 384 pixels per side. The transformation corrects perspective on a flat surface. Lens distortion, parallax over raised parts, camera movement and physical registration accuracy require separate measurements. General lens undistortion and 3D camera calibration remain open.
