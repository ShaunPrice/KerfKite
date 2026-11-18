# Troubleshooting and fault finding

Use this guide with the [user guide](USER-GUIDE.md), [current acceptance status](STATUS.md) and [HTTPS setup](WEB.md). Simulator results, browser tests and successful file transfers do not establish physical machine behavior.

## Preserve evidence first

Download the current `.cncstudio.json` project before restarting, refreshing or changing browser storage. Save the exact error text and a screenshot of the relevant settings. If motion or powered output is unsafe, use the machine's physical emergency-stop procedure; a browser refresh or network disconnect is not a stop command.

When asking for help, include:

- Application version/revision, OS and browser version.
- Local server, HTTPS editor, phone monitor or direct `file://` mode.
- Selected protocol/head, simulator versus physical machine, and the steps that reproduce the issue.
- A minimal project, generated file if relevant, and a short controller/server log excerpt.
- Whether the fault happened before generating, while transmitting, during actual movement or only in the preview.

Do not include passwords, pairing/upload tokens, certificate private keys or complete unrelated private documents.

## App startup and tools

| Symptom | Check and next action |
| --- | --- |
| Connection refused at the app URL | Confirm the intended server is running and read its actual host/port from the terminal. A saved browser URL may point to an old development port. |
| `ModuleNotFoundError` or missing package | Use the project's intended Python environment and reinstall the documented dependencies. `python -m pip` and the app must use the same interpreter. |
| Port already in use | Identify the existing Studio process or choose another explicit port. Do not terminate an unknown service simply to free the port. |
| Tools appear missing | Check the left and right tabs. Design/Text/Images/Templates/Projects/Objects are separate from CAM/3D relief/Rotary/Control/Quote. |
| Tabs are absent or scripts fail | Keep all static files together in filesystem mode. In server mode, save first, restart the updated service and reload. Inspect the first browser-console error for a missing script or stale/mixed build. |
| “Restart CNC Studio and reload” | The browser has newer controls than the running server supports. Save the project, restart the correct process, then reload its URL. |
| “Advanced operation requires the app server” | The direct-file build supports basic contour CAM but not all server operations. Open the saved project in the local/HTTPS server for area CAM, relief, extrusion, simulation or rotary export. |
| Browser warning about `--disable-blink-features=AutomationControlled` | This comes from how Chromium was launched, not from CNC Studio. Open the app URL in your normally launched browser. Studio does not require this flag. |

## Drawing, projects and templates

### Selection outline and artwork disagree

Cancel an active drag with Escape, select the object again and inspect the object list for another overlapping/open path. Save the project before refreshing. If a reload resolves the drawing but the issue recurs, capture the object geometry and exact move/resize sequence. Do not treat disappearance after refresh as proof that the underlying bug is fixed.

### My browser library disappeared

Check the browser profile, hostname, protocol and port. They affect storage identity. Hosted libraries are also separated by username. Private browsing, browser cleanup and site-data deletion can remove IndexedDB/localStorage records. Do not clear site data as a first repair step.

Use **Projects → Recover latest draft** if available, inspect saved revisions, or reopen a downloaded project. Drafts are delayed and there is only one latest draft per namespace; they are not a complete backup history. A template-library JSON export is separate from a project file.

### Saving or recovery fails

Download a portable project immediately. Check the browser's storage permissions/quota and whether private browsing restricts IndexedDB. Close other Studio tabs if a database upgrade is blocked. The portable project limit is 4 MB; split very large artwork rather than assuming a larger server upload limit guarantees it can be reopened as a project.

For edits made in several tabs, keep separate downloads before choosing which version to retain. The browser library does not merge concurrent project edits.

### An SVG or font will not import

Studio supports a restricted SVG geometry subset. Convert text, transforms/effects or unsupported compound structures to compatible plain paths in your source editor, then retry a small example. Full DXF/PDF/SVG document support is not implemented. Missing font glyphs and unsupported complex-script shaping are reported; import correctly shaped outlines rather than substituting arbitrary characters.

Custom font outlines travel with a project, but the font file must be imported again to regenerate the text. Check that you selected a supported TTF/OTF and that it is within the size limit.

### A gear refuses resize or tooth settings

