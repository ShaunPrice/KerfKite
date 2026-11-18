# Rotary engraving and cylindrical machining

Open **Rotary** in the machining tabs to wrap a flat design around a cylinder. A full turn consumes π × stock diameter millimetres of artwork. One design coordinate becomes a continuous angle. The other coordinate runs along the cylinder. A router can vary cut depth. A laser can vary power by segment, including paths from artwork with per-vertex power values.

## Simulate a full-turn border

1. In **CAM**, select **Router**, a line engraving operation and the **grblHAL** protocol. Start with a new design. In **Rotary**, enter a **Cylinder diameter** of `50` mm, choose **Y → angle**, **grblHAL** and **A around X**. One revolution is about `157.0796` mm along design Y. **Set design span to one revolution** changes the design area.
2. Set **Periodic border cycles** to `3` and choose **Add seamless periodic border**. This creates one sinusoidal path with matching end position and tangent. **Check seam endpoints** compares both edges of the output paths. For a laser it compares terminal power, and for a router it compares depth. It checks pairs within 0.01 mm in position/depth and half a controller power unit. For arbitrary artwork inspect tangency separately.
3. Choose **Preview wrapped job**. Inspect the angular range and the posted rotary words. Drag **Inspect around cylinder**, or use **Play simulation** and the percentage slider. The darkened side faces the viewer. The dashed line marks the seam. The display samples large jobs, while preview export retains every posted block. Use the posted program to inspect individual segments. The slider measures progress through cutting segments rather than real machine time.
4. Select **Reverse** or change the starting angle. The old preview is invalidated. Preview again. For example a single positive turn with a 15° offset runs from 15° to 375°. Reversing it gives an angle range from −345° to 15°. Crossing the seam retains the continuous angle. The post snaps rounded source coordinates within 0.00051 mm of a whole circumference to exact turns.
5. For an offline result select **Export rotary G-code** after preview. To exercise the same path through the workstation sender, open **Control**, choose **Simulator**, and connect with the matching grblHAL protocol. Return to **Rotary**, review the independent-axis setup and choose **Stream reviewed rotary job**. The target notice identifies the simulator. Use the ordinary workstation confirmation as well when using **Run** or **Arm phone start** in Control. Phone start requires the workstation's current-preview challenge. Editing the rotary setup or artwork cancels the previous review.

## Router depth and laser power

For a variable-depth router path, import or create the depth-bearing artwork, select its machining operation and open the rotary preview. **Nominal stock surface** computes G93 block duration from the unwrapped artwork, axial travel and depth change. **At cutting radius** accounts for the changing radius along each coordinated block. Compare the `F` words at the two settings. A cut reaching the cylinder centre is rejected.

For laser engraving, select **Laser** in CAM and a grblHAL or supported xPRO V5 rotary target. Retain the per-vertex power in the artwork and enable the intended layers for output. The posted program keeps dynamic `M4` and per-segment `S` values. Laser rapids require `M5`. Use the preview to check the wrap direction, seam placement and power transitions before exporting. A repeated pass at the same location changes the exposure. Power intensity in the drawing is a simulated command value; the actual mark depends on the laser, focus, feed and stock.

## Axis and postprocessor selection

| Target | Axis and working plane | Output available in Studio |
| --- | --- | --- |
| grblHAL with independent rotary drive | A about X with radial Z, B about Y with radial Z, or C about Z with radial X. C requires an appropriate side-facing tool or laser fixture. | Router and dynamic laser preview, export and qualified workstation streaming. |
| xPRO V5 independent-A firmware | A about X with radial Z. Requires vendor `CNC_xPRO_V5_XYAZ_NC` or `CNC_xPRO_V5_XYAZ_NO` readback, four reported coordinates and an independently calibrated A drive. | Router and dynamic laser preview, export and qualified workstation streaming. |
| LinuxCNC | A, B or C with the axis mapping above. Configure the chosen rotary joint and controller limits. | Router preview and `.ngc` file export for loading in LinuxCNC. |

