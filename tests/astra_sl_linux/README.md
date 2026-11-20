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

### Device timing

The default is `-icount shift=0,sleep=off`: guest instructions advance virtual
time, and idle CPUs jump to the next virtual timer without sleeping on the host.
TORQ execution is collected synchronously without advancing guest time, then
the guest runs concurrently with a virtual job until its completion deadline.
Completion status, shared-RAM output publication and IRQ delivery are deferred
to that deadline.

`TORQ_TIME_SCALE` sets the positive, finite multiplier for backend duration
(default `1`). FPGA duration is measured with a host monotonic clock from launch
to observed completion, excluding input uploads and output downloads. Completion
is polled at 100 us intervals, so the measurement includes polling and host
scheduling latency; it is not a hardware cycle measurement. C-model jobs
currently use a fixed 1000 ns duration, also multiplied by the scale.

```bash
TORQ_BACKEND=aws-fpga TORQ_TIME_SCALE=0.1 ./run.sh
```

This sets the virtual deadline to launch time plus the scaled duration, rounded
up to whole nanoseconds; it does not add time to the clock on completion.
The existing host-time server timeout remains separate from guest timeouts.
Job mappings and input/output buffers must remain stable until completion;
overlapping jobs and writes to completion registers during a virtual job are
unsupported. Reset discards a pending completion and its output.

`QEMU_ICOUNT` overrides the icount options; `QEMU_ICOUNT= ./run.sh` disables
instruction counting, but then backend host waits are no longer isolated from
guest time.

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
