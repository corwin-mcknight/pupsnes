#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
CI_DIR="${ROOT_DIR}/build/ci"
USER_PRESET="${ROOT_DIR}/CMakeUserPresets.json"

if [[ -f "${CI_DIR}/CMakeCache.txt" ]]; then
  CACHE_SOURCE_DIR="$(awk -F= '/^CMAKE_HOME_DIRECTORY:INTERNAL=/{print $2; exit}' "${CI_DIR}/CMakeCache.txt")"
  if [[ -n "${CACHE_SOURCE_DIR}" && "${CACHE_SOURCE_DIR}" != "${ROOT_DIR}" ]]; then
    STALE_CI_DIR="${CI_DIR}.stale"
    STALE_CI_INDEX=1
    while [[ -e "${STALE_CI_DIR}" ]]; do
      STALE_CI_DIR="${CI_DIR}.stale.${STALE_CI_INDEX}"
      ((STALE_CI_INDEX += 1))
    done
    echo "[ci-verify] Moving stale CI build cache (${CACHE_SOURCE_DIR}) to ${STALE_CI_DIR}"
    mv "${CI_DIR}" "${STALE_CI_DIR}"
  fi
fi

if [[ -f "${USER_PRESET}" ]] && grep -q '"conan"' "${USER_PRESET}"; then
  echo "[ci-verify] Removing Conan-generated CMakeUserPresets.json"
  rm -f "${USER_PRESET}"
fi

echo "[ci-verify] Configuring (ci preset — conan runs automatically if needed)"
cmake --preset ci

echo "[ci-verify] Building (ci preset)"
cmake --build --preset ci

echo "[ci-verify] Running unit tests"
"${CI_DIR}/pupsnes_tests" "[unit]"

echo "[ci-verify] Running lint (non-blocking — triage backlog)"
cmake --build --preset ci --target lint || echo "[ci-verify] lint reported issues (non-blocking)"

echo "[ci-verify] Success"
