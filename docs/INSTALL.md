# Install KerfKite Studio

KerfKite Studio 0.2 is a source installation with a browser interface. Python 3.10 or newer is required; use 64-bit Python 3.12 where possible. Install into the project's `.venv`, not the operating system Python environment. A network connection is required to fetch Python dependencies during installation. The desktop editor runs on the same computer as the Python service.

See [Platforms](PLATFORMS.md) for installation targets and measured CAD/CAM acceptance on macOS, Windows, Linux and Raspberry Pi.

## 1. Obtain the source

Clone the public repository with `git clone https://github.com/ShaunPrice/KerfKite.git` or download its source archive. Open a terminal in the directory containing `pyproject.toml`, `start.sh` and `studio/`. Keep the complete tree together. Do not copy a macOS `.venv` or compiled extension to a Pi or Windows computer; each host needs its own installation.

## 2. Install on Raspberry Pi OS or Debian/Ubuntu Linux

Use a 64-bit operating system. On the Pi, install the Python virtual environment package and optional C++ build tools:

```sh
sudo apt update
sudo apt install python3 python3-venv python3-dev build-essential
python3 --version
sh scripts/install.sh --web --mcp
sh start.sh --port 8765
```

If the OS Python is older than 3.10, use a supported newer OS/Python installation. `CNC_PYTHON` can choose an already installed interpreter:

```sh
CNC_PYTHON=python3.12 sh scripts/install.sh --web --mcp
```

The `--web` installer option adds HTTPS services; `--mcp` adds the artwork MCP server. Omit either option if you do not need it. Open **http://127.0.0.1:8765** on the Pi's desktop browser. This local editor listener deliberately binds to loopback.

## 3. Install on macOS

Install 64-bit Python 3.12 first. The old system Python is unsuitable. For the optional C++ relief engine, install Apple's command line tools and complete any displayed installation dialog:

```sh
xcode-select --install
```

Then, from the project directory:

```sh
python3 --version
sh scripts/install.sh --web --mcp
sh start.sh --port 8765
```

Open **http://127.0.0.1:8765**. If `python3` points to an older interpreter, set `CNC_PYTHON` to your Python 3.12 executable when installing. Metal is not required; no custom Metal compute backend is implemented.

## 4. Install on Windows 11 x86-64

Install 64-bit Python 3.12. The optional C++ extension needs Microsoft Visual C++ Build Tools with the C++ workload and Windows SDK. Without those tools, installation can use the Python relief fallback.

The following manual commands work without changing PowerShell script execution policy. Run from the project directory:

```powershell
py -3.12 -m venv .venv
.venv\Scripts\python.exe -m pip install ".[web,mcp]"
.\start.bat --port 8765
```

Alternatively, after reviewing the supplied script and where local execution policy permits:

```powershell
.\scripts\install.ps1 -Web -Mcp -Python "C:\Path\To\Python312\python.exe"
.\start.bat --port 8765
```

Open **http://127.0.0.1:8765**. The PowerShell installer has not been executed on Windows in this project; use the manual commands if your execution policy blocks scripts. Do not change a managed organization's execution policy merely to run this installer.

## 5. First simulator session

1. Leave the CNC disconnected. Launch the local service using one of the commands above.
2. Open the browser address and select the simulator connection.
3. Add a rectangle or open an example, choose the operation and generate a preview.
4. Review the geometry and G-code. Test manual controls in the simulator before configuring hardware.
5. Save a named browser project and download a portable `.cncstudio.json` backup. Browser-local storage does not synchronize automatically to other computers.

The default server permits simulator connections only. Physical serial ports require an explicit restart with `--enable-hardware`, machine commissioning and verified machine/tool settings. Installing the software does not commission a machine. The supplied machine profile belongs to the project's reference machine; users of other machines must configure their own travel, heads and tools.

## Optional native C++ relief engine

The installer tries to compile the extension if a suitable compiler and Python development headers exist. The extension is optional; the Python implementation remains available. Check which engine was installed:

```sh
.venv/bin/python -c "from studio.relief import _relief_native; print('C++' if _relief_native is not None else 'Python fallback')"
```

On Windows substitute `.venv\Scripts\python.exe` for `.venv/bin/python`. After adding compiler tools, rerun the installer to rebuild for this host. The engine accelerates relief contact calculations; it does not change physical controller speed. See the validation reports for measured Pi workloads and their limits.

## Offline file editor

Open `studio/static/index.html` directly from the filesystem. Keep its vendor, font, icon and sibling script files together. There is no login in this mode. Drawing, templates, portable projects and supported contour CAM work locally; advanced server-dependent geometry and relief calculations require the Python service. The UI should report unsupported operations instead of treating them as completed calculations.

