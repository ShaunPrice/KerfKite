#!/bin/sh
# Builds Studio's programs from this checkout into build/native and runs their self-tests.
# Does not configure services or hardware. Needs CMake 3.16 or newer and a C++17 compiler; with Go installed it
# also builds the TLS relay (HTTPS pairing, hosted transfers), and with Open CASCADE's development files the STEP
# add-on.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
for option in "$@"; do
  case "$option" in
    --web|--mcp) ;;  # the hosted editor and the assistant server are always built
    --help) echo 'Usage: sh scripts/install.sh'; echo 'Needs CMake 3.16+ and a C++17 compiler; Go adds the TLS relay.'; exit 0 ;;
    *) echo "Unknown option: $option" >&2; exit 2 ;;
  esac
done
command -v cmake >/dev/null 2>&1 || { echo 'CMake 3.16 or newer is required.' >&2; exit 1; }
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
cmake -S native -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel "$jobs"
ctest --test-dir build/native --output-on-failure
echo 'Built. Start the simulator: sh start.sh --port 8765'
echo 'Then open http://127.0.0.1:8765'
