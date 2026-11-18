# Building KerfKite installers

The desktop launcher contains only Go standard-library code. Each package carries the same portable Studio wheel and locked requirements. The launcher fetches the platform’s uv archive from the pinned upstream release, verifies its SHA-256, and uses it to install a private CPython 3.12 runtime. All application wheels are hash-checked. Python, geometry libraries and optional STEP binaries are installed on the user’s machine.

## Release workstation

Use macOS with Go, Python 3.12, Pillow, uv, dpkg-deb, Apple command line tools and NSIS (makensis). Alternatively the Windows packaging step accepts a local Docker image named kerfkite-installer-nsis containing Debian’s NSIS package. All paths below are relative to the checkout root.

1. Update the Studio version and download links. Refresh dependency locks when dependencies change. For a new Android build, update its version separately. Rebuild the bundled help with `python scripts/build_help.py`. To refresh the locks, run `uv pip compile pyproject.toml --universal --python-version 3.12 --only-binary :all: --generate-hashes --no-header --no-annotate -o packaging/desktop/requirements.lock` and repeat with `--extra step -c packaging/desktop/requirements.lock -o packaging/desktop/step.lock`.
2. In a clean checkout, run `KERFKITE_PORTABLE_WHEEL=1 uv build --wheel --out-dir dist`. Inspect the wheel for the editor, licences and desktop server. It uses the portable Python relief engine. The normal source installation can compile the optional native kernel.
3. Run `python packaging/desktop/build.py dist/kerfkite_studio-0.6.0-py3-none-any.whl dist/installers mac-arm64 mac-amd64 linux-arm64 linux-amd64 windows-amd64`. The staging filesystem needs free space for each package. `go build -trimpath` strips workstation paths. The macOS application has an ad-hoc signature. Windows Setup uses per-user installation.
4. When releasing the Android companion, build its release from the android directory using `./gradlew assembleRelease`. Sign the aligned release APK with the project's existing release alias and Android SDK apksigner. Keep signing credentials in private storage. Verify the APK using `apksigner verify --verbose --print-certs`. Future upgrades must use the same certificate and increment the Android version code.
5. For each release file, publish its SHA-256 as a line of `SHA256SUMS.txt`. Upload the packages and checksums to a release for the corresponding source commit. In **Actions → Check desktop release installers**, run the workflow against that tag. On Linux and macOS it installs the release, tests the window, generates a simulated toolpath, exercises STEP and relaunches offline. The hosted Windows Server runner checks that Setup rejects that operating system before installing any files. Run the graphical acceptance test on a Windows 10 or 11 x64 client.

For local smoke testing, run `python validation/installer_smoke.py "/path/to/installed/KerfKite" --step` with ports 18764 and 18765 free. The test uses a temporary application-data directory. The shortcut opens the setup tab. Starting a workshop opens the selected native interface. Platform-specific Gatekeeper, SmartScreen and desktop shell prompts need a graphical desktop to check.

The Debian packaging revision for Studio 0.2.1 is `-2`. It declares `libgl1` so OpenCascade STEP loads on minimal Debian installations. A portable Linux archive expects the desktop to provide `libGL.so.1` for STEP. The application wheel is shared across all platforms.

Build the assistant skill with `python scripts/package_skill.py dist/installers/kerfkite-workshop-skill.zip`.
Include the ZIP in `SHA256SUMS.txt`. Extracting it produces the `kerfkite-workshop` folder with its instructions, interface metadata and MIT license.

## Native windows

The release builder compiles AppKit with the macOS SDK for both architectures and signs the helper before signing the application bundle. Linux packages carry the executable GTK script and declare their GI and WebKitGTK dependencies. Building Windows requires the .NET SDK with Roslyn. The Windows build script verifies pinned NuGet packages before compiling, bundles the WebView2 managed assemblies and x64 loader, and includes their Microsoft licence and notices. The runtime is supplied separately by Microsoft. Set `KERFKITE_NATIVE_BUILD_CACHE` to reuse verified downloads.

From a signed-in desktop on a Windows 10 or 11 x64 client with Python, 7-Zip and the .NET SDK, run `validation/windows_client_smoke.ps1 -Installer <setup.exe> -Sha256 <release-digest>`. It checks the installer and both executables, exercises the extracted release in a temporary simulator, tests the native controls, and relaunches offline. The check leaves Start menu shortcuts and the installed application directory alone.

The window sources and platform smoke checks are in `packaging/desktop/{macos,windows,linux}/`. The shared document adapter lives in `studio/static/desktop-workspace.js`. Its asynchronous open uses the regular project validator, including simulation of saved machining settings. Export checks that the preview still matches the current design. Run `validation/simulate_solid_workspace.py` for the shared editor regression checks.

## Browser-only SBC packages

Run `python packaging/desktop/build_browser.py path/to/kerfkite_studio-0.6.0-py3-none-any.whl dist/sbc` with Python and dpkg-deb. Use the same verified wheel as the full release. It supplies the static editor and all vendor notices. The output is an architecture-independent Debian package with a desktop menu entry, and a portable ZIP with the same editor. Publish both under the release tag with `SBC-SHA256SUMS.txt`. Run **Check SBC release packages** in Actions against the tag to verify the downloads and exercise both installation paths.

To package the interactive materials guide as one offline HTML file, run `python scripts/build_materials_guide.py dist/KerfKite-Materials-Guide.html`. Include that file in the release and its checksum in `SHA256SUMS.txt`. The wheel and browser packages already carry the guide as linked static assets.

Build the stock-art SVG archive from the same source tree with `python scripts/export_stock_art.py dist/KerfKite-Stock-Artwork.zip`. Its offline index links to each individual design.
