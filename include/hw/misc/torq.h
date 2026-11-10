/*
 * TORQ device
 *
 * Copyright (c) 2026 Synaptics Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_TORQ_H
#define HW_MISC_TORQ_H

#include "hw/core/sysbus.h"
#include "hw/misc/berlin_reset.h"
#include "system/address-spaces.h"
#include "qom/object.h"
#include "hw/misc/torq-protocol.h"
#include "qemu/notify.h"
#include "qemu/timer.h"

#define TYPE_TORQ "torq"
OBJECT_DECLARE_SIMPLE_TYPE(TORQState, TORQ)

#define TORQ_MMIO_SIZE 0x00200000

struct TORQState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    hwaddr mmio_base;

    /* Address space used for DMA. Defaults to system memory; the board may
     * redirect it to an SMMU (IOMMU) address space for a stream id. */
    AddressSpace *dma_as;

    MemoryRegion *ram;
    char *server_path;
    char *backend;
    uint32_t timeout_ms;
    int socket_fd;
    pid_t server_pid;
    uint32_t sequence;
    uint64_t generation;
    GArray *mappings;
    IOMMUNotifier iommu_notifier;
    bool notifier_registered;
    Notifier exit_notifier;
    bool exit_registered;
    bool prepared;
    bool busy;
    QEMUTimer *watchdog;
    Error *migration_blocker;
};

/* Set the address space used for the device's memory (DMA) accesses. */
void torq_set_dma_as(DeviceState *dev, AddressSpace *as);

/*
 * NPU core reset line, per the DT "resets" property <&chip 0x378 1 1 0>:
 * register offset 0x378, bit 1, sticky, active-low (assert = bit cleared).
 */
extern const BerlinResetSpec torq_core_rst;

/*
 * Reset-line callback for torq_core_rst: resets the C model when the line is
 * asserted (@opaque must be a TORQState). The board registers it with
 * berlin_chip_ctrl_add_reset_line().
 */
void torq_reset_line_cb(void *opaque, bool asserted);

/* Create the TORQ device at addr on irq. */
DeviceState *torq_create(hwaddr addr, qemu_irq irq, AddressSpace *dma_as,
                          MemoryRegion *ram);

#endif
