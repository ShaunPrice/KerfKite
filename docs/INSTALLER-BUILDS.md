# Building KerfKite installers

The desktop launcher contains only Go standard-library code. Each package carries the workstation server `kerfkite-studio` (see [The workstation server](NATIVE.md#the-workstation-server)) with the editor's files, the HTTPS editor `kerfkite-web`, the MCP assistant server `kerfkite-mcp` with the template collection, `kerfkite-cam`, the phone pendant's TLS relay and STEP exchange as the Open CASCADE add-on `kerfkite-step` with the Open CASCADE 7.9.3 libraries and their notices (LGPL 2.1, as separate files that can be replaced). The launcher finds `kerfkite-studio` beside itself (in `Contents/Resources` on macOS) and starts it with the editor's files from `static/`, in simulation unless the operator enables the controller ports for the session. A package needs no Python and downloads nothing when it first starts. Windows Setup offers STEP as a component of its own, which a reinstall without it removes; the macOS and Linux packages carry it. The launcher's setup page says whether STEP is installed.

The installers are how every part of the product, and each third-party component kept inside it, reaches a user. [What the installers install](SUPPLY-CHAIN.md#what-the-installers-install) lists each part, what has to be put in place for it, and the rule that a component and its installation land in the same change.

## Release workstation

Use macOS with Go, Python 3.12 with Pillow (for the packaging script), dpkg-deb, Apple command line tools and NSIS (makensis). Alternatively the Windows packaging step accepts a local Docker image named kerfkite-installer-nsis containing Debian's NSIS package. All paths below are relative to the checkout root.

1. Set the version in `VERSION` for a new Studio release and check its download links. For a new Android build, update its version separately. Rebuild the bundled help with `python scripts/build_help.py`.
2. Run the manual **Build the desktop programs without Python** workflow (`native-programs.yml`). For each desktop target it builds Open CASCADE 7.9.3 from the pinned source with `native/step/open-cascade.cmake` (kept in the cache afterwards), builds and tests the programs (Linux on manylinux_2_28, for glibc 2.28 and later), and uploads a folder of them, made by `cmake --install`. Download the folders with `gh run download RUN_ID --pattern 'native-*' --dir dist/native`.
3. Run `python packaging/desktop/build.py dist/native dist/installers mac-arm64 mac-amd64 linux-arm64 linux-amd64 windows-amd64`. A target whose folder lacks any of the programs (the workstation server, the HTTPS editor, the MCP assistant server, `kerfkite-cam` or the relay) stops packaging. The macOS application has an ad-hoc signature and Windows Setup uses per-user installation.

   Every package puts KerfKite Studio where a new user looks for it. Windows Setup always adds it to Start, and its components page offers a desktop shortcut, ticked; the uninstaller removes both, and a reinstall without the shortcut removes an old one. The Debian package installs `org.kerfkite.Studio.desktop` in the applications menu. The DMG carries an Applications link to drag the app onto. The launcher does the rest on the first **Choose your workshop** page: a desktop shortcut on macOS and Linux, and for the portable Linux archive a menu entry pointing at wherever it was extracted, using the `kerfkite-studio.png` the archive carries. It marks the entries it writes with `X-KerfKite-Launcher=shortcut`, repoints them when the folder moves and leaves every other file alone. Opened from the disk image or its translocated copy, the Mac launcher asks the user to drag the app to Applications first.
4. When releasing the Android companion, build its release from the android directory using `./gradlew assembleRelease`. Sign the aligned release APK with the project's existing release alias and Android SDK apksigner. Keep signing credentials in private storage. Verify the APK using `apksigner verify --verbose --print-certs`. Future upgrades must use the same certificate and increment the Android version code.
5. For each release file, publish its SHA-256 as a line of `SHA256SUMS.txt`. Upload the packages and checksums to a release for the corresponding source commit. In **Actions → Check desktop release installers**, run the workflow against that tag. On Linux and macOS it installs the release, tests the window, generates a simulated toolpath, exercises STEP and relaunches offline. The hosted Windows Server runner checks that Setup rejects that operating system before installing any files. Run the graphical acceptance test on a Windows 10 or 11 x64 client.

For local smoke testing, run `python validation/installer_smoke.py "/path/to/installed/KerfKite" --step` with ports 18764 and 18765 free. The test uses a temporary application-data directory, checks that the launcher set nothing up and downloaded nothing, and writes and reads a STEP part through the add-on. The shortcut opens the setup tab. On Linux it also asks for both shortcuts, in temporary XDG data and desktop folders, and checks that the desktop entry starts the installed launcher and that only a portable copy writes its own menu entry. `go test` in `packaging/desktop` covers the shortcut rules on every platform, including folder names that need quoting. Starting a workshop opens the selected native interface. Platform-specific Gatekeeper, SmartScreen and desktop shell prompts need a graphical desktop to check.

On Apple Silicon with Rosetta, add `--also-intel "/path/to/Intel.app/Contents/MacOS/KerfKite"` to the Apple Silicon smoke command to exercise both installers with the same data directory.

The Debian package version combines the Studio version with the packaging revision, normally `-1`. The package declares `libgl1` so Open CASCADE loads on minimal Debian installations. A portable Linux archive expects the desktop to provide `libGL.so.1` for STEP.

Build the assistant skill with `python scripts/package_skill.py dist/installers/kerfkite-workshop-skill.zip`.
Include the ZIP in `SHA256SUMS.txt`. Extracting it produces the `kerfkite-workshop` folder with its instructions, interface metadata and MIT license.

## Native windows

The release builder compiles AppKit with the macOS SDK for both architectures and signs the helper before signing the application bundle. The Linux window is a C program (`linux/`) on GTK 3 that finds WebKitGTK 4.1, or else 4.0, when it starts, so one build runs with either; building it needs a C compiler, CMake, pkg-config and the GTK 3 headers (`libgtk-3-dev` on Debian and Ubuntu). Linux packages carry it and declare GTK 3 and WebKitGTK, and no Python. For the widest reach, build them on the oldest distribution you support: the window then needs no newer C library than it. Building Windows requires the .NET SDK with Roslyn. The Windows build script verifies pinned NuGet packages before compiling, bundles the WebView2 managed assemblies and x64 loader, and includes their Microsoft licence and notices. The runtime is supplied separately by Microsoft. Set `KERFKITE_NATIVE_BUILD_CACHE` to reuse verified downloads.

From a signed-in desktop on a Windows 10 or 11 x64 client with Python, 7-Zip and the .NET SDK, run `validation/windows_client_smoke.ps1 -Installer <setup.exe> -Sha256 <release-digest>`. It checks the installer and both executables, exercises the extracted release in a temporary simulator, tests the native controls, and relaunches offline. The check leaves Start menu shortcuts and the installed application directory alone.

The window sources and platform smoke checks are in `packaging/desktop/{macos,windows,linux}/`. The shared document adapter lives in `studio/static/desktop-workspace.js`. Its asynchronous open uses the regular project validator, including simulation of saved machining settings. Export checks that the preview still matches the current design. Run `validation/simulate_solid_workspace.py` for the shared editor regression checks.

## Browser-only SBC packages

From the release checkout, use Python 3.12 and dpkg-deb to package the editor's files:

```sh
python packaging/desktop/build_browser.py dist/sbc
```

The package carries the static editor from `studio/static` with all its vendor notices, at the version in `VERSION`. The output is an architecture-independent Debian package with a desktop menu entry, and a portable ZIP with the same editor. Publish both under the release tag with `SBC-SHA256SUMS.txt`. Run **Check SBC release packages** in Actions against the tag to verify the downloads and exercise both installation paths.

To package the interactive materials guide as one offline HTML file, run `python scripts/build_materials_guide.py dist/KerfKite-Materials-Guide.html`. Include that file in the release and its checksum in `SHA256SUMS.txt`. The desktop and browser packages already carry the guide as linked static assets.

Build the stock-art SVG archive from the same source tree with `python scripts/export_stock_art.py dist/KerfKite-Stock-Artwork.zip`. Its offline index links to each individual design.
