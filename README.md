# KerfKite Studio

KerfKite Studio is an open-source CAD/CAM workbench for interchangeable CNC router, laser and drag-knife heads. Design in a browser, simulate a job, and prepare output for a supported controller. The Python application runs locally or as an authenticated HTTPS editor. The repository also includes a simplified Android companion.

![Layered kingfisher cut-vinyl decal beside laser-engraved reeds on maple and a router-carved wave relief in walnut](studio/static/workshop-artwork.jpg)

*Workshop illustration of the three processes: cut vinyl, laser engraving and router carving.*

## Run it locally

1. Install a supported 64-bit Python and follow the [installation guide](docs/INSTALL.md).
2. On macOS or Linux, run `sh scripts/install.sh` and `sh start.sh`. On Windows, use `scripts/install.ps1` and `start.bat`.
3. Open the loopback address printed by the server. The default startup uses the simulator. Under **Projects**, choose **Try a sample** to import a laser badge. Under **3D CAD**, open the guided plate or bearing tutorial.
4. For STEP exchange install the optional extra with `python -m pip install ".[step]"` inside the application's virtual environment.

## Inside the workshop

* Parametric parts, sketches, Boolean features, assembly mates, interference checks, Bézier/NURBS surface editing and STEP exchange.
* Layer-based laser editing, image processing, material test grids, camera registration and 1,000 original vector templates.
* Router relief, profile and pocket operations, drag-knife paths, rotary file export and simulated fixture clearance.
* Reviewed imports for LightBurn projects and settings, DXF drawings, G-code, HP-GL cutter programs and LinuxCNC workspace settings.

See the [release report (PDF)](docs/KerfKite-Release-Review.pdf), [file compatibility](docs/LIGHTBURN-IMPORT.md), [controller matrix](docs/PROTOCOLS.md) and [platforms](docs/PLATFORMS.md) for tested scope and open work. The machine limits in the public profiles are example dimensions. Enter your measured setup before preparing a physical job.

## CNC simulations

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
