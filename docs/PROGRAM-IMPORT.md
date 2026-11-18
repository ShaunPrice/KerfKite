# Bringing drawings, toolpaths and cutter plots into Studio

Use the project importer in the Studio editor to open a drawing from another program. Imported outlines retain their source coordinates in millimetres and start with output disabled. Check a known dimension in the editor, choose the installed head and material, and generate a new CAM preview before enabling any imported artwork for output.

| Source | How to export | What to check in Studio |
| --- | --- | --- |
| FreeCAD, AutoCAD, LibreCAD and other drawing tools | Save an ASCII model-space DXF. Outline text before exporting. Set drawing units or select them when importing. | Layer, colour, overall size, spline/arc contours and unsupported-entity counts |
| A router CAM postprocessor | Export an absolute or incremental three-axis text program as .nc, .ngc, .cnc, .tap or .gcode | Feed contours, separate rapid approaches, work origin, depth units and the named source feed presets |
| A plotter or vinyl design program | Export plain HP-GL as .plt, .hpgl or .hpg with standard unscaled coordinates | Pen layers, pen-up gaps, available velocity presets, knife offset and source force values |

## DXF contours

LINE, ARC, CIRCLE, ELLIPSE, SPLINE and ordinary 2D/XY polylines are converted to editable paths, including curved bulge segments and transformed nested block instances. The reader applies the drawing's declared insertion units. For a unitless drawing, **Unitless DXF** supplies the scale. Studio imports horizontal contours into the XY plane. Unsupported text, fills, dimensions, meshes and non-horizontal geometry appear in the skip report. Convert labels to outlines and hatches to boundaries in the source drawing if you need them. Layer colours and visibility carry into the import report. [ezdxf path documentation](https://ezdxf.readthedocs.io/en/stable/path.html)

## Posted CNC contours

The importer tracks G20/G21 units, G90/G91 positioning and modal G0/G1/G2/G3 commands in the XY plane. It reads I/J centres, signed R arcs, full circles with I/J, helical Z and G90.1/G91.1 arc-centre modes. Tool changes separate source tool layers. G0 travel separates contours. An initial feed move with unknown starting XY produces a warning. Establish the starting position explicitly for incremental motion. Within Studio's supported depth range of −50 to 0 mm, varying cutting Z stays attached to each vertex. Other known Z values are retained as reference metadata. Rapids, stationary drilling moves and dwell timing do not form artwork. Controller macros, rotary axes, cycles, probing, coordinate resets, unsupported modal instructions and other ambiguous geometry stop the import with a line number. [LinuxCNC G-code reference](https://www.linuxcnc.org/docs/html/gcode/g-code.html)

## HP-GL pen and knife settings

The reader supports IN, DF, SP, PA, PR, PU, PD, CI, AA and AR, with semicolons between instructions. Separate pen selections become layers. Drawn arcs use the same chord tolerance as other imported circular motions. VS velocity in cm/s produces a named mm/min feed preset when it fits Studio's feed range. FS values appear in the report in their source units. Review pressure on the target machine. Device-specific scaling, page coordinates, transformations, text and raster commands require conversion in the plotter software. Ordinary unscaled HP-GL uses 40 units per millimetre. [HP PCL Implementor's Guide, glossary](https://developers.hp.com/system/files/attachments/PCL%20Implementors%20Guide-0-glossary.pdf)

## Try a two-pen motif

Save the following text as `motif.plt` and open it using the project importer.

```text
IN;SP1;VS2;FS5;PU400,400;PD800,400,800,800,400,800,400,400;PU;SP2;PU600,600;CI80;PU;
```

The first pen makes a 10 × 10 mm square starting at X 10, Y 10. The second makes a 4 mm circle centred at X 15, Y 15. The review includes a 1200 mm/min source feed and a force value of 5 in source units. Select the knife head, inspect the outlines and adjust CAM parameters for the intended cutter.

## Size and performance

Imported artwork fits Studio's portable project limits of 3000 paths, 100000 vertices and coordinates within ±2000 mm. Circular toolpaths target 0.025 mm chord error. DXF uses ezdxf's path approximation and flattening with a 0.025 mm target. Convert very large drawings to the relevant work region and relocate distant survey or building coordinates before import.

## LinuxCNC workspace reference

Choose a self-contained LinuxCNC **.ini** file and select **Review import**. The reader uses explicit `[TRAJ]LINEAR_UNITS` and `[AXIS_X]` / `[AXIS_Y]` minimum and maximum soft limits. A workspace width equals maximum X minus minimum X, including machines with negative travel coordinates. Z range, axis velocity and acceleration appear as measured-unit reference values when present. Applying the imported workspace changes the Studio drawing dimensions. INI files using external include statements require a resolved copy with the axis limits in the same file. [LinuxCNC INI reference](https://linuxcnc.org/docs/stable/html/config/ini-config.html)

## LightBurn user bundles

Choose a **.lbzip** in **Projects → Import artwork and settings**, then select a supported member for review. Studio reads device profiles, preferences, material libraries, settings and project files present in the archive. Other bundle components are counted as skipped. An ordinary LightBurn bundle export omits standalone project files. Archives support up to 4 MB compressed, 256 members and 16 MB expanded in total, with up to 4 MB per supported member. The reader keeps archive data in memory. [LightBurn User Bundles](https://docs.lightburnsoftware.com/latest/Reference/UserBundles/)
