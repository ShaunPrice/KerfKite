# Import LightBurn artwork and settings

In **Projects → Import artwork and settings**, choose a file, review the drawing and open **Conversion details**. Choose **Add to project** to append the conversion. Existing artwork stays in place. In **Layers**, review the geometry and CAM settings before enabling Output. The imported setups are also available after saving and reopening the Studio project.

## Interactive badge tutorial

1. Start an empty project and choose **Try a sample**. The review shows a 30 × 18 mm rounded badge. The project still contains zero imported objects.
2. Leave **Laser S at 100%** at 1000 and choose **Add to project**. The Outline layer contains a 480 mm/min feed and power S350, converted from 8 mm/s and 35%. Choose the correct maximum S for your own controller before reviewing an actual file.
3. Open **Layers** and turn on Output for Outline. In the laser CAM workspace, preview the badge. Inspect the posted feed and S value. Save the project and reopen it under **Projects** to edit the layer again.
4. For a material library or device file, choose a preset in **Imported setup library**, read the converted values, then choose **Apply to workspace**. A device setup changes the drawing area. Firmware configuration and machine motion have separate controls.

| Source | Imported | Fidelity and review |
| --- | --- | --- |
| LBRN / LBRN2 XML | Rectangles, rounded corners, ellipses, expanded and packed line/cubic paths, shared geometry, nested group transforms, saved text outlines and named cut layers | Vectors become editable Studio polylines. Groups flatten. Text requires a saved backup path. Source mirror flags are reported for orientation checks. Bitmap shapes, unsupported shape types and sublayers require manual conversion. |
| CLB material XML | Labelled material/thickness/description presets with supported line and hatch settings | Review speed, maximum power, pass count, scan interval, angle and crosshatching. Unmapped process settings appear in conversion details. |
| LBDEV and LBPREFS JSON | Supported bed sizes, device cut lists and preferences cut lists | Only selected numeric workspace and CAM fields enter the project. Network credentials, scripts and private host paths are excluded. |
| LBSET / SETTINGS | GRBL text backups containing numeric XY travel | Work envelope only. Controller-specific binary settings need the originating software. |
| LBZIP user bundle | Choose individual supported project/settings members for review | The service inspects bounded ZIP members in memory. Select another member or reopen the bundle for further imports. |
| DXF, G-code, HP-GL and LinuxCNC INI | Drawing geometry, supported program paths and numeric setup references | See [program and cutter import](PROGRAM-IMPORT.md) for entity, command and unit limits. |
| STEP / STP, STL, SVG, raster and Studio projects | Dedicated editors or importers for each source | Open STEP in **3D CAD**, STL in **3D relief**, SVG in **Images** and native Studio documents in **Projects**. |

LightBurn files vary across controller families and releases. Preserve the source file. Source-disabled objects keep an additional per-object Output restriction. Studio stores converted settings in its own project format. Recreating every LightBurn effect and writing a lossless LightBurn project remain open work. Commercial cutter-native formats such as Silhouette STUDIO3 and Cricut Design Space projects require a supported export from the originating program.

LightBurn documents its supported project, device, preference and bundle types. [LightBurn File menu](https://docs.lightburnsoftware.com/2.1/Reference/UI/FileMenu/) · [LightBurn migration guide](https://docs.lightburnsoftware.com/latest/Guides/Migration/) · [LightBurn bundles](https://docs.lightburnsoftware.com/latest/Reference/UserBundles/)
