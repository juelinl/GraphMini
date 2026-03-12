#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${VIRTUAL_ENV:-}" && -x "${VIRTUAL_ENV}/bin/python" ]]; then
  DEFAULT_PYTHON_BIN="${VIRTUAL_ENV}/bin/python"
else
  DEFAULT_PYTHON_BIN="python3"
fi
PYTHON_BIN="${PYTHON_BIN:-${DEFAULT_PYTHON_BIN}}"
args=(--python "${PYTHON_BIN}")

if [[ -n "${BUILD_DIR:-}" ]]; then
  args+=(--build-dir "${BUILD_DIR}")
fi

if [[ -n "${CMAKE_GENERATOR:-}" ]]; then
  args+=(--generator "${CMAKE_GENERATOR}")
fi

exec "${PYTHON_BIN}" "${ROOT_DIR}/scripts/install_python.py" \
  "${args[@]}" \
  "$@"