Change module and regenerate instead of stretching a gear. Check the conservative minimum tooth count for the pressure angle, bore clearance and tooth thinning. Old decorative gear objects are not equivalent to new involute gears. A successful geometry preview is not a meshing/load test; cutter diameter, kerf, backlash and actual centre distance matter.

### Vine stems, leaf gaps or cutout islands look wrong

Distinguish old/simple templates from the newer occlusion-aware botanical group. Inspect the actual exported paths at production size. Templates are engraving geometry, not automatically bridged stencils or unioned one-piece silhouettes. For a reproducible crossing, include the template name and a minimal exported project. Do not hide a line visually and assume it has been removed from G-code.

## Images and camera

| Symptom | Check and next action |
| --- | --- |
| Camera button reports unavailable/denied | Use a supported browser on HTTPS or localhost, allow camera permission and check whether another application owns the camera. Image upload remains available. |
| Camera is open but Capture is unavailable | Wait for a usable video frame. Stop and reopen if the device has disconnected. Closing the trace section or leaving the page releases its tracks. |
| A black stroke becomes two outlines | Choose **Stroke centrelines**, not **Closed shape outlines**. Outlining both edges of a thick stroke is expected in outline mode. |
| Centreline has spurs or missing short branches | Improve contrast/source quality and adjust the minimum stroke length. Thinning is pixel-based; it is not automatic reconstruction of an ideal pen stroke. Preview before adding. |
| Small holes disappear | Outline filtering uses minimum area and applies to holes too. Lower the minimum feature value and preview again. |
| “Too noisy” or complexity-limit error | Clean the image, adjust threshold/inversion, crop unrelated content externally or reduce small features. The trace is capped rather than producing an unbounded job. |
| Trace is stretched or wrong physical size | Width/height map the whole image rectangle. Match its aspect ratio and an independently measured size. The camera has not calibrated that measurement. |
| Picture is skewed relative to the stock | Correct perspective/lens effects externally or retake the picture square-on. Bed registration and lens calibration are not implemented. |
| Raster is outside the work area | Include the zero-power overscan on both sides in your placement. Reduce width/overscan or move the image inward. |
| Raster exceeds the pixel limit | Increase interval or reduce physical dimensions. Preview and generation have additional segment limits. |

## CAM and 3D geometry

### “Closed contours,” “self-crossing,” “tool does not fit” or empty output

Area operations need valid closed regions. An imported open centreline belongs in a line operation. Repair self-crossings, duplicate/canceling contours and zero-area shapes before using profiles, pockets, hatch fill or V-carving. Even-odd nesting creates holes; overlapping shapes can cancel in the filled region.

For a fit error, compare tool diameter with the actual feature width. Reducing the numeric diameter without changing the physical cutter creates an incorrect toolpath. Use a suitable tool or change the design. Internal corners narrower than the cutter cannot be machined sharp by that cutter.

### Pocket has residual material

Use a stepover no larger than half cutter diameter. Inspect inaccessible corners and narrow regions even when the stepover passes validation. Concentric pocketing is not adaptive clearing, and the tool must support the generated plunge entry. V-carving can also leave wide flat areas; it is not automatically a complete flat-clearing strategy.

### Text has depth outlines but no filled letter surface

Text outline depth and filled relief are separate workflows. Select V-carving, raised/recessed relief or the relevant area operation for the desired result. Y tilt varies the depth across text; it does not automatically generate a solid or a pocket. Review whether the chosen operation uses a flat cutter or a V-bit, and keep tool changes in separate jobs.

### Relief is slow or leaves extra material

Reduce grid resolution or increase stepover while preserving the required feature detail. The contact calculation uses conservative cell envelopes, so coarse maps can leave extra material deliberately. Use appropriate resolution and allowance for roughing/finishing review. Smaller tools and more rows can increase contact checks sharply; complexity limits reject work beyond the bounded workload.

The UI identifies **C++ accelerated relief** versus **Python relief**. To check the optional extension in the same environment as the server:

```sh
python -c "from studio import _relief_native; print(_relief_native.__file__)"
```

If that import fails, the native extension is not installed in that interpreter. The Python backend remains available. Rebuild using the documented compiler/install process for that platform, then restart the server. A native build is specific to its Python/OS/architecture; copying a macOS binary to a Pi or Windows installation will not work. Native acceleration covers the relief kernel, not every CAM operation or all browser interactions.

