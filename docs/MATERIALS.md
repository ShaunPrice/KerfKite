# Materials guide

Open **CAM → Materials guide** to start with the tool selected in Studio. **3D relief → Materials guide** starts with the relief cutter. The guide opens in a separate tab so you can keep the drawing and its preview in place. You can also [open the guide directly](../studio/static/materials-guide.html) from a complete local copy of Studio. It works with a keyboard, a touchscreen or a mouse.

## Look up a piece of stock

1. Select the physical **Tool** and the intended **Operation**. The router has separate entries for flat, ball and V cutters. The laser entry describes a blue diode head, including the 25 W optical class. The knife entry covers a passive drag knife.
2. Search by stock name or common term. Use **Material family** and **Suitability** to narrow the list. Select a matching material to see the reason for its rating, setup considerations, inspection points and references. A material name on a receipt may omit an adhesive, coating or liner. Check the exact product with the supplier.
3. Choose a **Second material** to compare two stocks for the same operation. Changing the tool or operation updates both ratings.
4. **Sample candidate** means the stock is a reasonable choice for a coupon. **Conditional** means there are specific requirements to meet first. **Avoid** means choose a different material or process for that combination. The guide supplies no machine-calibrated feeds, speeds, depths, blade exposure or laser power. For router cutters, the [tool library](TOOL-LIBRARY.md) works out starting feeds and speeds to test.
5. With the desired material selected, use the browser's print command to make a workshop reference sheet. Manufacturer links need an internet connection. The catalogue itself is bundled with Studio.

## Try the guide

- **Vinyl decal.** Select the drag knife, choose the kiss-cut operation and find PVC vinyl. Inspect how the design separates from the liner. Leave the same material selected and change to the blue diode laser to see the material exclusion.
- **Acrylic sign.** Select the blue diode laser, choose cutting, find clear acrylic and compare with dark opaque acrylic. Read each reason, including the grade and colour requirements. Then change to the flat end mill to compare the same two stocks for a routed profile.
- **Carved lettering.** Choose a V-bit, find plywood and compare with a solid hardwood. Check the surface, veneer and required detail before laying out your lettering. Open the separate [machining lessons](../studio/static/machining-lessons.html) for toolpath playback.

The guide does not load CAM settings. To retain settings that you measured for a particular machine, enter them in the editor and use its existing material/tool preset controls.

For drilled holes, select **Drill bit** and compare **Single plunge** with **Full-retract peck drilling**. The cross sections show the relationship between tip depth and finished bore depth. Record your measured hole diameter, exit-face condition and observations in the sample notebook. See the [drilling exercise](DRILLING.md).

## Keep a sample notebook

Choose the material, tool and operation you tested. Enter a supplier or batch, optionally add measured thickness in millimetres, and record the machine, actual settings, inspection result and observations. **Save sample** keeps the entry with that combination. Use **Edit sample** to correct it or **Delete sample** to remove it. Avoid-rated combinations show existing observations with recording disabled.

**Export notebook** downloads every sample as one JSON file. **Import notebook** adds previously unseen sample IDs and preserves existing entries. The notebook holds up to 1,000 samples. It stays in this browser and is grouped by signed-in account in hosted Studio. Filesystem, simulator and hosted copies have separate browser storage. If storage is unavailable, the page offers a temporary notebook and shows a reminder to export it before closing. Use export to back up samples or take them to another workstation.
