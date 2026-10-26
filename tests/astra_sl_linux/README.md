# Astra SL Linux Test Setup

This directory contains scripts to setup and execute a qemu instance that boots to a shell in
using the Astra SL SDK image.

## Setup

1. Build virtio kernel modules using the provided build scripts:

    ```bash
    ./build_modules.sh
    ```

    The modules are packaged in a tar.gz file in `kernel/syna-kernel-artifacts.tgz`.

2. Prepare the Astra SL image using the provided script:

    ```bash
    ./prepare_astra_sl_image.py
    ```

    This will create prepare a linux kernel image (with embedded initrd) in `linux` and
    an Astra SL SDK root fs image in `astra_sl_image/eMMCimg/rootfs.subimg`.

## Execution

1. Launch the qemu instance with the prepared images:

    ```bash
    ./run.sh
    ```

    An optional first argument is appended to the kernel command line.

## Networking / SSH

The guest gets a virtio network interface (`eth0`) backed by QEMU user-mode networking
(slirp). The rootfs configures it via DHCP (`10.0.2.15`), and the guest's dropbear SSH
server on port 22 is forwarded to `127.0.0.1:2222` on the host:

```bash
ssh -p 2222 root@127.0.0.1
```

Set `SSH_PORT` to use a different host port, e.g. `SSH_PORT=2223 ./run.sh`.

## ADB

There is no USB device controller in the emulated board, so `adbd` runs in TCP mode: 
`prepare_astra_sl_image.py` adds a systemd drop-in to the rootfs that skips the USB gadget
setup, and guest port 5555 is forwarded to `127.0.0.1:5555` on the host (`ADB_PORT` overrides it):

```bash
adb connect 127.0.0.1:5555
adb shell
```
