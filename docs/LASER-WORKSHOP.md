# Laser image workshop and material tests

Open **Images → Laser image engraving**. The preview maps black to the highest commanded power and white to zero. Pixels with zero opacity remain at zero power through every adjustment.

## Try a masked tonal wedge

1. Choose **Try tonal wedge**. A black-to-white gradient appears with a transparent square at its centre.
2. Under **Crop & mask**, crop 10% from the left, and choose **Ellipse**. Adjust **Gamma** to 2 and compare the midtones. Cropping resizes the remaining image to the destination width and height.
3. Choose **Floyd–Steinberg dither** and turn on **Invert**. Look at the corners and the centre square. Both stay white. Change **Threshold** to alter the balance of marked and unmarked samples.
4. Set the placement rectangles and pixel interval in millimetres. The work origin is at bottom left. Source crop percentages are measured from the indicated image edges. Allow room to either side for overscan.
5. Choose **Create raster paths**, then **Preview**. Scanlines alternate direction and travel across overscan with S0. Save the project under **Projects**. Reopening restores the generated paths, per-segment power, feed, maximum S, pass count and processing settings.
6. To use your own drawing as a mask, select a closed outline in the bed view, choose **Selected artwork** and create the raster. Nested contours produce holes by even–odd fill. The original artwork remains in the document. Disable its output if it should serve only as a mask.

**Recall selected raster settings** restores the processing controls for another pass. Select the original image file again when resampling a saved raster. The portable project contains the processed paths and recipe. Studio does not embed the input bitmap.

## Compare laser settings on one coupon

Expand **Laser material test** in the same Images tab. The grid places selectable samples in the work area with increasing power from left to right and increasing feed from bottom to top.

1. Enter the controller's full-scale S value. The software converts each percentage to the corresponding integer S command. Match this scale to your controller before physical use.
2. Pick the minimum and maximum feed, minimum and maximum power, and step counts. Start with a small grid. A 3 × 3 grid creates nine operations.
3. Choose square outlines to compare line marking or hatched patches to compare area engraving. Set the cell size and spacing, the grid origin and, for hatch, the line interval.
4. Click **Insert test grid**. Each object name records the feed, power percentage and S value. Generate a preview and inspect the commands. Download the project or save it to the library to retain a record of all samples.
5. To evaluate a real coupon, record its material and thickness, focus, actual controller scale and the acceptable cell. Those measurements determine the working preset for that material. The software preview shows commanded settings.

Raster processing supports 60,000 pixels and 2,500 scanlines per image. A material grid supports up to 10 steps per axis within the 100-operation job limit. Cropping, masks, generated rasters and outline/hatch test grids run locally in the browser.
