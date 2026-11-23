#!/usr/bin/env bash
# Local application checks for KerfKite Studio.
#
# GitHub Actions is not used for this repository. This script replaces the
# removed workflow
#
#     .github/workflows/test.yml   "Portable application checks"
#
# and runs the same commands, in the same order, on the machine it is started
# on. The workflow ran on push, on pull requests and on manual dispatch; none
# of those start anything now. The checks run only when someone runs this
# script.
#
# Every job starts from a fresh copy of the working tree, as actions/checkout
# gave every run of the workflow: the tracked files and the untracked files
# that are not ignored are copied to CI_LOCAL_DIR/test-<os>/src, and the steps
# run there. Work that is not committed yet is checked too. Nothing is built
# in the checkout itself, and build output that is already in it (build/, a
# compiled studio/_relief_native library) is not copied, so it cannot stand in
# for a build that failed. The copy is made with git, from a git checkout.
#
# Usage
#     scripts/ci-local.sh                run every job this machine can run
#     scripts/ci-local.sh --list         print the jobs, one per line
#     scripts/ci-local.sh JOB [JOB ...]  run only the named jobs
#     scripts/ci-local.sh --help         print this text
#
# Jobs, in the workflow's order. The workflow had one job, "test", with a
# matrix over three operating systems. Each matrix entry is a job here, and
# "test" selects all three:
#     test:ubuntu-latest    runs on a Linux machine, or in Docker (see below)
#     test:windows-latest   never runs here: it needs a Windows machine
#     test:macos-latest     runs on a macOS machine
#
# The script ends with a summary table (job, then passed / FAILED / skipped
# with the reason) and exits non-zero if any job that ran failed.
#
# Toolchains are the ones the workflow pinned, never another version:
#     Python 3.12   "python3.12" must be on PATH. Every run makes a fresh
#                   virtual environment from it (actions/setup-python).
#     Node.js 22    "node" on PATH, Homebrew's node@22, an nvm install, or the
#                   binary named by CI_LOCAL_NODE (actions/setup-node).
# A job whose toolchain is missing fails and names the version it needs.
#
# Environment
#     CI_LOCAL_DIR     where the copies of the working tree, environments,
#                      caches and state are kept. Default: .ci-local in the
#                      repository (git-ignored). A relative path is taken from
#                      the repository root.
#     CI_LOCAL_NODE    path of a Node.js 22 binary to use instead of searching.
#     CI_LOCAL_DOCKER  set to 1 to run test:ubuntu-latest in the ubuntu:24.04
#                      Docker image. The image must already be on this machine:
#                      it is never pulled. The container gets a copy of the
#                      working tree, installs Python 3.12, a compiler and CMake
#                      with apt-get, downloads Node.js 22 from nodejs.org (checked
#                      against its published SHA-256) and runs the job. The
#                      image runs at this machine's architecture; GitHub's
#                      ubuntu-latest was x86-64.

# Steps are functions run through step(), which shellcheck cannot follow.
# shellcheck disable=SC2329
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PYTHON_VERSION="3.12"       # test.yml: actions/setup-python@v5, python-version: '3.12'
NODE_VERSION="22"           # test.yml: actions/setup-node@v4, node-version: '22'
DOCKER_IMAGE="ubuntu:24.04" # stands in for runs-on: ubuntu-latest when CI_LOCAL_DOCKER=1
MATRIX_OS=(ubuntu-latest windows-latest macos-latest) # test.yml: strategy.matrix.os

