#!/usr/bin/env bash
set -euo pipefail

BASEDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QEMU="$BASEDIR/../../build/qemu-system-aarch64"
KERNEL="$BASEDIR/linux"
INITRD="$BASEDIR/initramfs.cpio.gz"
ROOTFS="$BASEDIR/astra_sl_image/eMMCimg/rootfs.subimg"
SSH_PORT="${SSH_PORT:-2222}"

for input in "$QEMU" "$KERNEL" "$INITRD" "$ROOTFS"; do
    if [[ ! -s "$input" ]]; then
        echo "Required file is missing or empty: $input" >&2
        exit 1
    fi
done

if [[ ! -x "$QEMU" ]]; then
    echo "QEMU is not executable: $QEMU" >&2
    exit 1
fi

# the machine doesn't simulate the SDHC controller, so use virtio instead
# net.ifnames=0 keeps the NIC as eth0, which the rootfs DHCP config (networkd/NM/ifupdown) expects;
# don't pass ip=, it disables systemd-networkd's 80-wired.network
kernel_cmdline="console=ttyS0,115200 root=/dev/vda rootfstype=ext4 net.ifnames=0 ${1:-}"

# disable unnecessary services and blacklist certain modules that are not emulated
kernel_cmdline="$kernel_cmdline systemd.mask=tee-supplicant.service systemd.mask=android-tools-adbd.service systemd.mask=swupdate.service systemd.mask=ncm-gadget-setup.service systemd.mask=-brcm_bt_start.service module_blacklist=syna_drm,synadhd103,synadhd361,synadhd461,synadhd591,bcmdhd"

echo "Starting Astra SL with the extracted kernel and initramfs (ssh: localhost:$SSH_PORT)..."

exec "$QEMU" \
    -machine astra_sl \
    -accel tcg \
    -initrd "$INITRD" \
    -kernel "$KERNEL" \
    -drive file="$ROOTFS",if=none,id=rootfs,format=raw,snapshot=on \
    -device virtio-blk-device,drive=rootfs \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:"$SSH_PORT"-:22 \
    -device virtio-net-device,netdev=net0 \
    -append "$kernel_cmdline" \
    -nographic