# KerfKite Studio

KerfKite Studio is an open-source CAD/CAM workbench for interchangeable CNC router, laser and drag-knife heads. Design in a browser, simulate a job, and prepare output for a supported controller. The Python application runs locally or as an authenticated HTTPS editor. KerfKite Pocket on Android includes an offline sketch editor and a CNC pendant paired with the Studio server.

[![KerfKite’s desktop editor showing wattle, banksia and scarlet gum tags on a shared sheet, with separate layers for the outlines, engraving and lettering](studio/static/screen-desktop-workspace.png)](studio/static/screen-desktop-workspace.png)

Three botanical tags on one sheet in Studio 0.4.0. The **Layers** tab separates engraving, lettering and cut outlines.

## Install KerfKite

Choose the installer for the computer where you want to design. First launch needs an internet connection to set up the desktop tools. Choose **Native desktop → Start workshop** to open the simulator in its own application window. Leave **Open directly next time** selected to start there on later launches.

| Device | Download | How to open it |
| --- | --- | --- |
| Windows 10 or 11, 64-bit Intel/AMD | [Windows setup wizard](https://github.com/ShaunPrice/KerfKite/releases/download/v0.5.0/KerfKite-Windows-x64-Setup.exe) | Run Setup, then open **KerfKite Studio** from Start. |
| Mac | [Apple silicon](https://github.com/ShaunPrice/KerfKite/releases/download/v0.5.0/KerfKite-macOS-Apple-Silicon.dmg) · [Intel](https://github.com/ShaunPrice/KerfKite/releases/download/v0.5.0/KerfKite-macOS-Intel.dmg) | Open the disk image, drag **KerfKite Studio** to **Applications**, then open it there. |
| Raspberry Pi OS, 64-bit, or Debian/Ubuntu ARM64 | [ARM64 installer](https://github.com/ShaunPrice/KerfKite/releases/download/v0.5.0/KerfKite-Linux-arm64.deb) | Open the package in the desktop software installer, then launch **KerfKite Studio** from the applications menu. |
| Debian/Ubuntu, 64-bit Intel/AMD | [AMD64 installer](https://github.com/ShaunPrice/KerfKite/releases/download/v0.5.0/KerfKite-Linux-amd64.deb) | Open the package in the desktop software installer. |
| Android, including Samsung Galaxy S23 | [KerfKite Pocket APK](https://github.com/ShaunPrice/KerfKite/releases/download/v0.5.0/KerfKite-Pocket-Android.apk) | Open the download on the phone and tap **Install**. Choose **CNC pendant** to pair with Studio. |

The [installation guide](docs/INSTALL.md) covers first launch, security prompts, updates, removal and optional STEP exchange and controller access, and the source install for HTTPS and MCP services. The [release downloads](https://github.com/ShaunPrice/KerfKite/releases/tag/v0.5.0) also include portable Linux archives and SHA-256 checksums.

For Orange Pi, Radxa, Jetson, BeagleBoard, ODROID, LattePanda and RISC-V systems, the [single-board computer guide](docs/SBC.md) maps each board to its installer. It also includes a browser-only edition for drawing and file export on smaller boards.

## Applications and features

The desktop window has system file dialogs, undo and redo, an object list, and numeric position and size controls for drawing. Add a rectangle, ellipse or line, select it, then move it in millimetres. Preview the toolpath before exporting G-code. The full 3D CAD workspace, CAM settings and stock simulation open inside the same window. [Desktop walkthrough](docs/DESKTOP.md)

[![GTK desktop with a pair of involute gears on the canvas and the selected gear in the native object inspector](studio/static/screen-native-linux.png)](studio/static/screen-native-linux.png)

The Linux native window running on ARM64, with the 18-tooth gear selected.

The following screenshots show the Studio 0.4.0 editor and the signed Pocket 0.4.1 Android app. Open an image for the full-size view.

| Parametric solid CAD | Router stock simulation |
| --- | --- |
| [![Solid modeller showing a hollow duct adapter with four mounting holes and its selected two-section loft](studio/static/screen-desktop-solid-cad.png)](studio/static/screen-desktop-solid-cad.png) | [![Oblique stock simulation showing scalloped clearing passes around an uncut circular island beside the router CAM settings](studio/static/screen-desktop-router.png)](studio/static/screen-desktop-router.png) |
| A loft joins a rectangular inlet to an offset round outlet. Its base dimensions remain editable in the feature history. [Solid CAD guide](docs/SOLID-CAD.md) | A 4 mm pocket leaves a central island in a scalloped coaster. The preview shows the cutter path against the remaining stock. [Simulation guide](docs/SIMULATION.md) |

| Artwork library | Interactive materials guide |
| --- | --- |
| [![Template library filtered to ten Australian wildflower designs, including wattle, banksia and scarlet gum](studio/static/screen-template-library.png)](studio/static/screen-template-library.png) | [![Blue diode laser and surface engraving selected in the guide for dark anodised aluminium, with a diagram of the marked surface](studio/static/screen-desktop-materials.png)](studio/static/screen-desktop-materials.png) |
| Select a template to place its editable paths in the drawing. [Stock catalogue](docs/TEMPLATE-CATALOGUE.md) | Pick the tool, operation and stock to see the material-specific conditions before making a test sample. [Materials guide](docs/MATERIALS.md) |

### KerfKite Pocket on Android

| Native project editor | Paired CNC pendant |
| --- | --- |
| [![Pocket editor showing a four-object alpine trail badge, the selected ridgeline, and Studio project and SVG export controls](studio/static/screen-pocket-project.png)](studio/static/screen-pocket-project.png) | [![Pocket pendant paired to an idle GRBL simulator with a 0.1 mm jog increment, axis controls and an armed job](studio/static/screen-pocket-pendant.png)](studio/static/screen-pocket-pendant.png) |
| Edit a drawing offline, reopen a named project or export the geometry as SVG. [Pocket guide](docs/ANDROID.md) | Jog or operate a prepared job through the paired Studio server. Enable **Keep screen awake** to keep the pendant visible while you work. This capture shows an Android emulator connected to the machine simulator. [Pairing guide](docs/PHONE-CONNECTIONS.md) |

## Inside the workshop

* Parametric parts, sketches, Boolean features, assembly mates, interference checks, Bézier/NURBS surface editing and STEP exchange.
* Layer-based laser editing, image processing, material test grids, camera registration and 1,768 original vector templates.
* Router relief, profile and pocket operations, drag-knife paths, rotary file export and simulated fixture clearance.
* Reviewed imports for LightBurn projects and settings, DXF drawings, G-code, HP-GL cutter programs and LinuxCNC workspace settings.

See the [release report (PDF)](docs/KerfKite-Release-Review.pdf), [file compatibility](docs/LIGHTBURN-IMPORT.md), [controller matrix](docs/PROTOCOLS.md) and [platforms](docs/PLATFORMS.md) for tested scope and open work. The machine limits in the public profiles are example dimensions. Enter your measured setup before preparing a physical job.

To trim two crossing lines, open the [geometry guide](docs/GEOMETRY.md). In a solid sketch, you can [trim a crossing, dimension a fillet or constrain a profile](docs/SKETCH.md). A rebuilt solid can update linked machining contours, sections and reliefs. See [solid CAD](docs/SOLID-CAD.md).

For phone control, open **Control → Phone pendant** on the Studio workstation and choose **Enable phone pairing**. Use the private pairing link in Pocket. Wi-Fi, USB tethering and Bluetooth tethering all connect the phone to the same server. See the [connection guide](docs/PHONE-CONNECTIONS.md).

## Workshop tools in 0.4

* **Router.** Create a single hole or drilling pattern, choose a peck depth, place holding tabs directly on a compensated contour, and use ramp or helical entry on eligible toolpaths. [Drilling](docs/DRILLING.md) · [Router entry](docs/ROUTER-ENTRY.md)
* **Laser and layout.** Place perforations or holding bridges, choose straight or curved leads, move layout groups and snap to a grid or visible vertices. [Laser patterns](docs/LASER-PATTERNS.md) · [Layout tools](docs/GEOMETRY.md)
* **Lettering and labels.** Engrave centreline lettering with Workshop Mono, select installed fonts where supported, and create numbered text, Code 39 or QR labels with a live proof. [Production labels](docs/LABELS.md)
* **CAD, import and camera.** Edit dimension-driven Bézier sketches, loft between two sections, inspect vector PDF pages, import compound SVG paths at physical scale and fit radial lens correction to measured targets. [Solid CAD](docs/SOLID-CAD.md) · [Sketches](docs/SKETCH.md) · [Import guide](docs/PROGRAM-IMPORT.md) · [Camera setup](docs/CAMERA-REGISTER.md)
* **Workshop setup.** Store measured machine profiles and estimate cycle time from programmed feeds, rapids, dwell and a measured sample. Pocket can reopen portable projects and manage named drafts. [User guide](docs/USER-GUIDE.md) · [Pocket](docs/ANDROID.md)

## Stock artwork

Browse **Templates → Template library** to insert one of **1,768 editable designs across 132 categories**. The collection includes Australian wildflowers, natural-history plates, woodcut landscapes, workshop tools, storybook illustrations, decorative panels and stencil lettering.

[![Twelve examples from the stock library, including native flowers, a coastal print, a twin-lens camera and a ginkgo earring](studio/static/stock-artwork-preview.png)](docs/TEMPLATE-CATALOGUE.md)

For an earlier Studio installation or another vector editor, [download the SVG library](https://github.com/ShaunPrice/KerfKite/releases/download/v0.5.0/KerfKite-Stock-Artwork.zip). Extract it and open **index.html** to search the gallery or browse the SVG files by category. The [template catalogue](docs/TEMPLATE-CATALOGUE.md) lists every category and its design count.

## Materials guide

Open **CAM → Materials guide** to choose stock for a drill bit, flat end mill, ball end mill, V-bit, blue diode laser or drag knife. Compare two materials for the same operation and keep your measured sample results in a portable notebook. To use the guide on its own, [download the single-file materials guide](https://github.com/ShaunPrice/KerfKite/releases/download/v0.5.0/KerfKite-Materials-Guide.html) and open it in your browser. [Guide and workshop examples](docs/MATERIALS.md)

## CNC simulations

![Layered kingfisher cut-vinyl decal beside laser-engraved reeds on maple and a router-carved wave relief in walnut](studio/static/workshop-artwork.jpg)

*Workshop illustration of the three processes: cut vinyl, laser engraving and router carving.*

**Router stock removal.** The purple lines show three 1 mm depth passes around a 20-tooth gear and its centre bore. The tan surface shows the sampled stock after a 2 mm flat end mill has followed the contours. See the stock from several angles in the [router simulation video (MP4, 7 seconds)](studio/static/simulation-router.mp4).

![Oblique view of simulated gear stock showing contour passes and the central bore](studio/static/simulation-router.png)

| Laser engraving | Drag-knife cutting |
| --- | --- |
| ![Laser lesson with a colour-coded entry ramp along the sample outline](studio/static/simulation-laser.png) | ![Drag-knife lesson showing compensated corner moves around the sample outline](studio/static/simulation-knife.png) |
| Power rises from zero over the first 5 mm of travel. [Watch the laser toolpath (MP4, 8 seconds)](studio/static/simulation-laser.mp4). | The pivot travels around each corner to turn a blade with a 0.5 mm offset. [Watch the knife toolpath (MP4, 8 seconds)](studio/static/simulation-knife.mp4). |

These captures come from KerfKite running in simulator mode. To change the settings and replay the paths yourself, open **Help → Interactive machining lessons** in the running app.

## Development and licensing

Start with [AGENTS.md](AGENTS.md) and the [developer handoff](docs/DEVELOPER.md) for the architecture, simulation and browser tests. The command names `cnc-studio` and `cnc-studio-web` and the project format `pi-cnc-studio/1` preserve interoperability with existing Studio installations.

[MIT](LICENSE) covers first-party software and documentation. Fonts, the bundled parser and native runtime components have their own licences. See the [licensing inventory](docs/LICENSING.md), [contributing guide](CONTRIBUTING.md) and [security policy](SECURITY.md).
