#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ISA_DIR="$ROOT_DIR/riscv-tests/isa"

TEST_NAME="${1:-add}"
TARGET="rv64ui-p-${TEST_NAME}"
BUILD_DIR="$ISA_DIR/build/rv64ui/${TARGET}"
ELF="$BUILD_DIR/$TARGET"
BIN="$ELF.bin"
DUMP="$ELF.dump"

RISCV_ARCH="${RISCV_ARCH:-rv64ima_zifencei}"
RISCV_ABI="${RISCV_ABI:-lp64}"

find_tool() {
  local env_name="$1"
  shift
  if [ -n "${!env_name:-}" ]; then
    printf '%s\n' "${!env_name}"
    return 0
  fi
  local candidate
  for candidate in "$@"; do
    if command -v "$candidate" >/dev/null 2>&1; then
      command -v "$candidate"
      return 0
    fi
  done
  return 1
}

RISCV_GCC="$(find_tool RISCV_GCC riscv64-unknown-elf-gcc riscv64-linux-gnu-gcc)"
RISCV_OBJCOPY="$(find_tool RISCV_OBJCOPY riscv64-unknown-elf-objcopy riscv64-linux-gnu-objcopy)"
RISCV_OBJDUMP="$(find_tool RISCV_OBJDUMP riscv64-unknown-elf-objdump riscv64-linux-gnu-objdump)"

SRC="$ISA_DIR/rv64ui/${TEST_NAME}.S"
if [ ! -f "$SRC" ]; then
  echo "missing rv64ui source: $SRC" >&2
  exit 2
fi

mkdir -p "$BUILD_DIR"

echo "RV64UI smoke workload: rv64ui/${TEST_NAME}.S"
echo "RISCV_GCC=$RISCV_GCC"
echo "RISCV_OBJCOPY=$RISCV_OBJCOPY"
echo "RISCV_OBJDUMP=$RISCV_OBJDUMP"
echo "RISCV_ARCH=$RISCV_ARCH"
echo "RISCV_ABI=$RISCV_ABI"
"$RISCV_GCC" --version | head -n 1

"$RISCV_GCC" \
  -march="$RISCV_ARCH" \
  -mabi="$RISCV_ABI" \
  -mcmodel=medany \
  -O2 \
  -g \
  -ffreestanding \
  -nostdlib \
  -nostartfiles \
  -Wl,--gc-sections \
  -Wl,--check-sections \
  -T "$ROOT_DIR/link.lds" \
  -I "$ISA_DIR" \
  -I "$ISA_DIR/macros/scalar" \
  "$ROOT_DIR/start.S" \
  "$SRC" \
  -o "$ELF"

"$RISCV_OBJCOPY" -O binary "$ELF" "$BIN"
"$RISCV_OBJDUMP" -alDS -M no-aliases "$ELF" > "$DUMP"

file "$ELF"
test -s "$BIN"
echo "ELF=$ELF"
echo "BIN=$BIN"
echo "DUMP=$DUMP"
