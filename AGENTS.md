# Working on KerfKite Studio

Start with [the architecture and development handoff](docs/DEVELOPER.md). It describes a complete edit–test cycle, the units and coordinate frames, the Python/browser boundary, and where each type of change belongs. [Release status](docs/STATUS.md) and [the acceptance matrix](docs/PRODUCT.md) distinguish implemented features from open product work. The [licence inventory](docs/LICENSING.md) covers bundled code, fonts, dependencies and media.

* Run the server in its default simulator mode for development. Use synthetic fixtures for tutorials and test cases. Any connection to a physical machine is a separate operating decision.
* Preserve a design's editable inputs, units and layer ownership through save/reload. Invalidate previews when inputs change. Imported machine files enter the design review flow. Keep controller writes and sender actions out of import and design APIs.
* Exercise changed workflows through their normal entry points. Run the simulation harness and the browser exercise for UI changes. Update the static guide and regenerate bundled help. Commit new fixtures and necessary attribution with the implementation.
* Use test data suitable for public redistribution. Keep private drawings, account data, configurations from third parties, signing keys, provider receipts and local environment paths outside tracked files. Retain copyright and licence notices.
