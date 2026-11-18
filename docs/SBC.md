# KerfKite on single-board computers

**For a full CAD/CAM workstation**, use the Studio installer that matches the board’s desktop operating system. Studio connects to a supported CNC controller over USB serial. The CAD and toolpath calculations run on the CPU. A board-specific GPIO driver, AI accelerator or custom firmware image is unnecessary for this workflow.

**For drawing and file export**, install **KerfKite Browser Editor**. It opens directly from local storage in a current graphical browser. You can draw, reopen portable projects, preview supported contour operations and export G-code for a router, laser or drag knife. For STEP, server-side solids, relief generation or machine control, transfer the project to full Studio. The browser package includes the editor and its artwork, fonts and help. It requires no Python runtime or local service.

## Choose a download

| What your board runs | Package |
| --- | --- |
| A 64-bit ARM Debian, Ubuntu or Armbian desktop | [Full Studio ARM64 DEB](https://github.com/ShaunPrice/KerfKite/releases/download/v0.3.0/KerfKite-Linux-arm64.deb) |
| A 64-bit Intel/AMD Debian or Ubuntu desktop | [Full Studio AMD64 DEB](https://github.com/ShaunPrice/KerfKite/releases/download/v0.3.0/KerfKite-Linux-amd64.deb) |
| A Debian-family desktop where you want browser editing only, including ARM32 or RISC-V with a modern browser | [Browser Editor DEB, all architectures](https://github.com/ShaunPrice/KerfKite/releases/download/v0.3.0/KerfKite-Browser-Editor-all.deb) |
| Another desktop OS with a modern browser, or a computer where you cannot install packages | [Portable Browser Editor ZIP](https://github.com/ShaunPrice/KerfKite/releases/download/v0.3.0/KerfKite-Browser-Editor.zip) |

Open the DEB in the desktop package manager and choose **Install**. Open **KerfKite Studio** or **KerfKite Browser Editor** from the applications menu. For the ZIP, extract the entire folder and open **index.html**. Save work through **Projects → Download project**. Browser storage is specific to the browser profile and can change when a portable folder moves. To remove the Browser Editor use the package manager, or delete the extracted ZIP folder. Saved project downloads stay where you chose to put them.

Choose a graphical desktop image with a current browser for local use. Check the installed OS architecture under system information. A 64-bit CPU can run a 32-bit OS. Debian’s `dpkg --print-architecture` reports `arm64`, `amd64`, `armhf` or `riscv64`. The full installer downloads architecture-specific geometry wheels. Its locked releases lack ARM32 and RISC-V wheels. These installations use the browser edition or the authenticated web editor on another computer. [Manifold release files](https://pypi.org/project/manifold3d/3.5.5/#files) · [OpenCascade wrapper release files](https://pypi.org/project/cadquery-ocp-novtk/7.9.3.1.1/#files)

## Board guide

These board matches are inferred from the manufacturer’s hardware and the available operating-system images. Physical testing to date covers the Raspberry Pi 5. The following alternatives have software-level validation described below.

| Board | Edition to try | Hardware or image source |
| --- | --- | --- |
| Radxa ROCK 5B | Full ARM64 on Radxa’s Debian Bookworm KDE desktop. A practical alternative for a larger workshop project on an 8 GB configuration. | [Radxa desktop downloads](https://docs.radxa.com/en/rock5/rock5b/download) |
| Orange Pi 5 Plus | Full ARM64 on the Armbian Ubuntu desktop image. Select a desktop variant when you want to work directly at the board. | [Armbian board images](https://armbian.com/boards/orangepi5-plus) |
| NVIDIA Jetson Orin Nano / Orin Nano Super Developer Kit | Full ARM64 on the JetPack 6 Ubuntu 22.04 desktop. KerfKite does not use CUDA or TensorRT for CAD/CAM. | [NVIDIA JetPack](https://developer.nvidia.com/embedded/jetpack-sdk-62) |
| BeagleBone AI-64 | Full ARM64 on the board’s Debian Xfce image. Start with a small design before loading a detailed relief. | [BeagleBoard hardware](https://docs.beagleboard.org/boards/beaglebone/ai-64/01-introduction.html) · [Desktop images](https://www.beagleboard.org/distros) |
| PINE64 ROCKPro64 | Full ARM64 on the 4 GB board with Armbian Xfce. Start with Browser Editor for 2 GB configurations. | [PINE64 specifications](https://pine64.org/documentation/ROCKPro64/) · [Armbian images](https://armbian.com/boards/rockpro64) |
| LattePanda 3 Delta | Full AMD64 on its documented 64-bit Ubuntu 22.04 desktop. | [LattePanda Ubuntu guide](https://docs.lattepanda.com/content/3rd_delta_edition/Operating_Systems_Ubuntu/) |
| Hardkernel ODROID-H4 or H5 | Full AMD64 on a compatible 64-bit Ubuntu desktop. These are Intel boards. | [H4 specifications](https://www.hardkernel.com/shop/odroid-h4/) · [H5 specifications](https://www.hardkernel.com/shop/odroid-h5/) |
| BeaglePlay | Browser Editor on its Debian desktop. The board has 2 GB of RAM, shared with the browser and desktop. | [BeaglePlay specifications](https://docs.beagleboard.org/boards/beagleplay/01-introduction.html) · [BeagleBoard images](https://www.beagleboard.org/distros) |
| StarFive VisionFive 2 | Browser Editor on a RISC-V desktop image with a current browser. Board graphics and response time still need an on-device check. | [Debian board guide](https://wiki.debian.org/InstallingDebianOn/StarFive/VisionFiveV2) · [StarFive software releases](https://github.com/starfive-tech/VisionFive2/releases) |
| Original ASUS Tinker Board S | Browser Editor only when the installed 32-bit ARM desktop can run a current graphical browser. The vendor’s older stock browser needs individual verification. | [ASUS board specifications](https://tinker-board.asus.com/series/tinker-board-s.html) · [Vendor OS releases](https://tinkerboard.github.io/docs/tinker_os-releases/tinker_board) |

## What was tested

The full Studio installer was previously exercised on 64-bit Ubuntu 24.04 for ARM and Intel/AMD and on a minimal Debian Bookworm ARM64 userland. The source application and its browser workflows have also been exercised on a physical Pi 5. [Installer checks](https://github.com/ShaunPrice/KerfKite/actions/runs/44299310815) · [Pi verification](PLATFORMS.md)

The SBC package check installs the released DEB in Ubuntu 22.04 and 26.04 userlands, opens the optional STEP library, generates a simulated laser path and repeats the launch with the runtime installer offline. In Debian 13 it opens both Browser Editor packages in Chromium with networking disabled, draws a rectangle, saves and reopens the project, then verifies the exported router, laser and knife programs against their previews. The containers are limited to two CPUs and 2 GiB. This tests package installation and a small project. The board’s GPU driver, thermal behaviour, USB serial connection, physical cutting and larger CAD jobs need testing on the target hardware. [SBC package check runs](https://github.com/ShaunPrice/KerfKite/actions/workflows/sbc-check.yml)

For full Studio, 8 GB gives more room for the browser and geometry calculations. On a 4 GB board, start with one tab and a small job. For a 2 GB board, try the Browser Editor first and move larger jobs to a workstation. These are starting points for testing your designs. [Installation and first launch](INSTALL.md)
