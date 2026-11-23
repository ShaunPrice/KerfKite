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
| The system's OpenCL library (`OpenCL.dll`, `libOpenCL.so.1`; not used on macOS), optional | Running large relief contact jobs on a discrete GPU | **Keep, as an interface of the operating system.** It belongs to the GPU driver, is loaded at run time where it is present, and is never shipped or fetched. KerfKite declares the few functions it calls from the public specification and contains no OpenCL header or loader. Without the library everything runs on the processor ([Threads and the GPU](NATIVE.md#threads-and-a-gpu-for-large-reliefs)). |
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

## Studio (Python), retired in Studio 0.7

The Python service was ported to the native programs under rule 5 and removed in Studio 0.7, with its dependencies. They remain only at the commit that keeps the Python reference, for the comparisons run from it ([The Python reference](DEVELOPER.md#the-python-reference)).

| Component | Use until 0.7 | Now |
| --- | --- | --- |
| Shapely 2.2.0, which carries GEOS | Polygon work in ten modules: CAM operations, entry moves, sketch profiles, imports, solid text, exchange | **Replaced** by the in-house polygon kernel and the core's ports of those modules. Shapely remains the measure in the comparisons kept at the reference commit. |
| NumPy | Arrays in seven modules: relief, stock simulation and others | **Replaced**: plain arrays in C++, with the core's own linear algebra (`least_squares.hpp`). |
| pyserial | The serial link to the controller | **Replaced**: the serial port through each operating system's own interface (`native/server/serial_port.cpp`). |
| Flask, with Werkzeug, Jinja2, Click, itsdangerous, MarkupSafe, blinker | The local web server behind the editor | **Replaced** by the workstation server's own HTTP server (`native/server/http.cpp`). |
| ezdxf, with pyparsing, fonttools, typing-extensions | DXF import | **Replaced** by the core's DXF reader (`native/core/src/dxf.cpp`). `scripts/build_aci_table.py` still reads ezdxf's colour table when the generated palette is rebuilt. |
| pypdf | Vector import from PDF | **Replaced** by the core's PDF reader (`native/core/src/pdf_import.cpp`). The PDF browser lesson writes its fixture with pypdf. |
| manifold3d | Solid modelling and the 2D drawings of CAD scripts where the compiled extension was not installed; the oracle the kernels were tested against | **Replaced** by the in-house solid and polygon kernels ([Solid kernel](SOLID-KERNEL.md#how-studio-uses-the-kernel)). It remains the oracle of the solid kernel's check at the reference commit. |
| Pillow | Packaging and guide-building scripts | A build-time requirement of `packaging/desktop/build.py`, not part of Studio. |
| cadquery-ocp (Open Cascade, LGPL 2.1 with exception), optional | STEP import and export | **Replaced** by `kerfkite-step` (`native/step/`), the workstation server's add-on, which uses the same Open Cascade through its C++ libraries. cadquery-ocp remains the reference the add-on is compared with. |
| cheroot and cryptography, optional | The hosted editor, and the encrypted pendant link | **Replaced**: the hosted editor is `kerfkite-web`, its password hashes come from the operating system, and TLS from the Go relay (rule 2). |
| fastmcp, pydantic, playwright, optional | The MCP service | **Replaced** by `kerfkite-mcp`, which drives an installed Chrome or Edge for its workshop. The comparisons at the reference commit and the browser lessons still use Playwright and the MCP client SDK. |
| Python and setuptools | The runtime and packaging | **Retired** from the product. Python remains for the development tools, on its standard library unless a tool says otherwise. |

## Desktop launcher and installers

| Component | Use | Plan |
| --- | --- | --- |
| Go standard library | The launcher | Keep: it is the language's own library, with no other packages. |
| uv, and the locked Python packages it installed | Installing the Python environment on first run | **Removed**: the packages carry the native programs and install no Python. |

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
| STEP add-on, optional | `kerfkite-step`, a thin program of KerfKite's own (`native/step/`), linked dynamically to Open Cascade; the server starts it for each STEP request | Open Cascade (LGPL 2.1 with its exception) | Installed separately. Its package meets the LGPL for Open Cascade; nothing else in the product is affected. |
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
| Studio | The launcher, `kerfkite-studio` with the editor's files, `kerfkite-web`, `kerfkite-mcp` with the template collection, `kerfkite-cam` and the pendant's relay; nothing is downloaded on the first start. Probe levelling and circuit-board import live in the core, so they arrive with the programs. | Packaging is ready: the **Build the desktop programs and installers** workflow builds the programs and packages every installer with `build.py`. **Not in a release yet**. The installer check starts the installed launcher and exercises the server. |
| The in-house kernels (2D polygons, 3D solids, JSON) | Nothing separate: they are compiled into the programs. | In use. The self-tests, which run on every platform the programs are built for, include a digest of generated solids that must be the same everywhere. |
| The editor of the C++ application | The program, and the Qt libraries it uses as separate files beside it, with Qt's notice, licence text and source offer. | To be built; the installer work is part of that stage, not after it. |
| STEP add-on | `kerfkite-step` and the Open Cascade 7.9.3 libraries with their notices, as separate files: a Setup component on Windows, carried by the macOS and Linux packages. | Packaging is ready, **not in a release yet**; the installer check writes and reads a part through it. |
| Encrypted links | Nothing separate: the system's cryptography and the Go relay. | In use. |
| KerfKite Pocket | The signed APK, downloaded from the release. | Published separately. |

Rules for this:

1. **A component and its installation land together.** A change that adds a component to a part also adds it to the installers for every platform, with its licence files, and to the installer check.
2. **The installer check proves presence by use**, not by listing files: it runs the installed program and exercises the component.
3. **A kept third-party library is installed as a separate, replaceable file**, never merged into the program, so that the licence position above holds on every platform.
4. **Nothing is fetched at install time that is not pinned by hash.**

## Order of work

1. The polygon kernel, replacing GEOS in the core and Shapely in the CAM operations. Done for the core and for Studio with its compiled extension, V-carving included; the other Shapely users in Studio follow.
2. The solid kernel, replacing manifold3d in Studio's 3D CAD. Done for Studio with its compiled extension; the library remains as the fallback without it and as the test oracle.
3. Pillow out of the installed dependencies.
4. The remaining items as their parts are ported.
