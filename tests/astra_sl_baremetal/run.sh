#!/usr/bin/env bash
set -euo pipefail

set -x

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <path_to_torq_cdesc_dir>"
    exit 1
fi

BASEDIR="$(dirname ${BASH_SOURCE[0]})"

pushd "$BASEDIR"
make  CDESC_DIR=$1
popd

pushd "$BASEDIR/../../build"

exec ./qemu-system-aarch64 \
  -machine astra_sl \
  -accel tcg \
  -display none \
  -monitor none \
  -serial stdio \
  -kernel "$BASEDIR/hello.elf"