**An xPRO V5 configured for independent X, two Y motors and Z already uses four motor outputs.** The secondary Y gantry drive counts as Y in that configuration. The xPRO firmware family also includes a separate independent-A configuration. Only select that target for hardware with the independent motor channel, correct physical geometry and degree calibration. Studio reads controller identification, reported axis count and settings before permitting rotary streaming. A profile selection cannot change the controller wiring or firmware. A simulator result does not verify the installed firmware or machine calibration. [xPRO V5 firmware and hardware documentation](https://github.com/Spark-Concepts/xPro-V5) · [Vendor firmware source](https://github.com/Spark-Concepts/xPro-V5-Firmware) · [Maker Store xPRO V5 hardware](https://www.makerstore.com.au/product/elec-scxprov5/)

For xPRO V5, Studio accepts the exact controller identity `CNC_xPRO_V5_XYAZ_NC` or `CNC_xPRO_V5_XYAZ_NO`, four coordinates in status, and positive A-axis resolution, speed and acceleration settings (`$103`, `$113` and `$123`). XYAZ capability alone cannot establish what is physically connected. Rotary laser streaming also requires `Spindle/Type=LASER` and a positive `Laser/FullPower` readback. The posted laser commands must fit that laser-specific power scale. [Vendor XYAZ machine definition](https://github.com/Spark-Concepts/xPro-V5-Firmware/blob/9fdb6695a21f56a5147cbe10508b142504b5e9a3/CNCxPROv5_20220808/src/Machines/CNC_xPRO_V5_XYAZ_NC.h) · [Vendor status reporting](https://github.com/Spark-Concepts/xPro-V5-Firmware/blob/9fdb6695a21f56a5147cbe10508b142504b5e9a3/CNCxPROv5_20220808/src/Report.cpp) · [Vendor laser implementation](https://github.com/Spark-Concepts/xPro-V5-Firmware/blob/9fdb6695a21f56a5147cbe10508b142504b5e9a3/CNCxPROv5_20220808/src/Spindles/Laser.cpp)

At the machine, confirm the rotary centreline, axial work zero, start angle, surface focus or radial Z/X tool zero as appropriate. Check the independent rotary drive direction, travel limits and units. **Check fixture clearance envelope** applies a tool-tip axial interval and protected core radius to posted destinations, including rapids. Enter measured limits for your fixture. This geometric envelope does not model holder length, chuck jaws, clamps, runout, backlash or stopping distance.

The post uses G93 inverse-time feed on each nonzero cutting block. A G93 `F` value is the reciprocal of the programmed block duration in minutes. The program restores G94 at the end. The xPRO V5 post uses G94 for rapids and establishes G93 before subsequent cuts. [LinuxCNC G93/G94 reference](https://www.linuxcnc.org/docs/stable/html/gcode/g-code.html#gcode:g93-g94-g95)

## Saved projects and troubleshooting

Rotary stock, wrap direction, drive axes, fixture and feed reference are kept in the Studio project. Reopen the project and rebuild the preview before a new run. Use **CAM → Generate preview** when you intentionally want the planar program again. An exported `.nc` or `.ngc` contains the posted program, while the Studio project retains the editable artwork and setup.

| Symptom | What to inspect |
| --- | --- |
| The seam check finds different ends | Match path count, axial position, depth or power at the two edges of the one-turn design. Include only the layers intended for this job. |
| Size or angle differs on the stock | Verify actual diameter, full-turn design span, degree calibration, drive direction and work zero. |
| Preview succeeds and Stream is unavailable | Check the selected post, machine protocol, local workstation service, idle connection and reported independent axis. The browser file editor and hosted design editor can preview and export. |
| Protected-core or axial-window refusal | Inspect cutter-tip radius, depth, axial position and rapid clearances against the fixture limits. |
| Multiple turns appear overlaid in simulation | Angles separated by one turn project to the same physical spot. Inspect the angular range and posted lines for the continuous value. |
