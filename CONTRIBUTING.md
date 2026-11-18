# Contributing

Start with the installation guide and implementation status. Keep CAD geometry, CAM, postprocessors and live controller transport separate. Add focused tests for behaviour changes and retain a Python reference for native geometry. New protocols must distinguish export support from live control and include acknowledgement/error/abort tests before enabling hardware.

Use synthetic fixtures and simulator-only tests by default. Never put tokens, passwords, private machine credentials or generated account data in commits. Preserve third-party licence files. Document unsupported capabilities explicitly. Run `./scripts/ci-local.sh` before submitting: GitHub Actions is not used, so the application checks run locally. Submit a branch and pull request with the problem, change and validation evidence; physical tests must state the actual controller and setup.

The repository-level [working instructions](AGENTS.md) and [developer handoff](docs/DEVELOPER.md) describe project schemas, extension points, the browser and simulation suites, and a worked example of extending a file reader.
