# Building KerfKite installers

The desktop launcher contains only Go standard-library code. Each package carries the same portable Studio wheel and locked requirements. The launcher fetches the platform’s uv archive from the pinned upstream release, verifies its SHA-256, and uses it to install a private CPython 3.12 runtime. All application wheels are hash-checked. Python, geometry libraries and optional STEP binaries are installed on the user’s machine.

## Release workstation

Use macOS with Go, Python 3.12, Pillow, uv, dpkg-deb, Apple command line tools and NSIS (makensis). Alternatively the Windows packaging step accepts a local Docker image named kerfkite-installer-nsis containing Debian’s NSIS package. All paths below are relative to the checkout root.

1. Update the Studio version and download links. Refresh dependency locks when dependencies change. For a new Android build, update its version separately. Rebuild the bundled help with `python scripts/build_help.py`. To refresh the locks, run `uv pip compile pyproject.toml --universal --python-version 3.12 --only-binary :all: --generate-hashes --no-header --no-annotate -o packaging/desktop/requirements.lock` and repeat with `--extra step -c packaging/desktop/requirements.lock -o packaging/desktop/step.lock`.
2. In a clean checkout, run `KERFKITE_PORTABLE_WHEEL=1 uv build --wheel --out-dir dist`. Inspect the wheel for the editor, licences and desktop server. It uses the portable Python relief engine. The normal source installation can compile the optional native kernel.
3. Run `python packaging/desktop/build.py dist/kerfkite_studio-0.4.1-py3-none-any.whl dist/installers mac-arm64 mac-amd64 linux-arm64 linux-amd64 windows-amd64`. The staging filesystem needs free space for each package. `go build -trimpath` strips workstation paths. The macOS application has an ad-hoc signature. Windows Setup uses per-user installation.
4. When releasing the Android companion, build its release from the android directory using `./gradlew assembleRelease`. Sign the aligned release APK with the project's existing release alias and Android SDK apksigner. Keep signing credentials in private storage. Verify the APK using `apksigner verify --verbose --print-certs`. Future upgrades must use the same certificate and increment the Android version code.
5. For each release file, publish its SHA-256 as a line of `SHA256SUMS.txt`. Upload the packages and checksums to a release for the corresponding source commit. In **Actions → Check desktop release installers**, run the workflow against that tag. It installs the actual release package, tests a clean first launch, generates a simulated toolpath, exercises STEP, then relaunches with uv offline.

For local smoke testing, run `python validation/installer_smoke.py "/path/to/installed/KerfKite" --step` with ports 18764 and 18765 free. The test uses a temporary application-data directory. The application shortcut opens a browser tab. Platform-specific Gatekeeper, SmartScreen and desktop shell prompts need a graphical desktop to check.

The Debian packaging revision for Studio 0.2.1 is `-2`. It declares `libgl1` so OpenCascade STEP loads on minimal Debian installations. A portable Linux archive expects the desktop to provide `libGL.so.1` for STEP. The application wheel is shared across all platforms.

## Browser-only SBC packages

Run `python packaging/desktop/build_browser.py path/to/kerfkite_studio-0.4.1-py3-none-any.whl dist/sbc` with Python and dpkg-deb. Use the same verified wheel as the full release. It supplies the static editor and all vendor notices. The output is an architecture-independent Debian package with a desktop menu entry, and a portable ZIP with the same editor. Publish both under the release tag with `SBC-SHA256SUMS.txt`. Run **Check SBC release packages** in Actions against the tag to verify the downloads and exercise both installation paths.

To package the interactive materials guide as one offline HTML file, run `python scripts/build_materials_guide.py dist/KerfKite-Materials-Guide.html`. Include that file in the release and its checksum in `SHA256SUMS.txt`. The wheel and browser packages already carry the guide as linked static assets.

Build the stock-art SVG archive from the same source tree with `python scripts/export_stock_art.py dist/KerfKite-Stock-Artwork.zip`. Its offline index links to each individual design.
