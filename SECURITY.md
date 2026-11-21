# Security reporting

Do not publish credentials or detailed exploit reports in a public issue. Contact the repository owner privately through an established channel. Until a dedicated disclosure address exists, do not invent one.

Hosted installations require HTTPS and individually configured credentials. Receiver upload tokens are secrets. The phone pendant listener is encrypted with a per-pairing certificate pinned by the pairing link when the `cryptography` package is installed, and otherwise uses HTTP. Either way, keep machine control on a trusted local network; uploads store files for review and never authorize movement. The default local server is simulator-only. Software hold/reset is not a hardware emergency stop.

See docs/WEB.md for deployment boundaries and docs/TROUBLESHOOTING.md for certificate and account recovery. No independent security audit or internet production deployment is claimed.
