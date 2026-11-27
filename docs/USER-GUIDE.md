# KerfKite Studio user guide

This guide describes the development application. It includes design, CAM, file transfer and machine controls, but it does not establish full LightBurn parity or certify a machine setup. Read the current [acceptance status](STATUS.md), [installation entry point](../README.md) and [troubleshooting guide](TROUBLESHOOTING.md).

Use **Find a tool** in the header or press **Ctrl+K** (**Command+K** on Mac). Search by control, section or tab, then choose a result with the arrow keys and Enter. Studio opens the tab, expands the section and focuses the existing control. You can inspect its settings before using it. Escape returns to your previous control.

## Choose how to open Studio

| Mode | Open it with | What it provides |
| --- | --- | --- |
| Local workstation | Start KerfKite Studio (the workstation server, `kerfkite-studio`); open its localhost URL | Design tools, CAM, reliefs, simulator and operator-enabled serial control |
| Hosted HTTPS editor | Open the configured HTTPS address and log in | Design, server CAM, private server files and receiver uploads; no machine-start endpoint |
| Local filesystem | Open `studio/static/index.html`, keeping the complete static folder together | Design and supported contour CAM without login; portable project/SVG/G-code export; authenticated HTTPS receiver upload |

The filesystem editor generates supported router, laser and knife contours, including ordered operations and per-vertex depths or powers. Its rotary post supports grblHAL and independent-A xPRO V5 router or laser files, plus LinuxCNC router files. Advanced area operations, holding tabs, relief, solid extrusion and stock simulation require the Python service. [HTTPS and local-file setup](WEB.md)

## Find the tools

The centre holds the work area, toolpath controls, messages and expandable G-code/3D previews. The side panels use tabs, so a tool may be present in another tab rather than farther down the page.

| Left tab | Main tools |
| --- | --- |
| Design | Rectangle, ellipse, line, polygon, gear, dimensions and work area |
| Text | Fonts, straight/down/curved layouts, rotation, depth outlines, relief and extrusion controls |
| Images | SVG import, laser raster images and image/camera tracing |
| Templates | Template library and template creation |
| Projects | Browser project library, recovery, portable files and hosted transfers |
| Objects | Selection, position, resize and available geometry-editing tools |

| Right tab | Main tools |
| --- | --- |
| CAM | Head, protocol, operation, feeds/depths, compensation, laser settings and ordered object operations |
| 3D relief | Heightmap/STL preparation and cutter-contact settings |
| Rotary | Diameter, continuous angle, seam checks, rotary preview, file export and supported simulator/controller streaming. [Rotary guide](ROTARY.md) |
| Control | Machine profile, connection-related setup, jogging, head mode, job/phone controls and manual pendant input |
| Quote | Cost estimates, quote output and design printing |

Open **CAM → Materials guide** to check the selected tool against your stock, compare materials and record sample results. [Materials guide and examples](MATERIALS.md)

Click a tab to select it. When a tab button has keyboard focus, Left/Right changes tabs and Home/End selects the first/last tab. In a server session, machine connection controls also appear in the application header. Hosted/file modes hide unavailable live-control functions.

## First project: design, preview and save

1. In **Design**, check the work-area dimensions and enter X/Y plus shape width/height in millimetres. Add a rectangle or another shape. Shape buttons can also be dragged onto the work area.
2. Select the shape. Drag to move it; use the handles to resize it. Escape cancels the current drag. Position and resize commands also use the numeric fields.
3. In **CAM**, choose the head and operation. For a simple demonstration choose the simulator workflow and a line operation. Enter the feed and relevant depth or laser power settings.
4. Select **Generate preview**. Inspect its warnings, path placement and generated G-code. The cut slider steps through cutting segments; the cut-only estimate covers cutting moves. For a quote, open **Machine cycle time** and enter the rapid rates and starting position to include positioning, plunge and dwell.
5. Name the project in **Projects**, then save it to the browser and download a portable project copy. Export G-code only after generating a current preview.