CI_LOCAL_DIR="${CI_LOCAL_DIR:-.ci-local}"
case "$CI_LOCAL_DIR" in
  /*) ;;
  *) CI_LOCAL_DIR="$ROOT/$CI_LOCAL_DIR" ;;
esac
CI_LOCAL_DIR="${CI_LOCAL_DIR%/}"
# CI_LOCAL_DIR relative to the repository when it is inside it, else empty.
# Its files are never part of a copy of the working tree.
case "$CI_LOCAL_DIR" in
  "$ROOT"/*) CI_LOCAL_REL="${CI_LOCAL_DIR#"$ROOT"/}" ;;
  *) CI_LOCAL_REL="" ;;
esac

case "$(uname -s)" in
  Darwin) HOST_OS="macos-latest" ;;
  Linux) HOST_OS="ubuntu-latest" ;;
  *) HOST_OS="" ;;
esac

JOB_NAME=""
STEP_FILE=""

usage() {
  # The header comment of this file is the help text.
  sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed -e 's/^# \{0,1\}//'
}

# How a matrix entry runs on this machine: "native", "docker" or "skip: <reason>".
plan_for() {
  case "$1" in
    ubuntu-latest)
      if [ "${CI_LOCAL_DOCKER:-0}" = "1" ]; then
        if ! command -v docker >/dev/null 2>&1; then
          echo "skip: CI_LOCAL_DOCKER=1 but docker is not installed"
        elif ! docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
          echo "skip: CI_LOCAL_DOCKER=1 but the Docker image $DOCKER_IMAGE is not on this machine or Docker is not running (images are never pulled)"
        else
          echo "docker"
        fi
      elif [ "$HOST_OS" = "ubuntu-latest" ]; then
        echo "native"
      else
        echo "skip: needs Linux (matrix os ubuntu-latest); set CI_LOCAL_DOCKER=1 to run it in the $DOCKER_IMAGE Docker image"
      fi
      ;;
    windows-latest)
      echo "skip: needs a Windows machine (matrix os windows-latest); this script does not run on Windows"
      ;;
    macos-latest)
      if [ "$HOST_OS" = "macos-latest" ]; then
        echo "native"
      else
        echo "skip: needs a macOS machine (matrix os macos-latest)"
      fi
      ;;
  esac
}

list_jobs() {
  local os plan here missing
  for os in "${MATRIX_OS[@]}"; do
    plan="$(plan_for "$os")"
    case "$plan" in
      native)
        here="runs on this machine"
        missing="$(missing_toolchains)"
        if [ -n "$missing" ]; then
          here="$here, and fails until it has $missing"
        fi
        ;;
      docker) here="runs in the $DOCKER_IMAGE Docker image" ;;
      *) here="skipped: ${plan#skip: }" ;;
    esac
    printf 'test:%s\ttest.yml job "test" on %s: install, native build, Python and JavaScript tests, MCP smoke test [here: %s]\n' "$os" "$os" "$here"
  done
}

# Runs one step: records its name for the summary, prints it, runs it.
step() {
  local label="$1"
  shift
  printf '%s\n' "$label" >"$STEP_FILE"
  printf '\n==> [%s] %s\n' "$JOB_NAME" "$label"
  "$@"
}

# A "run:" step of the workflow. The step's name is the command itself.
run() {
  step "$*" "$@"
}

# The interpreter of exactly the pinned Python minor version: prints its path,
# or says what is missing and returns 1.
find_python() {
  local want="$1" base found
  base="$(command -v "python$want" || true)"
  if [ -z "$base" ]; then
    echo "error: Python $want is required (the workflow pinned python-version '$want') and python$want is not on PATH. Install Python $want; no other version is used." >&2
    return 1
  fi
  found="$("$base" -c 'import sys; print("%d.%d" % sys.version_info[:2])' 2>/dev/null || true)"
  if [ "$found" != "$want" ]; then
    echo "error: Python $want is required (the workflow pinned python-version '$want') but $base is Python ${found:-of an unknown version}. No other version is used." >&2
    return 1
  fi
  printf '%s\n' "$base"
}

# actions/setup-python: a fresh virtual environment made with exactly the
# pinned minor version, first on PATH, so that "python" is that interpreter.
setup_python() {
  local want="$1" venv="$2" base
  base="$(find_python "$want")"
  "$base" -m venv --clear "$venv"
  PATH="$venv/bin:$PATH"
  VIRTUAL_ENV="$venv"
  # tests/js/rotary-cam.test.cjs starts Python: ./.venv/bin/python when the
  # checkout has one, else "python". A hosted runner had no .venv, so it used
  # the interpreter set up here. Name it, so a developer's .venv is not used.
  CNC_TEST_PYTHON="$venv/bin/python"
  export PATH VIRTUAL_ENV CNC_TEST_PYTHON
  unset PYTHONHOME
  hash -r
  python --version
}

# Looks for a Node.js of exactly the pinned major version. Sets NODE_FOUND to
# its path, or to nothing when there is none, and NODE_SEEN to what it looked
# at.
find_node() {
  local want="$1" candidate version
  local candidates=()
  NODE_FOUND=""
  NODE_SEEN=""
  if [ -n "${CI_LOCAL_NODE:-}" ]; then
    candidates=("$CI_LOCAL_NODE")
  else
    candidate="$(command -v node || true)"
    if [ -n "$candidate" ]; then
      candidates=("$candidate")
    fi
    candidates+=("/opt/homebrew/opt/node@$want/bin/node" "/usr/local/opt/node@$want/bin/node")
    for candidate in "${NVM_DIR:-${HOME:-}/.nvm}/versions/node/v$want".*/bin/node; do
      candidates+=("$candidate")
    done
  fi
  for candidate in "${candidates[@]}"; do
    if [ ! -x "$candidate" ]; then
      if [ -n "${CI_LOCAL_NODE:-}" ]; then
        NODE_SEEN="$NODE_SEEN $candidate is not an executable file;"
      fi
      continue
    fi
    version="$("$candidate" --version 2>/dev/null || true)"
    NODE_SEEN="$NODE_SEEN $candidate is ${version:-not a working node};"
    case "$version" in
      "v$want".*)
        NODE_FOUND="$candidate"
        break
        ;;
    esac
  done
  return 0
}

