Torq QEMU Emulation
===================

.. warning::

    This fork of QEMU is specifically tailored for the Torq hardware and is intended for
    Torq compiler/runtime testing purposes only. **The emulation does not replicate the actual
    behavior of the full Astra SoC nor the coral and machina boards.** The emulation of the Torq hardware is 
    also limited to features used by the compiler and doesn't fully reflect the actual Torq 
    hardware behavior.


This repository contains a fork of QEMU that can be used to emulate the Torq hardware found
in Synaptics Astra SL 2610.

The qemu device model for TORQ calls an external device model server. This device model server
can either use a host implementation or a FPGA backed implementation.


Build
-----

Prerequisites
~~~~~~~~~~~~~

The easiest way to build this tree is to check it out as part of the `torq-compiler` repository.

1. Ensure you have both `git` and `git-lfs` pre-installed on your machine:

   ```{code} shell
   $ git lfs --version
   ```

2. Clone the root repository:

   ```{code} shell
   $ git clone https://github.com/synaptics-torq/torq-compiler.git
   ```

3. Go to the directory that was cloned:

   ```{code} shell
   $ cd torq-compiler
   ```

4. Set the `TORQ_ROOT` environment variable to the root of the repository:

   ```{code} shell
   export TORQ_ROOT="$PWD"
   ```

5. Initialize the `qemu` submodule:

   ```{code} shell
   git submodule update --init third_party/qemu
   ```

Build Dependencies
~~~~~~~~~~~~~~~~~~

Use a native Linux or macOS build environment with the standard QEMU build prerequisites.

On Ubuntu 24.04, you can install the dependencies with the following command:

```{code} shell
${TORQ_ROOT}/scripts/install_dependencies.sh
```

On macOS, install the Xcode command-line tools and Homebrew dependencies:

```{code} shell
${TORQ_ROOT}/scripts/install_dependencies_qemu_macos.sh
```

You also need to download a pre-built MPACT library or build it as described 
in the `Torq compiler getting started guide <https://synaptics-torq.github.io/torq-compiler/v/latest/dev-manual/getting_started.html#download-pre-built-or-build-mpact-library>`_.

Optional FPGA support (Linux only)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

To also enable the AWS FPGA backend on Linux you need to first build the AWS FPGA SDK:

```{code} shell
${TORQ_ROOT}/scripts/build_fpga_lib.sh
```

This builds the SDK without installing it or accessing FPGA hardware; an
FPGA instance and a loaded bitstream are not needed to compile the server.
    

Configure and build QEMU and the server
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

After completing the steps above, configure from the parent checkout:

    ${TORQ_ROOT}/scripts/configure_qemu.sh

Then build the QEMU system emulator and the Torq C model server:

    ninja -C qemu-build qemu-system-aarch64 hw/misc/torq-cmodel-server

This produces ``qemu-build/qemu-system-aarch64`` and ``qemu-build/hw/misc/torq-cmodel-server``.

Usage
-----

Check the README in ``tests/astra_sl_baremetal`` and ``tests/astra_sl_linux`` for 
usage instructions.
    
