# Studio Pocket for Android

Studio Pocket is a native, offline vector sketch editor for Android 8 and later. It uses a touch canvas and numerical dimensions. Files saved from the app open in the desktop and Raspberry Pi workstation. The current template, solid modelling and machining workflows run on that workstation.

## Install and draw

For normal installation, download the [signed KerfKite Pocket APK](https://github.com/ShaunPrice/KerfKite/releases/download/v0.2.1/KerfKite-Pocket-Android.apk) on the phone, open it from Downloads and choose **Install**.

For development, build the debug APK using Android Studio with JDK 17 or newer, Android SDK Platform 35 and Build Tools 35 installed. Open the repository’s android directory as the project. Alternatively, from the android directory run:

```sh
./gradlew assembleDebug
```

On Windows run `gradlew.bat assembleDebug`. The first build fetches the pinned build tools. Subsequent builds can use `--offline` once the dependencies are cached. The installable debug APK appears at `app/build/outputs/apk/debug/app-debug.apk`. Transfer the APK to the phone and open it from Files, or choose Run in Android Studio with your development device selected.

1. Tap **Add shape** and choose Rectangle, Ellipse or Polygon. Enter **Width** and **Height** in millimetres. **X** and **Y** locate the lower-left corner on the 680 × 240 mm example work area. A polygon accepts 3–64 sides. Tap **Apply**.
2. Tap the drawing or its object row to select a shape. **Edit** reopens its dimensions. The preview fits all shapes automatically. For overlapping artwork, use the object list to select the lower shape. Each applied change saves the private draft on this device.
3. Choose **CNC router**, **Laser** or **Drag knife**. Tap **Studio project**, choose a destination in the system file picker and save. **SVG** writes a vector drawing with physical dimensions and preserves the placement on the work area.
4. Open the project in KerfKite Studio. The workstation reads the head selection and editable outline geometry. Set the intended tool, feed, depth or laser power, then generate the machining preview. The native app exports design files and does not send machine commands.

**Quick guide** inside the app walks through these same controls. Clearing app data deletes its private draft. Use the system file picker to keep a portable copy.

## Scope

A draft supports up to 128 objects. The phone retains primitive dimensions in its local draft. Portable project files contain sampled paths for compatibility with the workstation. The phone currently has one local draft and exports through Android's document picker; it does not reopen project files from other devices. Its only Android activity is the editor. The manifest requests no network or external storage permissions.
