#!/bin/sh
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
if [ ! -x build/native/kerfkite-studio ]; then
  echo 'Build first: sh scripts/install.sh'
  exit 1
fi
exec build/native/kerfkite-studio --static studio/static "$@"