Editing artwork or machining settings invalidates the previous preview and setup confirmation. Generate another preview before exporting or starting that job. A rendered design alone is not evidence that cutter compensation, material clearance or machine coordinates are correct.

## Projects, recovery and portability

**Save to this browser** stores the complete project in IndexedDB. **Save as a copy** creates a separate project. Search, open, delete and saved-revision controls appear in the same panel. Up to five earlier saves are retained per saved project.

Changes schedule a recovery draft after a short delay. **Recover latest draft** explicitly opens the available draft; startup does not automatically replace your canvas. There is one latest draft per local account namespace, not a permanent history of every unsaved document. Closing the browser immediately after a change can precede recovery storage.

**Download project** creates a `.cncstudio.json` file containing artwork, settings and supported project extras, including quote/rotary settings. Use this file to move work between computers. `Ctrl+S`/`Cmd+S` saves to the browser library; use Download project when you need a portable backup. Project files have a 4 MB import/save limit.

Browser storage belongs to the browser profile and origin. Clearing site data, changing the hostname/port, using a private window or switching browser profiles can make a library unavailable. Hosted accounts use separate library namespaces. This is not a substitute for downloaded backups or private server file storage. Concurrent editing in several tabs is not a collaborative document service.

SVG exports contain vector geometry, not the full editable project settings. G-code exports contain machine instructions, not an editable design. Custom font outlines remain in saved artwork, but reimport the custom font when regenerating that text in a new session. Browser template libraries and material presets should be backed up separately where export controls are provided.

## Shapes, gears and objects

Polygons accept a side count, rotation and centre-hole diameter. Width/height can stretch a polygon. The bore must fit within its actual edge clearance.

Gears use metric external involute geometry. Set teeth, module, pressure angle, centre hole and pitch-circle tooth thinning. Outside diameter is derived from module and tooth count. For a standard pair, use matching module and pressure angle; nominal centre distance is `module × (teeth A + teeth B) / 2`. Tooth thinning is specified per gear, so both gears contribute to pair backlash.

New gears cannot be stretched with ordinary resize controls; regenerate them with a different module. Low tooth counts and invalid bores are rejected. Existing decorative gears in old projects are not automatically upgraded. Use the correct cutter diameter or measured laser kerf for compensated cutting, and verify a test pair: no load rating or physical mesh certification is implied. See [gear geometry notes](GEOMETRY.md).

Available object tools include undo/redo, rotation, mirroring, rectangular arrays, a basic Bézier curve, node editing, weld/difference and offset. Node editing is limited to 500 vertices on the selected object. Advanced boolean/offset operations need the server. Avoid treating these tools as a complete illustration or parametric CAD system.

### Align and distribute several objects

Open **Design → Align & distribute**. Tick the objects in its list, use **Select all**, or select an object on the canvas and choose **Add canvas selection**. This selection is separate from the canvas's single highlighted object.

Choose **Selected objects’ bounds** or **Entire work area**. Left/right/top/bottom and centre controls translate the chosen objects without scaling them. Top means the maximum Y edge in the bottom-left coordinate system. Equal horizontal/vertical gaps require three or more objects; increase their spread or use the work area if the current span cannot fit them without overlap. One object can align to the entire work area.

Depths, power values, text settings and gear proportions are preserved. Use **Undo** to restore the previous arrangement and generate a fresh preview after edits. Alignment can intentionally put objects on top of one another; inspect the result before CAM.

## Text and depth

1. Open **Text**, enter the text and choose a bundled font or import a TTF/OTF file.
2. Set size and spacing, then choose straight, text-down or Bézier-baseline layout. Adjust page rotation or Y-axis rotation as required.
3. Set the shallowest depth and select **Add text as paths**. The font style controls letter outlines; the depth controls their Z values.
4. To revise the wording/settings, select that text object, load its text settings, make changes and use **Replace selected text**.

Y-axis rotation creates varying negative-Z depths. Large tilts across wide text can exceed the supported depth range. Router line CAM follows the outline at those depths; it does not clear the entire letter interior. Choose the separate V-carve, raised/recessed relief or extrusion workflow for those results. XYZ path export preserves depth coordinates; solid STL extrusion is an export, not an automatically machined solid.

