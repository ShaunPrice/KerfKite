# Third-party components and the plan for each

KerfKite prefers its own code to third-party components. The aim is a smaller supply chain and tighter control of what ships. This register lists every third-party component the product uses, what it is used for, and what is planned for it. [Licensing](LICENSING.md) holds the licence texts and notices; this document holds the decisions.

## The rule

1. **Write it ourselves** when the function is within reach: file formats, geometry, parsers, protocol handling, user interface code.
2. **Exceptions**, where our own code would raise risk and not lower it:
   - cryptography and TLS;
   - the operating system's own interfaces, compilers and build tools;
   - data that is not code, such as fonts.
3. **Whatever third-party code remains** is vendored in the repository or pinned by cryptographic hash, is never fetched unpinned while building something that ships, and has its licence recorded.
4. **A third-party library may serve as a test oracle** in development and in the checks, to compare our code against. An oracle is never part of anything that ships.
5. **When a Python part is ported to C++, the port uses our own code**, not a C++ library that happens to do the same job as the Python one.

## Native core (C++)

| Component | Use | Plan |
| --- | --- | --- |
| GEOS 3.14.1 (LGPL 2.1), fetched at build time by pinned hash | Offsets, pockets and hatch fills | **Replace now.** An in-house polygon kernel is being written. GEOS then stays only as a test oracle, off by default, and is not shipped. |
| JSON for Modern C++ 3.11.3 (MIT), one vendored header | Reading projects and writing answers | **Replace next.** The core needs a small part of JSON: parse, navigate, and write numbers as it already formats them itself. |

## KerfKite Pocket (Android)

| Component | Use | Plan |
| --- | --- | --- |
| ZXing core 3.5.3 (Apache 2.0), from Maven | Reading the pairing QR code | **Replace later.** The app reads one kind of symbol for one purpose; a decoder for that case is a contained piece of work. Until then the version stays pinned. |
| Android Gradle Plugin, Gradle, Android Build Tools | Building the app | Keep: build tools. Versions are pinned. |

## Studio editor (browser code, vendored in `studio/static/vendor/`)

| Component | Use | Plan |
| --- | --- | --- |
| qrcodegen (MIT) | Drawing the pairing QR code | **Replace later.** Small; a QR encoder for short byte strings. |
| opentype.js 2.0.0 (MIT), with tiny-inflate | Reading font outlines for the text tool | **Replace later**, with a reader for the outline tables the text tool uses. |
| Cormorant Garamond, Lora, Montserrat (OFL) | Fonts | Keep: data, not code. |

The rest of the editor is KerfKite's own JavaScript.

## Studio (Python), to be retired as the C++ port proceeds

Each of these goes when the part that uses it is ported, under rule 5.

| Component | Use | Plan |
| --- | --- | --- |
| Shapely 2.2.0, which carries GEOS | Polygon work in ten modules: CAM operations, entry moves, sketch profiles, imports, solid text, exchange | **Replace.** The CAM operations move to the in-house kernel first; the other modules follow as they are ported. |
| NumPy | Arrays in seven modules: relief, stock simulation and others | Replace on porting; plain arrays in C++. |
| pyserial | The serial link to the controller | Replace on porting the sender: the serial port through each operating system's own interface. |
| Flask, with Werkzeug, Jinja2, Click, itsdangerous, MarkupSafe, blinker | The local web server behind the editor | Replace on porting the server: a small local HTTP server of our own. |
| ezdxf, with pyparsing, fonttools, typing-extensions | DXF import and export | Replace on porting: a reader and writer for the DXF entities Studio handles. |
| pypdf | Vector import from PDF | Replace on porting. A sizeable piece: objects, compressed streams and drawing operators. |
| manifold3d | Solid modelling: unions, differences and intersections of 3D shapes | **Decision needed.** A robust 3D solid kernel is a much larger undertaking than the 2D one. Keep until the 2D kernel has proved the approach, then decide. |
| Pillow | Used only by packaging and guide-building scripts, not by Studio when it runs | **Quick win:** move it from the installed dependencies to a build-time requirement, after confirming the installers still build. |
| cadquery-ocp (Open Cascade, LGPL), optional | STEP import and export | **Decision needed.** Writing a STEP and B-rep kernel is not realistic. Keep it optional, as now, or drop STEP. |
| cheroot and cryptography, optional | The hosted editor, and the encrypted pendant link | Keep cryptography: rule 2. In a native application the operating system's own TLS can take its place. |
| fastmcp, pydantic, playwright, optional | The MCP service | Replace on porting the service; the browser driver goes when the editor no longer needs driving. |
| Python and setuptools | The runtime and packaging | Retired when the port is complete. |

## Desktop launcher and installers

| Component | Use | Plan |
| --- | --- | --- |
| Go standard library | The launcher | Keep: it is the language's own library, with no other packages. |
| uv, downloaded by pinned SHA-256 for each platform | Installing the Python environment on first run | Goes when Python goes. Until then it stays pinned by hash. |
| The locked Python packages, each pinned by hash (`packaging/desktop/requirements.lock`) | The installed environment | As for each package above. |

## Build and checks

| Component | Use | Plan |
| --- | --- | --- |
| GitHub Actions: checkout, setup-python, upload-artifact, cache | The checks | Keep. Each is pinned by commit hash, with its version beside it. |
| ruff, pytest, pytest-cov | Development only | Keep: they do not ship. |
| CMake and the platform compilers | Building | Keep: build tools. |

## The editor in the C++ application

The plan for the port had left open whether the C++ application would use Qt 6 for its editor. Under this rule Qt would become the largest third-party component in the product. The alternative that fits the rule is to keep the existing editor, which is already KerfKite's own code, and serve it from the core's own local HTTP server. This is a decision for the owner and is not yet made.

## Order of work

1. The polygon kernel, replacing GEOS in the core and Shapely in the CAM operations.
2. The JSON reader and writer in the core.
3. Pillow out of the installed dependencies.
4. The remaining items as their parts are ported.