# The pinned toolchains this machine lacks, for --list: empty when it has both.
missing_toolchains() {
  local missing=""
  if ! find_python "$PYTHON_VERSION" >/dev/null 2>&1; then
    missing="Python $PYTHON_VERSION"
  fi
  find_node "$NODE_VERSION"
  if [ -z "$NODE_FOUND" ]; then
    missing="${missing:+$missing and }Node.js $NODE_VERSION"
  fi
  printf '%s' "$missing"
}

# actions/setup-node: finds a Node.js of exactly the pinned major version and
# puts it first on PATH as "node".
setup_node() {
  local want="$1" shim="$2" found
  find_node "$want"
  if [ -z "$NODE_FOUND" ]; then
    echo "error: Node.js $want is required (the workflow pinned node-version '$want') and was not found.${NODE_SEEN:+ Looked at:$NODE_SEEN} Install Node.js $want (for example Homebrew's node@$want) or set CI_LOCAL_NODE to a Node.js $want binary. No other version is used." >&2
    return 1
  fi
  found="$(cd "$(dirname "$NODE_FOUND")" && pwd)/$(basename "$NODE_FOUND")"
  mkdir -p "$shim"
  ln -sf "$found" "$shim/node"
  PATH="$shim:$PATH"
  export PATH
  hash -r
  echo "node is $found"
  node --version
}

