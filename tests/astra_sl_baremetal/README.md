# Astra SL Bare-Metal Test

`run.sh` builds and launches the bare-metal firmware on the Astra SL QEMU
machine. Pass the directory containing the TORQ descriptor headers:

```bash
./run.sh /path/to/torq-cdesc
```

The script runs `make` with that directory as `CDESC_DIR`, then starts QEMU
with shared 128 MiB guest RAM. The firmware exercises TORQ interrupts, RAM-to-
LRAM DMA, and the NPU reset line. It prints a final completion message and then
idles; stop QEMU with Ctrl+C.

Optional environment variables:

- `QEMU_BUILD_DIR`: QEMU build directory. Defaults to `../../build` relative to
  this test directory.
- `QEMU_RAM_SIZE`: shared guest RAM size. Defaults to `128M`.

For example, from the torq-compiler-dev repository root:

```bash
QEMU_BUILD_DIR="$PWD/torq-qemu-build" \
./torq-compiler-dev/third_party/qemu/tests/astra_sl_baremetal/run.sh \
  "$PWD/torq-compiler-dev/third_party/torq-hw"
```
