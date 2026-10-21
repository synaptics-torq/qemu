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
    