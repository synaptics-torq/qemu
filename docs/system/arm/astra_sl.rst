Synaptics Astra SL (``astra_sl``)
================================

The TORQ device runs its backend in a separate ``torq-cmodel-server``
process. QEMU retains the guest register aperture, SMMUv3 translation,
and interrupt line. The server owns the C model, optional MPACT CSS
simulator, or optional AWS FPGA backend.

Build and launch
----------------

Configure the existing C-model include root and library using Meson options
``torq_ext_incdir`` and ``torq_ext_lib``. The include root must provide
``cm/torq_cm.h``. Build both executables::

  ninja -C BUILD qemu-system-aarch64 hw/misc/torq-cmodel-server

The normal ``torq_mpact_sim_incdir`` and ``torq_mpact_sim_lib`` options
enable MPACT in the server, not in QEMU. Existing ``TORQ_MPACT_VERBOSE``
and ``TORQ_MPACT_TRACE_FILE`` environment settings are inherited.

Guest RAM must have a shared file backing. For example, append the normal
kernel, disk, and console arguments to::

  BUILD/qemu-system-aarch64 \
    -machine astra_sl,memory-backend=ram -m 2G \
    -object memory-backend-memfd,id=ram,size=2G,share=on

QEMU finds the server beside its executable or in ``hw/misc`` under the
build directory. Override its executable with::

  -global torq.server=/absolute/path/to/torq-cmodel-server

The default backend is ``cmodel``. ``-global torq.timeout-ms=300000``
sets both the socket timeout and the host-clock job watchdog. A server
failure, invalid access, timeout, or unsupported active-job invalidation
stops the VM with ``io-error``; it does not fabricate successful completion.
QEMU terminates and reaps its child on shutdown.

Execution and synchronization
-----------------------------

Ordinary MMIO accesses are forwarded in order. A 64-bit access becomes
two ordered 32-bit accesses. Device-local LRAM and TCM use the same
register aperture and are not guest-RAM mappings.

Before NSS START, CDMA ENABLECMD, or CSS CPU release, QEMU snapshots the
stream's translations and the server copies mapped guest bytes to local
XRAM at the original IOVAs. FPGA launch writes are issued only after
upload completes. An execution-triggering status read also prepares
memory if no valid snapshot exists. Polling an existing job does not
re-upload inputs over its results.

For the C model, NSS STATUS and CDMA channel/NSEC status reads can execute
work. The server also samples the model IRQ independently after writes,
once memory is prepared; this sampling can run NSS, CSS CPU, and CDMA.
Consequently, a guest waiting for an IRQ need not poll STATUS to make
progress. The model's external-memory callbacks validate the staged
IOVA ranges and permissions and record exact written byte spans.

Before sending an IRQ event or returning an execution-triggering read,
the C-model server writes changed spans back into shared guest RAM.
QEMU marks writable mapped ranges dirty before raising the interrupt.
IRQ events carry levels, including deassertion after acknowledgement.

Snapshots are taken in QEMU using non-faulting stage-1 page-table walks;
scanning unmapped holes does not enqueue SMMU faults. SMMUv3 UNMAP
notifications invalidate staged translations. This is snapshot-at-job
enumeration, not MAP-notifier support: changes to guest page tables alone
do not produce MAP notifications.

AWS FPGA backend
----------------

Configure ``torq_fpga_incdir`` with the AWS SDK userspace
include directory and ``torq_fpga_lib`` with its built management library.
Select it using::

  -global torq.backend=aws-fpga

The adapter follows ``TorqAwsFpga``: slot 0, application PF, BAR0 for
device registers/local memories, and BAR4 DDR-C at ``0x800000000``.
XRAM uses the existing 16-byte transfer units and fourfold BAR address
spacing. A host advisory lock prevents cooperating server processes
from sharing slot 0; other FPGA applications must not use that slot.

Completion and IRQ sources are polled every millisecond. Native AWS
interrupt/event delivery is not implemented. Before completion is
exposed, all device-writable mapped ranges are downloaded to guest RAM.
The guest must therefore grant the device exclusive access to these
buffers for the entire job. Reset currently performs idle register
reinitialization only; it is not a board-level reset or an active-job
abort. Quiesce or reload the FPGA externally after an interrupted job.

Current limits
--------------

* Linux, same-host native ABI, and a 64-bit server process. Protocol v1
  uses fixed-size native-endian structures; both peers must use the same
  endianness and version. The socket is an inherited private Unix
  socketpair; it is not a network or untrusted-server interface.
* One fixed, shared board RAM region and the board's TORQ stream ID.
  RAM hotplug, memory topology changes, ROM/MMIO DMA, stage-2/nested
  translations, and device-state migration are unsupported.
* AArch64 stage-1 translations within the backend's 32-bit IOVA window.
  SMMU bypass/disabled mode stages RAM intersections of the identity map;
  use a translated, bounded job mapping set for normal FPGA operation.
* One job at a time. Mappings, page tables, and job buffers must remain
  stable until completion. IOVA aliases are rejected because discrete
  XRAM does not preserve guest physical-memory alias coherence.
* Unmapped or prohibited C-model accesses fail the server/VM rather than
  reproducing guest-visible SMMU fault-queue transactions. The FPGA
  bitstream must keep accesses within the published mappings; software
  snapshots alone cannot enforce hardware DMA permissions or holes.
* All mapped input memory is uploaded for each job. Dirty-range transfer
  optimization for the FPGA and incremental mapping updates are deferred.

Tests
-----

Run the focused checks from the build directory::

  ./pyvenv/bin/meson test --print-errorlogs '*torq*'

They cover framing/FD passing, staged memory and exact writeback,
register/reset RPC, asynchronous C-model CDMA completion, full-machine
MMIO, non-faulting snapshots, and alias rejection. They do not replace
guest workload tests or AWS hardware validation.
