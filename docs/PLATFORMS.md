# Platforms and verification

See the [single-board computer guide](SBC.md) for board-specific downloads and the offline Browser Editor.

[Download the desktop installers](https://github.com/ShaunPrice/KerfKite/releases/latest) or the [Android companion](ANDROID.md). The table below records source-installation and hardware evidence. See the [installation guide](INSTALL.md) for the graphical installers.

## Studio 0.6.1 package verification

The public Windows build passed fresh and offline starts on Windows 11 Pro x64. With hardware disabled, the extracted launcher produced the expected CAD volume, laser toolpath and router relief depths. This run used a noninteractive session. The earlier 0.5.1 run below covers the Windows desktop controls. [Release notes](https://github.com/ShaunPrice/KerfKite/releases/tag/v0.6.1)

The native CAM build passed on all five desktop targets. On macOS (Apple silicon and Intel) and Linux (AMD64 and ARM64), the published packages passed native window, STEP and offline relaunch checks. The Windows Server job confirmed that the installer rejects Server. Both Linux architectures passed the SBC package checks, including the offline Browser Editor. Physical Pi and CNC testing remains open for 0.6.1. [Native CAM builds](https://github.com/ShaunPrice/KerfKite/actions/runs/44426960223) · [Desktop installers](https://github.com/ShaunPrice/KerfKite/actions/runs/44427177409) · [SBC packages](https://github.com/ShaunPrice/KerfKite/actions/runs/44427181115)

## Earlier platform checks

**Studio 0.5.1 native desktop verification.** The published Windows installer and both executables accepted Windows 11 Pro x64 build 26300. In a signed-in desktop session, the packaged workbench created and moved geometry, handled unsaved-document cancellation, opened the materials guide, saved and reopened a project, previewed CAM and exported G-code. The Windows Server runner verified rejection before installation. Windows 10 x64 admission cases passed automated tests. A live Windows 10 desktop run is pending.

The Apple silicon and Intel macOS runners and both Linux architectures passed native window, project and simulation checks. The SBC workflow exercised the desktop package on Ubuntu 22.04 and 26.04 userlands and tested the browser packages offline. The 0.5.1 runs used simulation. [Desktop installer checks](https://github.com/ShaunPrice/KerfKite/actions/runs/44410337833) · [SBC package checks](https://github.com/ShaunPrice/KerfKite/actions/runs/44410344003)

**Earlier public source release (18 November).** All 11 CAD/CAM harness groups passed on macOS ARM64, Windows 11 Pro x86-64 and a physical Raspberry Pi 5 ARM64. The Windows and Pi hosts installed the public source in isolated environments. Each run exercised the STEP runtime and native C++ relief kernels, with 151 Python and 114 JavaScript tests. See the [release review](RELEASE-REVIEW.md) for the browser checks and the limits of physical verification. Earlier measurements below identify their own host and workload.

## Platform support

The desktop installers provide AppKit on macOS, Windows Forms and WebView2 on Windows, and GTK 3 with WebKitGTK on Linux. Each native window embeds the shared editor and connects to a local Python service. The browser interface remains available from the launcher. The service provides solid modelling, CAM and simulation. The Android edition provides native 2D design, portable project and SVG exchange, and a CNC pendant paired with the Studio server.

| Platform | Source installation and earlier hardware checks |
|---|---|
| macOS, Apple Silicon | Install 64-bit Python 3.12 and run the macOS installer below. The CAD and machining harness and Chrome editing workflows passed on macOS ARM64. |
| macOS, Intel | For a source installation, use x86-64 Python. The published Intel desktop package passed on a hosted Intel macOS runner. |
| Windows 11, x86-64 | Install 64-bit Python 3.12 and use the PowerShell installer. Package installation and all four core CAD and machining scenarios passed on an x64 Windows build 26300 machine. |
| Windows 10, x86-64 | Use 64-bit Python 3.12 and the same PowerShell installer. A Windows 10 runtime and UI test is pending. |
| Linux, x86-64 | Use 64-bit Python 3.11 or 3.12 on a recent glibc distribution. Installation and all four core scenarios passed on Ubuntu 24.04 x86-64 under WSL. |
| Linux, ARM64 | Use a 64-bit OS and Python environment. The four core scenarios passed on Debian ARM64 on a physical Raspberry Pi 5. |
| Raspberry Pi 4B or 5, 4 GB or 8 GB | Use 64-bit Raspberry Pi OS with a supported Python. On a Pi 5 with 8 GB, the server fixture passed under a 1.5 GiB memory ceiling and a two-core CPU quota in 5.49 s. Peak Python process RSS was 54,048 KiB. The Chromium browser and server also passed all nine CAD workflow groups together under a 2 GiB ceiling and two-core quota on that Pi 5, including sketch drag, project reopening, STL and router relief transfer. Pi 4 and whole-browser use on a physical 4 GB board still need measurement. |
| Android 8.0 (API 26) or later, minimum supported RAM 2 GB | Install the Android APK or build it from source. The editor, project and SVG exchange, cold-relaunch persistence and server-paired pendant controls were exercised on a 2 GB ARM64 emulator running API 37. Pocket 0.4.1 also passed on a physical Samsung Galaxy S23 Ultra (SM-S918B, Android 17, One UI 9.0); see [Android](ANDROID.md#physical-handset-check) for the scope and the open selection defect. |

## Desktop commands

From the source directory on macOS or Linux:

```sh
sh scripts/install.sh
sh start.sh --port 8765
```

On Windows 10/11 in PowerShell:

```powershell
.\scripts\install.ps1 -Python C:\Python312\python.exe
.\start.bat --port 8765
```

Set the executable path to the installed 64-bit Python. Open the printed localhost address. Add HTTPS/web or MCP extras as described in [Install](INSTALL.md). The geometry kernel and browser renderer use CPU resources. Custom Metal rendering and 32-bit x86/ARM builds have no acceptance evidence.

## Raspberry Pi sizing

Start on a 4 GB board with a single editor tab. Use a small solid, transfer a silhouette and preview a contour first. For relief work, start with a coarse stepover and measure a small region before increasing the sampling density. Large meshes, raster jobs and many open browser tabs can dominate memory. Run the same fixture on the target board with the following command:

```sh
.venv/bin/python scripts/simulate_studio.py --json pi-result.json
```

The report identifies the completed cases. Actual machine travel, cutter and laser calibration, clamping and material response need commissioning on the target machine.
