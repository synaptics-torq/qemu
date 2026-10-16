/*
 * TORQ placeholder device
 *
 * Copyright (c) 2026 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_TORQ_H
#define HW_MISC_TORQ_H

#include "hw/core/sysbus.h"
#include "system/address-spaces.h"
#include "qom/object.h"

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

    void *cm;
};

/* Set the address space used for the device's memory (DMA) accesses. */
void torq_set_dma_as(DeviceState *dev, AddressSpace *as);

DeviceState *torq_create(hwaddr addr, qemu_irq irq);

#endif
