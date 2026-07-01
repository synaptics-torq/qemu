#!/bin/bash 

BASEDIR="$(dirname ${BASH_SOURCE[0]})"

cd "$BASEDIR"

if [[ ! -d arm64 ]]; then  
  wget https://cdimage.ubuntu.com/ubuntu/releases/noble/release/ubuntu-24.04.4-netboot-arm64.tar.gz
  tar -xzf ubuntu-24.04.4-netboot-arm64.tar.gz
  rm ubuntu-24.04.4-netboot-arm64.tar.gz
fi

echo "Starting QEMU with Ubuntu 24.04.4 netboot image for Astra SL Baremetal..."

../../build/qemu-system-aarch64 -machine astra_sl -kernel arm64/linux -initrd arm64/initrd -append "console=ttyS0,115200 rdinit=/bin/sh" -nographic

