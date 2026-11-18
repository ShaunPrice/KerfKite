#!/bin/sh
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
if [ ! -x .venv/bin/python ]; then
  echo 'Install first: python3 -m venv .venv && .venv/bin/python -m pip install .'
  exit 1
fi
exec .venv/bin/python -m studio.server "$@"
