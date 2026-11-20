# Changelog

Notable changes to KerfKite Studio and KerfKite Pocket. Versions before this file was introduced are described in the [GitHub release notes](https://github.com/ShaunPrice/KerfKite/releases) and the [release review](docs/RELEASE-REVIEW.md).

## Unreleased

### Fixed
- Machine-control routes of the local server (`/api/connect`, `/api/command`, `/api/run`, `/api/head`, `/api/arm` and the arm follow-ups, `/api/generate`, `/api/phone-monitor`) now answer a malformed or non-object body with a JSON `400` instead of an unhandled `500`.

### Security
- `/api/command` accepts only `action`, `axis`, `distance` and `feed`; any other field is refused before the controller is called.
- The local session token is compared in constant time.

### Development
- Ruff configuration (`pyproject.toml`) covering syntax, pyflakes, likely-bug and security rules for `studio/`; `scripts/ci-local.sh` runs it.
- `scripts/hooks/pre-push`: a fast local gate (lint, Python tests, JavaScript tests) for use while hosted CI is not run on push.
- `dev` extra with the lint and test tools.
- Dependabot version updates for Python, Gradle and workflow actions.