### Stock preview differs from a real cut

The preview samples the stock and can miss fine details. It does not check holders, clamps, the chuck, tool deflection or every swept-volume collision. The STL workflow is top-access height-field machining, not undercut/multi-axis machining. Record the resolution, tool shape, allowance and source model when reporting a mismatch.

### Ordered operations are rejected

Check that every enabled object uses the same head, protocol, work area and compatible cutter geometry. A continuous file cannot silently swap a flat cutter for a V-bit or a different diameter. Split those into separately reviewed jobs. Remember that an object's assigned settings can differ from the currently visible global controls; clear/reassign its operation if needed.

## Rotary faults

| Symptom | Check and next action |
| --- | --- |
| Rotary export asks for router/GRBL source | Select router mode and GRBL source geometry. The export converts that source into LinuxCNC G93/A-axis instructions; it is not sending GRBL rotary commands. |
| File cannot be run by Studio's normal sender | Expected: rotary is export-only for configured LinuxCNC hardware. Current GRBL/Marlin senders do not support this file. |
| Seam endpoint check fails | Check the `π × diameter` design span and align opposite-edge endpoints in axial position and depth. The comparison tolerance is 0.01 mm. |
| Seam check passes but physical seam is wrong | Measure the real diameter and verify seam zero, axis direction and physical alignment. The endpoint check does not establish tangency or compensate an inaccurate stock diameter. |
| Rotary uses X instead of the other design axis | Expected: the unwrapped design coordinate becomes axial X; A rotates around X and Z is radial. Check the target machine configuration before execution. |
| Angles exceed 360° | Expected for a continuous unwrapped A axis. Do not apply modulo-360 rewriting: it can change the intended movement direction/path. |
| Cut crosses the cylinder centre | Reduce depth or use an appropriate cylinder; the exporter rejects Z at or beyond the centre radius. |

The rotary preview remains planar. G93 preserves nominal unwrapped movement timing; actual surface speed changes with radius, and the target controller's acceleration/axis limits still matter.

## Controller and manual input

### Physical port is disabled or missing

The server defaults to simulator-only. Physical use requires explicit `--enable-hardware`; adding that flag is not calibration or commissioning. Check the selected protocol, OS serial permissions, cable and whether another sender has the port open. A power-only USB cable does not provide a serial connection. Disconnect before switching protocols.

### GRBL mode, power or travel mismatch

Wait for settings readback. Check `$32` against the actual head and `$30` against the intended S/RPM command range. A 25 W optical laser rating is not an S-value of 25. Check work-area dimensions against current controller travel and the physical setup. Do not blindly apply an old settings export from another machine.

After changing a head, verify its mounting and re-establish focus/Z and work zero. The confirmed head-mode command turns powered outputs off while applying the supported mode change; it does not determine the new tool length or stock surface automatically.

### GRBL says ready but Marlin behaves differently

Marlin uses different initialization, acknowledgements, homing and zero commands. It needs firmware configured for the intended CNC spindle functions. Studio waits for an acknowledged M400 before claiming job completion; ordinary `ok` responses are not proof that all buffered motion has finished. Live physical compatibility remains a commissioning task.

### A timeout/reset occurs

Treat motion and output state as uncertain until checked locally. Record the last accepted command and controller log. Resolve the underlying connection/firmware issue, reconnect when required and re-establish the necessary origin/setup before generating or starting another job. Do not automatically replay an ambiguous job from the beginning: part of it may already have executed.

For a GRBL feed hold requested from Studio or the pendant, fresh `Hold:0` (or legacy `Hold`) reports confirm that movement has stopped. During **Decelerating**, wait for **Hold** before selecting Resume. On resume, Studio waits for the controller to report Run or Idle before transmitting further lines. During the stationary hold, Studio pauses the acknowledgement and completion timers until resume. The controller must keep reporting its status throughout the hold. A missing hold confirmation or lost status can still abort the job.

For Marlin, a long blocking command can exceed the configured acknowledgement timeout. Its fault handler requests M112, but immediate processing depends on firmware emergency-parser behavior. Do not repeatedly increase timeouts to hide a failing connection.

### Keyboard/gamepad will not jog

Use a local server session, enable manual input and confirm the controller is connected, idle and not running a job. The page must have focus, and text inputs must not have focus. Key-repeat is ignored. OS/browser shortcuts may reserve some keys.

