# Building KerfKite installers

The desktop launcher contains only Go standard-library code. Each package carries a platform-matched Studio wheel with the C++ relief and stock simulators and locked requirements. The launcher fetches the platform’s uv archive from the pinned upstream release, verifies its SHA-256, and uses it to install a private CPython 3.12 runtime. All application wheels are hash-checked. Python, geometry libraries and optional STEP binaries are installed on the user’s machine.

The installers are how every part of the product, and each third-party component kept inside it, reaches a user. [What the installers install](SUPPLY-CHAIN.md#what-the-installers-install) lists each part, what has to be put in place for it, and the rule that a component and its installation land in the same change.

## Release workstation

Use macOS with Go, Python 3.12, Pillow, uv, dpkg-deb, Apple command line tools and NSIS (makensis). Alternatively the Windows packaging step accepts a local Docker image named kerfkite-installer-nsis containing Debian’s NSIS package. All paths below are relative to the checkout root.

1. Set `__version__` in `studio/__init__.py` for a new Studio release and check its download links. Refresh dependency locks when dependencies change. For a new Android build, update its version separately. Rebuild the bundled help with `python scripts/build_help.py`. To refresh the locks, run `uv pip compile pyproject.toml --universal --python-version 3.12 --only-binary :all: --generate-hashes --no-header --no-annotate -o packaging/desktop/requirements.lock` and repeat with `--extra step -c packaging/desktop/requirements.lock -o packaging/desktop/step.lock`.
2. Build one CPython 3.12 wheel per desktop target. Run the manual **Build desktop CAM wheels** workflow to build and exercise all five target wheels. Download its artifacts to one directory, for example with `gh run download RUN_ID --pattern '*-wheel' --dir dist/wheels --merge`. To build one wheel locally on its target, run `KERFKITE_REQUIRE_NATIVE=1 python -m build --wheel --outdir dist/wheels` with a C++ compiler installed. On Windows PowerShell set `$env:KERFKITE_REQUIRE_NATIVE='1'` first. For Linux releases use the workflow's manylinux build and repair steps. For the browser-only installer also build a portable wheel with `KERFKITE_PORTABLE_WHEEL=1 uv build --wheel --out-dir dist/browser`.
3. Run `python packaging/desktop/build.py dist/wheels dist/installers mac-arm64 mac-amd64 linux-arm64 linux-amd64 windows-amd64`. Each desktop installer embeds its target's compiled wheel. A missing, ambiguous or portable-only wheel stops packaging. The launcher's private runtime installs that local wheel. The macOS application has an ad-hoc signature and Windows Setup uses per-user installation.
4. When releasing the Android companion, build its release from the android directory using `./gradlew assembleRelease`. Sign the aligned release APK with the project's existing release alias and Android SDK apksigner. Keep signing credentials in private storage. Verify the APK using `apksigner verify --verbose --print-certs`. Future upgrades must use the same certificate and increment the Android version code.
5. For each release file, publish its SHA-256 as a line of `SHA256SUMS.txt`. Upload the packages and checksums to a release for the corresponding source commit. In **Actions → Check desktop release installers**, run the workflow against that tag. On Linux and macOS it installs the release, tests the window, generates a simulated toolpath, exercises STEP and relaunches offline. The hosted Windows Server runner checks that Setup rejects that operating system before installing any files. Run the graphical acceptance test on a Windows 10 or 11 x64 client.

For local smoke testing, run `python validation/installer_smoke.py "/path/to/installed/KerfKite" --step` with ports 18764 and 18765 free. The test uses a temporary application-data directory. The shortcut opens the setup tab. Starting a workshop opens the selected native interface. Platform-specific Gatekeeper, SmartScreen and desktop shell prompts need a graphical desktop to check.

On Apple Silicon with Rosetta, add `--also-intel "/path/to/Intel.app/Contents/MacOS/KerfKite"` to the Apple Silicon smoke command to exercise both installers with the same data directory. Each architecture has its own private Python runtime and setup tool.

The Debian package version combines the wheel version with the packaging revision, normally `-1`. The package declares `libgl1` so OpenCascade STEP loads on minimal Debian installations. A portable Linux archive expects the desktop to provide `libGL.so.1` for STEP. The full application uses an architecture-specific wheel.

Build the assistant skill with `python scripts/package_skill.py dist/installers/kerfkite-workshop-skill.zip`.
Include the ZIP in `SHA256SUMS.txt`. Extracting it produces the `kerfkite-workshop` folder with its instructions, interface metadata and MIT license.

## Native windows

The release builder compiles AppKit with the macOS SDK for both architectures and signs the helper before signing the application bundle. Linux packages carry the executable GTK script and declare their GI and WebKitGTK dependencies. Building Windows requires the .NET SDK with Roslyn. The Windows build script verifies pinned NuGet packages before compiling, bundles the WebView2 managed assemblies and x64 loader, and includes their Microsoft licence and notices. The runtime is supplied separately by Microsoft. Set `KERFKITE_NATIVE_BUILD_CACHE` to reuse verified downloads.

From a signed-in desktop on a Windows 10 or 11 x64 client with Python, 7-Zip and the .NET SDK, run `validation/windows_client_smoke.ps1 -Installer <setup.exe> -Sha256 <release-digest>`. It checks the installer and both executables, exercises the extracted release in a temporary simulator, tests the native controls, and relaunches offline. The check leaves Start menu shortcuts and the installed application directory alone.

The window sources and platform smoke checks are in `packaging/desktop/{macos,windows,linux}/`. The shared document adapter lives in `studio/static/desktop-workspace.js`. Its asynchronous open uses the regular project validator, including simulation of saved machining settings. Export checks that the preview still matches the current design. Run `validation/simulate_solid_workspace.py` for the shared editor regression checks.

## Browser-only SBC packages

Build the portable wheel from the release checkout as described above. From that checkout, use Python 3.12 and dpkg-deb to package the wheel matching the project version.

```sh
studio_version=$(python -c 'from studio import __version__; print(__version__)')
python packaging/desktop/build_browser.py "dist/browser/kerfkite_studio-${studio_version}-py3-none-any.whl" dist/sbc
```

The portable wheel supplies the static editor and all vendor notices. The output is an architecture-independent Debian package with a desktop menu entry, and a portable ZIP with the same editor. Publish both under the release tag with `SBC-SHA256SUMS.txt`. Run **Check SBC release packages** in Actions against the tag to verify the downloads and exercise both installation paths.

To package the interactive materials guide as one offline HTML file, run `python scripts/build_materials_guide.py dist/KerfKite-Materials-Guide.html`. Include that file in the release and its checksum in `SHA256SUMS.txt`. The wheel and browser packages already carry the guide as linked static assets.

Build the stock-art SVG archive from the same source tree with `python scripts/export_stock_art.py dist/KerfKite-Stock-Artwork.zip`. Its offline index links to each individual design.