For Bézier text, choose **Symmetric · use curve rise** or **Custom · independent control points**. In custom mode, enter Control 1/2 X as a percentage of the original text width and Y in millimetres, or drag either orange handle in the baseline preview. The endpoints stay fixed. **Copy symmetric rise into custom controls** initializes the custom shape from the simple rise setting. Apply changes with **Add text as paths** or **Replace selected text**; editing controls alone does not change existing outlines. Independent values are restored with the selected text settings. Tight curves can overlap letters, and stationary/cusp tangents are rejected. Complex-script shaping and automatic ligatures are incomplete; import correctly shaped outlines when required. Check missing-glyph errors rather than accepting replacement characters.

## Images: raster versus tracing

**Laser raster** samples an image into alternating scan rows. Choose grayscale, threshold or Floyd–Steinberg dithering, physical dimensions, pixel interval, inversion and overscan. Smaller intervals increase the job size. Raster output is capped at 60,000 pixels before later CAM limits. Overscan adds zero-power travel beyond both sides of the artwork, so allow physical space for it.

**Image/camera tracing** creates actual editable vector paths:

1. Upload an image, or explicitly open the camera and capture a frame. Camera access depends on browser permission and a supported secure context such as HTTPS or localhost.
2. Choose **Closed shape outlines** for silhouette boundaries and holes, or **Stroke centrelines** for line drawings. Centrelines are skeletonized, split at junctions and remain open unless the source contains a genuine loop.
3. Set threshold, optional inversion and minimum feature size. In outline mode the filter uses pixel area, including small holes. In centreline mode it uses stroke length and can remove short branches.
4. Set the physical width and height of the **entire image rectangle**, then preview. Changing their ratio stretches the trace. Dimensions are not inferred from the camera.
5. Inspect the paths and select **Add preview to project**. They are inserted at the design X/Y placement. Use a line engraving operation for open centrelines.

Tracing reduces input to at most 384 pixels per side and caps contour/stroke complexity. It removes collinear vertices without smoothing away gaps. Expect pixel-scale corners, approximate endpoints and possible skeleton spurs. This is not camera-bed calibration, lens correction, perspective correction, automatic registration or a live machine overlay. Capture, Stop, closing the trace section and leaving the page release camera tracks. No trace action moves the machine.

For cutter artwork, import compound SVG contours at their original physical size or fit them to a chosen box. The [SVG guide](SVG.md) explains units, reused and hidden geometry, and unsupported effects. Import DXF and vector PDF files through the reviewed [program import](PROGRAM-IMPORT.md) workflow.

## Templates

Open **Templates → Template library** to browse, search, filter categories and mark favourites. Select a template to insert it at the design placement and requested size. Built-in artwork retains its natural aspect ratio. The library includes simple motifs and original botanical/ornamental examples; preview them at the intended production size.

To create a reusable template, select artwork or choose the whole design, then provide its name/category. Custom templates are stored in this browser. Export/import library JSON for a portable backup; the custom library is limited to 200 templates.

Leaf/berry occlusion in the newer botanical artwork is applied to actual vector paths. These are still engraving contours: one-piece silhouette union, cutout bridges and tool-size/minimum-feature checks are not automatically constructed. Inspect whether a cutout would release unwanted islands. See [MCP setup](MCP.md) for external artwork/template creation through the local stdio interface.

## CAM and ordered operations

