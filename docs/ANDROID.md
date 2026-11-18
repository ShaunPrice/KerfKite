# Studio Pocket for Android

Studio Pocket combines an offline vector sketch editor with a native CNC pendant for Android 8 and later. It uses a touch canvas and numerical dimensions. Files saved from the app open in the desktop and Raspberry Pi workstation. The current template, solid modelling and machining workflows run on that workstation.

## Install and draw

For normal installation, download the [signed KerfKite Pocket APK](https://github.com/ShaunPrice/KerfKite/releases/download/v0.4.0/KerfKite-Pocket-Android.apk) on the phone, open it from Downloads and choose **Install**.

For development, build the debug APK using Android Studio with JDK 17 or newer, Android SDK Platform 35 and Build Tools 35 installed. Open the repository’s android directory as the project. Alternatively, from the android directory run:

```sh
./gradlew assembleDebug
```

On Windows run `gradlew.bat assembleDebug`. The first build fetches the pinned build tools. Subsequent builds can use `--offline` once the dependencies are cached. The installable debug APK appears at `app/build/outputs/apk/debug/app-debug.apk`. Transfer the APK to the phone and open it from Files, or choose Run in Android Studio with your development device selected.

1. Tap **Add shape** and choose Rectangle, Ellipse or Polygon. Enter **Width** and **Height** in millimetres. **X** and **Y** locate the lower-left corner on the project’s work area (680 × 240 mm for a new drawing). A polygon accepts 3–64 sides. Tap **Apply**.
2. Tap the drawing or its object row to select a shape. **Edit** reopens its dimensions. The preview fits all shapes automatically. For overlapping artwork, use the object list to select the lower shape. Each applied change saves the private draft on this device.
3. Choose **CNC router**, **Laser** or **Drag knife**. Tap **Studio project**, choose a destination in the system file picker and save. **SVG** writes a vector drawing with physical dimensions and preserves the placement on the work area.
4. Open the project in KerfKite Studio. The workstation reads the head selection and editable outline geometry. Set the intended tool, feed, depth or laser power, then generate the machining preview. The sketch workspace exports design files. Open the separate **CNC pendant** screen to pair with the receiver.

**Quick guide** inside the app walks through these same controls. Clearing app data deletes its private project library. Use the system file picker to keep a portable copy.

## Open and organise projects

Choose **My projects → New project** and give the drawing a name. Edits save to the active project in private storage. **My projects** also lets you switch projects, rename one, duplicate it or delete the local copy. Upgrading from Pocket 0.3.0 keeps the previous single draft as **Pocket design**. Export important projects before uninstalling or clearing application data.

Choose **Open file**, select a Studio JSON document with the Android file picker, check the object count and work area, then tap **Open copy**. Your active project stays in the library. Opening or editing a copy leaves the source file untouched.

* **Pocket drawings** reopen with their machine head, dimensions and primitive controls. Compatible outlines from version 0.3.0 recover the original shape when its sampled points match. Other planar artwork supports moving and resizing, including open paths and objects with multiple contours.
* **Desktop references** show their XY outlines in a read-only view. Use **Studio project** to export the complete source document, including the original CAD history, layers, object metadata and machining settings. Renaming a reference changes its name in the Pocket library only.
* To edit the XY artwork separately, choose **My projects → Extract editable artwork** on a desktop reference. This makes a new drawing from all displayed outlines. SVG preserves open paths and separate contours. A Studio export from an artwork copy has output off for every object. Review and enable the intended objects in Studio before generating a toolpath.

Try opening a two-path desktop project containing an open line and a closed outline. Extract the artwork, edit an object's position, export it and reopen the original reference from **My projects**. Both drawings stay available.

## Pair the CNC pendant

Open **CNC pendant** in Pocket. Connect the phone and Studio receiver using Wi-Fi, a USB-tether IP link or Bluetooth tethering/PAN. The receiver continues to control the machine. See the [phone connection guide](PHONE-CONNECTIONS.md) for the transport setup.

1. Start the Studio receiver with the phone monitor and the desired remote control permissions enabled. Paste the monitor’s complete pairing link into **Monitor pairing link**, then tap **Pair**. Include the token after the `#`. For HTTPS, the certificate must match the address and chain to a system CA or a CA installed on the phone. An HTTP pairing requires explicit confirmation and a private IPv4 address or loopback.
2. Check the displayed receiver state, protocol and coordinate basis. **Work** identifies work coordinates and **Machine** identifies controller machine coordinates. Some controllers omit position. With a connected idle machine and manual controls enabled, select the increment and feed, then tap an X, Y or Z button for a single signed jog. **Home** moves the axes. **Zero XYZ** sets the current work origin. Both ask for confirmation.
3. For a job, prepare and arm the preview in Studio. Confirm **Start** in Pocket to run the armed job. **Pause** and **Resume** follow controller status. **Stop** asks for confirmation before aborting. If an acknowledgement is lost, check the receiver’s current state before issuing another action.
4. Backgrounding the pendant disables its controls. Returning to it requests fresh status. **Forget pairing** clears the token. Closing the pendant or restarting the app also requires a new pairing. Restarting the receiver revokes its previous link.

## Scope

Portable imports accept Studio version 1 vector documents up to 4 MB, 128 objects and 100,000 XY points within the declared work area. Projects with only solid features, unsupported formats, invalid coordinates or larger geometry stay on the desktop. Pocket displays sampled planar outlines. The workstation owns 3D modelling, CAM, simulation and the controller connection. Portable import and export use Android’s system document picker; project files require no storage permission.
