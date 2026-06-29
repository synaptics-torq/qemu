#!/bin/bash 

wget https://cdimage.ubuntu.com/ubuntu/releases/noble/release/ubuntu-24.04.4-netboot-arm64.tar.gz

tar -xzf ubuntu-24.04.4-netboot-arm64.tar.gz

./build/qemu-system-aarch64 -machine astra_sl -kernel arm64/linux -initrd arm64/initrd -append "console=ttyAMA0 rdinit=/bin/sh" -nographic

