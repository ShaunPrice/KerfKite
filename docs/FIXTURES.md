# Checking tool and holder clearance

The **Fixture clearance** panel in **CAM** checks the tool and holder against measured clamp boxes along every move in the generated XYZ program. Use the app server or hosted editor. Each result identifies the G-code line, rapid or feed motion, fixture, component and first tool-tip contact position in millimetres.

## Try a rapid that crosses a clamp

1. Open **CAM → Fixture clearance → Try the clamp clearance lesson**. Click **Run lesson simulation** with the clamp at Y 21 mm.
2. The two cuts lie on either side of X 30 mm. During the rapid between them, the 12 mm holder on the 2 mm cutter contacts the clamp. The reported line is a G0 move. At contact the tool tip is still around X 24.08 mm.
3. Enter **40** for the clamp's minimum Y and run the lesson again. The report now shows clearance on the same posted program. This lesson uses its own CAM project and fixture dimensions.

## Check a design

1. Generate a fresh toolpath preview. Measure the tool envelope diameter, exposed length from tip to holder base, holder diameter and modelled holder length. Enter these dimensions in **Mounted head**. Use a diameter enclosing all flutes and shank features within the modelled tool length. For a knife or laser head, choose conservative envelope dimensions enclosing the mounted assembly.
2. Enter the tool tip's actual **starting XYZ** in the work coordinate frame used by the program. Studio's posted moves use absolute millimetres. Stock top is normally Z 0. For a fixed-height laser, the entered Z remains the height for the entire job.
3. Click **Add clamp**. Give it a recognizable name. Enter its minimum and maximum X, Y and Z, measured from the same work origin. For irregular or angled clamps, enclose them in one or more axis-aligned boxes. Include clamp bolts and other workholding projections.
4. Choose **Check generated preview**. Review the first reported line in **Generated G-code**. Reposition the fixture or change the programmed path or travel clearance, regenerate as needed, then repeat the check. Studio saves the setup with the project. Recheck starting position and mounting dimensions after reopening, remounting the head or changing work offsets.

## What the result covers

The calculation sweeps upright, flat-ended circular tool and holder envelopes continuously along each straight move, including the first move from the entered position, rapids, plunges and retracts. Contact at a face, edge or corner counts. The result reports the total number of motion–fixture–component contacts and lists the first 100 in program order.

The checker reads the posted G0/G1 lines and explicit setup words emitted by Studio's three-axis profiles. Unsupported motion or coordinate-state instructions stop the check with a line number. Rotary motion, arcs, machine-specific rapid doglegs, machine-frame offsets, runout, flexible tooling, stock removal and machine-frame geometry are outside this model. A clear result covers only the fixture boxes, tool envelopes, initial position and straight travel supplied. It provides an offline planning check. It does not command the controller or interlock the Run control.