# The working tree as a fresh checkout would have it, plus work not yet
# committed: tracked files and untracked files that are not ignored. Ignored
# files (build output, environments) and CI_LOCAL_DIR are left out.
worktree_files() {
  local file
  git ls-files -z --cached --others --exclude-standard | while IFS= read -r -d '' file; do
    if [ -n "$CI_LOCAL_REL" ]; then
      case "$file" in
        "$CI_LOCAL_REL"/*) continue ;;
      esac
    fi
    if [ -e "$file" ] || [ -L "$file" ]; then
      printf '%s\0' "$file"
    fi
  done
}

# actions/checkout: every run of the workflow began from a fresh checkout, so
# no step could be satisfied by something an earlier build left behind. Here
# the job gets a fresh copy of the working tree in $1 and runs there; the
# checkout itself is only read.
fresh_checkout() {
  local src="$1"
  if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "error: $ROOT is not a git checkout (or git is not installed). The job runs on a copy of the tracked and the not-ignored files, and git is what lists them." >&2
    return 1
  fi
  rm -rf "$src"
  mkdir -p "$src"
  worktree_files | COPYFILE_DISABLE=1 tar --no-xattrs -cf - --null -T - | tar -xf - -C "$src"
  cd "$src"
  echo "the job runs in $src"
}

# CI_LOCAL_DOCKER=1: the Linux matrix entry in a throwaway container. The
# container reads a copy of the working tree from a tar stream and writes
# nothing back to this checkout; only download caches are kept, under
# CI_LOCAL_DIR.
run_in_docker() {
  local os="$1" cache="$CI_LOCAL_DIR/test-$1/docker-cache"
  mkdir -p "$cache/apt" "$cache/pip" "$cache/node"
  : >"$cache/current-step"
  worktree_files | COPYFILE_DISABLE=1 tar --no-xattrs -cf - --null -T - | docker run --rm -i --pull=never \
    -v "$cache:/ci-cache" \
    -e CI_LOCAL_HOST_OWNER="$(id -u):$(id -g)" \
    "$DOCKER_IMAGE" \
    bash -c "set -e; mkdir -p /work; tar --no-same-owner -xf - -C /work; cd /work; exec bash scripts/ci-local.sh --in-container test:$os </dev/null"
}

# Inside the container only: what a hosted runner image and the setup actions
# provided. Python and the compiler come from Ubuntu; Node.js is the official
# binary for the pinned major version, checked against its published SHA-256.
provision_container() {
  local arch base line sha file
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -q
  apt-get install -y -q --no-install-recommends -o Dir::Cache::archives=/ci-cache/apt \
    "python$PYTHON_VERSION" "python$PYTHON_VERSION-venv" "python$PYTHON_VERSION-dev" \
    g++ cmake make ca-certificates curl xz-utils
  case "$(uname -m)" in
    aarch64 | arm64) arch="arm64" ;;
    x86_64) arch="x64" ;;
    *)
      echo "error: no Node.js $NODE_VERSION download is known for $(uname -m)" >&2
      return 1
      ;;
  esac
  base="https://nodejs.org/dist/latest-v$NODE_VERSION.x"
  curl -fsSL "$base/SHASUMS256.txt" -o /ci-cache/node/SHASUMS256.txt
  line="$(grep -E "  node-v$NODE_VERSION\.[0-9]+\.[0-9]+-linux-$arch\.tar\.xz\$" /ci-cache/node/SHASUMS256.txt || true)"
  if [ -z "$line" ]; then
    echo "error: $base/SHASUMS256.txt lists no Node.js $NODE_VERSION archive for linux-$arch" >&2
    return 1
  fi
  sha="${line%% *}"
  file="${line##* }"
  if ! echo "$sha  /ci-cache/node/$file" | sha256sum -c - >/dev/null 2>&1; then
    curl -fsSL "$base/$file" -o "/ci-cache/node/$file"
  fi
  echo "$sha  /ci-cache/node/$file" | sha256sum -c -
  mkdir -p /opt/ci-local-node
  tar -xJf "/ci-cache/node/$file" -C /opt/ci-local-node --strip-components=1
  CI_LOCAL_NODE="/opt/ci-local-node/bin/node"
  PIP_CACHE_DIR="/ci-cache/pip"
  export CI_LOCAL_NODE PIP_CACHE_DIR
}

# Workflow job "test" (.github/workflows/test.yml), one matrix entry per call.
# $1 is the matrix os; $2 is "native", "docker" or "container".
job_test() {
  local os="$1" mode="$2" work="$CI_LOCAL_DIR/test-$1"

  if [ "$mode" = "docker" ]; then
    step "docker run $DOCKER_IMAGE: this job inside a Linux container" run_in_docker "$os"
    return
  fi
  if [ "$mode" = "container" ]; then
    step "container setup: apt-get install python$PYTHON_VERSION, g++ and CMake, download Node.js $NODE_VERSION" provision_container
  fi

  # test.yml, step "actions/checkout@v4"
  # A fresh copy of the working tree, and the job runs in it. In the container
  # the tree is already such a copy (see run_in_docker).
  if [ "$mode" = "native" ]; then
    step "actions/checkout: fresh copy of the working tree" fresh_checkout "$work/src"
  fi

  # test.yml, step "actions/setup-python@v5" with python-version: '3.12'
  step "actions/setup-python: Python $PYTHON_VERSION" setup_python "$PYTHON_VERSION" "$work/venv"

  # test.yml, step "actions/setup-node@v4" with node-version: '22'
  step "actions/setup-node: Node.js $NODE_VERSION" setup_node "$NODE_VERSION" "$work/bin"

  # test.yml, step: run: python -m pip install ".[mcp,web]" setuptools
  run python -m pip install ".[mcp,web]" setuptools

  # test.yml, step: run: python setup.py build_ext --inplace
  run python setup.py build_ext --inplace

  # test.yml, step: run: python -c "from studio import _relief_native; print(_relief_native.__file__)"
  run python -c "from studio import _relief_native; print(_relief_native.__file__)"

  # Added after test.yml was removed: the lint gate configured in pyproject.toml.
  run python -m pip install "ruff>=0.8"
  run python -m ruff check studio
  run python -m ruff format --check .

  # The native programs, as scripts/install.sh builds them: the fixture and the MCP check start them from build/native.
  run cmake -S native -B build/native -DCMAKE_BUILD_TYPE=Release
  run cmake --build build/native --parallel
  run ctest --test-dir build/native --output-on-failure

  # Exercise modelling and the workstation server's simulator, then discover both language suites.
  run python scripts/simulate_studio.py --all

  # test.yml, step: run: node --check studio/static/app.js
  run node --check studio/static/app.js

  # test.yml, step: run: node --check studio/static/monitor.js
  run node --check studio/static/monitor.js


  # test.yml, step: run: node --check studio/static/library.js
  run node --check studio/static/library.js

  # test.yml, step: run: node --check studio/static/text-tool.js
  run node --check studio/static/text-tool.js

  # test.yml, step: run: node --check studio/static/transfer.js
  run node --check studio/static/transfer.js

  # test.yml, step: run: node --check studio/static/advanced-editor.js
  run node --check studio/static/advanced-editor.js

  # test.yml, step: run: node --check studio/static/raster.js
  run node --check studio/static/raster.js

  # test.yml, step: run: node --check studio/static/layers.js
  run node --check studio/static/layers.js

  # test.yml, step: run: python validation/mcp_smoke.py (now kerfkite-mcp from build/native)
  run python validation/mcp_smoke.py
}

# Runs one matrix entry in a subshell, so that a failing step ends that job
# only and its PATH changes do not leak. Returns the job's exit status.
# The caller turns errexit off ("set +e") and calls this as a plain command:
# inside "if" or "||" bash ignores "set -e" for everything the call runs, and
# a failing step would not stop the job.
run_job() {
  local os="$1" mode="$2"
  JOB_NAME="test:$os"
  mkdir -p "$CI_LOCAL_DIR/test-$os" || return 1
  if [ "$mode" != "container" ]; then
    STEP_FILE="$CI_LOCAL_DIR/test-$os/current-step"
  fi
  : >"$STEP_FILE" || return 1
  (
    set -e
    job_test "$os" "$mode"
  )
}

# The step a failed job stopped at, for the summary.
failed_step() {
  local os="$1" mode="$2" outer inner=""
  outer="$(cat "$CI_LOCAL_DIR/test-$os/current-step" 2>/dev/null || true)"
  if [ "$mode" = "docker" ]; then
    inner="$(cat "$CI_LOCAL_DIR/test-$os/docker-cache/current-step" 2>/dev/null || true)"
  fi
  if [ -n "$inner" ]; then
    printf 'in the container at step: %s' "$inner"
  else
    printf 'at step: %s' "${outer:-unknown}"
  fi
}

is_selected() {
  local wanted
  if [ "${#SELECTED[@]}" -eq 0 ]; then
    return 0
  fi
  for wanted in "${SELECTED[@]}"; do
    if [ "$wanted" = "$1" ]; then
      return 0
    fi
  done
  return 1
}

SELECTED=()
DO_LIST=0
IN_CONTAINER=0
while [ $# -gt 0 ]; do
  case "$1" in
    --list) DO_LIST=1 ;;
    -h | --help)
      usage
      exit 0
      ;;
    --in-container) IN_CONTAINER=1 ;; # internal: set by run_in_docker
    test) SELECTED+=("${MATRIX_OS[@]}") ;;
    test:ubuntu-latest | test:windows-latest | test:macos-latest) SELECTED+=("${1#test:}") ;;
    *)
      echo "error: unknown job or option '$1'. Jobs:" >&2
      list_jobs >&2
      exit 2
      ;;
  esac
  shift
done

if [ "$DO_LIST" = "1" ]; then
  list_jobs
  exit 0
fi

if [ "$IN_CONTAINER" = "1" ]; then
  if [ ! -f /.dockerenv ] || [ "${#SELECTED[@]}" -ne 1 ]; then
    echo "error: --in-container is used by this script inside its Docker container only" >&2
    exit 2
  fi
  CI_LOCAL_DIR="/var/tmp/ci-local"
  STEP_FILE="/ci-cache/current-step"
  set +e
  run_job "${SELECTED[0]}" container
  status=$?
  # Give the cache files to the user who started the script. On a Linux host
  # the container's root user would otherwise own them.
  if [ -n "${CI_LOCAL_HOST_OWNER:-}" ]; then
    chown -R "$CI_LOCAL_HOST_OWNER" /ci-cache 2>/dev/null
  fi
  exit "$status"
fi

RESULT_JOBS=()
RESULT_TEXT=()
FAILED=0
for os in "${MATRIX_OS[@]}"; do
  if ! is_selected "$os"; then
    continue
  fi
  plan="$(plan_for "$os")"
  RESULT_JOBS+=("test:$os")
  case "$plan" in
    native | docker)
      printf '\n######## test:%s ########\n' "$os"
      set +e
      run_job "$os" "$plan"
      status=$?
      set -e
      if [ "$status" -eq 0 ]; then
        RESULT_TEXT+=("passed")
      else
        RESULT_TEXT+=("FAILED $(failed_step "$os" "$plan")")
        FAILED=1
      fi
      ;;
    *)
      RESULT_TEXT+=("skipped: ${plan#skip: }")
      ;;
  esac
done

printf '\n######## Summary ########\n'
printf '%-21s %s\n' "job" "result"
for ((i = 0; i < ${#RESULT_JOBS[@]}; i++)); do
  printf '%-21s %s\n' "${RESULT_JOBS[$i]}" "${RESULT_TEXT[$i]}"
done
exit "$FAILED"