| Operation | Use and important limits |
| --- | --- |
| Line / depth contour | Follows the path centreline; no radius compensation. Supports router depths, laser vectors/raster powers and knife preparation. |
| Outside / inside | Compensates closed valid contours using cutter radius or laser kerf. Nested contours use even-odd holes. A tool may not reach narrow features. |
| Pocket | Concentric router clearing around preserved islands. Stepover is limited to half cutter diameter. Plunge entry needs a suitable centre-cutting tool; inaccessible corners can remain. |
| Hatch fill · laser or pen | Scans an even-odd region at the specified interval/angle, with optional crosshatch. |
| V-carve | Sampled medial-axis paths with V-angle, depth and cutter-diameter limits. The cut stays one sampling step (**V detail sampling**) short of every wall. The pieces are joined into longer paths and cut in an order with little travel. Fine detail and wide-area clearing need inspection. |
| Vinyl drag knife | Offset pivot paths, alignment leads and corner swivels, with powered outputs off. The alignment lead marks material before each contour. |
| Pen · drawing | Draws lines and hatch fill with a spring-loaded pen, lifted by the Z axis or by a servo on the spindle or laser output. See [Drawing with a pen](PEN.md). |

Assign current operation settings to a selected object when objects need different operations or feeds. Object order becomes operation order; output can be disabled per object. Continuous jobs reject changes of head, protocol or cutter geometry. Make separate reviewed jobs for tool changes. Saving a material/tool preset records your entered settings; it does not calibrate feeds, cutter dimensions or machine limits.

Laser lead-ins/outs are straight tangent **cutting** moves. Start/end power ramps extend over the chosen distances and can mark beyond the original outline. Preview shows commanded power; GRBL dynamic laser mode additionally changes effective output with motion speed. The laser's optical watts are not the controller S-value: validate the controller's configured range and your material tests.

## 3D relief and native acceleration

In **3D relief**, choose a grayscale image or ASCII/binary STL and set placement, physical dimensions, depth, resolution, tool diameter/shape, stepover and stock allowance. Generate the relief, then review its normal router step-downs and clearance moves.

Image heights map white to the stock top and black to the selected depth. STL processing makes a top-access height field; undercuts and inaccessible sides are not represented. Flat/ball cutter-contact preparation is conservative and can leave material on a coarse grid. It does not model the holder, chuck, clamps or material outside the input rectangle.

Reliefs and stock previews are computed in the native core, which shares the relief's contact calculation out over the processor's threads. Image maps can have up to 1024 points a side; STL and artwork maps stay at 128. A path holds at most 100,000 points, so a large map needs a wider stepover. On a computer with a GPU that has double precision, a large job may run on the GPU, or on the GPU and the processor together, with the same result to the last bit. Studio measures both each time it starts, in the background, and shares large jobs between them in proportion to their speeds; on a computer without such a GPU, such as a Raspberry Pi, everything runs on the processor's cores. The UI reports **C++ accelerated relief on the GPU** or **on the GPU and processor together** when the GPU took part, and **C++ accelerated relief** when the threads computed it. Other CAM stages and the stock preview run on the processor, and nothing uses Metal or Hailo. See [threads and the GPU](NATIVE.md#threads-and-a-gpu-for-large-reliefs), [platform notes](PLATFORMS.md) and the installation instructions for the selected host.

The central 3D view rotates by mouse drag. **Simulate sampled stock removal** adds an approximate stock surface. Resolution limits can hide thin remaining material or small defects; this is not a clamp/holder collision check or a guarantee against gouging.

## Holding tabs for router contours

1. Select a closed contour object and choose router **line**, **inside** or **outside** with a flat end mill. Set cut depth to reach the stock bottom.
2. In **CAM → Holding tabs · router contours**, enter stock thickness, nominal tab width and tab height above the stock bottom. Height must be positive and below stock thickness.
3. Choose an evenly spaced count or explicit positions as comma-separated percentages from 0 up to, but not including, 100. Positions are measured around each compensated contour.
4. Click **Assign tabs and current CAM settings to selected object**. Draft fields alone do not modify the job. Assignment also updates the project stock-thickness default.
5. Generate a fresh preview and inspect the depth plateaus. With 10 mm stock and 2 mm tabs, plateaus should stop at Z −8 mm while the main cut reaches the bottom. Use **Remove tabs from selected object** and regenerate to remove them.

