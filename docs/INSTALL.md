# Install KerfKite Studio

See the [single-board computer guide](SBC.md) for board-specific downloads and the offline Browser Editor.

## Desktop or phone installation

Download the desktop package from the [official KerfKite release](https://github.com/ShaunPrice/KerfKite/releases/latest).

* **Windows 10 or Windows 11 on Intel/AMD x64.** Open the setup wizard and choose **Install**. It installs for your account and always adds KerfKite Studio to Start. On **Choose where KerfKite Studio appears**, leave **Desktop shortcut** ticked to put a KerfKite Studio icon on the desktop, or untick it. Leave **Open KerfKite Studio** ticked on the last page to start it straight away. Windows may display a SmartScreen warning for this unsigned installer. Check the download came from the official release. On a managed PC, follow your organisation’s software approval process.
* **macOS.** In **Apple menu → About This Mac**, check whether the computer has an Apple chip or an Intel processor. Open the matching DMG, drag KerfKite Studio onto Applications and launch it there or from Launchpad. If you open it straight from the disk image instead, the launcher says so and asks you to drag it to Applications first, because it disappears when the image is ejected. This community build has an ad-hoc signature and has not been notarised by Apple. If macOS blocks it, review the app under **System Settings → Privacy & Security** after the first open attempt. Managed Macs may require administrator approval.
* **Raspberry Pi OS, Debian and Ubuntu.** Use a 64-bit graphical desktop. Open the ARM64 DEB on a Pi 4/5 or other ARM64 system, or the AMD64 DEB on an Intel/AMD PC. Choose **Install** in the package manager and open KerfKite Studio from the applications menu. On another compatible glibc desktop with a browser and xdg-open, extract the matching Linux archive to a permanent folder and open the KerfKite executable inside once. Its first start offers to add KerfKite Studio to the applications menu, so later starts work like an installed package. If you move the folder, open the executable in its new place once and the shortcuts follow it. Menus cannot start a program from a folder whose name contains `%`.
* **Android.** The minimum supported configuration is Android 8.0 (API 26) and 2 GB of device RAM. Download the [Pocket APK](https://github.com/ShaunPrice/KerfKite/releases/latest/download/KerfKite-Pocket-Android.apk) on your phone, open **Downloads** in the file manager, tap the APK and choose **Install**. If prompted, allow this source to install apps and revoke that permission afterwards. Device management policies can restrict app installation. If replacing a development APK, export your draft first. Android requires the previous build to be removed if its signing certificate differs. [Pocket user guide](ANDROID.md)

### First launch

1. The desktop launcher opens a setup tab in your default browser. From Studio 0.7 everything Studio needs is in the package, so the first launch downloads nothing and works offline. Earlier releases downloaded a private Python runtime and the design libraries on first launch, which needed an internet connection.
2. The first time, **Shortcuts** offers a KerfKite Studio shortcut on the desktop on macOS and Linux, and on the portable Linux edition an entry in the applications menu. Both are ticked; untick what you do not want. On macOS, the system may ask once whether KerfKite Studio may use the Desktop folder. To add one later, choose **Add a desktop shortcut** or **Add to the applications menu** in the launcher once the workshop is ready.
3. Leave **Native desktop** selected and choose **Start workshop**. The application window opens when the simulator is ready. To use a browser instead, select **Web browser** during startup, then choose **Open in browser**. In **Projects → Try a sample**, load the badge and generate a preview. The setup page says whether STEP import and export is installed: the macOS and Linux packages carry it, and on Windows it is a component you choose in Setup. For a commissioned machine, expand **Physical machine access** and enable controller ports for that session. Studio still requires a separate connection. The HTTPS editor and the MCP assistant server are installed beside Studio (see below).
4. Save your document and close the desktop window, then use **Quit KerfKite** in the launcher tab to shut down the local simulator. Reopening the application shortcut brings back that tab. Download important projects as portable files.

When **Open directly next time** is selected, the next launch opens the native simulator window directly. Closing that window shows the launcher. Choose **Show setup next launch** there to change the controller options on the next start. Controller access always requires an explicit choice in setup.

### Desktop window requirements

Windows Setup checks for a 64-bit Intel/AMD Windows 10 or 11 client before installation. The Windows launcher also checks the host before opening the workshop.

The Windows window uses .NET Framework 4.8 and the Microsoft Edge WebView2 Evergreen Runtime. Windows 11 includes WebView2. If the window reports that the runtime is missing, install the x64 Evergreen Runtime and reopen the window. [Microsoft WebView2 Runtime](https://developer.microsoft.com/en-us/microsoft-edge/webview2/)

The macOS window uses AppKit and the system WebKit. Keep macOS and Safari updated. On Linux the Debian package installs GTK 3 and WebKitGTK. For a tar archive, install your distribution's GTK 3 and WebKitGTK 4.1 or 4.0 (on Debian and Ubuntu, `libgtk-3-0` and `libwebkit2gtk-4.1-0`). Run it from a graphical session. You can always select **Open in browser** from the launcher.

[Drawing and saving in the desktop window](DESKTOP.md)

### Updates, setup problems and removal

Quit KerfKite, download the newer package and install it over the previous version. On macOS choose **Replace**. Studio keeps the same local browser address across launches so a browser profile can reopen its saved projects.

If Studio does not start, choose **Retry**. For a port conflict, quit any other KerfKite launcher or Studio server using port 18764 or 18765. If the browser did not open, visit `http://127.0.0.1:18764/` on the same computer. The setup page shows the location of `setup.log` when an error occurs.

Uninstall through **Installed apps** on Windows, which also removes the Start menu entry and the desktop shortcut. On macOS move the app to Trash and delete the KerfKite Studio shortcut from the desktop. On Linux remove the package in your software manager and delete `kerfkite-studio.desktop` from the desktop. For the portable Linux edition remove its extracted folder, the desktop shortcut and `~/.local/share/applications/org.kerfkite.Studio.Portable.desktop`. The launcher keeps its log and settings in a folder of your account. Earlier packages also installed a private Python runtime there; to reclaim that space after quitting, remove its `runtime-…` folders, or the whole folder to reset the launcher:

| Platform | Launcher folder |
| --- | --- |
| Windows | `%LOCALAPPDATA%\KerfKite Studio\Data` |
| macOS | `~/Library/Application Support/KerfKite Studio` |
| Linux | `$XDG_CONFIG_HOME/KerfKite Studio` or `~/.config/KerfKite Studio` |

## Source installation

The source build makes the same programs the packages carry: the workstation server `kerfkite-studio`, the HTTPS editor `kerfkite-web`, the MCP assistant server `kerfkite-mcp`, `kerfkite-cam`, and, where their tools are installed, the TLS relay `kerfkite-pendant-tls` (Go) and the STEP add-on `kerfkite-step` (Open CASCADE 7.6 or later). It needs CMake 3.16 or newer and a C++17 compiler, and downloads nothing. It supports the optional services and an explicitly enabled hardware controller.

See [Platforms](PLATFORMS.md) for installation targets and measured CAD/CAM acceptance on macOS, Windows, Linux and Raspberry Pi.

## 1. Obtain the source

Clone the public repository with `git clone https://github.com/ShaunPrice/KerfKite.git` or download its source archive. Open a terminal in the directory containing `VERSION`, `start.sh` and `native/`. Keep the complete tree together. Do not copy a build from one computer to another; each host builds its own.

## 2. Install on Raspberry Pi OS or Debian/Ubuntu Linux

Use a 64-bit operating system. Install the build tools, and Go for HTTPS pairing and hosted transfers:

```sh
sudo apt update
sudo apt install build-essential cmake golang
sh scripts/install.sh
sh start.sh --port 8765
```

For STEP import and export, also install `libocct-data-exchange-dev` and `libocct-modeling-algorithms-dev` before building. `scripts/install.sh` builds into `build/native` and runs the self-tests. Open **http://127.0.0.1:8765** on the Pi's desktop browser. This local editor listener deliberately binds to loopback.

## 3. Install on macOS

Install Apple's command line tools and complete any displayed installation dialog, then CMake (for example with Homebrew, `brew install cmake go`):

```sh
xcode-select --install
```

Then, from the project directory:

```sh
sh scripts/install.sh
sh start.sh --port 8765
```

Open **http://127.0.0.1:8765**. For STEP import and export, build Open CASCADE first as `native/README.md` describes. Metal is not required; no custom Metal compute backend is implemented.

## 4. Install on Windows 11 x86-64

Install Visual Studio 2022 (or its Build Tools) with the **Desktop development with C++** workload, which includes CMake, and optionally Go. From a Developer PowerShell for Visual Studio, in the project directory:

```powershell
cmake -S native -B build\native -A x64
cmake --build build\native --config Release --parallel
.\start.bat --port 8765
```

Alternatively, after reviewing the supplied script and where local execution policy permits, `.\scripts\install.ps1` runs the same build and the self-tests. Open **http://127.0.0.1:8765**. Do not change a managed organization's execution policy merely to run this installer.

## 5. First simulator session

1. Leave the CNC disconnected. Launch the local service using one of the commands above.
2. Open the browser address and select the simulator connection.
3. Add a rectangle or open an example, choose the operation and generate a preview.
4. Review the geometry and G-code. Test manual controls in the simulator before configuring hardware.
5. Save a named browser project and download a portable `.cncstudio.json` backup. Browser-local storage does not synchronize automatically to other computers.

The default server permits simulator connections only. Physical serial ports require an explicit restart with `--enable-hardware`, machine commissioning and verified machine/tool settings. Installing the software does not commission a machine. The supplied machine profile belongs to the project's reference machine; users of other machines must configure their own travel, heads and tools.

## Offline file editor

Open `studio/static/index.html` directly from the filesystem. Keep its vendor, font, icon and sibling script files together. There is no login in this mode. Drawing, templates, portable projects and supported contour CAM work locally; advanced server-dependent geometry and relief calculations require the workstation server. The UI should report unsupported operations instead of treating them as completed calculations.

Use portable project files to move between the file editor and the server editor. Browser storage permissions differ for `file://`; if the project library is unavailable, use Download project. Uploading from this editor still requires an authenticated HTTPS receiver and an explicitly configured file-upload receiver option.

## HTTPS editor and Pi upload inbox

The service is `kerfkite-web`, in the program folder beside `kerfkite-studio` with its TLS relay `kerfkite-pendant-tls` (in `build/native` for a source build). Replace `YOUR_USERNAME`, hostnames and addresses with real values.

Create a user. The password is prompted for twice; no default password exists:

```sh
kerfkite-web --data-dir web-data create-user YOUR_USERNAME
```

To generate a cryptographically random password instead, append `--generate-password` and save the one-time displayed value in a password manager. Do not place passwords in shell arguments.

Generate a self-signed certificate, or provide an installed certificate and its matching private key:

```sh
kerfkite-web certificate --cert server.pem --key server-key.pem --hosts your-pi.local 192.0.2.50
kerfkite-web --data-dir web-data serve --public-url https://your-pi.local:8443 --cert server.pem --key server-key.pem --port 8443
```

Use the actual hostname/IP in the certificate. Trust the self-signed certificate explicitly on each client, or use a trusted CA certificate. Generated certificates expire after 90 days. Certificate renewal requires a service restart. Private keys and `web-data` contain private operational data; keep them outside shared/public source archives and protect them with OS access controls, including Windows ACLs where applicable.

The HTTPS service is both a design editor and an upload inbox. **Uploads are stored for review; the HTTPS inbox does not start the CNC.** For a separate hosted design server and Pi receiver, configure each separately with its own data directory and accounts. On the Pi, issue an upload-only token:

```sh
kerfkite-web --data-dir web-data issue-token YOUR_USERNAME
```

On the design server, register the Pi receiver; paste its token into the hidden prompt:

```sh
kerfkite-web --data-dir web-data destination workshop --url https://your-pi.local:8443 --ca-file server.pem
```

Here `server.pem` is the receiver certificate trusted by the sender. Omit `--ca-file` when the receiver's certificate is already trusted by the host. Restart the design service after destination changes. To permit uploads from `file://` editors, add `--allow-file-upload` to the receiver's `serve` command. File uploads still require the token. Do not disable certificate checks to resolve trust errors.

For internet hosting use a real DNS name, publicly trusted certificate and deliberate firewall access to the HTTPS listener. The service expects direct TLS or TCP passthrough, or with `--behind-proxy` a TLS terminator on the same host that sends the PROXY protocol line; it does not trust HTTP reverse-proxy forwarded headers. A `web-data` folder of the Python service is moved to the new store the first time `kerfkite-web` opens it ([WEB.md](WEB.md#moving-from-the-python-service)). Automatic certificate renewal, system services and public deployment are not configured by the installer. See [WEB.md](WEB.md) for session limits, quotas and upload details.

## Phone monitor and controls

In the desktop installer, choose **Start workshop → Open Studio → Control → Phone pendant → Enable phone pairing**. Enter the server address reachable from the phone. Follow the [Android connection guide](PHONE-CONNECTIONS.md) to pair over Wi-Fi, USB tethering or Bluetooth tethering.

From the command line, `sh start.sh --monitor` with the TLS relay built serves an encrypted monitor whose certificate is identified in the pairing link; add `--monitor-plaintext` for Pocket 0.4.1 and earlier. To serve a certificate of your own instead, supply one valid for the server hostname used on the phone:

```sh
sh start.sh --monitor --monitor-port 8766 --monitor-cert server.pem --monitor-key server-key.pem
```

The console displays a fresh pairing URL containing a random token. Replace its `<PI-LAN-IP>` placeholder with the certificate's matching host/IP, then open it on the phone. Treat this URL as a credential; do not include it in screenshots or repository files. Restarting revokes it. This example remains simulator-only. Without the TLS relay the monitor falls back to HTTP on the LAN and the console says so.

Monitor control is distinct from the upload inbox. Releasing a software pendant or sending feed hold is not a physical emergency stop, and accepted/buffered motion can continue depending on firmware.

## MCP assistant workshop

Configure your MCP client to launch the absolute path to `kerfkite-mcp` (in the program folder, or `build/native` for a source build) with `--output-dir` and a private folder. Its workshop opens an installed Google Chrome, Chromium or Microsoft Edge. The [assistant setup guide](MCP.md) covers Claude, ChatGPT, optional HTTP hosting and access to the workshop. No MCP setup automatically connects or moves a machine.

## Updating, stopping and removing

Stop the foreground service with Ctrl+C. Before updating, download unsaved projects and back up `web-data`, exported template libraries and configuration. Update the source in the same project, rerun the build, restart the service and refresh the browser after saving its work. A build is made for one OS and CPU architecture.

Removing `build` removes the programs a source build made. Removing source does not delete browser IndexedDB projects; browser site-data controls manage those separately. Keep or remove `web-data` deliberately according to whether its accounts/files are still needed. Do not delete the only copy of project files when uninstalling.

## Reopening a project in an older Studio version

Version 0.6.1 upgrades the browser project library the first time it opens. Before downgrading, open each design and revision you want to keep and choose **Download project**. Older releases can open the downloaded project files. The upgraded browser library requires version 0.6.1 or later.
