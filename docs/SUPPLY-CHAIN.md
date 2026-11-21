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
| GEOS 3.14.1 (LGPL 2.1), formerly fetched at build time by pinned hash | Offsets, pockets and hatch fills | **Replaced.** The in-house polygon kernel computes them ([Polygon kernel](POLYGON-KERNEL.md)). GEOS is gone from the native build, with its build files and options; nothing is fetched while building, and the core contains no third-party code. GEOS remains only inside Shapely, in Studio's Python (below) and as the oracle of the validation scripts, which are not shipped in the core. |
| JSON for Modern C++ 3.11.3 (MIT), one vendored header | Reading projects and writing answers | **Replaced.** The core reads and writes JSON with its own code (`kerfkite::Json`), and the vendored header is gone. The old library was the test oracle while the replacement was written. |

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
| Shapely 2.2.0, which carries GEOS | Polygon work in ten modules: CAM operations, entry moves, sketch profiles, imports, solid text, exchange | **Being replaced.** Offsets, pockets and hatch fills now run on the in-house kernel, through the compiled extension. Shapely stays for now as their fallback where the extension is not installed (the portable wheel), as the reference the kernel is compared with in the checks, and in the other modules (V-carving, router entry clearance, sketch profiles, imports, text, exchange) until each is ported. |
| NumPy | Arrays in seven modules: relief, stock simulation and others | Replace on porting; plain arrays in C++. |
| pyserial | The serial link to the controller | Replace on porting the sender: the serial port through each operating system's own interface. |
| Flask, with Werkzeug, Jinja2, Click, itsdangerous, MarkupSafe, blinker | The local web server behind the editor | Replace on porting the server: a small local HTTP server of our own. |
| ezdxf, with pyparsing, fonttools, typing-extensions | DXF import and export | Replace on porting: a reader and writer for the DXF entities Studio handles. |
| pypdf | Vector import from PDF | Replace on porting. A sizeable piece: objects, compressed streams and drawing operators. |
| manifold3d | Solid modelling: unions, differences and intersections of 3D shapes | **Replace** (owner's decision, 21 November 2026). An in-house 3D solid kernel is being written, because engravings can be three-dimensional and multi-axis machines are to follow. It also provides what those need: sections on any plane, ray casting with surface normals, and height maps. The kernel is written and tested ([Solid kernel](SOLID-KERNEL.md)); Studio is not yet moved onto it. |
| Pillow | Used only by packaging and guide-building scripts, not by Studio when it runs | **Quick win:** move it from the installed dependencies to a build-time requirement, after confirming the installers still build. |
| cadquery-ocp (Open Cascade, LGPL 2.1 with exception), optional | STEP import and export | **Keep, optional** (owner's decision, 21 November 2026). Writing a STEP and B-rep kernel is not realistic. It stays a separately installed part, so the main product does not carry it. |
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

**Qt 6 is kept** for the editor of the C++ application (owner's decision, 21 November 2026). It is a deliberate exception to rule 1: a user interface toolkit for three desktop platforms is not within reach to write. Qt is used through dynamic linking, and only the modules Qt offers under the LGPL are used; some Qt modules are offered to open-source users under the GPL only and are avoided.

## Licences by part

Where a component cannot be replaced, its licence comes with it. The way to keep that contained is to keep each such component inside one part of the product, behind a clear boundary, and to let each part carry only the licences of what it contains. The core never links a component that belongs to another part.

| Part | What it contains | Third-party inside | Licence position |
| --- | --- | --- | --- |
| Core and `kerfkite-cam`: CAM, the geometry and solid kernels, file formats, levelling | KerfKite's own code | None: GEOS and the JSON library have been replaced | MIT, with no third-party obligations |
| Editor application | KerfKite's own code, linked dynamically to Qt | Qt 6 (LGPL 3) | The code stays MIT. A distributed package must meet the LGPL for Qt: the notice and licence text, Qt's source or an offer of it, and the means to replace the Qt libraries. A commercial Qt licence is the alternative. |
| STEP add-on, optional | A thin wrapper | Open Cascade (LGPL 2.1 with its exception) | Installed separately. Its package meets the LGPL for Open Cascade; nothing else in the product is affected. |
| Encrypted links | The pendant listener and hosted editor | A cryptography library, or the operating system's own TLS | The library's permissive licence; notices only |
| KerfKite Pocket | KerfKite's own Java | ZXing (Apache 2.0) until replaced | Notices only |

The owner has decided both questions that this raised (21 November 2026):

1. **KerfKite's own code stays under MIT throughout.** MIT is compatible with every component above, so no part needs a different licence for its own code.
2. **KerfKite is distributed through GitHub only, not through app stores.** Releases are downloads: installers, the command-line tool and the Pocket APK. The LGPL's requirement that a user can replace the library is met by shipping Qt and Open Cascade as separate libraries beside the program. If store distribution is ever considered, this has to be looked at again first.

This register describes what is done and what the licences ask; it is not legal advice.

## What the installers install

Every part of the product reaches a user through an installer, so each part, and each component kept inside it, has to be installed by one. A part that the installer does not install does not exist for the user.

| Part | What the installer puts in place | State |
| --- | --- | --- |
| Studio and its Python environment | The launcher, the locked Python packages, and the compiled extension for the platform. Probe levelling and circuit-board import live in that extension, so they arrive with it. | Installed today; the installer check exercises it. |
| The in-house kernels (2D polygons, 3D solids, JSON) | Nothing separate: they are compiled into the core and the extension. | Arrive with the extension once each is switched in. The JSON code and the polygon kernel are; the installed-wheel check runs offsets, a pocket and a fill through the kernel and compares the programs with a recorded digest. |
| `kerfkite-cam`, the command-line tool | The program for the platform. | **Not installed yet.** GEOS is out of it and it carries no third-party code; adding it to the installers is what remains. |
| The editor of the C++ application | The program, and the Qt libraries it uses as separate files beside it, with Qt's notice, licence text and source offer. | To be built; the installer work is part of that stage, not after it. |
| STEP add-on | Open Cascade and its wrapper, as a component the user chooses, with its notice and licence text. | Installed today as an option; the installer check has a STEP run. |
| Encrypted links | The cryptography library for the platform. | Installed today. |
| KerfKite Pocket | The signed APK, downloaded from the release. | Published separately. |

Rules for this:

1. **A component and its installation land together.** A change that adds a component to a part also adds it to the installers for every platform, with its licence files, and to the installer check.
2. **The installer check proves presence by use**, not by listing files: it runs the installed program and exercises the component.
3. **A kept third-party library is installed as a separate, replaceable file**, never merged into the program, so that the licence position above holds on every platform.
4. **Nothing is fetched at install time that is not pinned by hash.**

## Order of work

1. The polygon kernel, replacing GEOS in the core and Shapely in the CAM operations. Done for the core and for Studio with its compiled extension; the other Shapely users in Studio follow.
3. Pillow out of the installed dependencies.
4. The remaining items as their parts are ported.
