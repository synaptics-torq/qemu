#!/usr/bin/env bash
set -euo pipefail

set -x

if [[ "$#" -ne 1 ]]; then
    echo "Usage: $0 <path_to_torq_cdesc_dir>"
    exit 1
fi

BASEDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QEMU_BUILD_DIR="${QEMU_BUILD_DIR:-$BASEDIR/../../build}"
QEMU_RAM_SIZE="${QEMU_RAM_SIZE:-128M}"

make -C "$BASEDIR" "CDESC_DIR=$1" \
  ${SYNPU:+"SYNPU=$SYNPU"} ${REG:+"REG=$REG"}

exec "$QEMU_BUILD_DIR/qemu-system-aarch64" \
  -machine astra_sl,memory-backend=ram \
  -m "$QEMU_RAM_SIZE" \
  -object "memory-backend-memfd,id=ram,size=$QEMU_RAM_SIZE,share=on" \
  -accel tcg \
  -display none \
  -monitor none \
  -serial stdio \
  -kernel "$BASEDIR/hello.elf"
