# Tool library and starting feeds

**CAM → Tool library · feeds and speeds** keeps a list of the cutters you own and works out starting settings for one of them in a material. **Use these settings** enters them in the CAM fields: cutter diameter, feed, plunge, spindle speed, step down and stepover, with the V-bit or drill angle where the tool has one. Nothing is sent to a machine, and the preview is cleared so that the next one uses the new values.

The values are a place to begin. They come from a calculation, not from a test on your machine. Cut a sample in scrap, listen to the cut, look at the chips and adjust. Record what worked in the [sample notebook](MATERIALS.md#keep-a-sample-notebook) or save it with **Save settings preset**.

## Choose a tool and a material

1. Switch **Head** to the router. The library is for router jobs.
2. Choose the **Tool**. The library starts with twelve common cutters: end mills of 2, 3.175, 6 and 6.35 mm, two ball end mills, three V bits, a surfacing bit and two drills.
3. Choose the **Material** and the **Cut**: a profile or slot, pocket clearing, or a light finishing pass.
4. Open **This machine** once and describe the machine: how rigid it is, the lowest and highest spindle speed, and the fastest feed at which it still cuts well. These are kept with the library.
5. Read the line below the lists. It gives the spindle speed, feed, plunge, depth for each pass, stepover and the chip each tooth takes, with a warning where one applies.
6. Choose **Use these settings**. Then set the total **Depth** for the job yourself: the library sets the step for each pass, not how deep the job goes.

The tool's name, shape and number are also entered in **Manual tool changes**, ready to assign to an object.

## How the values are worked out

- **Spindle speed.** Each material has a cutting speed at the edge of the cutter. The spindle speed is that speed divided by the cutter's circumference, kept inside the range of the machine.
- **Feed.** Spindle speed × flutes × chip load. The chip load is a fraction of the cutter's diameter that depends on the material, multiplied by 0.5 for a light machine, 0.75 for a medium one and 1 for a rigid one. A finishing pass takes 0.6 of the chip.
- **A feed limit.** When the machine cannot feed that fast, the feed is held at the limit and the spindle slows so that each tooth still takes its chip. If the spindle reaches its lowest speed first, the chip is thinner than intended, the cutter may rub and burn, and a warning says so. A cutter with fewer flutes is the usual remedy.
- **Depth for each pass.** A fraction of the diameter that depends on the material (half the diameter in softwood on a rigid machine, a tenth in aluminium), scaled by the rigidity.
- **Stepover.** 40% of a flat cutter for a pocket, 30% of a ball cutter, and 10% for a finishing pass. A surfacing bit steps over 60%.
- **Plunge.** A fraction of the feed: about a third in wood and plastics, a fifth in metal.
- **V bits.** A V bit is rated on the width it cuts at the job's depth, so a shallow engraving feeds more slowly than a deep carve.
- **Drills.** A drill only plunges. Its feed is the plunge, and it pecks by its own diameter.

| Material | Chip load, fraction of diameter | Cutting speed, m/min | Depth for each pass, fraction of diameter |
| --- | ---: | ---: | ---: |
| Softwood | 0.016 | 400 | 0.5 |
| Hardwood | 0.0125 | 400 | 0.4 |
| Plywood | 0.014 | 400 | 0.5 |
| MDF | 0.016 | 400 | 0.5 |
| Cast acrylic | 0.015 | 250 | 0.4 |
| HDPE and soft plastics | 0.019 | 250 | 0.5 |
| Rigid foam | 0.025 | 400 | 1 |
| Aluminium | 0.004 | 200 | 0.1 |
| Brass | 0.003 | 150 | 0.08 |
| Copper-clad board (FR-4) | 0.006 | 150 | 0.2 |

These figures are KerfKite's own conservative starting values for a rigid machine. They are not a cutter manufacturer's data. Where the maker of a cutter publishes a chip load for it, prefer that and enter the feed yourself.

## Keep your own tools

Open **Add or change a tool**, enter a name, shape, diameter and number of flutes, and choose **Save tool**. Saving under an existing name changes that tool. A V bit or drill also takes its included angle. An optional tool number is carried to **Manual tool changes**. **Delete tool** removes the selected tool.

**Export library…** downloads the tools and the machine description as one JSON file (`kerfkite-tools/1`). **Import library…** replaces the library with such a file. The library holds up to 200 tools. It stays in this browser, separately for each signed-in account in hosted Studio, so export it to move it to another computer.

## What has been checked

`tests/js/tool-library.test.cjs` checks the calculation against the formulas above, the limits of the machine, every starter tool in every material, and the refusals. `validation/tool_library_workflow.py` drives the panel in a browser against the simulator: it applies a tool, reads the spindle speed and feeds in the previewed program, saves a tool and finds it after a reload. No value has been confirmed by cutting on a machine.
