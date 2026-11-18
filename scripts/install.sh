#!/bin/sh
# Installs into this checkout's .venv. Does not configure services or hardware.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cnc_python=${CNC_PYTHON:-python3}
cnc_extras=''
cnc_assistant=false
for cnc_option in "$@"; do
  case "$cnc_option" in
    --web) cnc_extras="${cnc_extras:+$cnc_extras,}web" ;;
    --mcp) cnc_assistant=true; cnc_extras="${cnc_extras:+$cnc_extras,}mcp" ;;
    --help) echo 'Usage: sh scripts/install.sh [--web] [--mcp]'; echo 'Set CNC_PYTHON to the Python 3.10+ executable if needed.'; exit 0 ;;
    *) echo "Unknown option: $cnc_option" >&2; exit 2 ;;
  esac
done
"$cnc_python" -c 'import sys; sys.exit("Python 3.10 or newer is required") if sys.version_info < (3,10) else None'
if [ ! -x .venv/bin/python ]; then "$cnc_python" -m venv .venv; fi
.venv/bin/python -c 'import sys; sys.exit("Existing .venv requires Python 3.10 or newer") if sys.version_info < (3,10) else None'
cnc_package='.'
if [ -n "$cnc_extras" ]; then cnc_package=".[$cnc_extras]"; fi
.venv/bin/python -m pip install "$cnc_package"
if [ "$cnc_assistant" = true ] && [ -z "${KERFKITE_MCP_CHROMIUM:-}" ]; then
  .venv/bin/python -m playwright install chromium
fi
.venv/bin/python -c 'from studio.relief import _relief_native; print("Relief engine:", "C++ available" if _relief_native is not None else "Python fallback (optional C++ compiler not available)")'
echo 'Installed. Start simulator: sh start.sh --port 8765'
echo 'Then open http://127.0.0.1:8765'
