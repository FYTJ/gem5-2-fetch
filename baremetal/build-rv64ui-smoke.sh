#!/usr/bin/env bash
set -euo pipefail

TEST_NAME="${1:-add}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="rv64ui-p-${TEST_NAME}"
"$ROOT_DIR/build-riscv-tests-manifest.sh" \
  --suite rv64ui \
  --test "rv64ui/${TEST_NAME}" \
  --manifest "$ROOT_DIR/riscv-tests/isa/build/rv64ui/${TARGET}/manifest.json"
