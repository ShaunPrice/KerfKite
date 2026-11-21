# Changelog

Notable changes to KerfKite Studio and KerfKite Pocket. Versions before this file was introduced are described in the [GitHub release notes](https://github.com/ShaunPrice/KerfKite/releases) and the [release review](docs/RELEASE-REVIEW.md).

## Unreleased

### Fixed
- Pocket: every shape can be selected from the drawing preview. New shapes no longer start exactly on top of the previous one, a repeated tap moves to the next overlapping shape, and a tap within 12 dp of an outline counts.
- Machine-control routes of the local server (`/api/connect`, `/api/command`, `/api/run`, `/api/head`, `/api/arm` and the arm follow-ups, `/api/generate`, `/api/phone-monitor`) now answer a malformed or non-object body with a JSON `400` instead of an unhandled `500`.

### Security
- `/api/command` accepts only `action`, `axis`, `distance` and `feed`; any other field is refused before the controller is called.
- The local session token is compared in constant time.

### Development
- Python sources are formatted with `ruff format` (120 columns, existing quote style kept). The change is layout only: the syntax tree of every file is identical before and after. The pre-push hook and `scripts/ci-local.sh` check formatting.
- `scripts/build_help.py` orders the guide sections by file name, so the bundled help is identical whichever machine builds it.
- `validation/android_handset_pendant_workflow.py` runs the pendant sequence on a supervised physical handset.
- Android: Build Tools 35.0.0 is pinned to match the guide.
- Ruff configuration (`pyproject.toml`) covering syntax, pyflakes, likely-bug and security rules for `studio/`; `scripts/ci-local.sh` runs it.
- `scripts/hooks/pre-push`: a fast local gate (lint, Python tests, JavaScript tests) for use while hosted CI is not run on push.
- `dev` extra with the lint and test tools.
- Dependabot version updates for Python, Gradle and workflow actions.