Use a gamepad with browser-standard mapping. Press a button, refresh the device list and select it. After enabling or changing mapping, release the hold-to-jog button, centre the stick, then hold it and deflect for one increment. Centre again before the next increment. Gamepad steps are capped at 1 mm, one axis at a time. Focus loss/disconnection disables input. Dedicated XHC pendants are not supported by this browser-gamepad adapter.

If the feed is rejected, use a feed accepted by the selected controller's jog guard. The manual panel's upper bound does not override a stricter driver limit. Test direction/mapping with the simulator and then a commissioned, supervised setup.

## HTTPS login, receiver uploads and phone controls

| Symptom | Check and next action |
| --- | --- |
| No login/password works on a new install | There are no default credentials. Create a user with the setup CLI, choosing a password or using its secure generator. |
| Forgotten password | Use the operator `reset-password` command from [WEB.md](WEB.md). It revokes that account's sessions/upload tokens; issue replacement upload tokens where needed. |
| Certificate warning or rejected HTTPS connection | Check certificate expiry, hostname/IP SAN, matching key and client trust. Trust the intended self-signed certificate explicitly or install a trusted certificate. Do not disable verification as the fix. |
| HTTPS request rejected for host/origin | Open the configured public HTTPS URL exactly. The service expects that origin and direct TLS/TCP passthrough; forwarded proxy headers are not implicitly trusted. |
| Session expired or CSRF/reload error | Download unsaved artwork first, then log in/reload. A session has idle and absolute expiry. |
| Login throttled | Stop repeated attempts, confirm the account details and allow the throttle window to expire. Use operator password recovery if needed. |
| No receiver destination appears | Configure it on the hosted server with the receiver-issued upload token, then restart to load the destination configuration. |
| File upload receipt missing/timeout | Inspect the Pi receiver inbox before retrying. A failed response can occur after the file was stored. Successful receipt confirms file checksum/size, not execution. |
| `file://` upload blocked | The receiver needs `--allow-file-upload`, the browser must trust its HTTPS certificate and the upload token must be valid. Browser private-network/CORS policies may still restrict the flow. |
| Receiver storage full | Download needed files, then delete unwanted server copies. Quotas apply per account; never assume deletion creates a separate backup. |
| Phone Start is unavailable | Generate and confirm a job at the local workstation, then arm it. The one-use arm must still be valid and the controller ready. An upload does not arm a job. |
| Phone displays stale/disconnected data | Check connectivity and the running monitor; do not infer a successful pause/stop from missing updates. Check the machine locally. |
| Phone pairing fails after restart | Restarting generates a new pairing token. Use the new pairing link. |

The account-based HTTPS editor and bearer-token phone monitor have different purposes. The monitor can use HTTPS with both certificate/key options; without them it uses plain HTTP on a trusted LAN. Do not expose the plain HTTP monitor through public port forwarding. The hosted receiver's upload tokens only store files, while the paired monitor can act on an explicitly armed job.

## Quotes and printed templates

Check quantity, per-unit versus per-order fields, time multiplier, travel/startup allowance, currency and tax. Margin is not markup: a 20% gross margin means cost divided by 0.8, not cost multiplied by 1.2. Preview cutting time excludes several real production costs.

If a quote/design does not print, allow the print popup and use the browser's PDF/printer dialog. For 1:1 designs choose tiled A4 landscape, set the final print to 100%/actual size and measure the 50 mm reference. Disable driver/PDF fit-to-page scaling. A correctly sized SVG/PDF does not guarantee the printer used the same scale.

## Known acceptance limits

Full LightBurn workflow parity, robust general DXF/PDF import, complete SVG semantics, camera calibration, arbitrary complex-script text shaping, dedicated XHC pendant integration and additional live controller ecosystems are not complete. Advanced CAM does not have full filesystem-mode parity. Tool changes/probing, production-grade clamp/holder collision checking and optimized industrial toolpath strategies remain separate gaps.

Cross-platform builds, independent internet-service security acceptance and physical machine/gear/laser/knife/phone-stop testing must be tracked as their own evidence. Consult [STATUS.md](STATUS.md) for the current record rather than treating the existence of a button or a passed unit test as deployment acceptance.
