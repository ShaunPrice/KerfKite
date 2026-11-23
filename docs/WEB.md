# Web editor, local-file editor and Pi receiver

The hosted editor and machine inbox run using the same HTTPS service. Uploaded files are stored for review in the recipient account; this service has no controller or job-start routes. Use separate data directories/accounts on the design server and Pi. This is an implementation tested locally, not a deployed or independently audited internet service.

## HTTPS setup (LAN or internet host)

The service is `kerfkite-web`, one of KerfKite's native programs: it is in the program folder beside `kerfkite-studio` and needs no Python. Its HTTPS comes from KerfKite's TLS relay, `kerfkite-pendant-tls`, which must be in the same folder. Studio 0.6.1 and earlier ran the Python service as `python -m studio.web_server` with the same commands and options; to move its accounts and files, see [Moving from the Python service](#moving-from-the-python-service).

```sh
kerfkite-web --data-dir web-data create-user YOUR_USERNAME
```

The command prompts twice for a user-created password of 15–256 characters. Add `--generate-password` to generate a 256-bit random password and display it once. No account or password ships with the app. Do not put passwords into shell command arguments. `reset-password YOUR_USERNAME` uses the same prompt/generator and revokes that account's sessions and upload tokens.

Use an installed certificate and matching key, or generate a 90-day self-signed certificate:

```sh
kerfkite-web certificate --cert server.pem --key server-key.pem --hosts your-pi.local 192.0.2.50
kerfkite-web --data-dir web-data serve --public-url https://your-pi.local:8443 --cert server.pem --key server-key.pem --port 8443
```

Replace hostnames/IPs with the actual host. Certificate names and the public URL must match. Trust a self-signed certificate explicitly on each client, or use a certificate from an installed trusted CA. Certificate verification is never disabled by the application. For internet hosting, use a publicly trusted certificate for your domain and configure DNS/firewall access to the HTTPS listener. Serve direct TLS (or TCP passthrough). To put the service behind a TLS terminator of your own on the same host (nginx, HAProxy), start it with `serve --public-url URL --behind-proxy --port N` in place of `--cert` and `--key`: it then listens on 127.0.0.1 only, and the terminator must send the PROXY protocol line (version 1) that names each client. Forwarded HTTP headers are never trusted. Certificate issuance/renewal, OS services and internet deployment are operator tasks, not automated here. Restart the service after certificate renewal.

The listener is KerfKite's TLS relay (Go's `crypto/tls`) with TLS 1.2 minimum; it passes each connection to `kerfkite-web` on the loopback interface. Sessions use Secure/HttpOnly/SameSite=Strict cookies, CSRF validation, eight-hour absolute expiry and 30-minute idle expiry. Password hashes are PBKDF2-HMAC-SHA256 with 600,000 iterations, computed by the operating system's cryptography (CNG on Windows, CommonCrypto on macOS, OpenSSL's libcrypto on Linux). Login attempts are limited by address/account. Uploaded content is always downloaded as an attachment. Account files are isolated and limited to 200 files / 64 MiB total, 8 MiB per file. Secure the data directory with OS access controls on every platform (POSIX permissions alone do not configure Windows ACLs).

## Configure uploads to the Pi

On the Pi, create an account and issue its upload-only token:

```sh
kerfkite-web --data-dir web-data issue-token YOUR_USERNAME
```

On the hosted design server, register the receiver. The token is requested through a hidden prompt:

```sh
kerfkite-web --data-dir web-data destination workshop --url https://your-pi.local:8443 --ca-file server.pem
```

Omit `--ca-file` for a certificate already trusted by the OS. Restart the design server to load destination changes. The browser sees destination names, never stored receiver tokens. Uploads verify the returned SHA-256 and byte count. Redirects are not followed. A failed upload may have arrived: inspect the receiver before retrying. Use `revoke-tokens YOUR_USERNAME` on the Pi to invalidate all its upload tokens.

The sender can target an explicitly configured HTTP origin with `--allow-http` for a compatible receiver/proxy, but credentials and contents would be unencrypted. CNC Studio's own receiver is HTTPS-only. The local-file upload UI also requires HTTPS.

## Moving from the Python service

Point `kerfkite-web` at the Python service's data directory (`--data-dir web-data`). The first time it opens a directory that holds the Python's `web.sqlite3` and no store of its own, it moves the accounts, their password hashes, the upload tokens and the stored files into its own store (`web-store.json` and `files/`), and renames the database `web.sqlite3.migrated`. Sessions are not moved: everyone logs in again. Stop the Python service first; a database with an unfinished change in its journal is refused until the Python finishes it. Receivers in `destinations.json` are read as they are.

The Python service hashed passwords with scrypt. On Linux `kerfkite-web` checks those hashes with OpenSSL's scrypt and replaces each with a PBKDF2 hash at the account's next login. Windows and macOS have no scrypt: there, set each moved account's password again with `kerfkite-web --data-dir web-data reset-password YOUR_USERNAME` (which also ends its sessions and revokes its upload tokens; issue new tokens for its receivers). The server prints which account could not be checked when one tries to log in.

## Local filesystem editor

Open `studio/static/index.html` directly in a browser. Keep the entire static directory together, including vendor, icon and font files. No login, web server or network access is required for drawing, text outlines, templates, SVG/project/XYZ downloads. Local contour CAM and G-code export support router/laser/knife paths, per-vertex depth/power, laser ramps and ordered contour operations. The browser rotary post exports supported grblHAL and xPRO V5 router/laser contour jobs and LinuxCNC router contour jobs. Advanced polygon/area operations, holding tabs, relief/extrusion and stock simulation require the Python app server; unsupported offline operations fail explicitly. The browser can inspect the wrapped path and download the posted program. Controller streaming uses the workstation service. [Rotary guide](ROTARY.md)

To upload from a local file, launch the Pi receiver with `--allow-file-upload`. Enter the receiver HTTPS origin and upload token into the editor's Files & receiver uploads section. The token stays in page memory, not browser persistent storage. The receiver permits opaque `Origin: null` only on its token-protected receive endpoint when this flag is enabled. Browsers cannot distinguish file origins from other opaque origins; the upload token remains mandatory. Certificate trust and browser private-network policies still apply. This flow has not been verified on every target browser/OS.

Hosted mode has private server file downloads and destination uploads. Its template library and IndexedDB project library are namespaced by username in browser storage. Named projects retain up to five prior save revisions and a recovery draft. Browser data is local to that browser profile; private server files and portable downloads are separate storage choices. Project downloads preserve metadata and extra business/rotary settings; exported library backups remain portable. Save unsaved artwork locally before logout or reloading an editor update.

## Current operating guidance

See [INSTALL.md](INSTALL.md) for complete platform and HTTPS setup, [USER-GUIDE.md](USER-GUIDE.md) for save/upload workflows and [STATUS.md](STATUS.md) for current software/hardware boundaries. Cost estimates, quotes and print layouts are available in the editor; they do not change the receiver's upload-only behavior. Paired phone monitoring/control belongs to the separate local controller service, not this HTTPS inbox.
