# KerfKite Studio | CAD and workshop release review

## 0.4.1 pendant and workflow update · 19 November 2026

Pocket adds an optional **Keep screen awake** switch to its editor and pendant. A signed upgrade retained the previous version’s project. With the emulator set to a 10-second screen timeout, Pocket kept the foreground editor visible until the option was switched off. Leaving Pocket restored Android’s normal display behaviour. A physical Galaxy S23 and CNC were unavailable.

The GRBL simulator now exercises long confirmed feed holds and the remaining timeout budget on resume. DXF import handles planar block clipping before exposing artwork to CAM. In the regression drawing, a 100 mm source line clipped from X=10 to X=35 imports as a 25 mm path and survives project reopening at that size. See the [Android guide](ANDROID.md), [troubleshooting guide](TROUBLESHOOTING.md) and [program import coverage](PROGRAM-IMPORT.md).

## 0.4.0 workshop update · 19 November 2026

The new router operations generate explicit peck, ramp and helix moves. The laser can leave material bridges and generate a perforation pattern. A two-section parametric loft rebuilds from named dimensions and can feed linked machining geometry, STL and exact STEP exchange. The Android editor now keeps named drawings and can retain a desktop project for lossless re-export.

| Workflow checked | Observed result |
| --- | --- |
| Parametric loft | Increasing the circular outlet diameter by 4 mm changed the linked halfway section from 29 × 21 mm to 31 × 23 mm. Its dimensions and dependency survived a save/reopen cycle. |
| Production labels | An independent decoder read the rendered Code 39 and UTF-8 QR toolpaths. Material and engraving settings still determine contrast on the finished stock. |
| Router and laser | Browser tests checked four-hole peck drilling, measured hole depths, power-off gaps across laser bridges, compensated tab centres, closed-profile ramps and pocket helix entry. |
| Vector exchange | The editor preserved the bore and perimeter of a gear as separate SVG contours. A two-page vector PDF was reviewed at physical size before enabling output. |
| Pocket | A signed APK upgraded a previous installation and retained its editable draft. A grouped 228,565-byte desktop project with labelled parts and machining settings survived byte-for-byte re-export. Pendant tests used the Studio simulator. |

The [feature matrix](PRODUCT.md) records present scope. The [installer checks](INSTALLER-BUILDS.md) exercise separate desktop and SBC packages. The [interactive lessons](../studio/static/machining-lessons.html) and [simulation guide](SIMULATION.md) describe how to repeat workshop exercises without a machine. Physical cutting and handheld Galaxy S23 acceptance remain outstanding for this release.

## Earlier CAD release review · 18 November 2026

Public source repository: [github.com/ShaunPrice/KerfKite](https://github.com/ShaunPrice/KerfKite).

KerfKite Studio can fit parts in an assembly, edit freeform surfaces, exchange geometry with other CAD software and check a posted toolpath against modelled clamps. An operator can bring an existing laser or cutter design into the editor, inspect the conversion and choose which layers to machine.

## Workflows delivered

| Workflow | Operator capability | Verification |
| --- | --- | --- |
| Assembly | Constrain named parts with fixed, planar, concentric and point mates. Inspect contact and interference volume. | A shaft cleared its modelled bore. Moving its mating datum 4 mm introduced 175.02 mm³ of overlap. Returning it to centre restored clearance. |
| Surfaces and STEP | Edit Bézier and clamped B-spline/NURBS control nets, degrees and weights. Export solid and open-surface STEP, insert a closed imported body into a part, or transfer an open patch to top relief. | An open patch returned as one face. An analytic cylinder returned as three faces with volume 197.9203 mm³. Browser tests exercised editing, exchange, reopening and CAM transfer. |
| Machining clearance | Sweep the cutter and holder along supported posted three-axis moves against rectangular fixtures positioned in work coordinates. | A holder struck a clamp during a rapid between cuts. Relocating the clamp cleared the same 11 moves. Unsupported instructions produce a diagnostic. |
| Laser preparation | Crop and mask raster images, adjust tone, generate a feed/power test grid, and plan eligible line cuts within layer boundaries. | Editor tests preserved masked zero-power pixels, saved per-cell machining settings, placed an inner cut first and removed an eligible duplicate from the generated job. |
| Camera placement | Register a flat image to the bed from four landmarks, inspect an independent check point, overlay the image and place traced paths. | A synthetic target exercised projective registration, profile recall and trace placement. Accuracy with a physical lens and fixture remains unmeasured. |
| File interchange | Review LightBurn vector projects, supported preferences, materials, device setups and selected bundle members. Import DXF entities, G-code geometry, HP-GL pen paths and numeric LinuxCNC setup data. | A saved LightBurn project regenerated its laser power and feed. Replayed HP-GL produced 21 knife segments. Additional compatibility checks read public LightBurn vector, material and preference samples. |

## Test evidence

The public source release passed **all 11 harness groups on macOS ARM64, Windows 11 Pro x86-64 and a physical Raspberry Pi 5 ARM64**. Each run included STEP exchange, native C++ relief, **151 Python tests and 114 JavaScript tests**, with no regression tests skipped. **21 Chromium workflow groups passed on the Mac** against a hardware-disabled service. Separate repository checks passed on macOS ARM64 and Ubuntu ARM64 in a local container. Physical tool motion, Windows 10, the Galaxy S23 and the smaller Raspberry Pi configurations remain outside this release's verification.

The [machine-readable simulation record](../validation/advanced-cad-simulation.json), [developer handoff](DEVELOPER.md), [interactive workshop entry points](../studio/static/machining-lessons.html) and [import compatibility guide](LIGHTBURN-IMPORT.md) describe how to repeat the checks.

## Publication and licensing

First-party software and documentation use MIT. The source distribution includes third-party font and decoder notices. The licence audit covers the bundled assets, a macOS wheel, an Android debug APK and a reference environment with 83 external Python distributions. Redistributing a complete binary runtime requires the notices and source arrangements applicable to its exact components, including Open CASCADE and GEOS. The public source edition excludes the narration and narrated video. [Licence inventory](LICENSING.md)

This public edition starts with a fresh source history. Its publication review covered source, validation records, media metadata, screenshot text and commit identity. The edition uses generic machine profiles and excludes private device configurations, recordings and provider receipts. Public copyright attribution remains. Credential-pattern scanning and manual review have limited coverage.

## Scope recorded in the earlier review

For current scope see the [feature matrix](PRODUCT.md). At the time of the earlier review, the assembly solver accepted directed mate hierarchies. Closed-loop mechanical constraint solving and general surface trimming or stitching remain open. STEP exchange covers geometry; CAD feature-tree, PMI and assembly-mate round trips remain open. Tool and holder clearance depends on the supplied work frame, envelopes and fixture measurements. Rotary collision simulation and machine-frame component collisions require additional models.

LightBurn compatibility covers documented subsets of the source formats. Lossless LightBurn round trips, some image effects, complete text shaping, proprietary controller protocols, camera lens-distortion correction and native commercial cutter-project formats remain open. Supervised physical commissioning is still required to measure cutting quality, clearance and stopping behaviour.
