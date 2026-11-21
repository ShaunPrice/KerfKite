# Changelog

Notable changes to KerfKite Studio and KerfKite Pocket. Versions before this file was introduced are described in the [GitHub release notes](https://github.com/ShaunPrice/KerfKite/releases) and the [release review](docs/RELEASE-REVIEW.md).

## Unreleased

### Fixed
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
- `/api/command` accepts only `action`, `axis`, `distance` and `feed`; any other field is refused before the controller is called.
- The local session token is compared in constant time.

### Development
- `tests/test_design_api.py` covers the design routes (28% to 87% of `studio/design_api.py`). Overall coverage is 86% with the `mcp`, `web` and `step` extras and 76% on a plain install; the pre-push hook enforces a 75% floor when `pytest-cov` is installed.
- Python sources are formatted with `ruff format` (120 columns, existing quote style kept). The change is layout only: the syntax tree of every file is identical before and after. The pre-push hook and `scripts/ci-local.sh` check formatting.
- `scripts/build_help.py` orders the guide sections by file name, so the bundled help is identical whichever machine builds it.
- `validation/android_handset_pendant_workflow.py` runs the pendant sequence on a supervised physical handset.
- Android: Build Tools 35.0.0 is pinned to match the guide.
- Ruff configuration (`pyproject.toml`) covering syntax, pyflakes, likely-bug and security rules for `studio/`; `scripts/ci-local.sh` runs it.
- `scripts/hooks/pre-push`: a fast local gate (lint, Python tests, JavaScript tests) for use while hosted CI is not run on push.
- `dev` extra with the lint and test tools.
- Dependabot version updates for Python, Gradle and workflow actions.
