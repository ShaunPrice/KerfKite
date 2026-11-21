# Studio Pocket for Android

Studio Pocket combines an offline vector sketch editor with a native CNC pendant for Android. Pocket supports Android 8.0 (API 26) or later with a minimum supported device RAM of 2 GB. It uses a touch canvas and numerical dimensions. Files saved from the app open in the desktop and Raspberry Pi workstation. The current template, solid modelling and machining workflows run on that workstation.

The minimum OS comes from the APK manifest. The editor and server-paired pendant were exercised on an ARM64 emulator configured with 2 GB of RAM, and on one physical handset (see Physical handset check below).

## Install and draw

For normal installation, download the [signed KerfKite Pocket APK](https://github.com/ShaunPrice/KerfKite/releases/download/pocket-v0.5.0/KerfKite-Pocket-Android.apk) on the phone, open it from Downloads and choose **Install**.

For development, build the debug APK using Android Studio with JDK 17 or newer, Android SDK Platform 35 and Build Tools 35 installed. Open the repository’s android directory as the project. Alternatively, from the android directory run:

```sh
./gradlew assembleDebug
```

On Windows run `gradlew.bat assembleDebug`. The first build fetches the Gradle distribution, the plugin and the ZXing core library (QR decoding) from Maven Central; it uses the installed Build Tools 35.0.0. Subsequent builds can use `--offline` once the dependencies are cached. The installable debug APK appears at `app/build/outputs/apk/debug/app-debug.apk`. A debug build is a separate app: it installs as **Pocket debug** (`au.cncstudio.pocket.debug`) beside the released KerfKite Pocket, keeps its own projects, and can be removed without touching the released app. Transfer the APK to the phone and open it from Files, or choose Run in Android Studio with your development device selected.

1. Tap **Add shape** and choose Rectangle, Ellipse or Polygon. Enter **Width** and **Height** in millimetres. **X** and **Y** locate the lower-left corner on the project’s work area (680 × 240 mm for a new drawing). A polygon accepts 3–64 sides. Tap **Apply**.
2. Tap the drawing or its object row to select a shape. **Edit** reopens its dimensions. The preview fits all shapes automatically. A tap on or just outside an outline counts. Where shapes overlap, the smallest one under the tap is selected first; tap again to move to the next, or use the object list. Each new shape starts 5 mm up and to the right of the previous default position so it does not hide the last one. Each applied change saves the private draft on this device.
3. Choose **CNC router**, **Laser** or **Drag knife**. Tap **Studio project**, choose a destination in the system file picker and save. **SVG** writes a vector drawing with physical dimensions and preserves the placement on the work area.
4. Open the project in KerfKite Studio. The workstation reads the head selection and editable outline geometry. Set the intended tool, feed, depth or laser power, then generate the machining preview. The sketch workspace exports design files. Open the separate **CNC pendant** screen to pair with the receiver.

**Quick guide** inside the app walks through these same controls. Clearing app data deletes its private project library. Use the system file picker to keep a portable copy.

## Keep the display on

Turn on **Keep screen awake** above the drawing or above **Jog** on the pendant to keep Pocket visible during a long job. The switch starts off. Both screens share the choice and remember it after a restart. Leaving Pocket or turning the switch off restores the phone’s usual screen timeout. You can still lock the phone with its power button.

## Open and organise projects

Choose **My projects → New project** and give the drawing a name. Edits save to the active project in private storage. **My projects** also lets you switch projects, rename one, duplicate it or delete the local copy. Upgrading from Pocket 0.3.0 keeps the previous single draft as **Pocket design**. Export important projects before uninstalling or clearing application data.

Choose **Open file**, select a Studio JSON document with the Android file picker, check the object count and work area, then tap **Open copy**. Your active project stays in the library. Opening or editing a copy leaves the source file untouched.

* **Pocket drawings** reopen with their machine head, dimensions and primitive controls. Compatible outlines from version 0.3.0 recover the original shape when its sampled points match. Other planar artwork supports moving and resizing, including open paths and objects with multiple contours.
* **Desktop references** show their XY outlines in a read-only view. Use **Studio project** to export the complete source document, including the original CAD history, layers, object metadata and machining settings. Renaming a reference changes its name in the Pocket library only.
* To edit the XY artwork separately, choose **My projects → Extract editable artwork** on a desktop reference. This makes a new drawing from all displayed outlines. SVG preserves open paths and separate contours. A Studio export from an artwork copy has output off for every object. Review and enable the intended objects in Studio before generating a toolpath.

Try opening a two-path desktop project containing an open line and a closed outline. Extract the artwork, edit an object's position, export it and reopen the original reference from **My projects**. Both drawings stay available.

## Pair the CNC pendant

Open **CNC pendant** in Pocket. Connect the phone and Studio receiver using Wi-Fi, a USB-tether IP link or Bluetooth tethering/PAN. The receiver continues to control the machine. See the [phone connection guide](PHONE-CONNECTIONS.md) for the transport setup.

1. Start the Studio receiver with the phone monitor and the desired remote control permissions enabled. Tap **Scan QR code** and hold the code shown under **Phone pendant** in Studio inside the square; Pocket asks for camera access the first time, and pairs as soon as the code is read. Or paste the monitor’s complete pairing link into **Monitor pairing link**, then tap **Pair**, including the token after the `#`. A link that ends in `&fp=` and a fingerprint is encrypted: Pocket 0.5.0 or later accepts only the certificate Studio created for that pairing. For an HTTPS link without a fingerprint, the certificate must match the address and chain to a system CA or a CA installed on the phone. An HTTP pairing requires explicit confirmation and a private IPv4 address or loopback.
2. Check the displayed receiver state, protocol and coordinate basis. **Work** identifies work coordinates and **Machine** identifies controller machine coordinates. Some controllers omit position. With a connected idle machine and manual controls enabled, select the increment and feed, then tap an X, Y or Z button for a single signed jog. **Home** moves the axes. **Zero XYZ** sets the current work origin. Both ask for confirmation.
3. For a job, prepare and arm the preview in Studio. Confirm **Start** in Pocket to run the armed job. **Pause** and **Resume** follow controller status. **Stop** asks for confirmation before aborting. If an acknowledgement is lost, check the receiver’s current state before issuing another action.
While Pocket waits for a Start or jog response, you can still tap **Stop** and confirm the abort. A pending Start needs a current preview confirmation from the workstation. After a timeout, check the receiver state before arming again. If Pocket displays **Update the Studio receiver for motion controls**, update the receiver for motion commands; paired Stop and status remain available.

4. Backgrounding the pendant disables its controls. Returning to it requests fresh status. **Forget pairing** clears the token. Closing the pendant or restarting the app also requires a new pairing. Restarting the receiver revokes its previous link.

## Scope

Portable imports accept Studio version 1 vector documents up to 4 MB, 128 objects and 100,000 XY points within the declared work area. Projects with only solid features, unsupported formats, invalid coordinates or larger geometry stay on the desktop. Pocket displays sampled planar outlines. The workstation owns 3D modelling, CAM, simulation and the controller connection. Portable import and export use Android’s system document picker; project files require no storage permission.

## Physical handset check

On 21 November 2026 the published Pocket 0.4.1 APK was exercised on a Samsung Galaxy S23 Ultra (SM-S918B, Android 17 / API 37, One UI 9.0, display set to 1080 × 2316) over wireless ADB. The installed package had the same SHA-256 as the release download. The pendant ran against Studio 0.6.1's simulated GRBL controller on a Windows workstation over Wi-Fi; no machine was connected.

| Check | Result |
| --- | --- |
| Cold start | Two cold starts, 273 ms and 499 ms, with no crash or not-responding entry |
| Project exchange | A two-shape drawing exported as `pi-cnc-studio/1`; Studio generated G-code from the file; the drawing survived a cold restart and reopened from the file as a copy |
| Layout | In portrait and landscape every control cleared the status bar, camera cut-out and three-button navigation bar |
| Pendant | Pair, 0.1 mm X jog, Home, Zero XYZ, Start, Pause, Resume, Stop and Forget pairing; each action reached Studio once |
| Background return | With a job running, Pocket spent 6 s in the background, returned to the pendant from the launcher and Stop then aborted the job |
| S Pen | 53 pen contacts were recorded. Add shape, Apply and the objects list worked with the pen as a pointer. Pressure and hover are not used |

`validation/android_handset_pendant_workflow.py` repeats the pendant and background-return rows on a supervised handset. The device workflows drive the debug build; set `KERFKITE_POCKET_PACKAGE=au.cncstudio.pocket` to drive the released app. Add `--encrypted` to pair through Studio's pinned HTTPS listener. `validation/android_handset_selection_workflow.py` adds three overlapping rectangles to the debug build's drawing and checks that each can be selected by tapping the preview. `validation/pairing_qr_check.py --zxing <core jar>` needs no phone: it decodes the pairing codes Studio draws with Pocket's own decoder from simulated camera frames. The workstation firewall must admit the handset on the monitor port: the first attempt showed **The receiver timed out** until the inbound rule covered every network profile.

**Selection defect found in this check.** In Pocket 0.4.1 a new shape is placed at the same default position and size as the previous one, and a tap in the drawing preview selects only the topmost shape whose bounding box contains it. Two shapes left at the defaults therefore overlap exactly, and the lower one can be selected only from the objects list. Of 44 pen taps on the preview, 31 landed inside the shared box and selected the topmost shape each time, and 13 missed the box by a few pixels because the test had no tolerance. Injected finger and stylus taps behaved identically, so this is not specific to the pen. The source after 0.4.1 steps each new shape clear of the last, cycles through overlapping shapes on repeated taps and accepts taps within 12 dp of a shape. Replaying the 13 recorded near-miss taps through that logic registers 12 of them. The change has been compiled and checked off-device; it has not yet been run on the handset.

**Pocket 0.5.0 on the same handset.** On 21 November 2026 a debug build of the 0.5.0 source, installed beside the released app, was driven over wireless ADB:

| Check | Result |
| --- | --- |
| Shape selection | With three overlapping default rectangles: a corner covered by one shape selected it, repeated taps over two shapes alternated between them, three taps at the common centre reached all three, and a tap clear of every shape kept the selection |
| Encrypted pendant | The full pendant sequence over the pinned HTTPS link passed in 14 of 15 runs. In the first run **Home** was unavailable for about 25 seconds after the jog and the run stopped; it did not recur and is unexplained |
| Plain pendant | The same sequence over a plain link, on the shrunk build |
| QR pairing | The scanner asked for camera access, opened the rear camera at 1280 × 720 and released it on Cancel, Back and in the background. A code shown on a workstation screen was scanned by hand and Pocket paired and showed live status |

The signed 0.5.0 release APK has the same contents, entry for entry, as the unsigned build of that source. An in-place upgrade of the released 0.4.1 app on a phone was not part of this check.

Not exercised on the handset: the **Keep screen awake** switch, the pendant in landscape, the panel's full-resolution display mode, USB and Bluetooth tethering, the HTTPS monitor, and any physical controller.

