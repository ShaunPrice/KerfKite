# Bringing drawings, toolpaths and cutter plots into Studio

Use the project importer in the Studio editor to open a drawing from another program. Imported outlines retain their source coordinates in millimetres and start with output disabled. Check a known dimension in the editor, choose the installed head and material, and generate a new CAM preview before enabling any imported artwork for output.

| Source | How to export | What to check in Studio |
| --- | --- | --- |
| FreeCAD, AutoCAD, LibreCAD and other drawing tools | Save an ASCII model-space DXF. Outline text before exporting. Set drawing units or select them when importing. | Layer, colour, overall size, spline/arc contours and unsupported-entity counts |
| A vector illustration or dimensioned PDF | Export paths in a PDF with known page units. Outline text, expand dashes, and unite overlapping fill regions in the source. | Review one page at a time, check a known length and inspect omitted content. |
| A router CAM postprocessor | Export an absolute or incremental three-axis text program as .nc, .ngc, .cnc, .tap or .gcode | Feed contours, separate rapid approaches, work origin, depth units and the named source feed presets |
| A plotter or vinyl design program | Export plain HP-GL as .plt, .hpgl or .hpg with standard unscaled coordinates | Pen layers, pen-up gaps, available velocity presets, knife offset and source force values |

## Vector PDF pages

In **Projects → Import artwork and settings**, choose a PDF up to 4 MB and use **PDF page** to select the page. The displayed page size uses the crop box, page rotation and document units. A page with ordinary 72-point inches therefore imports a 72-point line as 25.4 mm. The lower-left of the displayed crop is the drawing origin. Review the preview and **Conversion details**, then choose **Add to project**. The new layer starts with Output off.

Straight lines, rectangles, closed paths and cubic Béziers retain their transformed geometry. Strokes become centrelines. Simple and nested filled outlines use the PDF's even–odd or nonzero fill rule. Transformed vector forms can be read recursively. A new move in the source begins a separate contour. Stacked objects may cover paths in the printed illustration. Inspect each layer before machining.

The review lists omitted text, images, shades, dashed strokes, hidden content, masked forms and unsupported areas of clipping. Paint that crosses a clipping boundary is omitted in full. Convert labels and dashes to paths, apply clips and resolve overlapping fill outlines in the drawing program before re-export. Password-protected PDFs require an unencrypted export. For scanned artwork, import the image using the image engraving or camera tracing tools.

The workstation and the authenticated hosted editor read PDFs with Studio's native core. It reads the files producers write, makes the repairs PDF readers commonly make to damaged ones and skips inline images; it refuses encrypted files, and what it cannot read, with a request for a simplified, unencrypted vector PDF. For offline browser editing, save the converted Studio project and reopen it in the browser editor.

## DXF contours

LINE, ARC, CIRCLE, ELLIPSE, SPLINE and ordinary 2D/XY polylines are converted to editable paths, including curved bulge segments and transformed nested block instances. The reader applies the drawing's declared insertion units. For a unitless drawing, **Unitless DXF** supplies the scale. Studio imports horizontal contours into the XY plane. Unsupported text, fills, dimensions, meshes and non-horizontal geometry appear in the skip report. Convert labels to outlines and hatches to boundaries in the source drawing if you need them. Layer colours and visibility carry into the import report. [ezdxf path documentation](https://ezdxf.readthedocs.io/en/stable/path.html)

For a block cropped with **XCLIP**, Studio retains the visible parts of each outline. If a boundary crosses the same line twice, the separated pieces stay separate in the editor. Planar rectangles and simple polygons up to 256 corners support rotated, scaled, nested and arrayed block references. Turning off the clip in the source restores the full block. Inverted clips, 3D clipping planes and unsupported coordinate frames skip the affected instance and appear in the review warnings. To bring one of these into Studio, export the visible outlines as ordinary XY geometry. [DXF spatial filters](https://ezdxf.readthedocs.io/en/stable/dxfobjects/spatial_filter.html)

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

Imported artwork fits Studio's portable project limits of 3000 paths, 100000 vertices and coordinates within ±2000 mm. Circular toolpaths target 0.025 mm chord error. DXF curves are flattened to the same 0.025 mm target. Convert very large drawings to the relevant work region and relocate distant survey or building coordinates before import.

G-code, HP-GL and LinuxCNC INI files are read by Studio's native core, which gives the same artwork, presets, warnings and messages, to the last digit, as the Python reader it replaced. DXF drawings are read by the core's own DXF reader, which divides curves into equal chords within 0.025 mm of the true curve and draws rational, low-degree and closed splines exactly. Program text that is not plain ASCII is read with Python's rules for case, spaces and digits, as the Python reader read it, and DXF splines given only by fit points are interpolated as that reader interpolated them. See [Foreign drawings](NATIVE.md#foreign-drawings).

## LinuxCNC workspace reference

Choose a self-contained LinuxCNC **.ini** file and select **Review import**. The reader uses explicit `[TRAJ]LINEAR_UNITS` and `[AXIS_X]` / `[AXIS_Y]` minimum and maximum soft limits. A workspace width equals maximum X minus minimum X, including machines with negative travel coordinates. Z range, axis velocity and acceleration appear as measured-unit reference values when present. Applying the imported workspace changes the Studio drawing dimensions. INI files using external include statements require a resolved copy with the axis limits in the same file. [LinuxCNC INI reference](https://linuxcnc.org/docs/stable/html/config/ini-config.html)

## LightBurn user bundles

Choose a **.lbzip** in **Projects → Import artwork and settings**, then select a supported member for review. Studio reads device profiles, preferences, material libraries, settings and project files present in the archive. Other bundle components are counted as skipped. An ordinary LightBurn bundle export omits standalone project files. Archives support up to 4 MB compressed, 256 members and 16 MB expanded in total, with up to 4 MB per supported member. The reader keeps archive data in memory. [LightBurn User Bundles](https://docs.lightburnsoftware.com/latest/Reference/UserBundles/)
