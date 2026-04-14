#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
CI_DIR="${ROOT_DIR}/build/ci"

echo "[ci-verify] Configuring (ci preset — conan runs automatically if needed)"
cmake --preset ci

echo "[ci-verify] Building (ci preset)"
cmake --build --preset ci

echo "[ci-verify] Running unit tests"
"${CI_DIR}/pupsnes_tests" "[unit]"

# echo "[ci-verify] Running lint"
# cmake --build --preset ci --target lint

echo "[ci-verify] Success"
