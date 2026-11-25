# Project wizards

**Templates → Project wizards** draws a standard project from a few dimensions. The parts are ordinary closed outlines, laid out in rows on the sheet with the **Gap between parts**, and added in one step that **Undo** reverses. In router mode each part carries an outside profile with the current CAM settings; in laser or knife mode the parts follow the CAM settings as they are.

Cut one joint in scrap of the same material first. Sheet material varies in thickness, and a joint is only as good as the thickness you entered.

## Finger-jointed box

Enter the **outside** width, depth and height, the **material thickness** and a **finger width**. The wizard makes a bottom, a front, a back and two sides, and with **With a lid** a top made like the bottom.

- Each edge is divided into an odd number of fingers as near your finger width as its length allows, so every edge starts and ends the same way.
- The bottom carries tabs on all four edges. The front and back carry tabs up their sides, and the sides fit between them. Every corner block of the box belongs to exactly one panel.
- **Fit allowance** leaves that gap on each side of every finger: each tab is narrower and each slot wider. Use a small positive value for a router and glue. For a laser, a negative value near the kerf tightens the joint.
- **Dog-bone the inside corners** relieves the corners of every slot for a router, using the CAM cutter diameter (see [dog-bone corners](JOINERY.md#dog-bone-corners)). Untick it for a laser. If the fingers are too tight for the cutter, the message says how many corners could not be relieved.

The message gives the number of fingers along each direction and the inside dimensions. A box whose fingers would be narrower than the material is thick is refused.

For a router, set the cut depth to the material thickness, add [holding tabs](USER-GUIDE.md) and use a cutter narrower than the fingers.

## Slot-together dividers

Enter the inside width and depth of the box or drawer, the divider height, the material thickness and the number of compartments across and deep. The wizard makes one strip for each division: strips across the width with slots from the top, and strips along the depth with slots from the bottom, each slot half the height deep. The message gives the size of one compartment. **Fit allowance** widens every slot by that much.

## Sign blank with hanging holes

Enter the width, height and corner radius, and a hole diameter and inset for two hanging holes near the top (a diameter of 0 leaves them out). The holes are added ahead of the outline so that they are cut first, with an inside profile in router mode.

## What has been checked

`tests/js/project-wizards.test.cjs` checks that the panels of a box hold exactly the material of its shell (their areas times the thickness equal the outside volume less the cavity), that mating edges are complementary along every joint, the fit allowance, the divider slots and the sign's outline. `validation/joinery_workflow.py` adds each project in a browser and previews the box to full depth on the simulator. No project has been cut and assembled.
