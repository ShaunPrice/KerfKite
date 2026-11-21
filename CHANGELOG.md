# Changelog

Notable changes to KerfKite Studio and KerfKite Pocket. Versions before this file was introduced are described in the [GitHub release notes](https://github.com/ShaunPrice/KerfKite/releases) and the [release review](docs/RELEASE-REVIEW.md).

## Unreleased

### Added
- Probe levelling for thin engraving such as circuit boards. On a Grbl-family controller, **Probe levelling** under **Control** probes the stock on a grid over the previewed router job (`G38.2`), sets work Z zero where the first point touches, and runs the job with every cutting move split and corrected to follow the measured surface. The levelled program can also be exported, and `kerfkite-cam` carries the same steps (`probe-plan`, `height-map`, `level`) without Studio. The simulator has a tilted stock surface and answers probes, so the whole sequence can be rehearsed. Levelling is written once, in the native core, and needs the compiled extension. It has not yet been run on a physical controller. See [Probe levelling](docs/PROBE-LEVELING.md).
- A Python-free native core (`native/`): the `kerfkite-cam` command-line tool writes G-code for router line jobs, laser line jobs (leads, power ramps, perforation, holding bridges, raster powers) and ordered multi-operation jobs, for every supported controller, with no Python installed. It is the first stage of porting the application to C++. Anything it cannot reproduce exactly is declined. `validation/native_core_parity.py` compares it with the Python reference.
- The native core writes drilling programs (point markers and circular hole outlines, in one plunge or in pecks) and drag-knife programs (alignment lead-in and corner swivels), identical to Studio's.
- Pair Pocket by QR code. Studio draws the pairing link as a QR code under **Phone pendant**, and Pocket's **CNC pendant** has **Scan QR code**, which reads it with the rear camera and pairs exactly as a pasted link does. Pocket asks for camera access when the scanner is first opened and uses it for nothing else. Decoding uses ZXing core (Apache-2.0).

### Fixed
- Phone pairing offers the workstation's routed network address first. It used to offer the numerically lowest address, which on a machine with WSL, Hyper-V or a container bridge is a virtual adapter no phone can reach; the pairing link, and now the QR code, then pointed the phone at it. The other addresses are listed in the address box.
- Pocket: the light navigation bar attribute, which needs API 27, is no longer referenced on API 26.
- Design routes (`/api/geometry`, `/api/extrude`, `/api/artwork-relief` and the other design endpoints) answer malformed bodies with a JSON `400`. Contours that are not lists of finite XY points are rejected before any geometry is built.
- Pocket: every shape can be selected from the drawing preview. New shapes no longer start exactly on top of the previous one, a repeated tap moves to the next overlapping shape, and a tap within 12 dp of an outline counts.
- Machine-control routes of the local server (`/api/connect`, `/api/command`, `/api/run`, `/api/head`, `/api/arm` and the arm follow-ups, `/api/generate`, `/api/phone-monitor`) now answer a malformed or non-object body with a JSON `400` instead of an unhandled `500`.

### Performance
- The per-move loops of G-code and rotary generation are ported to C++ in the optional native extension (`studio/_gcode_native.cpp`). With it the relief benchmark generates 4.5 times faster and the rotary benchmark 4.4 times faster than the Python loops, with byte-identical programs. Python remains the reference and the fallback; `backend='python'` or `KERFKITE_GCODE_BACKEND=python` selects it.
- G-code generation is 21% to 31% faster and rotary generation 27% faster on the relief benchmark, with byte-identical programs (`validation/benchmark_gcode.py`).
- Packaged screenshots are recompressed losslessly (635 KiB smaller, pixels identical). The two generated artwork images are untouched because re-encoding would invalidate their content credentials.
- Pocket no longer allocates while drawing the preview and computes each shape's outline once.

### Security
- The phone pendant listener is encrypted by default when the `cryptography` package is installed. Studio creates a certificate for each pairing and places its SHA-256 fingerprint in the pairing link; Pocket 0.5.0 accepts only that certificate. **Pocket 0.4.1 and earlier cannot use these links**: tick **Unencrypted pairing** or start with `--monitor-plaintext` until the phone is updated. The desktop installers do not yet bundle `cryptography` and continue to use HTTP.
- The desktop installers now include `cryptography`, so an installed Studio encrypts the pendant without a further step. Intel macOS receives 48.0.1, the last release published for it; the other targets receive 50.0.2.
- `/api/command` accepts only `action`, `axis`, `distance` and `feed`; any other field is refused before the controller is called.
- The local session token is compared in constant time.

### Development
- Pocket builds remove code they never reach (R8, names kept), for debug and release alike. With the QR scanner the debug APK is 115 KB and the unsigned release APK 73 KB; unshrunk, the scanner's library took the debug APK from 56 KB to 358 KB.
- `validation/pairing_qr_check.py` decodes the pairing codes Studio draws with Pocket's decoder, from simulated camera frames.
- A debug build of Pocket installs beside the released app as **Pocket debug** (`au.cncstudio.pocket.debug`), so a change can be tried on a phone without replacing the released app or its projects. The device workflows drive the debug build unless `KERFKITE_POCKET_PACKAGE` names another id.
- `validation/pendant_tls_pin_check.py` checks Pocket's pinning class against a real phone listener with a JDK, without a phone.
- Pocket is versioned 0.5.0 (version code 6).
- `tests/test_design_api.py` covers the design routes (28% to 87% of `studio/design_api.py`). Overall coverage is 86% with the `mcp`, `web` and `step` extras and 76% on a plain install; the pre-push hook enforces a 75% floor when `pytest-cov` is installed.
- Python sources are formatted with `ruff format` (120 columns, existing quote style kept). The change is layout only: the syntax tree of every file is identical before and after. The pre-push hook and `scripts/ci-local.sh` check formatting.
- `scripts/build_help.py` orders the guide sections by file name, so the bundled help is identical whichever machine builds it.
- `validation/android_handset_pendant_workflow.py` runs the pendant sequence on a supervised physical handset.
- Android: Build Tools 35.0.0 is pinned to match the guide.
- Ruff configuration (`pyproject.toml`) covering syntax, pyflakes, likely-bug and security rules for `studio/`; `scripts/ci-local.sh` runs it.
- `scripts/hooks/pre-push`: a fast local gate (lint, Python tests, JavaScript tests) for use while hosted CI is not run on push.
- `dev` extra with the lint and test tools.
- Dependabot version updates for Python, Gradle and workflow actions.