Use portable project files to move between the file editor and the server editor. Browser storage permissions differ for `file://`; if the project library is unavailable, use Download project. Uploading from this editor still requires an authenticated HTTPS receiver and an explicitly configured file-upload receiver option.

## HTTPS editor and Pi upload inbox

Install the `web` extra. These commands use the POSIX virtual environment path; on Windows use `.venv\Scripts\python.exe`. Replace `YOUR_USERNAME`, hostnames and addresses with real values.

Create a user. The password is prompted for twice; no default password exists:

```sh
.venv/bin/python -m studio.web_server --data-dir web-data create-user YOUR_USERNAME
```

To generate a cryptographically random password instead, append `--generate-password` and save the one-time displayed value in a password manager. Do not place passwords in shell arguments.

Generate a self-signed certificate, or provide an installed certificate and its matching private key:

```sh
.venv/bin/python -m studio.web_server certificate --cert server.pem --key server-key.pem --hosts your-pi.local 192.0.2.50
.venv/bin/python -m studio.web_server --data-dir web-data serve --public-url https://your-pi.local:8443 --cert server.pem --key server-key.pem --port 8443
```

Use the actual hostname/IP in the certificate. Trust the self-signed certificate explicitly on each client, or use a trusted CA certificate. Generated certificates expire after 90 days. Certificate renewal requires a service restart. Private keys and `web-data` contain private operational data; keep them outside shared/public source archives and protect them with OS access controls, including Windows ACLs where applicable.

The HTTPS service is both a design editor and an upload inbox. **Uploads are stored for review; the HTTPS inbox does not start the CNC.** For a separate hosted design server and Pi receiver, configure each separately with its own data directory and accounts. On the Pi, issue an upload-only token:

```sh
.venv/bin/python -m studio.web_server --data-dir web-data issue-token YOUR_USERNAME
```

On the design server, register the Pi receiver; paste its token into the hidden prompt:

```sh
.venv/bin/python -m studio.web_server --data-dir web-data destination workshop --url https://your-pi.local:8443 --ca-file server.pem
```

Here `server.pem` is the receiver certificate trusted by the sender. Omit `--ca-file` when the receiver's certificate is already trusted by the host. Restart the design service after destination changes. To permit uploads from `file://` editors, add `--allow-file-upload` to the receiver's `serve` command. File uploads still require the token. Do not disable certificate checks to resolve trust errors.

For internet hosting use a real DNS name, publicly trusted certificate and deliberate firewall access to the HTTPS listener. The current service expects direct TLS or TCP passthrough; it does not trust HTTP reverse-proxy forwarded headers. Automatic certificate renewal, system services and public deployment are not configured by the installer. See [WEB.md](WEB.md) for session limits, quotas and upload details.

## Phone monitor and controls

The local controller service can expose a separate paired monitor. Install `web` first for TLS and supply a certificate valid for the phone's Pi hostname:

```sh
sh start.sh --monitor --monitor-port 8766 --monitor-cert server.pem --monitor-key server-key.pem
```

The console displays a fresh pairing URL containing a random token. Replace its `<PI-LAN-IP>` placeholder with the certificate's matching host/IP, then open it on the phone. Treat this URL as a credential; do not include it in screenshots or repository files. Restarting revokes it. This example remains simulator-only. Omitting both certificate options selects the explicit HTTP LAN monitor; HTTPS is preferable for credential protection.

Monitor control is distinct from the upload inbox. Releasing a software pendant or sending feed hold is not a physical emergency stop, and accepted/buffered motion can continue depending on firmware.

## MCP artwork integration

Install with `--mcp`, then configure your MCP client to launch the absolute path to `.venv/bin/python` (Windows: `.venv\Scripts\python.exe`) with arguments `-m`, `studio.mcp_server` and the project directory as its working directory. This is a stdio artwork/template service. See [MCP.md](MCP.md) for the actual tools and a client configuration example. No MCP setup automatically connects or moves a machine.

## Updating, stopping and removing

Stop the foreground service with Ctrl+C. Before updating, download unsaved projects and back up `web-data`, exported template libraries and configuration. Install new source into the same project, rerun the installer, restart the service and refresh the browser after saving its work. Native extensions must be rebuilt for a different OS, CPU architecture or incompatible Python version.

Removing `.venv` removes that installation's Python dependencies. Removing source does not delete browser IndexedDB projects; browser site-data controls manage those separately. Keep or remove `web-data` deliberately according to whether its accounts/files are still needed. Do not delete the only copy of project files when uninstalling.