The plateau adds one cutter diameter to the nominal width along the path. Inspect corners, inner contours and nearby cuts: geometric tabs do not guarantee stock retention and do not replace clamping. Overlapping/oversized placements fail CAM validation. Relief/depth paths, non-flat cutters, open contours and unsupported operations are rejected. Direct-on-contour dragging of tab positions is not available. Holding-tab CAM currently requires the server; local-file generation explicitly rejects it.

## Rotary design and export

Use the **Rotary** tab to wrap a router or laser design around a cylinder, inspect the seam and preview the posted path. Enter the measured diameter, design wrap axis and physical rotary axis. A periodic border is one full-turn example. Use **Stream reviewed rotary job** with a qualified grblHAL or independent-A xPRO V5 workstation connection, or export the file. LinuxCNC supports router file export. The original xPRO V5 dual-Y gantry setup needs a separate independent rotary motor channel to perform independent fourth-axis machining. [Rotary setup, simulator walkthrough and controller requirements](ROTARY.md)

## Local machine, keyboard and gamepad control

The local server starts with physical ports disabled. Test with **Simulator**. Enabling real ports requires the explicit `--enable-hardware` startup option and a machine that has been commissioned independently. Select the correct protocol before connecting; disconnect before changing it.

The Control profile is a setup aid, not a command to overwrite the controller. Check current travel, head, work origin and clearance against the actual machine. **Apply mounted head mode · outputs off** requests a confirmed mode transition; it does not mechanically swap or measure the head. Re-establish router/knife Z zero or laser focus after changing heads. GRBL readback checks include mode/power and available travel; firmware and physical setup still determine what happens.

For keyboard/gamepad jogging, explicitly enable manual control in **Control**. The machine must be connected and idle.

- Arrow keys request one X/Y step; Page Up/Down request one Z step. Held-key repeats are ignored. Typing in inputs does not jog the machine.
- For a browser-standard gamepad, press a button if necessary, then **Find gamepads** and select it. Check the mapping in the simulator.
- Release the selected hold-to-jog button, centre the stick, then hold the button and deflect for one increment. Return to centre before another increment. Only the strongest axis moves; gamepad increments are capped at 1 mm.
- Losing window focus, hiding the page, changing the mapping or disconnecting the gamepad disables manual input. Releasing the button prevents new requests; already accepted movement can continue.

Dedicated XHC/USB pendant protocols and continuous handwheel jogging are not implemented. GRBL feed hold and Marlin host pause have different behavior. Marlin pause stops transmission while buffered moves may continue; its abort uses M112 and requires reconnect. Software hold/reset is not a physical emergency stop.

## Phone control and uploads

Phone control and file upload are separate services. The hosted HTTPS editor/receiver stores files for review; an upload never starts a job. Use **Projects** to download/upload project, SVG or current G-code files. A transfer receipt confirms checksum and byte count, not execution. If a transfer result is ambiguous, inspect the receiver before retrying. See [WEB.md](WEB.md) for accounts, certificates and upload-only tokens.

In Studio, open **Control → Phone pendant → Enable phone pairing**. Choose the server address reachable from the phone. Paste the private pairing link into **CNC pendant** in KerfKite Pocket. The Studio server owns the controller connection. A phone can reach it through Wi-Fi, USB tethering or Bluetooth tethering. See the [connection guide](PHONE-CONNECTIONS.md) for setup and troubleshooting. Use **Disable and revoke pairing** to end access.

From Pocket, jog one XYZ axis by a selected increment, home the machine or zero the work coordinates. These setup actions require an idle controller and disarm any prepared job. To run a job, generate a current preview at the workstation, confirm setup and choose **Arm preview for phone start**. Pocket can start the armed job once, pause, resume and stop it. If a connection drops, inspect the machine and refresh its status before issuing another action.

## Costs, quotes and printing

In **Quote**, enter your own material, waste, machine-time, hourly-rate, setup, finishing, consumables and other costs. Use the current preview time as a starting point, then add travel/startup and realistic production allowances. There are no built-in authoritative material prices or calibrated production rates.

