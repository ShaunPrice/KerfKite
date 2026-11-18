# Platforms and verification

See the [single-board computer guide](SBC.md) for board-specific downloads and the offline Browser Editor.

[Download the desktop installers or Android app](https://github.com/ShaunPrice/KerfKite/releases/tag/v0.2.1). The table below records source-installation and hardware evidence. See the [installation guide](INSTALL.md) for the graphical installers.

**Public source release.** All 11 CAD/CAM harness groups passed on macOS ARM64, Windows 11 Pro x86-64 and a physical Raspberry Pi 5 ARM64. The Windows and Pi hosts installed the public source in isolated environments. Each run exercised the STEP runtime and native C++ relief kernels, with 151 Python and 114 JavaScript tests. See the [release review](RELEASE-REVIEW.md) for the browser checks and the limits of physical verification. Earlier measurements below identify their own host and workload.

Use the desktop browser editor with a local Python service on 64-bit macOS, Windows or Linux. The service provides solid modelling, CAM and simulation. The Android edition is a native 2D design companion with portable project and SVG exchange.

| Platform | Installation and evidence |
|---|---|
| macOS, Apple Silicon | Install 64-bit Python 3.12 and run the macOS installer below. The CAD and machining harness and Chrome editing workflows passed on macOS ARM64. |
| macOS, Intel | Use the same installer with x86-64 Python. The geometry kernel publishes Intel wheels. Hardware acceptance is pending. |
| Windows 11, x86-64 | Install 64-bit Python 3.12 and use the PowerShell installer. Package installation and all four core CAD and machining scenarios passed on an x64 Windows build 26300 machine. |
| Windows 10, x86-64 | Use 64-bit Python 3.12 and the same PowerShell installer. A Windows 10 runtime and UI test is pending. |
| Linux, x86-64 | Use 64-bit Python 3.11 or 3.12 on a recent glibc distribution. Installation and all four core scenarios passed on Ubuntu 24.04 x86-64 under WSL. |
| Linux, ARM64 | Use a 64-bit OS and Python environment. The four core scenarios passed on Debian ARM64 on a physical Raspberry Pi 5. |
| Raspberry Pi 4B or 5, 4 GB or 8 GB | Use 64-bit Raspberry Pi OS with a supported Python. On a Pi 5 with 8 GB, the server fixture passed under a 1.5 GiB memory ceiling and a two-core CPU quota in 5.49 s. Peak Python process RSS was 54,048 KiB. The Chromium browser and server also passed all nine CAD workflow groups together under a 2 GiB ceiling and two-core quota on that Pi 5, including sketch drag, project reopening, STL and router relief transfer. Pi 4 and whole-browser use on a physical 4 GB board still need measurement. |
| Samsung Galaxy S23 | Install the Android APK or build it from source. Drawing, editing, draft persistence and project/SVG export were exercised in an ARM64 emulator at S23 display dimensions. The exported polygon generated six laser segments on the workstation. Physical S23 testing is pending. See [Android](ANDROID.md). |

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
