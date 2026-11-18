# KerfKite Studio | CAD and workshop release review

18 November 2026

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

The macOS ARM64 release harness passed all eleven scenarios, including **151 Python tests and 114 JavaScript tests**, with the optional STEP runtime installed. **21 Chromium workflow groups passed** against a hardware-disabled service. Separate repository checks passed on macOS ARM64 and Ubuntu ARM64 in a local container. The Windows, physical Raspberry Pi and phone results documented for previous versions do not validate these additions on those targets.

The [machine-readable simulation record](../validation/advanced-cad-simulation.json), [developer handoff](DEVELOPER.md), [interactive workshop entry points](../studio/static/machining-lessons.html) and [import compatibility guide](LIGHTBURN-IMPORT.md) describe how to repeat the checks.

## Publication and licensing

First-party software and documentation use MIT. The source distribution includes third-party font and decoder notices. The licence audit covers the bundled assets, a macOS wheel, an Android debug APK and a reference environment with 83 external Python distributions. Redistributing a complete binary runtime requires the notices and source arrangements applicable to its exact components, including Open CASCADE and GEOS. The public source edition excludes the narration and narrated video. [Licence inventory](LICENSING.md)

This public edition starts with a fresh source history. Its publication review covered source, validation records, media metadata, screenshot text and commit identity. The edition uses generic machine profiles and excludes private device configurations, recordings and provider receipts. Public copyright attribution remains. Credential-pattern scanning and manual review have limited coverage.

## Remaining scope

The assembly solver accepts directed mate hierarchies. Closed-loop mechanical constraint solving and general surface trimming or stitching remain open. STEP exchange covers geometry; CAD feature-tree, PMI and assembly-mate round trips remain open. Tool and holder clearance depends on the supplied work frame, envelopes and fixture measurements. Rotary collision simulation and machine-frame component collisions require additional models.

LightBurn compatibility covers documented subsets of the source formats. Lossless LightBurn round trips, some image effects, complete text shaping, proprietary controller protocols, camera lens-distortion correction and native commercial cutter-project formats remain open. Supervised physical commissioning is still required to measure cutting quality, clearance and stopping behaviour.