Gross margin is profit as a percentage of selling price, not a markup on cost. Choose the quantity, currency and tax rate appropriate to the quote. Calculate, then download quote JSON or print/save PDF. Unit prices are rounded; order totals use the unrounded estimate before final currency rounding. These are estimates, not accounting or tax determinations.

For design printing, choose **Fit on A4 landscape** or **1:1 tiled A4 landscape**. Allow the print popup. For 1:1 output, use A4 landscape and 100%/actual-size scaling in the browser/printer dialog; turn off any additional fit-to-page scaling. Measure the printed **50 mm reference** with a physical ruler before using the print as a machining template. Printer margins, drivers and PDF viewers can change scale even when the on-screen dimensions are correct.

## Reporting a problem

Keep a portable copy before restarting or clearing any browser data. Record the application revision, operating system/browser, server mode, selected head/protocol, operation settings and exact error. Include a minimal project that reproduces the issue and distinguish simulator behavior from physical hardware behavior. Redact passwords, upload/pairing tokens and private keys. Use the symptom-specific [fault-finding guide](TROUBLESHOOTING.md).

## Circuit boards

Under Projects, Circuit board imports a board's Gerber copper layer, its outline and its Excellon drill file. Review shows what was read from each file, every warning and, in red on the canvas, each place where two pieces of copper are too close for the cutter to pass between them. Add to design makes one layer for each tool with Output off: the isolation contours, each drill diameter, slots and the cut-out. Choose a stage under Board stages to make it the job, preview it, fit its tool and set work Z; use Probe levelling for the engraving. The underside is imported mirrored so that both sides register. [Circuit boards](PCB.md) is the full guide, with what has and has not been tested.

## Manual tool changes

In CAM, open Manual tool changes. Set the cutter diameter, operation, depth and feeds; select an object, assign a tool number/name/shape and its current settings. Repeat for every active object. Generate separate stage files, then download each G-code file and the plan JSON. Consecutive operations using the same tool geometry/number share a stage. Returning to a previous tool creates another stage.

Each stage ends retracted with the spindle off. Wait for motion and spindle to cease, install the next tool, re-establish Z zero or verified tool length, check XY origin/clearance and review the next stage before starting it manually. Do not concatenate stage files. Automatic tool changers, probing and automatic continuation are not implemented. Ordinary Run rejects differing tool numbers even if their diameters match. Tool-change generation requires the app server.

Holding tabs are retained when reassigning CAM settings or a manual tool. Use **Remove tabs from selected object** before clearing operation settings; the editor prevents implicit removal. Incompatible tool/mode combinations retain the tab request and fail CAM validation.


### Calibrate quote time from a sample

Open **Quote → Machine cycle time** after generating a preview. Enter the measured rapid rate for each axis, and the XYZ work-coordinate position where the program will start. The example rates are editable. Studio reads programmed feeds and dwell units from its posted XYZ moves and includes plunges, linked moves and retracts. Rapids assume coordinated axes, each within its entered maximum rate. Without a measured sample, the calculation uses constant speed.

Run a representative sample under your normal workshop process and record elapsed time from program start to finish, excluding setup or pauses. Enter the duration in minutes, then choose **Calibrate from sample**. Studio scales the programmed motion portion and preserves the commanded dwell. The project retains this sample for its selected machine profile, head, protocol and rapid rates. Use a similar path type and material when transferring its factor to another preview. **Use cycle time in quote** sets the quoted machining minutes and resets the separate cut-time multiplier to one. Existing loading or handling allowances stay separate.

### Use more than one machine

Under **Control → My machine profiles**, enter a name, measured X/Y travel, optional Z travel and design dialect. Choose **Save as new profile**. The machine selector switches the current project to the work area and dialect. Selecting a smaller custom work area prompts you to move any artwork outside its limits first. A profile records planning dimensions. The controller's configuration and work origin are established separately at the machine.

**Export profile** writes a portable JSON file. On a different computer, choose **Import profile**, inspect the fields and save it to that browser. Projects carry a snapshot of the active custom profile. Opening one from another machine displays the saved profile even if that machine has no local library entry. Hosted browser libraries are scoped to the signed-in account.
