#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
make

cd ../../build
./qemu-system-aarch64 \
  -machine astra_sl \
  -accel tcg \
  -display none \
  -monitor none \
  -serial stdio \
  -kernel ../tests/astra_sl_baremetal/hello.elf
